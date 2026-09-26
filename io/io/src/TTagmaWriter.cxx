// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ROOT/TTagmaWriter.hxx"

#include "ROOT/TTagmaHeader.hxx"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <sys/stat.h>
#endif

namespace ROOT {

namespace {

// Seeks an open stream to an absolute byte offset, 64-bit clean so that a
// store larger than the 32-bit long of some platforms still seeks. Returns 0
// on success.
int Seek(std::FILE *file, std::uint64_t offset)
{
#if defined(_WIN32)
   return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET);
#else
   return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET);
#endif
}

// File size in bytes, or zero when the file cannot be stat'ed.
std::uint64_t FileSize(const char *path)
{
#if defined(_WIN32)
   struct _stat64 st;
   if (::_stat64(path, &st) != 0)
      return 0;
#else
   struct stat st;
   if (::stat(path, &st) != 0)
      return 0;
#endif
   return static_cast<std::uint64_t>(st.st_size);
}

} // namespace

TTagmaWriter::TTagmaWriter(const TTagmaSchema &schema, std::uint64_t entries) : fSchema(schema), fEntries(entries)
{
   if (entries == 0)
      throw std::invalid_argument("TTagmaWriter: the entry count must be nonzero");
   fIndexRecordSize = fSchema.IndexRecordSize();
   std::string why;
   if (!fSchema.Validate(fIndexRecordSize, &why))
      throw std::invalid_argument("TTagmaWriter: " + why);
   if (entries > std::numeric_limits<std::uint64_t>::max() / fIndexRecordSize)
      throw std::invalid_argument("TTagmaWriter: the index region overflows");
}

TTagmaWriter::~TTagmaWriter()
{
   if (fFile != nullptr)
      std::fclose(fFile);
}

bool TTagmaWriter::Open(const char *path)
{
   if (fFile != nullptr || path == nullptr || path[0] == '\0')
      return false;
   std::FILE *file = std::fopen(path, "wb");
   if (file == nullptr)
      return false;
   fFile = file;
   fDataSize = 0;
   fAdded = 0;
   return true;
}

bool TTagmaWriter::AddEvent(const void *scalars, const void *slice)
{
   if (fFile == nullptr || scalars == nullptr || fAdded >= fEntries)
      return false;

   // The index record holds the scalar region, the count fields among it, and
   // the data base of the event.
   std::vector<char> record(static_cast<std::size_t>(fIndexRecordSize), 0);
   const std::uint64_t scalarExtent = fSchema.ScalarExtent();
   std::memcpy(record.data(), scalars, static_cast<std::size_t>(scalarExtent));

   // The counts come out of the record the reader reads them from, so the
   // slice size and the counts the read path resolves cannot disagree.
   const auto &collections = fSchema.GetCollections();
   std::vector<std::uint64_t> counts(collections.size(), 0);
   for (std::size_t i = 0; i < collections.size(); ++i) {
      counts[i] = fSchema.CountOf(i, record.data());
      if (counts[i] > collections[i].fMaxCount)
         return false;
   }
   const std::uint64_t sliceBytes = fSchema.SliceBytes(counts.data());
   if (sliceBytes > 0 && slice == nullptr)
      return false;

   const std::uint64_t dataOffset = IndexBytes() + fDataSize;
   if (dataOffset + sliceBytes < dataOffset) // slice extent overflows uint64
      return false;
   if (fSchema.HasCollections())
      std::memcpy(record.data() + fSchema.DataBaseOffset(), &dataOffset, sizeof(dataOffset));

   const std::uint64_t indexOffset = fAdded * fIndexRecordSize;
   if (Seek(fFile, indexOffset) != 0)
      return false;
   if (std::fwrite(record.data(), 1, record.size(), fFile) != record.size())
      return false;

   if (sliceBytes > 0) {
      if (Seek(fFile, dataOffset) != 0)
         return false;
      if (std::fwrite(slice, 1, static_cast<std::size_t>(sliceBytes), fFile) != static_cast<std::size_t>(sliceBytes))
         return false;
   }

   fDataSize += sliceBytes;
   ++fAdded;
   return true;
}

bool TTagmaWriter::Close()
{
   if (fFile == nullptr)
      return false;
   const bool complete = fAdded == fEntries;
   const bool described = complete ? WriteDescriptor() : false;
   std::FILE *file = fFile;
   fFile = nullptr;
   const bool flushed = std::fclose(file) == 0;
   return complete && described && flushed;
}

// Appends the field table and the descriptor after the payload, so the store
// carries its own layout and schema.
bool TTagmaWriter::WriteDescriptor()
{
   const std::string table = fSchema.Text();

   TTagmaHeader header;
   const TTagmaStore::Layout layout = GetLayout();
   header.fRunMax = layout.fRunMax;
   header.fLumiMax = layout.fLumiMax;
   header.fEventMax = layout.fEventMax;
   header.fRecordSize = layout.fRecordSize;
   header.fDataSize = layout.fDataSize;
   header.fFieldTableBytes = table.size();
   header.fChecksum = TTagmaHeader::Checksum(table.data(), table.size());

   if (Seek(fFile, IndexBytes() + fDataSize) != 0)
      return false;
   if (!table.empty() && std::fwrite(table.data(), 1, table.size(), fFile) != table.size())
      return false;
   const std::array<unsigned char, TTagmaHeader::kSize> bytes = header.Serialize();
   return std::fwrite(bytes.data(), 1, bytes.size(), fFile) == bytes.size();
}

bool TTagmaWriter::ReadStore(const char *path, TTagmaStore::Layout *layout, TTagmaSchema *schema, std::string *why)
{
   const auto reject = [why](const std::string &reason) {
      if (why)
         *why = reason;
      return false;
   };
   if (path == nullptr || layout == nullptr || schema == nullptr)
      return reject("no path or output");

   const std::uint64_t size = FileSize(path);
   if (size < TTagmaHeader::kSize)
      return reject("the store is shorter than its descriptor");

   std::FILE *in = std::fopen(path, "rb");
   if (in == nullptr)
      return reject("cannot open the store");

   const std::uint64_t descriptorOffset = size - TTagmaHeader::kSize;
   std::array<unsigned char, TTagmaHeader::kSize> bytes{};
   if (Seek(in, descriptorOffset) != 0 || std::fread(bytes.data(), 1, bytes.size(), in) != bytes.size()) {
      std::fclose(in);
      return reject("cannot read the descriptor");
   }

   TTagmaHeader header;
   if (!TTagmaHeader::Parse(bytes.data(), bytes.size(), &header, why)) {
      std::fclose(in);
      return false;
   }

   // The field table sits immediately before the descriptor.
   if (header.fFieldTableBytes > descriptorOffset) {
      std::fclose(in);
      return reject("the field table runs past the start of the store");
   }
   std::string table(static_cast<std::size_t>(header.fFieldTableBytes), '\0');
   if (header.fFieldTableBytes > 0) {
      if (Seek(in, descriptorOffset - header.fFieldTableBytes) != 0 ||
          std::fread(&table[0], 1, table.size(), in) != table.size()) {
         std::fclose(in);
         return reject("cannot read the field table");
      }
   }
   std::fclose(in);

   if (TTagmaHeader::Checksum(table.data(), table.size()) != header.fChecksum)
      return reject("the field table checksum disagrees");

   TTagmaSchema parsed;
   if (!parsed.ParseText(table))
      return reject("the field table holds no valid field");

   // The descriptor has to account for the whole file: the payload it names,
   // the field table, and the descriptor itself.
   const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
   if (header.fRunMax > max / header.fLumiMax)
      return reject("the descriptor names a store larger than uint64");
   const std::uint64_t plane = header.fRunMax * header.fLumiMax;
   if (plane > max / header.fEventMax)
      return reject("the descriptor names a store larger than uint64");
   const std::uint64_t records = plane * header.fEventMax;
   if (records > max / header.fRecordSize)
      return reject("the descriptor names a store larger than uint64");
   const std::uint64_t payload = records * header.fRecordSize;
   if (payload > max - header.fDataSize || payload + header.fDataSize > max - header.fFieldTableBytes)
      return reject("the descriptor names a store larger than uint64");
   const std::uint64_t expected = payload + header.fDataSize + header.fFieldTableBytes + TTagmaHeader::kSize;
   if (expected != size)
      return reject("the descriptor disagrees with the store size");

   *layout = header.Layout();
   *schema = std::move(parsed);
   return true;
}

TTagmaStore::Layout TTagmaWriter::GetLayout() const
{
   TTagmaStore::Layout layout;
   layout.fRunMax = 1;
   layout.fLumiMax = 1;
   layout.fEventMax = fEntries;
   layout.fRecordSize = fIndexRecordSize;
   layout.fDataSize = fDataSize;
   return layout;
}

} // namespace ROOT
