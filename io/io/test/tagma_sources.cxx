// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

// Verifies the byte-source seams of the reader backend (ssccsorg/ssccs#121):
// TTagmaSource is the interface the coordinate read path asks for a range, so a
// source that wraps another one (the bounded block cache) is an implementation
// the hook uses without branching on it.

#include "ROOT/TTagmaSource.hxx"

#include "gtest/gtest.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

// A source that counts the reads it served, so what the cache asked the
// delegate for can be told from what the caller asked of the cache.
class CountingSource final : public ROOT::TTagmaSource {
public:
   explicit CountingSource(std::vector<char> bytes) : fBytes(std::move(bytes)) {}

   std::int64_t Read(char *buf, std::uint64_t pos, std::uint64_t len) override
   {
      ++fReads;
      if (buf == nullptr || pos > fBytes.size() || len > fBytes.size() - pos)
         return -1;
      std::memcpy(buf, fBytes.data() + pos, static_cast<std::size_t>(len));
      return static_cast<std::int64_t>(len);
   }

   bool IsMapped() const override { return false; }

   std::uint64_t Reads() const { return fReads; }

private:
   std::vector<char> fBytes;
   std::uint64_t fReads = 0;
};

std::vector<char> Pattern(std::size_t size)
{
   std::vector<char> bytes(size);
   for (std::size_t i = 0; i < size; ++i)
      bytes[i] = static_cast<char>(i);
   return bytes;
}

} // namespace

TEST(TTagmaSources, TheCacheServesARepeatFromMemory)
{
   const std::vector<char> bytes = Pattern(1024);
   auto delegate = std::make_shared<CountingSource>(bytes);
   ROOT::TTagmaCachedSource cache(delegate, 256, 8, bytes.size());

   std::vector<char> out(64);
   ASSERT_EQ(cache.Read(out.data(), 300, 64), 64);
   // The whole range is inside one block, so the delegate saw a single read.
   EXPECT_EQ(delegate->Reads(), 1u);
   for (int i = 0; i < 64; ++i)
      EXPECT_EQ(out[i], static_cast<char>(300 + i));

   // A second range inside the same block is a memory copy.
   ASSERT_EQ(cache.Read(out.data(), 320, 32), 32);
   EXPECT_EQ(delegate->Reads(), 1u);
   EXPECT_EQ(cache.Hits(), 1u);
   EXPECT_EQ(cache.Misses(), 1u);
   for (int i = 0; i < 32; ++i)
      EXPECT_EQ(out[i], static_cast<char>(320 + i));

   // A range in another block is a miss.
   ASSERT_EQ(cache.Read(out.data(), 700, 32), 32);
   EXPECT_EQ(delegate->Reads(), 2u);
}

TEST(TTagmaSources, TheCacheEvictsTheLeastRecentlyUsedBlock)
{
   const std::vector<char> bytes = Pattern(1024);
   auto delegate = std::make_shared<CountingSource>(bytes);
   ROOT::TTagmaCachedSource cache(delegate, 256, 2, bytes.size());

   std::vector<char> out(256);
   ASSERT_EQ(cache.Read(out.data(), 0, 256), 256);   // block 0, miss
   ASSERT_EQ(cache.Read(out.data(), 256, 256), 256); // block 1, miss
   ASSERT_EQ(cache.Read(out.data(), 0, 256), 256);   // block 0, hit, now most recent
   ASSERT_EQ(cache.Read(out.data(), 768, 256), 256); // block 3, miss, evicts the least recent
   EXPECT_EQ(cache.Cached(), 2u);
   EXPECT_EQ(delegate->Reads(), 3u);

   // Block 1 was the least recently used, so it is read again.
   ASSERT_EQ(cache.Read(out.data(), 256, 256), 256);
   EXPECT_EQ(delegate->Reads(), 4u);
}

TEST(TTagmaSources, TheCacheRejectsARangePastThePayload)
{
   const std::vector<char> bytes = Pattern(600);
   auto delegate = std::make_shared<CountingSource>(bytes);
   // The last block is only 88 bytes, so a read is clamped to the payload.
   ROOT::TTagmaCachedSource cache(delegate, 256, 4, bytes.size());

   std::vector<char> out(256);
   EXPECT_EQ(cache.Read(out.data(), 0, 256), 256);
   EXPECT_EQ(cache.Read(out.data(), 512, 88), 88);
   EXPECT_EQ(cache.Read(out.data(), 600, 16), -1);
   EXPECT_EQ(cache.Read(out.data(), 512, 256), 88);
}
