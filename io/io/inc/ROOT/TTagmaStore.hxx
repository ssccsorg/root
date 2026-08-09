// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_TTagmaStore
#define ROOT_TTagmaStore

// TTagmaStore: coordinate-indexed store for fixed-width event records.
//
// Production extension of the ROOT fork implementing the Tagma coordinate
// principle at the byte-source layer. An event is addressed as a
// coordinate (run, luminosity block, event number) and resolves to a
// direct byte offset with closed-form arithmetic, no hash function, and
// no index scan:
//
//   index(run, lumi, event) = (run * lumi_max + lumi) * event_max + event
//   offset(index)           = index * record_size
//
// The canonical Tagma coordinate engine is provided by ssccsorg/syntagma
// (sw/cpp, namespace tagma). This class is the ROOT-side extension that
// the TTree read path consumes. It is self-contained C++17 with no ROOT
// dependencies.

#include <cstdint>
#include <stdexcept>
#include <tuple>

namespace ROOT {

class TTagmaStore {
public:
   struct Layout {
      std::uint64_t fRunMax = 0;      // exclusive bound of the run axis
      std::uint64_t fLumiMax = 0;     // exclusive bound of the lumi axis
      std::uint64_t fEventMax = 0;    // exclusive bound of the event axis
      std::uint64_t fRecordSize = 0;  // bytes per fixed-width event record
   };

   // Validates the layout: every axis and the record size must be
   // nonzero, the record count must fit in uint64, and the total store
   // extent must fit in uint64. Violations throw std::invalid_argument
   // so that Compose and Offset can never overflow for in-bounds
   // coordinates.
   explicit TTagmaStore(const Layout &layout);

   // Compose (run, lumi, event) into the linear coordinate index.
   std::uint64_t Compose(std::uint64_t run, std::uint64_t lumi,
                         std::uint64_t event) const;

   // Decompose a linear index back into (run, lumi, event).
   std::tuple<std::uint64_t, std::uint64_t, std::uint64_t> Decompose(
       std::uint64_t index) const;

   // Byte offset of the record for (run, lumi, event).
   std::uint64_t Offset(std::uint64_t run, std::uint64_t lumi,
                        std::uint64_t event) const;

   // Number of addressable records (run_max * lumi_max * event_max).
   std::uint64_t RecordCount() const;

   // Total store extent in bytes.
   std::uint64_t SizeBytes() const;

   // True when all three axes fall inside the layout bounds.
   bool Contains(std::uint64_t run, std::uint64_t lumi,
                 std::uint64_t event) const;

   const Layout &GetLayout() const { return fLayout; }

private:
   Layout fLayout;
};

}  // namespace ROOT

#endif  // ROOT_TTagmaStore
