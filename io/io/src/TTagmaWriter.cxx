// Author: SSCCS Initiative 2026

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

namespace {

void PutLE32(unsigned char *bytes, std::uint32_t value)
{
   for (int i = 0; i < 4; ++i)
      bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xffu);
}

void PutLE64(unsigned char *bytes, std::uint64_t value)
{
   for (int i = 0; i < 8; ++i)
      bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xffu);
}

} // namespace

TTagmaWriter::TTagmaWriter(const TTagmaSchema &schema, std::uint64_t entries) : TTagmaWriter(schema, 1, 1, entries) {}

TTagmaWriter::TTagmaWriter(const TTagmaSchema &schema, std::uint64_t entries, std::uint64_t blockEvents)
   : TTagmaWriter(schema, entries)
{
   if (blockEvents == 0)
      throw std::invalid_argument("TTagmaWriter: a columnar block needs a length");
   if (!fSchema.HasCollections())
      throw std::invalid_argument("TTagmaWriter: a columnar block needs a collection");
   fBlockEvents = blockEvents;
}

TTagmaWriter::TTagmaWriter(const TTagmaSchema &schema, std::uint64_t runMax, std::uint64_t lumiMax,
                           std::uint64_t eventMax)
   : fSchema(schema), fRunMax(runMax), fLumiMax(lumiMax)
{
   if (runMax == 0 || lumiMax == 0 || eventMax == 0)
      throw std::invalid_argument("TTagmaWriter: every axis maximum must be nonzero");
   constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
   if (runMax > kMax / lumiMax || runMax * lumiMax > kMax / eventMax)
      throw std::invalid_argument("TTagmaWriter: the record count overflows");
   fEntries = runMax * lumiMax * eventMax;
   fIndexRecordSize = fSchema.IndexRecordSize();
   std::string why;
   if (!fSchema.Validate(fIndexRecordSize, &why))
      throw std::invalid_argument("TTagmaWriter: " + why);
   if (fEntries > kMax / fIndexRecordSize)
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
   fBlockBase = 0;
   fBlockFilled = 0;
   fBlockSlices.clear();
   fBlockCounts.clear();
   fTable.clear();
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

   // A columnar store addresses a block, so every record of a block carries the
   // block's data base; an event-major store carries its own slice.
   if (fBlockEvents != 0 && fBlockFilled == 0)
      fBlockBase = IndexBytes() + fDataSize;
   const std::uint64_t dataOffset = fBlockEvents != 0 ? fBlockBase : IndexBytes() + fDataSize;
   if (dataOffset + sliceBytes < dataOffset) // slice extent overflows uint64
      return false;
   if (fSchema.HasCollections())
      std::memcpy(record.data() + fSchema.DataBaseOffset(), &dataOffset, sizeof(dataOffset));

   const std::uint64_t indexOffset = fAdded * fIndexRecordSize;
   if (Seek(fFile, indexOffset) != 0)
      return false;
   if (std::fwrite(record.data(), 1, record.size(), fFile) != record.size())
      return false;

   if (fBlockEvents == 0) {
      if (sliceBytes > 0) {
         if (Seek(fFile, dataOffset) != 0)
            return false;
         if (std::fwrite(slice, 1, static_cast<std::size_t>(sliceBytes), fFile) !=
             static_cast<std::size_t>(sliceBytes))
            return false;
      }
      fDataSize += sliceBytes;
   } else {
      // The block's slices are transposed when the block is full, so they are
      // buffered rather than written where the record's data base points.
      if (sliceBytes > 0) {
         const char *bytes = static_cast<const char *>(slice);
         fBlockSlices.insert(fBlockSlices.end(), bytes, bytes + sliceBytes);
      }
      fBlockCounts.insert(fBlockCounts.end(), counts.begin(), counts.end());
      if (++fBlockFilled == fBlockEvents && !FlushBlock())
         return false;
   }

   ++fAdded;
   return true;
}

bool TTagmaWriter::FlushBlock()
{
   if (fBlockEvents == 0 || fBlockFilled == 0)
      return true;
   const auto &collections = fSchema.GetCollections();
   const std::uint64_t events = fBlockFilled;
   const std::size_t collectionCount = collections.size();

   // Where each buffered event's slice starts inside the block's buffer.
   std::vector<std::uint64_t> starts(static_cast<std::size_t>(events) + 1, 0);
   for (std::uint64_t e = 0; e < events; ++e)
      starts[static_cast<std::size_t>(e) + 1] =
         starts[static_cast<std::size_t>(e)] + fSchema.SliceBytes(&fBlockCounts[e * collectionCount]);
   const std::uint64_t total = starts[static_cast<std::size_t>(events)];

   // One run per field, in the order the data region holds them: collection 0's
   // fields, then collection 1's, each in schema order.
   std::vector<char> transposed(static_cast<std::size_t>(total), 0);
   std::vector<std::uint64_t> runStart;
   runStart.reserve(static_cast<std::size_t>(fSchema.DataFieldCount()));
   std::uint64_t cursor = 0;
   for (std::size_t c = 0; c < collectionCount; ++c) {
      const auto &collection = collections[c];
      for (const auto &field : collection.fFields) {
         runStart.push_back(cursor);
         for (std::uint64_t e = 0; e < events; ++e) {
            const std::uint64_t *counts = &fBlockCounts[e * collectionCount];
            const std::uint64_t bytes = counts[c] * field.Size();
            if (bytes == 0)
               continue;
            const std::uint64_t from = starts[static_cast<std::size_t>(e)] +
                                       fSchema.ChunkOffset(c, counts) +
                                       fSchema.FieldOffset(collection, field.fName, counts[c]);
            std::memcpy(transposed.data() + cursor, fBlockSlices.data() + from,
                        static_cast<std::size_t>(bytes));
            cursor += bytes;
         }
      }
   }
   if (cursor != total)
      return false;

   if (Seek(fFile, fBlockBase) != 0)
      return false;
   if (total > 0 && std::fwrite(transposed.data(), 1, static_cast<std::size_t>(total), fFile) !=
                       static_cast<std::size_t>(total))
      return false;

   // The table entry: one run start per field, then one object prefix per
   // collection per event, padded to the full block length so the table's size
   // follows from the descriptor and the schema alone.
   const std::uint64_t fields = fSchema.DataFieldCount();
   const std::size_t entryBytes = fields * 8 + collectionCount * fBlockEvents * 4;
   const std::size_t entryStart = fTable.size();
   fTable.resize(entryStart + entryBytes, 0);
   for (std::size_t f = 0; f < runStart.size(); ++f)
      PutLE64(fTable.data() + entryStart + f * 8, runStart[f]);
   for (std::size_t c = 0; c < collectionCount; ++c) {
      std::uint32_t prefix = 0;
      for (std::uint64_t e = 0; e < fBlockEvents; ++e) {
         PutLE32(fTable.data() + entryStart + fields * 8 + (c * fBlockEvents + e) * 4, prefix);
         if (e < events)
            prefix += static_cast<std::uint32_t>(fBlockCounts[e * collectionCount + c]);
      }
   }

   fDataSize += total;
   fBlockFilled = 0;
   fBlockSlices.clear();
   fBlockCounts.clear();
   return true;
}

bool TTagmaWriter::Close()
{
   if (fFile == nullptr)
      return false;
   const bool blockFlushed = FlushBlock();
   const bool complete = fAdded == fEntries;
   const bool described = (complete && blockFlushed) ? WriteDescriptor() : false;
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
   header.fLayoutKind = layout.fLayoutKind;
   header.fBlockEvents = layout.fBlockEvents;
   header.fFieldTableBytes = table.size();
   header.fChecksum = TTagmaHeader::Checksum(table.data(), table.size());

   if (Seek(fFile, IndexBytes() + fDataSize) != 0)
      return false;
   if (!fTable.empty() && std::fwrite(fTable.data(), 1, fTable.size(), fFile) != fTable.size())
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
   if (size < TTagmaHeader::kLegacySize)
      return reject("the store is shorter than its descriptor");

   std::FILE *in = std::fopen(path, "rb");
   if (in == nullptr)
      return reject("cannot open the store");

   // The descriptor ends the file and its magic opens the block, so the sizes this
   // build reads are tried newest first: a legacy block read at the larger size
   // starts past the magic and is skipped, and a candidate whose magic matches is
   // parsed at the size that found it, so a refusal names the real reason.
   TTagmaHeader header;
   std::uint64_t descriptorBytes = 0;
   for (const std::size_t candidate : {TTagmaHeader::kSize, TTagmaHeader::kLegacySize}) {
      if (size < candidate)
         continue;
      std::array<unsigned char, TTagmaHeader::kSize> bytes{};
      if (Seek(in, size - candidate) != 0 || std::fread(bytes.data(), 1, candidate, in) != candidate) {
         std::fclose(in);
         return reject("cannot read the descriptor");
      }
      if (std::memcmp(bytes.data(), TTagmaHeader::kMagic.data(), TTagmaHeader::kMagic.size()) != 0)
         continue;
      std::string parseWhy;
      if (!TTagmaHeader::Parse(bytes.data(), candidate, &header, &parseWhy)) {
         std::fclose(in);
         return reject(parseWhy);
      }
      descriptorBytes = candidate;
      break;
   }
   if (descriptorBytes == 0) {
      std::fclose(in);
      return reject("the store carries no coordinate descriptor");
   }
   const std::uint64_t descriptorOffset = size - descriptorBytes;
   // A block-compressed store ends with the same descriptor at the compressed
   // version; it is read through TTagmaBlockSource, not as a plain store.
   if (TTagmaHeader::IsCompressed(header.fVersion)) {
      std::fclose(in);
      return reject("the store is block-compressed");
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

   // A columnar layout carries a block table between the data region and the
   // field table. Its size follows from the block division and the field table
   // rather than from the descriptor, so the reader derives it here to account
   // for the file. A descriptor whose layout another reader refuses is still
   // read here: the refusal belongs to the reader that walks the layout, as the
   // compressed refusal does.
   std::uint64_t tableBytes = 0;
   if (header.fLayoutKind == TTagmaHeader::kColumnarBlockLayout) {
      const std::uint64_t blocks = records / header.fBlockEvents + (records % header.fBlockEvents != 0 ? 1 : 0);
      try {
         tableBytes = TTagmaStore::BlockTableBytes(blocks, parsed.DataFieldCount(),
                                                   parsed.GetCollections().size(), header.fBlockEvents);
      } catch (const std::invalid_argument &) {
         return reject("the block table the descriptor implies is larger than uint64");
      }
   }
   if (payload > max - header.fDataSize || payload + header.fDataSize > max - tableBytes ||
       payload + header.fDataSize + tableBytes > max - header.fFieldTableBytes)
      return reject("the descriptor names a store larger than uint64");
   const std::uint64_t expected =
      payload + header.fDataSize + tableBytes + header.fFieldTableBytes + descriptorBytes;
   if (expected != size)
      return reject("the descriptor disagrees with the store size");

   *layout = header.Layout();
   *schema = std::move(parsed);
   return true;
}

TTagmaStore::Layout TTagmaWriter::GetLayout() const
{
   TTagmaStore::Layout layout;
   layout.fRunMax = fRunMax;
   layout.fLumiMax = fLumiMax;
   layout.fEventMax = fEntries / (fRunMax * fLumiMax);
   layout.fRecordSize = fIndexRecordSize;
   layout.fDataSize = fDataSize;
   layout.fLayoutKind =
      fBlockEvents != 0 ? TTagmaHeader::kColumnarBlockLayout : TTagmaHeader::kEventMajorLayout;
   layout.fBlockEvents = fBlockEvents;
   return layout;
}

} // namespace ROOT
