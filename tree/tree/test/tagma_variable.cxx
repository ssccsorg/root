// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

// The variable-length gate of the reader backend (ssccsorg/ssccs#121): a
// store that carries collections reads through the ordinary leaf machinery.
//
// The store holds a fixed-width index record per event and a packed data
// region. The index record carries the scalar fields, the object count of
// each collection, and the offset of the event's slice in the data region.
// The slice packs each collection field by field, so the elements of one
// field are contiguous and the offset of a field follows from the count.
//
// A collection field becomes a branch whose count field names the bounding
// leaf, so the leaf machinery reports the array length of the entry, and a
// read copies the entry's elements into the branch's buffer.

#include "ROOT/TTagmaSchema.hxx"
#include "ROOT/TTagmaStore.hxx"
#include "ROOT/TestSupport.hxx"

#include "TFile.h"
#include "TLeaf.h"
#include "TTree.h"

#include "gtest/gtest.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

const char *kStorePath = "tagma_variable_store.bin";
constexpr int kEntries = 4;
constexpr std::uint64_t kIndexRecordSize = 16;
constexpr std::uint64_t kDataBaseOffset = 8;
constexpr std::uint32_t kMaxMuon = 4;
constexpr std::uint32_t kMaxJet = 3;

// The object counts per event.
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

// Bytes of one event's slice: the two muon fields then the jet field, each
// field contiguous.
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

   ROOT::TTagmaSchema::Field MuonPtField;
   MuonPtField.fName = "Muon_pt";
   MuonPtField.fCountField = "nMuon";
   MuonPtField.fMaxCount = kMaxMuon;
   MuonPtField.fOffset = 0;
   MuonPtField.fType = ROOT::TTagmaSchema::EType::kFloat;
   schema.AddField(MuonPtField);

   ROOT::TTagmaSchema::Field MuonEtaField;
   MuonEtaField.fName = "Muon_eta";
   MuonEtaField.fCountField = "nMuon";
   MuonEtaField.fMaxCount = kMaxMuon;
   MuonEtaField.fOffset = 4;
   MuonEtaField.fType = ROOT::TTagmaSchema::EType::kFloat;
   schema.AddField(MuonEtaField);

   ROOT::TTagmaSchema::Field nJet;
   nJet.fName = "nJet";
   nJet.fOffset = 4;
   nJet.fType = ROOT::TTagmaSchema::EType::kUInt32;
   schema.AddField(nJet);

   ROOT::TTagmaSchema::Field JetPtField;
   JetPtField.fName = "Jet_pt";
   JetPtField.fCountField = "nJet";
   JetPtField.fMaxCount = kMaxJet;
   JetPtField.fOffset = 0;
   JetPtField.fType = ROOT::TTagmaSchema::EType::kFloat;
   schema.AddField(JetPtField);
   return schema;
}

// Writes the store: the index region, then the data region.
void WriteStore()
{
   const std::uint64_t indexBytes = kEntries * kIndexRecordSize;
   std::vector<char> data;
   std::vector<std::uint64_t> bases(kEntries, 0);
   for (int entry = 0; entry < kEntries; ++entry) {
      bases[entry] = indexBytes + data.size();
      for (std::uint32_t i = 0; i < kMuonCount[entry]; ++i) {
         const float value = MuonPt(entry, i);
         const char *bytes = reinterpret_cast<const char *>(&value);
         data.insert(data.end(), bytes, bytes + sizeof(value));
      }
      for (std::uint32_t i = 0; i < kMuonCount[entry]; ++i) {
         const float value = MuonEta(entry, i);
         const char *bytes = reinterpret_cast<const char *>(&value);
         data.insert(data.end(), bytes, bytes + sizeof(value));
      }
      for (std::uint32_t i = 0; i < kJetCount[entry]; ++i) {
         const float value = JetPt(entry, i);
         const char *bytes = reinterpret_cast<const char *>(&value);
         data.insert(data.end(), bytes, bytes + sizeof(value));
      }
   }

   FILE *out = std::fopen(kStorePath, "wb");
   ASSERT_NE(out, nullptr);
   for (int entry = 0; entry < kEntries; ++entry) {
      char index[kIndexRecordSize] = {};
      std::memcpy(index + 0, &kMuonCount[entry], sizeof(kMuonCount[entry]));
      std::memcpy(index + 4, &kJetCount[entry], sizeof(kJetCount[entry]));
      std::memcpy(index + kDataBaseOffset, &bases[entry], sizeof(bases[entry]));
      ASSERT_EQ(std::fwrite(index, 1, kIndexRecordSize, out), kIndexRecordSize);
   }
   ASSERT_EQ(std::fwrite(data.data(), 1, data.size(), out), data.size());
   std::fclose(out);
}

TFile *OpenStore()
{
   return TFile::Open((std::string(kStorePath) + "?filetype=raw").c_str());
}

void Attach(TTree &tree, ROOT::TTagmaSchema &schema)
{
   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1;
   layout.fLumiMax = 1;
   layout.fEventMax = kEntries;
   layout.fRecordSize = kIndexRecordSize;
   layout.fDataSize = 0;
   for (int entry = 0; entry < kEntries; ++entry)
      layout.fDataSize += SliceBytes(entry);
   tree.SetTagmaStore(std::make_shared<ROOT::TTagmaStore>(layout));
   ASSERT_TRUE(tree.SetTagmaSchema(schema));
}

} // namespace

TEST(TTagmaVariable, CollectionsReadThroughTheLeaves)
{
   WriteStore();
   ROOT::TTagmaSchema schema = MakeSchema();
   std::string why;
   ASSERT_TRUE(schema.Validate(kIndexRecordSize, &why)) << why;

   TFile *file = OpenStore();
   ASSERT_NE(file, nullptr);
   ASSERT_FALSE(file->IsZombie());

   TTree tree("Events", "Events");
   tree.SetDirectory(file);
   tree.SetEntries(kEntries);
   Attach(tree, schema);

   TLeaf *nMuon = tree.GetLeaf("nMuon");
   TLeaf *muonPtLeaf = tree.GetLeaf("Muon_pt");
   TLeaf *muonEtaLeaf = tree.GetLeaf("Muon_eta");
   TLeaf *jetPtLeaf = tree.GetLeaf("Jet_pt");
   ASSERT_NE(nMuon, nullptr);
   ASSERT_NE(muonPtLeaf, nullptr);
   ASSERT_NE(muonEtaLeaf, nullptr);
   ASSERT_NE(jetPtLeaf, nullptr);
   EXPECT_EQ(muonPtLeaf->GetLeafCount(), nMuon);

   // Two reads per event: the index record, then the event's data slice.
   const Int_t calls0 = file->GetReadCalls();
   for (int entry = 0; entry < kEntries; ++entry) {
      ASSERT_GT(tree.GetEntry(entry), 0) << "entry " << entry;
      EXPECT_EQ(nMuon->GetValue(0), kMuonCount[entry]) << "entry " << entry;
      EXPECT_EQ(tree.GetLeaf("nJet")->GetValue(0), kJetCount[entry]);
      for (std::uint32_t i = 0; i < kMuonCount[entry]; ++i) {
         EXPECT_FLOAT_EQ(muonPtLeaf->GetValue(i), MuonPt(entry, i)) << "entry " << entry;
         EXPECT_FLOAT_EQ(muonEtaLeaf->GetValue(i), MuonEta(entry, i));
      }
      for (std::uint32_t i = 0; i < kJetCount[entry]; ++i)
         EXPECT_FLOAT_EQ(jetPtLeaf->GetValue(i), JetPt(entry, i)) << "entry " << entry;
   }
   EXPECT_EQ(file->GetReadCalls(), calls0 + 2 * kEntries);

   // A repeated read of the same entry reuses the loaded record and slice.
   ASSERT_GT(tree.GetEntry(kEntries - 1), 0);
   EXPECT_EQ(file->GetReadCalls(), calls0 + 2 * kEntries);

   delete file;
}

TEST(TTagmaVariable, SchemaAndStoreBoundsAreEnforced)
{
   WriteStore();
   ROOT::TTagmaSchema schema = MakeSchema();

   TFile *file = OpenStore();
   ASSERT_NE(file, nullptr);
   ASSERT_FALSE(file->IsZombie());

   TTree tree("Events", "Events");
   tree.SetDirectory(file);
   tree.SetEntries(kEntries);

   // The index record size the schema needs.
   EXPECT_EQ(schema.IndexRecordSize(), kIndexRecordSize);
   EXPECT_EQ(schema.DataBaseOffset(), kDataBaseOffset);
   ASSERT_EQ(schema.GetCollections().size(), 2u);
   ASSERT_EQ(schema.GetCollections()[0].fFields.size(), 2u);
   EXPECT_EQ(schema.GetCollections()[0].fCountField, "nMuon");
   EXPECT_EQ(schema.GetCollections()[1].fCountField, "nJet");

   // The chunk of the second collection follows the first, field by field.
   const std::uint64_t counts[2] = {2, 1};
   EXPECT_EQ(schema.ChunkOffset(0, counts), 0u);
   EXPECT_EQ(schema.ChunkOffset(1, counts), 16u);
   EXPECT_EQ(schema.FieldOffset(schema.GetCollections()[0], "Muon_eta", 2), 8u);
   EXPECT_EQ(schema.SliceBytes(counts), 20u);

   Attach(tree, schema);

   // A count past the schema bound fails the read rather than copying past
   // the branch buffer. Event 2 carries four muons against a bound of four;
   // the first event that exceeds it is written here.
   const std::uint32_t tooMany = kMaxMuon + 1;
   {
      FILE *out = std::fopen(kStorePath, "r+b");
      ASSERT_NE(out, nullptr);
      ASSERT_EQ(std::fseek(out, 0, SEEK_SET), 0);
      ASSERT_EQ(std::fwrite(&tooMany, 1, sizeof(tooMany), out), sizeof(tooMany));
      std::fclose(out);
   }
   ROOT_EXPECT_ERROR_PARTIAL(EXPECT_LE(tree.GetEntry(0), 0), "TTree::LoadTagmaRecord",
                             "collection nMuon reports 5 objects at entry 0, the schema carries 4");

   // The failing count is reported once, not on every read of the entry.
   ROOT_EXPECT_NODIAG(EXPECT_LE(tree.GetEntry(0), 0));

   delete file;
}
