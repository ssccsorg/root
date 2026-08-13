// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

// Consumes the mmap-backed dense space fetched from the canonical
// reference: CoordSpaceM3 place/at_path/vacate round trips over the
// 19 x 21 x 28 lattice, the fork's closed-form composition over the
// canonical 11172^3 radix reproduces the reference linear index, and the
// fork lattice contains exactly Coord::kNValid cells.

#include "ROOT/TTagmaStore.hxx"
#include "tagma_core/coord.h"
#include "tagma_core/coord_path.h"
#include "tagma_core/coord_space_m.h"

#include "gtest/gtest.h"

#include <array>
#include <cstdint>

namespace {

// A trivially destructible fixed-width record payload.
struct Record {
   std::uint8_t bytes[64];
};

// A path whose three coordinates carry the (run, lumi, event) axes as
// independent indices, the addressing the reference dense space uses.
tagma::CoordPath<3> Path(int run, int lumi, int event)
{
   return tagma::CoordPath<3>(std::array<tagma::Coord, 3>{
      *tagma::Coord::from_index(static_cast<std::uint16_t>(run)),
      *tagma::Coord::from_index(static_cast<std::uint16_t>(lumi)),
      *tagma::Coord::from_index(static_cast<std::uint16_t>(event))});
}

}  // namespace

TEST(TTagmaCoordSpaceM, PlaceAndReadBackOverTheLattice)
{
   tagma::CoordSpaceM3<Record> space;

   for (int run = 0; run < tagma::Coord::kInitialMax; ++run)
      for (int lumi = 0; lumi < tagma::Coord::kMedialMax; ++lumi)
         for (int event = 0; event < tagma::Coord::kFinalMax; ++event) {
            Record rec{};
            rec.bytes[0] = static_cast<std::uint8_t>(run);
            rec.bytes[1] = static_cast<std::uint8_t>(lumi);
            rec.bytes[2] = static_cast<std::uint8_t>(event);
            space.place_path(Path(run, lumi, event), rec);
         }
   EXPECT_EQ(space.len(), static_cast<std::size_t>(tagma::Coord::kNValid));
   EXPECT_FALSE(space.is_empty());

   for (int run = 0; run < tagma::Coord::kInitialMax; ++run)
      for (int lumi = 0; lumi < tagma::Coord::kMedialMax; ++lumi)
         for (int event = 0; event < tagma::Coord::kFinalMax; ++event) {
            const Record *got = space.at_path(Path(run, lumi, event));
            ASSERT_NE(got, nullptr);
            EXPECT_EQ(got->bytes[0], static_cast<std::uint8_t>(run));
            EXPECT_EQ(got->bytes[1], static_cast<std::uint8_t>(lumi));
            EXPECT_EQ(got->bytes[2], static_cast<std::uint8_t>(event));
         }

   // Vacating a cell removes it and returns the previous value.
   const auto prev = space.vacate_path(Path(0, 0, 0));
   ASSERT_TRUE(prev.has_value());
   EXPECT_EQ(prev->bytes[2], 0u);
   EXPECT_EQ(space.len(),
             static_cast<std::size_t>(tagma::Coord::kNValid) - 1u);
   EXPECT_EQ(space.at_path(Path(0, 0, 0)), nullptr);
   EXPECT_EQ(space.vacate_path(Path(0, 0, 0)), std::nullopt);
}

TEST(TTagmaCoordSpaceM, ForkCompositionMatchesReferenceLinearIndex)
{
   // The reference dense space addresses a path (c0, c1, c2) at
   // ((c0 * kNValid) + c1) * kNValid + c2. The fork store with the
   // canonical uniform radix reproduces the same closed form.
   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = static_cast<std::uint64_t>(tagma::Coord::kNValid);
   layout.fLumiMax = static_cast<std::uint64_t>(tagma::Coord::kNValid);
   layout.fEventMax = static_cast<std::uint64_t>(tagma::Coord::kNValid);
   layout.fRecordSize = 64;
   const ROOT::TTagmaStore store(layout);

   const std::uint64_t k = static_cast<std::uint64_t>(tagma::Coord::kNValid);
   const int runs[] = {0, 1, 18, tagma::Coord::kNValid - 1};
   const int lumis[] = {0, 7, 20, tagma::Coord::kNValid - 1};
   const int events[] = {0, 1, 27, tagma::Coord::kNValid - 1};
   for (const int run : runs)
      for (const int lumi : lumis)
         for (const int event : events) {
            const std::uint64_t expected =
               (static_cast<std::uint64_t>(run) * k + lumi) * k + event;
            EXPECT_EQ(store.Compose(run, lumi, event), expected);
         }
}

TEST(TTagmaCoordSpaceM, ForkLatticeMatchesTheCanonicalScale)
{
   // The fork's default lattice bounds mirror the canonical engine axes:
   // 19 x 21 x 28 = 11,172 = Coord::kNValid cells.
   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = static_cast<std::uint64_t>(tagma::Coord::kInitialMax);
   layout.fLumiMax = static_cast<std::uint64_t>(tagma::Coord::kMedialMax);
   layout.fEventMax = static_cast<std::uint64_t>(tagma::Coord::kFinalMax);
   layout.fRecordSize = 64;
   const ROOT::TTagmaStore store(layout);
   EXPECT_EQ(store.RecordCount(),
             static_cast<std::uint64_t>(tagma::Coord::kNValid));
}
