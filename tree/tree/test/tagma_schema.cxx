// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

// Verifies the schema-driven branch materialization: with a schema
// attached, GetEntry fills the record buffer in place and the materialized
// leaves read the store-backed field values, so ordinary leaf access works
// on store-backed events.

#include "ROOT/TTagmaSchema.hxx"
#include "ROOT/TTagmaStore.hxx"
#include "ROOT/TestSupport.hxx"

#include "TFile.h"
#include "TLeaf.h"
#include "TTree.h"
#include "TTreeReader.h"
#include "TTreeReaderValue.h"

#include "gtest/gtest.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

namespace {

const char *kStorePath = "tagma_schema_store.bin";
const char *kLayoutPath = "tagma_schema_store.bin.layout";
constexpr int kEntries = 64;
constexpr std::uint64_t kRecordSize = 16;

double FieldX(int entry) { return 1000.0 + 0.5 * entry; }
int FieldN(int entry) { return 3 * entry; }
bool FieldFlag(int entry) { return (entry % 2) != 0; }

// Writes the record array: x at offset 0, n at 8, flag at 12, 16 bytes per
// record.
void WriteStore()
{
   FILE *out = std::fopen(kStorePath, "wb");
   ASSERT_NE(out, nullptr);
   for (int entry = 0; entry < kEntries; ++entry) {
      char record[kRecordSize] = {};
      const double x = FieldX(entry);
      const int n = FieldN(entry);
      const bool flag = FieldFlag(entry);
      std::memcpy(record + 0, &x, sizeof(x));
      std::memcpy(record + 8, &n, sizeof(n));
      std::memcpy(record + 12, &flag, sizeof(flag));
      ASSERT_EQ(std::fwrite(record, 1, kRecordSize, out), kRecordSize);
   }
   std::fclose(out);
}

void WriteLayoutSidecar()
{
   std::ofstream out(kLayoutPath);
   out << "# name offset type\n";
   out << "x 0 double\n";
   out << "n 8 int\n";
   out << "\n";
   out << "flag 12 bool\n";
}

ROOT::TTagmaSchema MakeSchema()
{
   ROOT::TTagmaSchema schema;
   ROOT::TTagmaSchema::Field x;
   x.fName = "x";
   x.fOffset = 0;
   x.fType = ROOT::TTagmaSchema::EType::kDouble;
   schema.AddField(x);
   ROOT::TTagmaSchema::Field n;
   n.fName = "n";
   n.fOffset = 8;
   n.fType = ROOT::TTagmaSchema::EType::kInt32;
   schema.AddField(n);
   ROOT::TTagmaSchema::Field flag;
   flag.fName = "flag";
   flag.fOffset = 12;
   flag.fType = ROOT::TTagmaSchema::EType::kBool;
   schema.AddField(flag);
   return schema;
}

std::shared_ptr<ROOT::TTagmaStore> MakeStore()
{
   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1;
   layout.fLumiMax = 1;
   layout.fEventMax = kEntries;
   layout.fRecordSize = kRecordSize;
   return std::make_shared<ROOT::TTagmaStore>(layout);
}

}  // namespace

TEST(TTagmaSchema, LeavesReadTheStoreRecord)
{
   WriteStore();
   WriteLayoutSidecar();

   TFile *file = TFile::Open((std::string(kStorePath) + "?filetype=raw").c_str());
   ASSERT_NE(file, nullptr);
   ASSERT_FALSE(file->IsZombie());

   TTree tree("Events", "Events");
   tree.SetDirectory(file);
   tree.SetEntries(kEntries);
   tree.SetTagmaStore(MakeStore());

   ROOT::TTagmaSchema schema;
   ASSERT_TRUE(schema.Read(kLayoutPath));
   ASSERT_EQ(schema.GetFields().size(), 3u);
   ASSERT_EQ(schema.Extent(), std::uint64_t(13));
   ASSERT_TRUE(tree.SetTagmaSchema(schema));

   TLeaf *xLeaf = tree.GetLeaf("x");
   TLeaf *nLeaf = tree.GetLeaf("n");
   TLeaf *flagLeaf = tree.GetLeaf("flag");
   ASSERT_NE(xLeaf, nullptr);
   ASSERT_NE(nLeaf, nullptr);
   ASSERT_NE(flagLeaf, nullptr);

   const Int_t calls0 = file->GetReadCalls();
   for (int entry : {0, 1, 7, 63}) {
      ASSERT_GT(tree.GetEntry(entry), 0) << "entry " << entry;
      EXPECT_DOUBLE_EQ(xLeaf->GetValue(0), FieldX(entry));
      EXPECT_DOUBLE_EQ(nLeaf->GetValue(0), FieldN(entry));
      EXPECT_DOUBLE_EQ(flagLeaf->GetValue(0), FieldFlag(entry) ? 1.0 : 0.0);
   }
   // One read per entry, none from the branch machinery.
   EXPECT_EQ(file->GetReadCalls(), calls0 + 4);

   // The record buffer accessors keep the same view as the leaves.
   EXPECT_EQ(tree.GetTagmaRecordSize(), static_cast<Int_t>(kRecordSize));
   char expected[kRecordSize];
   const double x0 = FieldX(63);
   const int n0 = FieldN(63);
   const bool flag0 = FieldFlag(63);
   std::memcpy(expected + 0, &x0, sizeof(x0));
   std::memcpy(expected + 8, &n0, sizeof(n0));
   std::memcpy(expected + 12, &flag0, sizeof(flag0));
   EXPECT_EQ(std::memcmp(tree.GetTagmaRecordBuffer(), expected, kRecordSize), 0);

   delete file;
}

TEST(TTagmaSchema, SetTagmaSchemaRejectsInvalidSchemas)
{
   WriteStore();

   TFile *file = TFile::Open((std::string(kStorePath) + "?filetype=raw").c_str());
   ASSERT_NE(file, nullptr);
   ASSERT_FALSE(file->IsZombie());

   TTree tree("Events", "Events");
   tree.SetDirectory(file);
   tree.SetEntries(kEntries);

   // Without a store the schema is rejected.
   ROOT::TTagmaSchema schema = MakeSchema();
   ROOT_EXPECT_ERROR_PARTIAL(EXPECT_FALSE(tree.SetTagmaSchema(schema)),
                             "TTree::SetTagmaSchema",
                             "no coordinate store is attached to Events");

   tree.SetTagmaStore(MakeStore());

   // A field extending past the record size is rejected.
   ROOT::TTagmaSchema oversized;
   ROOT::TTagmaSchema::Field past;
   past.fName = "past";
   past.fOffset = kRecordSize - 4;
   past.fType = ROOT::TTagmaSchema::EType::kDouble;
   oversized.AddField(past);
   std::string why;
   EXPECT_FALSE(oversized.Validate(kRecordSize, &why));
   EXPECT_FALSE(why.empty());
   ROOT_EXPECT_ERROR_PARTIAL(EXPECT_FALSE(tree.SetTagmaSchema(oversized)),
                             "TTree::SetTagmaSchema",
                             "field past extends past the record size");

   // Overlapping fields are rejected.
   ROOT::TTagmaSchema overlapping;
   ROOT::TTagmaSchema::Field first;
   first.fName = "first";
   first.fOffset = 0;
   first.fType = ROOT::TTagmaSchema::EType::kDouble;
   overlapping.AddField(first);
   ROOT::TTagmaSchema::Field second;
   second.fName = "second";
   second.fOffset = 4;
   second.fType = ROOT::TTagmaSchema::EType::kDouble;
   overlapping.AddField(second);
   ROOT_EXPECT_ERROR_PARTIAL(EXPECT_FALSE(tree.SetTagmaSchema(overlapping)),
                             "TTree::SetTagmaSchema",
                             "field second overlaps the preceding field");

   // Repeated names are rejected.
   ROOT::TTagmaSchema repeated;
   repeated.AddField(first);
   ROOT::TTagmaSchema::Field same;
   same.fName = "first";
   same.fOffset = 8;
   same.fType = ROOT::TTagmaSchema::EType::kInt32;
   repeated.AddField(same);
   ROOT_EXPECT_ERROR_PARTIAL(EXPECT_FALSE(tree.SetTagmaSchema(repeated)),
                             "TTree::SetTagmaSchema",
                             "field name first repeats");

   // A valid schema still attaches after the rejections.
   EXPECT_TRUE(tree.SetTagmaSchema(schema));

   delete file;
}

TEST(TTagmaSchema, ReaderValuesAdvanceFromTheStore)
{
   WriteStore();

   TFile *file = TFile::Open((std::string(kStorePath) + "?filetype=raw").c_str());
   ASSERT_NE(file, nullptr);
   ASSERT_FALSE(file->IsZombie());

   TTree tree("Events", "Events");
   tree.SetDirectory(file);
   tree.SetEntries(kEntries);
   tree.SetTagmaStore(MakeStore());
   ROOT::TTagmaSchema schema = MakeSchema();
   ASSERT_TRUE(tree.SetTagmaSchema(schema));

   // TTreeReaderValue reads through the branch proxy, which enters the
   // branch read path and never reaches TTree::GetEntry. Before the
   // branches served the record, the loop iterated the right number of
   // entries with every value frozen at the last leaf state.
   TTreeReader reader(&tree);
   TTreeReaderValue<Double_t> x(reader, "x");
   TTreeReaderValue<Int_t> n(reader, "n");

   int entry = 0;
   while (reader.Next()) {
      ASSERT_LT(entry, kEntries);
      EXPECT_DOUBLE_EQ(*x, FieldX(entry)) << "entry " << entry;
      EXPECT_EQ(*n, FieldN(entry)) << "entry " << entry;
      ++entry;
   }
   EXPECT_EQ(entry, kEntries);

   delete file;
}

TEST(TTagmaSchema, SchemaTextFormIsParsed)
{
   // The text form is the sidecar layout: one field per line. Comments
   // and blank lines are skipped by Read, and AddLine rejects them along
   // with unknown types and trailing tokens.
   ROOT::TTagmaSchema schema;
   EXPECT_TRUE(schema.AddLine("x 0 double"));
   EXPECT_FALSE(schema.AddLine("# a comment line"));
   EXPECT_FALSE(schema.AddLine("y 8 notatype"));
   EXPECT_FALSE(schema.AddLine("z 4 int trailing"));
   EXPECT_FALSE(schema.AddLine("w"));
   EXPECT_EQ(schema.GetFields().size(), 1u);

   ROOT::TTagmaSchema::EType type = ROOT::TTagmaSchema::EType::kBool;
   EXPECT_TRUE(ROOT::TTagmaSchema::ParseType("ULong64_t", &type));
   EXPECT_EQ(type, ROOT::TTagmaSchema::EType::kUInt64);
   EXPECT_EQ(std::string(ROOT::TTagmaSchema::LeafCode(type)), "l");
   EXPECT_EQ(ROOT::TTagmaSchema::SizeOf(type), std::uint64_t(8));
   EXPECT_FALSE(ROOT::TTagmaSchema::ParseType("vector<float>", &type));
}
