// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ROOT/TTagmaWriter.hxx"

#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/types.h>
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
   std::FILE *file = fFile;
   fFile = nullptr;
   const bool complete = fAdded == fEntries;
   const bool flushed = std::fclose(file) == 0;
   return complete && flushed;
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
