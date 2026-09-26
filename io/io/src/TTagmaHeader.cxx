// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ROOT/TTagmaHeader.hxx"

#include <cstring>

namespace ROOT {

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

} // namespace

std::uint64_t TTagmaHeader::Checksum(const void *bytes, std::size_t len)
{
   // FNV-1a, 64-bit.
   std::uint64_t hash = 1469598103934665603ull;
   const unsigned char *data = static_cast<const unsigned char *>(bytes);
   for (std::size_t i = 0; i < len; ++i) {
      hash ^= data[i];
      hash *= 1099511628211ull;
   }
   return hash;
}

TTagmaStore::Layout TTagmaHeader::Layout() const
{
   TTagmaStore::Layout layout;
   layout.fRunMax = fRunMax;
   layout.fLumiMax = fLumiMax;
   layout.fEventMax = fEventMax;
   layout.fRecordSize = fRecordSize;
   layout.fDataSize = fDataSize;
   return layout;
}

std::array<unsigned char, TTagmaHeader::kSize> TTagmaHeader::Serialize() const
{
   std::array<unsigned char, kSize> bytes{};
   std::memcpy(bytes.data(), kMagic.data(), kMagic.size());
   PutLE32(bytes.data() + 8, kVersion);
   PutLE32(bytes.data() + 12, 0); // reserved
   PutLE64(bytes.data() + 16, fRunMax);
   PutLE64(bytes.data() + 24, fLumiMax);
   PutLE64(bytes.data() + 32, fEventMax);
   PutLE64(bytes.data() + 40, fRecordSize);
   PutLE64(bytes.data() + 48, fDataSize);
   PutLE64(bytes.data() + 56, fFieldTableBytes);
   PutLE64(bytes.data() + 64, fChecksum);
   return bytes;
}

bool TTagmaHeader::Parse(const unsigned char *bytes, std::size_t len, TTagmaHeader *out, std::string *why)
{
   const auto reject = [why](const std::string &reason) {
      if (why)
         *why = reason;
      return false;
   };
   if (bytes == nullptr || out == nullptr || len < kSize)
      return reject("the descriptor is shorter than its fixed block");
   if (std::memcmp(bytes, kMagic.data(), kMagic.size()) != 0)
      return reject("the store carries no coordinate descriptor");
   const std::uint32_t version = GetLE32(bytes + 8);
   if (version != kVersion)
      return reject("unknown descriptor version " + std::to_string(version));

   TTagmaHeader header;
   header.fRunMax = GetLE64(bytes + 16);
   header.fLumiMax = GetLE64(bytes + 24);
   header.fEventMax = GetLE64(bytes + 32);
   header.fRecordSize = GetLE64(bytes + 40);
   header.fDataSize = GetLE64(bytes + 48);
   header.fFieldTableBytes = GetLE64(bytes + 56);
   header.fChecksum = GetLE64(bytes + 64);
   if (header.fRunMax == 0 || header.fLumiMax == 0 || header.fEventMax == 0 || header.fRecordSize == 0)
      return reject("the descriptor names a zero axis or a zero record size");
   *out = header;
   return true;
}

} // namespace ROOT
