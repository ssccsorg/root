// Author: SSCCS Foundation 2026

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

#include "ROOT/TTagmaSchema.hxx"
#include "ROOT/TTagmaStore.hxx"
#include "ROOT/TTagmaWriter.hxx"

#include "gtest/gtest.h"

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
   ASSERT_EQ(bytes.size(), indexBytes + dataSize);

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
   EXPECT_EQ(expectedBase, bytes.size());

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
