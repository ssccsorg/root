// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

// Verifies the tree-level coordinate read path: GetTagmaRecord resolves
// the entry through the coordinate-indexed store and serves the
// fixed-width record with exactly one TFile::ReadBuffer call, bypassing
// the branch, basket, and read-cache machinery. Entries outside the
// store layout, small caller buffers, and trees without a store are
// rejected without reading.

#include "ROOT/TTagmaStore.hxx"

#include "TFile.h"
#include "TTree.h"

#include "gtest/gtest.h"

#include <memory>

TEST(TTagmaRecord, GetTagmaRecordReadsOneRecordPerEntry)
{
   {
      TFile file("tagma_record_test.root", "RECREATE");
      TTree tree("t", "t");
      double x = 0;
      tree.Branch("x", &x);
      for (int i = 0; i < 20000; ++i) {
         x = i;
         tree.Fill();
      }
      tree.Write();
      file.Close();
   }

   TFile file("tagma_record_test.root");
   auto *tree = file.Get<TTree>("t");
   ASSERT_NE(tree, nullptr);

   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1;
   layout.fLumiMax = 1;
   layout.fEventMax = 100;
   layout.fRecordSize = 64;
   tree->SetTagmaStore(std::make_shared<ROOT::TTagmaStore>(layout));

   char buf[64];
   const Int_t calls0 = file.GetReadCalls();
   EXPECT_EQ(tree->GetTagmaRecord(0, buf, 64), 64);
   EXPECT_EQ(file.GetReadCalls(), calls0 + 1);  // exactly one read per record
   EXPECT_EQ(tree->GetTagmaRecord(99, buf, 64), 64);
   EXPECT_EQ(file.GetReadCalls(), calls0 + 2);

   // Out-of-layout entries are rejected without issuing a read.
   EXPECT_EQ(tree->GetTagmaRecord(100, buf, 64), -1);
   EXPECT_EQ(file.GetReadCalls(), calls0 + 2);

   // A caller buffer smaller than the record is rejected without reading.
   EXPECT_EQ(tree->GetTagmaRecord(0, buf, 32), -2);
   EXPECT_EQ(file.GetReadCalls(), calls0 + 2);

   // A negative caller buffer size is rejected without reading.
   EXPECT_EQ(tree->GetTagmaRecord(0, buf, -1), -2);
   EXPECT_EQ(file.GetReadCalls(), calls0 + 2);

   // A tree without a store falls through: a plain detached tree has
   // neither a store nor a directory, so the record read is rejected.
   TTree plain("p", "p");
   EXPECT_EQ(plain.GetTagmaRecord(0, buf, 64), -1);
}

TEST(TTagmaRecord, GetEntryServesCoveredEntriesFromTheStore)
{
   {
      TFile file("tagma_getentry_test.root", "RECREATE");
      TTree tree("t", "t");
      double x = 0;
      tree.Branch("x", &x);
      for (int i = 0; i < 20000; ++i) {
         x = i;
         tree.Fill();
      }
      tree.Write();
      file.Close();
   }

   TFile file("tagma_getentry_test.root");
   auto *tree = file.Get<TTree>("t");
   ASSERT_NE(tree, nullptr);

   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1;
   layout.fLumiMax = 1;
   layout.fEventMax = 100;
   layout.fRecordSize = 64;
   tree->SetTagmaStore(std::make_shared<ROOT::TTagmaStore>(layout));

   // Covered entries: GetEntry serves the record with exactly one read
   // and exposes it through the record buffer accessors.
   const Int_t calls0 = file.GetReadCalls();
   EXPECT_EQ(tree->GetEntry(0), 64);
   EXPECT_EQ(file.GetReadCalls(), calls0 + 1);
   EXPECT_NE(tree->GetTagmaRecordBuffer(), nullptr);
   EXPECT_EQ(tree->GetTagmaRecordSize(), 64);

   EXPECT_EQ(tree->GetEntry(99), 64);
   EXPECT_EQ(file.GetReadCalls(), calls0 + 2);

   // Entries outside the layout fall through to the ordinary branch
   // path: the real branch payload is read and the record buffer is
   // left untouched.
   const Int_t fallthrough = tree->GetEntry(100);
   EXPECT_GT(fallthrough, 0);
   EXPECT_NE(fallthrough, 64);
   EXPECT_GT(file.GetReadCalls(), calls0 + 2);
   EXPECT_EQ(tree->GetTagmaRecordSize(), 64);
}

TEST(TTagmaRecord, ZeroRecordSizeLayoutIsRejected)
{
   // A degenerate layout with a zero record size is rejected before any
   // read is attempted.
   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1;
   layout.fLumiMax = 1;
   layout.fEventMax = 1;
   layout.fRecordSize = 0;
   TTree tree("p", "p");
   tree.SetTagmaStore(std::make_shared<ROOT::TTagmaStore>(layout));
   char buf[64];
   EXPECT_EQ(tree.GetTagmaRecord(0, buf, 64), -1);
}

TEST(TTagmaRecord, OversizedRecordSizeLayoutIsRejected)
{
   // A layout whose record size exceeds the sanity bound is rejected
   // before any allocation or read: resize() would throw and the size
   // would not fit the Int_t length argument of ReadBuffer.
   {
      TFile file("tagma_oversized_test.root", "RECREATE");
      TTree tree("t", "t");
      double x = 0;
      tree.Branch("x", &x);
      tree.Fill();
      tree.Write();
      file.Close();
   }
   TFile file("tagma_oversized_test.root");
   auto *tree = file.Get<TTree>("t");
   ASSERT_NE(tree, nullptr);

   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1;
   layout.fLumiMax = 1;
   layout.fEventMax = 1;
   layout.fRecordSize = (1ull << 31);  // 2 GiB, beyond the 1 GiB bound
   tree->SetTagmaStore(std::make_shared<ROOT::TTagmaStore>(layout));

   char buf[64];
   EXPECT_EQ(tree->GetTagmaRecord(0, buf, 64), -1);
   EXPECT_EQ(tree->GetEntry(0), 0);
}
