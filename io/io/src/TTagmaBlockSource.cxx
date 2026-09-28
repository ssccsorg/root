// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ROOT/TTagmaBlockSource.hxx"

#include "ROOT/TTagmaHeader.hxx"
#include "ROOT/TTagmaWriter.hxx"

#include "RZip.h"

#include <algorithm>
#include <array>
#include <cstring>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace ROOT {

namespace {

// Bytes of one block table entry: the file offset, the stored bytes, and the
// bytes the block decompresses to.
constexpr std::size_t kEntryBytes = 16;

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

std::uint32_t GetLE32(const unsigned char *bytes)
{
   std::uint32_t value = 0;
   for (int i = 0; i < 4; ++i)
      value |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
   return value;
}

std::uint64_t GetLE64(const unsigned char *bytes)
{
   std::uint64_t value = 0;
   for (int i = 0; i < 8; ++i)
      value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
   return value;
}

int Seek(std::FILE *file, std::uint64_t offset)
{
#if defined(_WIN32)
   return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET);
#else
   return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET);
#endif
}

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

TTagmaBlockSource::~TTagmaBlockSource()
{
   Close();
}

void TTagmaBlockSource::Close()
{
   if (fFile != nullptr) {
      std::fclose(fFile);
      fFile = nullptr;
   }
}

bool TTagmaBlockSource::Open(const std::string &path, std::string *why)
{
   Close();
   const auto reject = [why](const std::string &reason) {
      if (why)
         *why = reason;
      return false;
   };

   fFileBytes = FileSize(path.c_str());
   if (fFileBytes < TTagmaHeader::kSize + kEntryBytes)
      return reject("the store is shorter than its descriptor");

   std::FILE *in = std::fopen(path.c_str(), "rb");
   if (in == nullptr)
      return reject("cannot open the store");

   std::array<unsigned char, TTagmaHeader::kSize> bytes{};
   if (Seek(in, fFileBytes - TTagmaHeader::kSize) != 0 ||
       std::fread(bytes.data(), 1, bytes.size(), in) != bytes.size()) {
      std::fclose(in);
      return reject("cannot read the descriptor");
   }

   TTagmaHeader header;
   if (!TTagmaHeader::Parse(bytes.data(), bytes.size(), &header, why)) {
      std::fclose(in);
      return false;
   }
   if (header.fVersion != TTagmaHeader::kCompressedVersion) {
      std::fclose(in);
      return reject("the store is not block-compressed");
   }

   fLayout = header.Layout();
   const TTagmaStore store(fLayout);
   fPayloadBytes = store.SizeBytes();
   if (fPayloadBytes == 0) {
      std::fclose(in);
      return reject("the descriptor names an empty payload");
   }

   const std::uint64_t trailer = fFileBytes - TTagmaHeader::kSize;
   if (header.fFieldTableBytes > trailer) {
      std::fclose(in);
      return reject("the field table runs past the start of the store");
   }
   fBlockCount = (fPayloadBytes + kBlockBytes - 1) / kBlockBytes;
   const std::uint64_t tableBytes = fBlockCount * kEntryBytes;
   if (header.fFieldTableBytes + tableBytes > trailer) {
      std::fclose(in);
      return reject("the block table runs past the start of the store");
   }
   const std::uint64_t tableOffset = trailer - header.fFieldTableBytes - tableBytes;

   std::vector<unsigned char> table(static_cast<std::size_t>(tableBytes));
   if (Seek(in, tableOffset) != 0 || std::fread(table.data(), 1, table.size(), in) != table.size()) {
      std::fclose(in);
      return reject("cannot read the block table");
   }
   fBlocks.clear();
   fBlocks.reserve(static_cast<std::size_t>(fBlockCount));
   std::uint64_t covered = 0;
   for (std::uint64_t i = 0; i < fBlockCount; ++i) {
      Block block;
      block.fOffset = GetLE64(&table[i * kEntryBytes]);
      block.fStoredBytes = GetLE32(&table[i * kEntryBytes + 8]);
      block.fUncompressedBytes = GetLE32(&table[i * kEntryBytes + 12]);
      if (block.fStoredBytes == 0 || block.fUncompressedBytes == 0 || block.fOffset + block.fStoredBytes > tableOffset) {
         std::fclose(in);
         return reject("the block table names a block outside the store");
      }
      covered += block.fUncompressedBytes;
      fBlocks.push_back(block);
   }
   if (covered != fPayloadBytes) {
      std::fclose(in);
      return reject("the block table does not cover the payload");
   }

   // The field table sits immediately before the descriptor, after the block
   // table.
   std::string text(static_cast<std::size_t>(header.fFieldTableBytes), '\0');
   if (header.fFieldTableBytes > 0) {
      if (Seek(in, trailer - header.fFieldTableBytes) != 0 ||
          std::fread(&text[0], 1, text.size(), in) != text.size()) {
         std::fclose(in);
         return reject("cannot read the field table");
      }
   }
   if (TTagmaHeader::Checksum(text.data(), text.size()) != header.fChecksum) {
      std::fclose(in);
      return reject("the field table does not match its checksum");
   }
   if (!text.empty() && !fSchema.ParseText(text)) {
      std::fclose(in);
      return reject("the field table does not parse");
   }

   fFile = in;
   return true;
}

const char *TTagmaBlockSource::Load(const Block &block)
{
   if (block.fUncompressedBytes > kBlockBytes * 2)
      return nullptr;
   if (Seek(fFile, block.fOffset) != 0)
      return nullptr;
   fScratch.assign(static_cast<std::size_t>(block.fUncompressedBytes), '\0');
   if (block.fStoredBytes == block.fUncompressedBytes) {
      // The block did not shrink and is stored whole.
      if (std::fread(fScratch.data(), 1, fScratch.size(), fFile) != fScratch.size())
         return nullptr;
      return fScratch.data();
   }
   std::vector<unsigned char> compressed(static_cast<std::size_t>(block.fStoredBytes));
   if (std::fread(compressed.data(), 1, compressed.size(), fFile) != compressed.size())
      return nullptr;
   int srcsize = static_cast<int>(block.fStoredBytes);
   int tgtsize = static_cast<int>(block.fUncompressedBytes);
   int irep = 0;
   R__unzip(&srcsize, compressed.data(), &tgtsize, reinterpret_cast<unsigned char *>(fScratch.data()), &irep);
   if (irep <= 0 || static_cast<std::uint64_t>(irep) != block.fUncompressedBytes)
      return nullptr;
   return fScratch.data();
}

std::int64_t TTagmaBlockSource::Read(char *buf, std::uint64_t pos, std::uint64_t len)
{
   if (fFile == nullptr || buf == nullptr || pos >= fPayloadBytes)
      return -1;
   len = std::min(len, fPayloadBytes - pos);
   std::uint64_t at = pos;
   std::uint64_t left = len;
   char *out = buf;
   while (left > 0) {
      const std::uint64_t index = at / kBlockBytes;
      if (index >= fBlocks.size())
         return -1;
      const Block &block = fBlocks[static_cast<std::size_t>(index)];
      const std::uint64_t within = at - index * kBlockBytes;
      if (within >= block.fUncompressedBytes)
         return -1;
      const char *bytes = Load(block);
      if (bytes == nullptr)
         return -1;
      const std::uint64_t take = std::min(left, block.fUncompressedBytes - within);
      std::memcpy(out, bytes + within, static_cast<std::size_t>(take));
      out += take;
      at += take;
      left -= take;
   }
   return static_cast<std::int64_t>(len);
}

bool TTagmaBlockSource::Compress(const std::string &inPath, const std::string &outPath, std::string *why)
{
   const auto reject = [why](const std::string &reason) {
      if (why)
         *why = reason;
      return false;
   };

   TTagmaStore::Layout layout;
   TTagmaSchema schema;
   if (!TTagmaWriter::ReadStore(inPath.c_str(), &layout, &schema, why))
      return false;

   const TTagmaStore store(layout);
   const std::uint64_t payload = store.SizeBytes();
   if (payload == 0)
      return reject("the store carries an empty payload");
   const std::string table = schema.Text();

   std::FILE *in = std::fopen(inPath.c_str(), "rb");
   if (in == nullptr)
      return reject("cannot open the store");
   std::FILE *out = std::fopen(outPath.c_str(), "wb");
   if (out == nullptr) {
      std::fclose(in);
      return reject("cannot create the compressed store");
   }

   const std::uint64_t blockCount = (payload + kBlockBytes - 1) / kBlockBytes;
   std::vector<unsigned char> entries(static_cast<std::size_t>(blockCount * kEntryBytes));
   std::vector<char> source(static_cast<std::size_t>(kBlockBytes));
   std::vector<char> target(static_cast<std::size_t>(kBlockBytes + kBlockBytes / 2 + 1024));

   bool ok = true;
   std::uint64_t offset = 0;
   for (std::uint64_t i = 0; ok && i < blockCount; ++i) {
      const std::uint64_t start = i * kBlockBytes;
      const std::uint64_t size = std::min(kBlockBytes, payload - start);
      if (Seek(in, start) != 0 || std::fread(source.data(), 1, size, in) != size) {
         ok = false;
         break;
      }
      int srcsize = static_cast<int>(size);
      int tgtsize = static_cast<int>(target.size());
      int irep = 0;
      R__zipMultipleAlgorithm(1, &srcsize, source.data(), &tgtsize, target.data(), &irep,
                              RCompressionSetting::EAlgorithm::kZLIB);
      const char *bytes = source.data();
      std::uint64_t stored = size;
      if (irep > 0 && static_cast<std::uint64_t>(irep) < size) {
         stored = static_cast<std::uint64_t>(irep);
         bytes = target.data();
      }
      PutLE64(&entries[i * kEntryBytes], offset);
      PutLE32(&entries[i * kEntryBytes + 8], static_cast<std::uint32_t>(stored));
      PutLE32(&entries[i * kEntryBytes + 12], static_cast<std::uint32_t>(size));
      if (std::fwrite(bytes, 1, stored, out) != stored) {
         ok = false;
         break;
      }
      offset += stored;
   }

   if (ok && std::fwrite(entries.data(), 1, entries.size(), out) != entries.size())
      ok = false;
   if (ok && !table.empty() && std::fwrite(table.data(), 1, table.size(), out) != table.size())
      ok = false;
   if (ok) {
      TTagmaHeader header;
      header.fVersion = TTagmaHeader::kCompressedVersion;
      header.fRunMax = layout.fRunMax;
      header.fLumiMax = layout.fLumiMax;
      header.fEventMax = layout.fEventMax;
      header.fRecordSize = layout.fRecordSize;
      header.fDataSize = layout.fDataSize;
      header.fFieldTableBytes = table.size();
      header.fChecksum = TTagmaHeader::Checksum(table.data(), table.size());
      const std::array<unsigned char, TTagmaHeader::kSize> bytes = header.Serialize();
      if (std::fwrite(bytes.data(), 1, bytes.size(), out) != bytes.size())
         ok = false;
   }

   std::fclose(in);
   std::fclose(out);
   if (!ok)
      return reject("cannot write the compressed store");
   return true;
}

} // namespace ROOT
