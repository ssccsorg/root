// Author: SSCCS Initiative 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

// Verifies the block-compressed byte source of the reader backend
// (ssccsorg/ssccs#121): TTagmaBlockSource keeps the store addressing and
// compresses the bytes under it, so a read decompresses the blocks it touches
// and comes back byte for byte.

#include "ROOT/TTagmaBlockSource.hxx"
#include "ROOT/TTagmaSchema.hxx"
#include "ROOT/TTagmaWriter.hxx"

#include "gtest/gtest.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char *kPlain = "tagma_compressed_plain.bin";
const char *kPacked = "tagma_compressed_packed.bin";
// The payload is 800,000 bytes, so it spans several blocks of kBlockBytes.
constexpr std::uint64_t kEntries = 200000;

ROOT::TTagmaSchema MakeSchema()
{
   ROOT::TTagmaSchema schema;
   ROOT::TTagmaSchema::Field value;
   value.fName = "value";
   value.fOffset = 0;
   value.fType = ROOT::TTagmaSchema::EType::kUInt32;
   schema.AddField(value);
   return schema;
}

// Writes a plain store whose record carries the entry index.
void WritePlain()
{
   ROOT::TTagmaWriter writer(MakeSchema(), kEntries);
   ASSERT_TRUE(writer.Open(kPlain));
   const char dummy = 0;
   for (std::uint64_t entry = 0; entry < kEntries; ++entry) {
      const std::uint32_t value = static_cast<std::uint32_t>(entry);
      ASSERT_TRUE(writer.AddEvent(&value, &dummy));
   }
   ASSERT_TRUE(writer.Close());
}

std::vector<char> ReadFile(const char *path)
{
   std::FILE *in = std::fopen(path, "rb");
   EXPECT_NE(in, nullptr);
   std::vector<char> bytes;
   if (in == nullptr)
      return bytes;
   std::fseek(in, 0, SEEK_END);
   const long size = std::ftell(in);
   std::fseek(in, 0, SEEK_SET);
   bytes.resize(static_cast<std::size_t>(size));
   EXPECT_EQ(std::fread(bytes.data(), 1, bytes.size(), in), bytes.size());
   std::fclose(in);
   return bytes;
}

} // namespace

TEST(TTagmaCompressed, RoundTripsThePayload)
{
   WritePlain();
   const std::vector<char> plain = ReadFile(kPlain);
   ASSERT_FALSE(plain.empty());

   std::string why;
   ASSERT_TRUE(ROOT::TTagmaBlockSource::Compress(kPlain, kPacked, &why)) << why;

   ROOT::TTagmaBlockSource source;
   ASSERT_TRUE(source.Open(kPacked, &why)) << why;
   const std::uint64_t recordSize = source.GetLayout().fRecordSize;
   const std::uint64_t payload = kEntries * recordSize;
   EXPECT_EQ(source.PayloadBytes(), payload);
   EXPECT_EQ(source.GetLayout().fEventMax, kEntries);
   EXPECT_EQ(source.GetSchema().IndexRecordSize(), recordSize);
   EXPECT_EQ(source.GetSchema().Text(), MakeSchema().Text());
   EXPECT_GT(source.BlockCount(), 1u);
   EXPECT_LT(source.FileBytes(), plain.size());

   // The whole payload comes back byte for byte.
   std::vector<char> whole(static_cast<std::size_t>(payload));
   ASSERT_EQ(source.Read(whole.data(), 0, whole.size()), static_cast<std::int64_t>(whole.size()));
   EXPECT_EQ(std::memcmp(whole.data(), plain.data(), whole.size()), 0);

   // A range that crosses a block boundary comes back too.
   const std::uint64_t start = ROOT::TTagmaBlockSource::kBlockBytes - 100;
   std::vector<char> across(3 * recordSize);
   ASSERT_EQ(source.Read(across.data(), start, across.size()), static_cast<std::int64_t>(across.size()));
   EXPECT_EQ(std::memcmp(across.data(), plain.data() + start, across.size()), 0);

   // A read past the payload is refused.
   EXPECT_EQ(source.Read(across.data(), payload, 4), -1);

   std::remove(kPlain);
   std::remove(kPacked);
}

TEST(TTagmaCompressed, KeepsThePlainReaderOutOfTheCompressedStore)
{
   WritePlain();
   std::string why;

   // A plain store is not block-compressed.
   ROOT::TTagmaBlockSource source;
   EXPECT_FALSE(source.Open(kPlain, &why));

   ASSERT_TRUE(ROOT::TTagmaBlockSource::Compress(kPlain, kPacked, &why)) << why;

   // The plain reader refuses the compressed store, so a caller cannot read it
   // as if the payload sat at the offsets the descriptor addresses.
   ROOT::TTagmaStore::Layout layout;
   ROOT::TTagmaSchema schema;
   EXPECT_FALSE(ROOT::TTagmaWriter::ReadStore(kPacked, &layout, &schema, &why));
   EXPECT_TRUE(ROOT::TTagmaWriter::ReadStore(kPlain, &layout, &schema, &why)) << why;

   std::remove(kPlain);
   std::remove(kPacked);
}
