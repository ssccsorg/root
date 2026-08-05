// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

// Verifies the coordinate-indexed store extension: composition and
// decomposition round trip, byte offsets, record count, extent, and axis
// bounds. The store is self-contained C++17 and does not depend on ROOT.

#include "ROOT/TTagmaStore.hxx"

#include "gtest/gtest.h"

namespace {

ROOT::TTagmaStore MakeStore()
{
   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1000;
   layout.fLumiMax = 128;
   layout.fEventMax = 10000;
   layout.fRecordSize = 128;
   return ROOT::TTagmaStore(layout);
}

using Axes = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;

}  // namespace

TEST(TTagmaStore, ComposeDecomposeRoundTrip)
{
   const ROOT::TTagmaStore store = MakeStore();
   const std::uint64_t runs[] = {0, 1, 42, 999};
   const std::uint64_t lumis[] = {0, 7, 127};
   const std::uint64_t events[] = {0, 1, 4096, 9999};

   for (const std::uint64_t run : runs) {
      for (const std::uint64_t lumi : lumis) {
         for (const std::uint64_t event : events) {
            const std::uint64_t index = store.Compose(run, lumi, event);
            EXPECT_EQ(store.Decompose(index), Axes(run, lumi, event));
         }
      }
   }
}

TEST(TTagmaStore, OffsetIsClosedForm)
{
   const ROOT::TTagmaStore store = MakeStore();
   EXPECT_EQ(store.Offset(0, 0, 0), 0u);
   EXPECT_EQ(store.Offset(999, 127, 9999),
             store.Compose(999, 127, 9999) * 128u);
   EXPECT_EQ(store.Offset(42, 63, 2048),
             (std::uint64_t(42) * 128u + 63u) * 10000u * 128u +
                 std::uint64_t(2048) * 128u);
}

TEST(TTagmaStore, RecordCountAndExtent)
{
   const ROOT::TTagmaStore store = MakeStore();
   EXPECT_EQ(store.RecordCount(), 1000ull * 128ull * 10000ull);
   EXPECT_EQ(store.SizeBytes(), 1000ull * 128ull * 10000ull * 128ull);
}

TEST(TTagmaStore, AxisBounds)
{
   const ROOT::TTagmaStore store = MakeStore();
   EXPECT_TRUE(store.Contains(0, 0, 0));
   EXPECT_TRUE(store.Contains(999, 127, 9999));
   EXPECT_FALSE(store.Contains(1000, 0, 0));
   EXPECT_FALSE(store.Contains(0, 128, 0));
   EXPECT_FALSE(store.Contains(0, 0, 10000));
}
