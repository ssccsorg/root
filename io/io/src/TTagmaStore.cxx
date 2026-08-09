// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ROOT/TTagmaStore.hxx"

#include <limits>

namespace ROOT {

TTagmaStore::TTagmaStore(const Layout &layout) : fLayout(layout)
{
   if (fLayout.fRunMax == 0 || fLayout.fLumiMax == 0 ||
       fLayout.fEventMax == 0 || fLayout.fRecordSize == 0) {
      throw std::invalid_argument(
         "TTagmaStore: layout axes and record size must be nonzero");
   }
   if (fLayout.fRunMax > std::numeric_limits<std::uint64_t>::max() /
                            fLayout.fLumiMax) {
      throw std::invalid_argument("TTagmaStore: run_max * lumi_max overflows");
   }
   const std::uint64_t plane = fLayout.fRunMax * fLayout.fLumiMax;
   if (plane > std::numeric_limits<std::uint64_t>::max() /
                   fLayout.fEventMax) {
      throw std::invalid_argument("TTagmaStore: record count overflows");
   }
   const std::uint64_t count = plane * fLayout.fEventMax;
   if (count > std::numeric_limits<std::uint64_t>::max() /
                   fLayout.fRecordSize) {
      throw std::invalid_argument("TTagmaStore: store extent overflows");
   }
}

std::uint64_t TTagmaStore::Compose(std::uint64_t run, std::uint64_t lumi,
                                   std::uint64_t event) const
{
   return (run * fLayout.fLumiMax + lumi) * fLayout.fEventMax + event;
}

std::tuple<std::uint64_t, std::uint64_t, std::uint64_t> TTagmaStore::Decompose(
    std::uint64_t index) const
{
   const std::uint64_t event = index % fLayout.fEventMax;
   const std::uint64_t rest = index / fLayout.fEventMax;
   const std::uint64_t lumi = rest % fLayout.fLumiMax;
   const std::uint64_t run = rest / fLayout.fLumiMax;
   return {run, lumi, event};
}

std::uint64_t TTagmaStore::Offset(std::uint64_t run, std::uint64_t lumi,
                                  std::uint64_t event) const
{
   return Compose(run, lumi, event) * fLayout.fRecordSize;
}

std::uint64_t TTagmaStore::RecordCount() const
{
   return fLayout.fRunMax * fLayout.fLumiMax * fLayout.fEventMax;
}

std::uint64_t TTagmaStore::SizeBytes() const
{
   return RecordCount() * fLayout.fRecordSize;
}

bool TTagmaStore::Contains(std::uint64_t run, std::uint64_t lumi,
                           std::uint64_t event) const
{
   return run < fLayout.fRunMax && lumi < fLayout.fLumiMax &&
          event < fLayout.fEventMax;
}

}  // namespace ROOT
