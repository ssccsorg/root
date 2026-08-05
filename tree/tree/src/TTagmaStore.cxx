// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ROOT/TTagmaStore.hxx"

namespace ROOT {

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
