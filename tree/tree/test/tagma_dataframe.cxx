// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

// The interoperability gate of the reader backend (ssccsorg/ssccs#121):
// RDataFrame reads a store-backed tree through the materialized branches,
// and the same expression over the original file produces the same
// histogram. RDataFrame reaches the tree through TTreeReader, whose value
// proxy drives the branch read path and never calls TTree::GetEntry, so
// this is the path that an entry-level short circuit cannot serve.

#include "ROOT/TTagmaSchema.hxx"
#include "ROOT/TTagmaStore.hxx"

#include <ROOT/RDataFrame.hxx>

#include "TFile.h"
#include "TH1D.h"
#include "TTree.h"

#include "gtest/gtest.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

const char *kFileTreePath = "tagma_dataframe_file.root";
const char *kStorePath = "tagma_dataframe_store.bin";
constexpr int kEntries = 64;
constexpr std::uint64_t kRecordSize = 16;
// The selection of the comparison: n >= 6 keeps the entries from e = 2 on.
constexpr unsigned long long kSelected = 62;

double FieldX(int entry)
{
   return 1000.0 + 0.5 * entry;
}
int FieldN(int entry)
{
   return 3 * entry;
}

// The baseline: the same events in an ordinary ROOT file.
void WriteFileTree()
{
   TFile file(kFileTreePath, "RECREATE");
   TTree tree("Events", "Events");
   Double_t x = 0;
   Int_t n = 0;
   tree.Branch("x", &x, "x/D");
   tree.Branch("n", &n, "n/I");
   for (int entry = 0; entry < kEntries; ++entry) {
      x = FieldX(entry);
      n = FieldN(entry);
      tree.Fill();
   }
   tree.Write();
   file.Close();
}

// The store: the same values as fixed-width records, the double at offset
// 0 and the int at offset 8.
void WriteStore()
{
   FILE *out = std::fopen(kStorePath, "wb");
   ASSERT_NE(out, nullptr);
   for (int entry = 0; entry < kEntries; ++entry) {
      char record[kRecordSize] = {};
      const double x = FieldX(entry);
      const int n = FieldN(entry);
      std::memcpy(record + 0, &x, sizeof(x));
      std::memcpy(record + 8, &n, sizeof(n));
      ASSERT_EQ(std::fwrite(record, 1, kRecordSize, out), kRecordSize);
   }
   std::fclose(out);
}

} // namespace

TEST(TTagmaDataFrame, StoreTreeAndFileTreeAgreeUnderRDataFrame)
{
   WriteFileTree();
   WriteStore();

   // The baseline expression over the ordinary file.
   TFile baselineFile(kFileTreePath);
   ASSERT_FALSE(baselineFile.IsZombie());
   TTree *baselineTree = nullptr;
   baselineFile.GetObject("Events", baselineTree);
   ASSERT_NE(baselineTree, nullptr);

   ROOT::RDataFrame baselineFrame(*baselineTree);
   auto baselineHist = baselineFrame.Filter("n >= 6").Histo1D("x");

   // The same expression over the store, which reaches the record through
   // the branches the schema materialized.
   TFile *storeFile = TFile::Open((std::string(kStorePath) + "?filetype=raw").c_str());
   ASSERT_NE(storeFile, nullptr);
   ASSERT_FALSE(storeFile->IsZombie());

   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1;
   layout.fLumiMax = 1;
   layout.fEventMax = kEntries;
   layout.fRecordSize = kRecordSize;

   TTree storeTree("Events", "Events");
   storeTree.SetDirectory(storeFile);
   storeTree.SetEntries(kEntries);
   storeTree.SetTagmaStore(std::make_shared<ROOT::TTagmaStore>(layout));

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
   ASSERT_TRUE(storeTree.SetTagmaSchema(schema));

   ROOT::RDataFrame storeFrame(storeTree);
   auto storeCount = storeFrame.Filter("n >= 6").Count();
   EXPECT_EQ(storeCount.GetValue(), kSelected);
   auto storeHist = storeFrame.Filter("n >= 6").Histo1D("x");

   EXPECT_EQ(storeHist->GetEntries(), baselineHist->GetEntries());
   EXPECT_DOUBLE_EQ(storeHist->GetMean(), baselineHist->GetMean());
   ASSERT_EQ(storeHist->GetNbinsX(), baselineHist->GetNbinsX());
   for (int bin = 1; bin <= baselineHist->GetNbinsX(); ++bin)
      EXPECT_DOUBLE_EQ(storeHist->GetBinContent(bin), baselineHist->GetBinContent(bin)) << "bin " << bin;

   delete storeFile;
}
