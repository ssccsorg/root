// Author: SSCCS Initiative 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

// Verifies the store-side producer of the reader backend (ssccsorg/ssccs#121):
// TTagmaWriter lays out the index region and the packed data region exactly as
// TTagmaSchema addresses them, so the store a writer produces is the store the
// read path consumes. The bytes are checked against the schema arithmetic, not
// through the read path, which the tree gate covers.

#include "ROOT/TTagmaColumn.hxx"
#include "ROOT/TTagmaHeader.hxx"
#include "ROOT/TTagmaSchema.hxx"
#include "ROOT/TTagmaStore.hxx"
#include "ROOT/TTagmaWriter.hxx"

#include "gtest/gtest.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char *kStorePath = "tagma_writer_store.bin";
constexpr int kEntries = 4;
constexpr std::uint64_t kIndexRecordSize = 16;
constexpr std::uint64_t kDataBaseOffset = 8;
constexpr std::uint32_t kMaxMuon = 4;
constexpr std::uint32_t kMaxJet = 3;

const std::uint32_t kMuonCount[kEntries] = {2, 0, 4, 1};
const std::uint32_t kJetCount[kEntries] = {1, 3, 0, 2};

float MuonPt(int entry, std::uint32_t i)
{
   return 10 * entry + i + 0.5f;
}
float MuonEta(int entry, std::uint32_t i)
{
   return 100 * entry + i + 0.25f;
}
float JetPt(int entry, std::uint32_t i)
{
   return 1000 * entry + i + 0.125f;
}

std::uint64_t SliceBytes(int entry)
{
   return kMuonCount[entry] * 4ull * 2 + kJetCount[entry] * 4ull;
}

ROOT::TTagmaSchema MakeSchema()
{
   ROOT::TTagmaSchema schema;
   ROOT::TTagmaSchema::Field nMuon;
   nMuon.fName = "nMuon";
   nMuon.fOffset = 0;
   nMuon.fType = ROOT::TTagmaSchema::EType::kUInt32;
   schema.AddField(nMuon);

   ROOT::TTagmaSchema::Field muonPt;
   muonPt.fName = "Muon_pt";
   muonPt.fCountField = "nMuon";
   muonPt.fMaxCount = kMaxMuon;
   muonPt.fOffset = 0;
   muonPt.fType = ROOT::TTagmaSchema::EType::kFloat;
   schema.AddField(muonPt);

   ROOT::TTagmaSchema::Field muonEta;
   muonEta.fName = "Muon_eta";
   muonEta.fCountField = "nMuon";
   muonEta.fMaxCount = kMaxMuon;
   muonEta.fOffset = 4;
   muonEta.fType = ROOT::TTagmaSchema::EType::kFloat;
   schema.AddField(muonEta);

   ROOT::TTagmaSchema::Field nJet;
   nJet.fName = "nJet";
   nJet.fOffset = 4;
   nJet.fType = ROOT::TTagmaSchema::EType::kUInt32;
   schema.AddField(nJet);

   ROOT::TTagmaSchema::Field jetPt;
   jetPt.fName = "Jet_pt";
   jetPt.fCountField = "nJet";
   jetPt.fMaxCount = kMaxJet;
   jetPt.fOffset = 0;
   jetPt.fType = ROOT::TTagmaSchema::EType::kFloat;
   schema.AddField(jetPt);
   return schema;
}

// The scalar region of one event, at the schema offsets: nMuon at 0, nJet at 4.
void ScalarRegion(int entry, char *scalars)
{
   std::memset(scalars, 0, 8);
   std::memcpy(scalars + 0, &kMuonCount[entry], sizeof(kMuonCount[entry]));
   std::memcpy(scalars + 4, &kJetCount[entry], sizeof(kJetCount[entry]));
}

// One event's packed data slice: the two muon fields then the jet field, each
// field contiguous.
std::vector<char> Slice(int entry)
{
   std::vector<char> data;
   const auto append = [&data](float value) {
      const char *bytes = reinterpret_cast<const char *>(&value);
      data.insert(data.end(), bytes, bytes + sizeof(value));
   };
   for (std::uint32_t i = 0; i < kMuonCount[entry]; ++i)
      append(MuonPt(entry, i));
   for (std::uint32_t i = 0; i < kMuonCount[entry]; ++i)
      append(MuonEta(entry, i));
   for (std::uint32_t i = 0; i < kJetCount[entry]; ++i)
      append(JetPt(entry, i));
   return data;
}

void WriteStore()
{
   ROOT::TTagmaWriter writer(MakeSchema(), kEntries);
   ASSERT_TRUE(writer.Open(kStorePath));
   for (int entry = 0; entry < kEntries; ++entry) {
      char scalars[8];
      ScalarRegion(entry, scalars);
      const std::vector<char> slice = Slice(entry);
      ASSERT_TRUE(writer.AddEvent(scalars, slice.data()));
   }
   ASSERT_TRUE(writer.Close());
}

std::vector<char> ReadFile()
{
   FILE *in = std::fopen(kStorePath, "rb");
   EXPECT_NE(in, nullptr);
   std::vector<char> bytes;
   if (in == nullptr)
      return bytes;
   char buffer[4096];
   std::size_t got = 0;
   while ((got = std::fread(buffer, 1, sizeof(buffer), in)) > 0)
      bytes.insert(bytes.end(), buffer, buffer + got);
   std::fclose(in);
   return bytes;
}

template <typename T>
T ReadAt(const std::vector<char> &bytes, std::size_t offset)
{
   T value = 0;
   std::memcpy(&value, bytes.data() + offset, sizeof(value));
   return value;
}

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

void WriteFile(const std::vector<char> &bytes)
{
   FILE *out = std::fopen(kStorePath, "wb");
   ASSERT_NE(out, nullptr);
   ASSERT_EQ(std::fwrite(bytes.data(), 1, bytes.size(), out), bytes.size());
   std::fclose(out);
}

// The fixture written as a block-transposed store, with the geometry the table
// of that store is addressed with.
struct ColumnarFixture {
   ROOT::TTagmaSchema fSchema = MakeSchema();
   std::uint64_t fBlockEvents = 3; // four entries: one full block and one partial
   std::vector<char> fBytes;
   std::size_t fTableAt = 0;
   ROOT::TTagmaStore::BlockTable fTable;
};

ColumnarFixture MakeColumnarFixture()
{
   ColumnarFixture fixture;
   ROOT::TTagmaWriter writer(fixture.fSchema, kEntries, fixture.fBlockEvents);
   EXPECT_TRUE(writer.Open(kStorePath));
   for (int entry = 0; entry < kEntries; ++entry) {
      char scalars[8];
      ScalarRegion(entry, scalars);
      const std::vector<char> slice = Slice(entry);
      EXPECT_TRUE(writer.AddEvent(scalars, slice.data()));
   }
   EXPECT_TRUE(writer.Close());
   fixture.fBytes = ReadFile();
   if (fixture.fBytes.empty())
      return fixture;

   ROOT::TTagmaHeader header;
   std::string why;
   EXPECT_TRUE(ROOT::TTagmaHeader::Parse(
      reinterpret_cast<const unsigned char *>(fixture.fBytes.data()) + fixture.fBytes.size() -
         ROOT::TTagmaHeader::kSize,
      ROOT::TTagmaHeader::kSize, &header, &why)) << why;
   fixture.fTableAt = static_cast<std::size_t>(kEntries * fixture.fSchema.IndexRecordSize() + header.fDataSize);
   fixture.fTable = {fixture.fSchema.DataFieldCount(), fixture.fSchema.GetCollections().size(),
                     fixture.fBlockEvents};
   return fixture;
}

// Bytes one block's data region holds: the field bytes of its events, which the
// writer packs without a gap between runs.
std::uint64_t BlockSpan(const ROOT::TTagmaSchema &schema, const std::vector<char> &bytes,
                        std::uint64_t block, std::uint64_t blockEvents, std::uint64_t records)
{
   const auto &collections = schema.GetCollections();
   const std::uint64_t events = std::min(blockEvents, records - block * blockEvents);
   std::uint64_t span = 0;
   for (std::uint64_t e = 0; e < events; ++e) {
      const char *record = bytes.data() + (block * blockEvents + e) * schema.IndexRecordSize();
      for (std::size_t c = 0; c < collections.size(); ++c)
         span += schema.CountOf(c, record) * collections[c].ElementBytes();
   }
   return span;
}

// The counts and the block base of one event, both read from its index record.
std::vector<std::uint64_t> EventCounts(const ROOT::TTagmaSchema &schema, const std::vector<char> &bytes,
                                       std::uint64_t entry)
{
   const char *record = bytes.data() + entry * schema.IndexRecordSize();
   std::vector<std::uint64_t> counts(schema.GetCollections().size(), 0);
   for (std::size_t c = 0; c < counts.size(); ++c)
      counts[c] = schema.CountOf(c, record);
   return counts;
}

std::uint64_t EventBase(const ROOT::TTagmaSchema &schema, const std::vector<char> &bytes, std::uint64_t entry)
{
   std::uint64_t base = 0;
   std::memcpy(&base, bytes.data() + entry * schema.IndexRecordSize() + schema.DataBaseOffset(), sizeof(base));
   return base;
}

} // namespace

TEST(TTagmaWriter, LaysOutTheIndexAndDataRegions)
{
   const ROOT::TTagmaSchema schema = MakeSchema();
   ASSERT_EQ(schema.IndexRecordSize(), kIndexRecordSize);
   ASSERT_EQ(schema.DataBaseOffset(), kDataBaseOffset);

   WriteStore();

   std::uint64_t dataSize = 0;
   for (int entry = 0; entry < kEntries; ++entry)
      dataSize += SliceBytes(entry);
   const std::uint64_t indexBytes = kEntries * kIndexRecordSize;

   const std::vector<char> bytes = ReadFile();
   // The payload is the index region then the data region; the field table
   // and the descriptor follow, checked by the descriptor tests.
   ASSERT_GE(bytes.size(), indexBytes + dataSize);

   // The index region: one record per event, the counts at their offsets and
   // the data base at the offset the schema resolves.
   std::uint64_t expectedBase = indexBytes;
   for (int entry = 0; entry < kEntries; ++entry) {
      const std::size_t record = static_cast<std::size_t>(entry) * kIndexRecordSize;
      EXPECT_EQ(ReadAt<std::uint32_t>(bytes, record + 0), kMuonCount[entry]) << "entry " << entry;
      EXPECT_EQ(ReadAt<std::uint32_t>(bytes, record + 4), kJetCount[entry]) << "entry " << entry;
      EXPECT_EQ(ReadAt<std::uint64_t>(bytes, record + kDataBaseOffset), expectedBase) << "entry " << entry;
      expectedBase += SliceBytes(entry);
   }
   EXPECT_EQ(expectedBase, indexBytes + dataSize);

   // The data region: each event's slice packs the fields in schema order,
   // each field contiguous, at the base the index record names.
   for (int entry = 0; entry < kEntries; ++entry) {
      const std::uint64_t base =
         ReadAt<std::uint64_t>(bytes, static_cast<std::size_t>(entry) * kIndexRecordSize + kDataBaseOffset);
      const std::vector<char> expected = Slice(entry);
      ASSERT_LE(base + expected.size(), bytes.size());
      EXPECT_EQ(std::memcmp(bytes.data() + base, expected.data(), expected.size()), 0) << "entry " << entry;
   }
}

TEST(TTagmaWriter, RejectsACountPastTheSchemaBound)
{
   ROOT::TTagmaWriter writer(MakeSchema(), kEntries);
   ASSERT_TRUE(writer.Open(kStorePath));

   // Five muons against a bound of four: the event is rejected and does not
   // consume a record, so the next, valid event still lands first.
   char scalars[8];
   ScalarRegion(0, scalars);
   std::memcpy(scalars + 0, &kMaxMuon, sizeof(kMaxMuon));
   const std::uint32_t tooMany = kMaxMuon + 1;
   std::memcpy(scalars + 0, &tooMany, sizeof(tooMany));
   const std::vector<char> slice = Slice(0);
   EXPECT_FALSE(writer.AddEvent(scalars, slice.data()));
   EXPECT_EQ(writer.DataSize(), 0u);

   for (int entry = 0; entry < kEntries; ++entry) {
      ScalarRegion(entry, scalars);
      const std::vector<char> data = Slice(entry);
      EXPECT_TRUE(writer.AddEvent(scalars, data.data())) << "entry " << entry;
   }
   EXPECT_TRUE(writer.Close());
}

TEST(TTagmaWriter, RejectsEventsPastTheEntryCount)
{
   ROOT::TTagmaWriter writer(MakeSchema(), 1);
   ASSERT_TRUE(writer.Open(kStorePath));
   char scalars[8];
   ScalarRegion(0, scalars);
   const std::vector<char> slice = Slice(0);
   EXPECT_TRUE(writer.AddEvent(scalars, slice.data()));
   EXPECT_FALSE(writer.AddEvent(scalars, slice.data()));
   EXPECT_TRUE(writer.Close());
   std::remove(kStorePath);
}

TEST(TTagmaWriter, CloseReportsAnIncompleteStore)
{
   ROOT::TTagmaWriter writer(MakeSchema(), 2);
   ASSERT_TRUE(writer.Open(kStorePath));
   char scalars[8];
   ScalarRegion(0, scalars);
   const std::vector<char> slice = Slice(0);
   ASSERT_TRUE(writer.AddEvent(scalars, slice.data()));
   EXPECT_FALSE(writer.Close());
   std::remove(kStorePath);
}

TEST(TTagmaWriter, RequiresOpenBeforeTheFirstEvent)
{
   ROOT::TTagmaWriter writer(MakeSchema(), kEntries);
   char scalars[8];
   ScalarRegion(0, scalars);
   const std::vector<char> slice = Slice(0);
   EXPECT_FALSE(writer.AddEvent(scalars, slice.data()));
   EXPECT_FALSE(writer.Close());
   EXPECT_FALSE(writer.Open(""));
}

TEST(TTagmaWriter, GetLayoutMatchesTheWrittenStore)
{
   ROOT::TTagmaWriter writer(MakeSchema(), kEntries);
   ASSERT_TRUE(writer.Open(kStorePath));
   for (int entry = 0; entry < kEntries; ++entry) {
      char scalars[8];
      ScalarRegion(entry, scalars);
      const std::vector<char> data = Slice(entry);
      ASSERT_TRUE(writer.AddEvent(scalars, data.data()));
   }
   ASSERT_TRUE(writer.Close());

   std::uint64_t dataSize = 0;
   for (int entry = 0; entry < kEntries; ++entry)
      dataSize += SliceBytes(entry);

   const ROOT::TTagmaStore::Layout layout = writer.GetLayout();
   EXPECT_EQ(layout.fEventMax, static_cast<std::uint64_t>(kEntries));
   EXPECT_EQ(layout.fRecordSize, kIndexRecordSize);
   EXPECT_EQ(layout.fDataSize, dataSize);

   // The layout drives a store whose extent is exactly the file the writer
   // laid out, and the store covers every event's slice.
   const ROOT::TTagmaStore store(layout);
   EXPECT_EQ(store.SizeBytes(), kEntries * kIndexRecordSize + dataSize);
   std::uint64_t base = kEntries * kIndexRecordSize;
   for (int entry = 0; entry < kEntries; ++entry) {
      EXPECT_TRUE(store.Covers(base, SliceBytes(entry)));
      base += SliceBytes(entry);
   }
   std::remove(kStorePath);
}

TEST(TTagmaWriter, DescriptorMakesTheStoreSelfDescribing)
{
   const ROOT::TTagmaSchema schema = MakeSchema();
   WriteStore();

   std::uint64_t dataSize = 0;
   for (int entry = 0; entry < kEntries; ++entry)
      dataSize += SliceBytes(entry);
   const std::string table = schema.Text();

   // The file is the payload, then the field table, then the fixed descriptor.
   const std::vector<char> bytes = ReadFile();
   ASSERT_EQ(bytes.size(), kEntries * kIndexRecordSize + dataSize + table.size() + ROOT::TTagmaHeader::kSize);

   // The store carries its own layout and schema: no sidecar is read.
   ROOT::TTagmaStore::Layout layout;
   ROOT::TTagmaSchema read;
   std::string why;
   ASSERT_TRUE(ROOT::TTagmaWriter::ReadStore(kStorePath, &layout, &read, &why)) << why;
   EXPECT_EQ(layout.fRunMax, 1u);
   EXPECT_EQ(layout.fLumiMax, 1u);
   EXPECT_EQ(layout.fEventMax, static_cast<std::uint64_t>(kEntries));
   EXPECT_EQ(layout.fRecordSize, kIndexRecordSize);
   EXPECT_EQ(layout.fDataSize, dataSize);
   EXPECT_EQ(layout.fLayoutKind, ROOT::TTagmaHeader::kEventMajorLayout);
   EXPECT_EQ(layout.fBlockEvents, 0u);
   EXPECT_EQ(read.Text(), table);

   // The block declares its generation and the event-major layout, which is what
   // a later reader needs to tell the descriptors apart.
   const std::size_t descriptor = bytes.size() - ROOT::TTagmaHeader::kSize;
   EXPECT_EQ(ReadAt<std::uint32_t>(bytes, descriptor + 8), ROOT::TTagmaHeader::kVersion);
   EXPECT_EQ(ReadAt<std::uint64_t>(bytes, descriptor + 72), ROOT::TTagmaHeader::kEventMajorLayout);
   EXPECT_EQ(ReadAt<std::uint64_t>(bytes, descriptor + 80), 0u);

   std::remove(kStorePath);
}

TEST(TTagmaWriter, DescriptorRejectsACorruptedFieldTable)
{
   WriteStore();
   std::vector<char> bytes = ReadFile();
   ASSERT_GT(bytes.size(), ROOT::TTagmaHeader::kSize);

   // Flip a byte of the field table, which sits just before the descriptor.
   const std::size_t tableByte = bytes.size() - ROOT::TTagmaHeader::kSize - 1;
   bytes[tableByte] = bytes[tableByte] == 'X' ? 'Y' : 'X';
   FILE *out = std::fopen(kStorePath, "wb");
   ASSERT_NE(out, nullptr);
   ASSERT_EQ(std::fwrite(bytes.data(), 1, bytes.size(), out), bytes.size());
   std::fclose(out);

   ROOT::TTagmaStore::Layout layout;
   ROOT::TTagmaSchema read;
   std::string why;
   EXPECT_FALSE(ROOT::TTagmaWriter::ReadStore(kStorePath, &layout, &read, &why));
   EXPECT_FALSE(why.empty());
   std::remove(kStorePath);
}

TEST(TTagmaWriter, DescriptorRejectsAFileWithoutOne)
{
   FILE *out = std::fopen(kStorePath, "wb");
   ASSERT_NE(out, nullptr);
   const std::vector<char> junk(256, 'j');
   ASSERT_EQ(std::fwrite(junk.data(), 1, junk.size(), out), junk.size());
   std::fclose(out);

   ROOT::TTagmaStore::Layout layout;
   ROOT::TTagmaSchema read;
   std::string why;
   EXPECT_FALSE(ROOT::TTagmaWriter::ReadStore(kStorePath, &layout, &read, &why));
   EXPECT_FALSE(why.empty());
   std::remove(kStorePath);
}

TEST(TTagmaWriter, SchemaTextRoundTripsAndRejectsGarbage)
{
   const ROOT::TTagmaSchema schema = MakeSchema();
   const std::string text = schema.Text();

   ROOT::TTagmaSchema parsed;
   ASSERT_TRUE(parsed.ParseText(text));
   EXPECT_EQ(parsed.Text(), text);

   // A malformed line leaves the schema unchanged.
   ROOT::TTagmaSchema unchanged = parsed;
   EXPECT_FALSE(unchanged.ParseText("Muon_pt notanumber float nMuon 4\n"));
   EXPECT_EQ(unchanged.Text(), text);
}

// A store an earlier build wrote still reads. The payload here is the store this
// build writes and only the trailing block is replaced, so the case isolates the
// descriptor generation rather than the layout of the data.
TEST(TTagmaWriter, ReadsAStoreThatCarriesTheLegacyDescriptor)
{
   WriteStore();
   std::vector<char> bytes = ReadFile();
   ASSERT_GT(bytes.size(), ROOT::TTagmaHeader::kSize);

   // The legacy block is the current one truncated at the checksum with the
   // legacy version written into it, which also says the two generations agree on
   // every field they share.
   const std::size_t offset = bytes.size() - ROOT::TTagmaHeader::kSize;
   std::vector<unsigned char> legacy(bytes.begin() + offset,
                                    bytes.begin() + offset + ROOT::TTagmaHeader::kLegacySize);
   PutLE32(legacy.data() + 8, ROOT::TTagmaHeader::kLegacyVersion);

   bytes.resize(offset);
   bytes.insert(bytes.end(), legacy.begin(), legacy.end());
   WriteFile(bytes);

   ROOT::TTagmaStore::Layout layout;
   ROOT::TTagmaSchema read;
   std::string why;
   ASSERT_TRUE(ROOT::TTagmaWriter::ReadStore(kStorePath, &layout, &read, &why)) << why;
   EXPECT_EQ(layout.fEventMax, static_cast<std::uint64_t>(kEntries));
   EXPECT_EQ(read.Text(), MakeSchema().Text());

   std::remove(kStorePath);
}

// The descriptor may name a layout this build cannot address, so the block parses
// and the reader is where the store is refused, as with a compressed store.
TEST(TTagmaWriter, DescriptorRefusesALayoutItCannotAddress)
{
   WriteStore();
   std::vector<char> bytes = ReadFile();
   ASSERT_GT(bytes.size(), ROOT::TTagmaHeader::kSize);

   const std::size_t descriptor = bytes.size() - ROOT::TTagmaHeader::kSize;
   PutLE64(reinterpret_cast<unsigned char *>(bytes.data()) + descriptor + 72,
           ROOT::TTagmaHeader::kColumnarBlockLayout);
   PutLE64(reinterpret_cast<unsigned char *>(bytes.data()) + descriptor + 80, 64);
   WriteFile(bytes);

   ROOT::TTagmaStore::Layout layout;
   ROOT::TTagmaSchema read;
   std::string why;
   EXPECT_FALSE(ROOT::TTagmaWriter::ReadStore(kStorePath, &layout, &read, &why));
   EXPECT_NE(why.find("cannot address"), std::string::npos) << why;
   std::remove(kStorePath);
}

TEST(TTagmaWriter, DescriptorRefusesALayoutKindItDoesNotKnow)
{
   WriteStore();
   std::vector<char> bytes = ReadFile();
   ASSERT_GT(bytes.size(), ROOT::TTagmaHeader::kSize);

   const std::size_t descriptor = bytes.size() - ROOT::TTagmaHeader::kSize;
   PutLE64(reinterpret_cast<unsigned char *>(bytes.data()) + descriptor + 72, 7);
   PutLE64(reinterpret_cast<unsigned char *>(bytes.data()) + descriptor + 80, 64);
   WriteFile(bytes);

   ROOT::TTagmaStore::Layout layout;
   ROOT::TTagmaSchema read;
   std::string why;
   EXPECT_FALSE(ROOT::TTagmaWriter::ReadStore(kStorePath, &layout, &read, &why));
   EXPECT_NE(why.find("does not know"), std::string::npos) << why;
   std::remove(kStorePath);
}

TEST(TTagmaWriter, DescriptorRefusesABlockLengthInTheEventMajorLayout)
{
   WriteStore();
   std::vector<char> bytes = ReadFile();
   ASSERT_GT(bytes.size(), ROOT::TTagmaHeader::kSize);

   // An event-major store has no block division, so a block length contradicts
   // the kind the block declares.
   const std::size_t blockOffset = bytes.size() - ROOT::TTagmaHeader::kSize + 80;
   PutLE64(reinterpret_cast<unsigned char *>(bytes.data()) + blockOffset, 1024);
   WriteFile(bytes);

   ROOT::TTagmaStore::Layout layout;
   ROOT::TTagmaSchema read;
   std::string why;
   EXPECT_FALSE(ROOT::TTagmaWriter::ReadStore(kStorePath, &layout, &read, &why));
   EXPECT_NE(why.find("block length"), std::string::npos) << why;
   std::remove(kStorePath);
}

// A columnar store reads back through its block table. The expectation is formed
// from the schema's own offsets over the source slices, and the actual bytes are
// located through the table, so a transposition that moved a field or lost an
// object prefix fails here rather than in the read path.
TEST(TTagmaWriter, ColumnarBlocksReadBackThroughTheTable)
{
   const ROOT::TTagmaSchema schema = MakeSchema();
   // Four entries and three per block: one full block and one partial.
   const std::uint64_t blockEvents = 3;
   ROOT::TTagmaWriter writer(schema, kEntries, blockEvents);
   ASSERT_TRUE(writer.Open(kStorePath));
   for (int entry = 0; entry < kEntries; ++entry) {
      char scalars[8];
      ScalarRegion(entry, scalars);
      const std::vector<char> slice = Slice(entry);
      ASSERT_TRUE(writer.AddEvent(scalars, slice.data()));
   }
   ASSERT_TRUE(writer.Close());

   const std::vector<char> bytes = ReadFile();
   ASSERT_GT(bytes.size(), ROOT::TTagmaHeader::kSize);
   ROOT::TTagmaHeader header;
   std::string why;
   ASSERT_TRUE(ROOT::TTagmaHeader::Parse(
      reinterpret_cast<const unsigned char *>(bytes.data()) + bytes.size() - ROOT::TTagmaHeader::kSize,
      ROOT::TTagmaHeader::kSize, &header, &why)) << why;
   EXPECT_EQ(header.fLayoutKind, ROOT::TTagmaHeader::kColumnarBlockLayout);
   EXPECT_EQ(header.fBlockEvents, blockEvents);

   const std::uint64_t records = kEntries;
   const std::uint64_t indexBytes = records * schema.IndexRecordSize();
   const std::uint64_t blocks = (records + blockEvents - 1) / blockEvents;
   const std::uint64_t fields = schema.DataFieldCount();
   const std::uint64_t collectionCount = schema.GetCollections().size();
   const std::uint64_t tableBytes = ROOT::TTagmaStore::BlockTableBytes(
      blocks, fields, collectionCount, blockEvents);
   EXPECT_EQ(bytes.size(), indexBytes + header.fDataSize + tableBytes +
                              header.fFieldTableBytes + ROOT::TTagmaHeader::kSize);

   const std::size_t tableAt = static_cast<std::size_t>(indexBytes + header.fDataSize);
   const ROOT::TTagmaStore::BlockTable table{fields, collectionCount, blockEvents};
   EXPECT_EQ(table.EntryOffset(blocks), tableBytes);
   const unsigned char *tableBytesAt = reinterpret_cast<const unsigned char *>(bytes.data()) + tableAt;

   for (std::uint64_t entry = 0; entry < records; ++entry) {
      const std::uint64_t block = entry / blockEvents;
      const std::uint64_t within = entry % blockEvents;
      const std::size_t recordAt = static_cast<std::size_t>(entry * schema.IndexRecordSize());
      std::vector<std::uint64_t> counts(static_cast<std::size_t>(collectionCount), 0);
      for (std::size_t c = 0; c < collectionCount; ++c)
         counts[c] = schema.CountOf(c, bytes.data() + recordAt);
      std::uint64_t base = 0;
      std::memcpy(&base, bytes.data() + recordAt + schema.DataBaseOffset(), sizeof(base));

      const std::vector<char> slice = Slice(static_cast<int>(entry));
      std::uint64_t fieldIndex = 0;
      for (std::size_t c = 0; c < collectionCount; ++c) {
         const ROOT::TTagmaSchema::Collection &collection = schema.GetCollections()[c];
         for (const ROOT::TTagmaSchema::Field &field : collection.fFields) {
            const std::uint64_t runStart = table.RunStart(tableBytesAt, block, fieldIndex);
            const std::uint32_t prefix = table.ObjectPrefix(tableBytesAt, block, c, within);
            const std::uint64_t length = counts[c] * field.Size();
            const std::uint64_t actual = base + runStart + prefix * field.Size();
            const std::uint64_t expectedAt = schema.ChunkOffset(c, counts.data()) +
                                             schema.FieldOffset(collection, field.fName, counts[c]);
            ASSERT_LE(actual + length, bytes.size());
            ASSERT_LE(expectedAt + length, slice.size());
            EXPECT_EQ(std::memcmp(bytes.data() + actual, slice.data() + expectedAt,
                                  static_cast<std::size_t>(length)),
                      0)
               << "event " << entry << " field " << field.fName;
            ++fieldIndex;
         }
      }
   }
   std::remove(kStorePath);
}

// The column addressing reconstructs each event's slice from the block its values
// are transposed into, so the field copy downstream sees the event-major slice it
// would have seen from an event-major store.
TEST(TTagmaWriter, ReconstructsEveryColumnarSliceFromItsBlock)
{
   const ColumnarFixture fixture = MakeColumnarFixture();
   ASSERT_FALSE(fixture.fBytes.empty());
   ASSERT_GT(fixture.fBytes.size(), fixture.fTableAt);
   const std::uint64_t records = kEntries;
   const unsigned char *tableBytesAt =
      reinterpret_cast<const unsigned char *>(fixture.fBytes.data()) + fixture.fTableAt;

   for (std::uint64_t entry = 0; entry < records; ++entry) {
      const std::uint64_t block = entry / fixture.fBlockEvents;
      const std::uint64_t within = entry % fixture.fBlockEvents;
      const std::vector<std::uint64_t> counts = EventCounts(fixture.fSchema, fixture.fBytes, entry);
      const std::uint64_t spanBytes =
         BlockSpan(fixture.fSchema, fixture.fBytes, block, fixture.fBlockEvents, kEntries);
      const std::uint64_t base = EventBase(fixture.fSchema, fixture.fBytes, entry);
      const std::vector<char> expected = Slice(static_cast<int>(entry));

      // A slice left at a pattern fails on any field the reconstruction misses.
      std::vector<char> reconstructed(static_cast<std::size_t>(fixture.fSchema.SliceBytes(counts.data())),
                                      static_cast<char>(0xff));
      ASSERT_EQ(reconstructed.size(), expected.size()) << "event " << entry;
      const unsigned char *entryAt = tableBytesAt + fixture.fTable.EntryOffset(block);
      ASSERT_TRUE(ROOT::ReconstructTagmaSlice(fixture.fSchema, fixture.fTable, entryAt, within, counts.data(),
                                              reinterpret_cast<const unsigned char *>(fixture.fBytes.data()) +
                                                 base,
                                              spanBytes, reconstructed.data()))
         << "event " << entry;
      EXPECT_EQ(reconstructed, expected) << "event " << entry;
   }
   std::remove(kStorePath);
}

// A table entry that points a run outside its block is refused rather than read,
// since the bytes beyond the span belong to another block or to the table.
TEST(TTagmaWriter, ReconstructRefusesARunOutsideTheBlockSpan)
{
   const ColumnarFixture fixture = MakeColumnarFixture();
   ASSERT_FALSE(fixture.fBytes.empty());

   const std::uint64_t blocks = (kEntries + fixture.fBlockEvents - 1) / fixture.fBlockEvents;
   std::vector<unsigned char> table(static_cast<std::size_t>(fixture.fTable.EntryOffset(blocks)));
   std::memcpy(table.data(), fixture.fBytes.data() + fixture.fTableAt, table.size());
   const unsigned char *original =
      reinterpret_cast<const unsigned char *>(fixture.fBytes.data()) + fixture.fTableAt;

   const std::uint64_t spanBytes = BlockSpan(fixture.fSchema, fixture.fBytes, 0, fixture.fBlockEvents, kEntries);
   const std::uint64_t base = EventBase(fixture.fSchema, fixture.fBytes, 0);
   const std::vector<std::uint64_t> counts = EventCounts(fixture.fSchema, fixture.fBytes, 0);
   ASSERT_GT(counts[0], 0u);
   std::vector<char> slice(static_cast<std::size_t>(fixture.fSchema.SliceBytes(counts.data())), 0);
   const unsigned char *span = reinterpret_cast<const unsigned char *>(fixture.fBytes.data()) + base;

   // The first field's run of block 0, moved to the end of the span.
   PutLE64(table.data(), spanBytes);
   EXPECT_FALSE(ROOT::ReconstructTagmaSlice(fixture.fSchema, fixture.fTable, table.data(), 0, counts.data(), span,
                                            spanBytes, slice.data()));

   // The same entry at the run start the writer wrote is accepted, so the refusal
   // above is the bound and not a fault in the fixture.
   PutLE64(table.data(), fixture.fTable.RunStartInEntry(original, 0));
   EXPECT_TRUE(ROOT::ReconstructTagmaSlice(fixture.fSchema, fixture.fTable, table.data(), 0, counts.data(), span,
                                           spanBytes, slice.data()));
   std::remove(kStorePath);
}
