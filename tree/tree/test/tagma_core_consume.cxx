// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

// Verifies that the fork consumes the canonical Tagma C++ core (vendored
// from ssccsorg/syntagma): TTagmaStore's closed-form composition and
// decomposition must agree with tagma::Coord over the entire 19 x 21 x 28
// lattice, and byte offsets must follow from the composed index.

#include "ROOT/TTagmaStore.hxx"
#include "tagma_core/coord.h"

#include "gtest/gtest.h"

TEST(TTagmaStore, MatchesCanonicalEngineOverTheLattice)
{
   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = static_cast<std::uint64_t>(tagma::Coord::kInitialMax);
   layout.fLumiMax = static_cast<std::uint64_t>(tagma::Coord::kMedialMax);
   layout.fEventMax = static_cast<std::uint64_t>(tagma::Coord::kFinalMax);
   layout.fRecordSize = 8;
   const ROOT::TTagmaStore store(layout);

   for (std::uint64_t run = 0; run < layout.fRunMax; ++run) {
      for (std::uint64_t lumi = 0; lumi < layout.fLumiMax; ++lumi) {
         for (std::uint64_t event = 0; event < layout.fEventMax; ++event) {
            const std::uint64_t composed = store.Compose(run, lumi, event);
            const auto coord = tagma::Coord::from_axes(
               static_cast<int>(run), static_cast<int>(lumi),
               static_cast<int>(event));
            ASSERT_TRUE(coord.has_value());
            EXPECT_EQ(composed, static_cast<std::uint64_t>(coord->index()));

            const auto [r, l, e] = store.Decompose(composed);
            EXPECT_EQ(r, run);
            EXPECT_EQ(l, lumi);
            EXPECT_EQ(e, event);

            EXPECT_EQ(store.Offset(run, lumi, event), composed * 8u);
         }
      }
   }
}
