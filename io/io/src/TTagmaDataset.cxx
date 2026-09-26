// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ROOT/TTagmaDataset.hxx"

#include "ROOT/TTagmaSchema.hxx"
#include "ROOT/TTagmaWriter.hxx"

#include <cstring>
#include <stdexcept>

namespace ROOT {

std::size_t TTagmaDataset::AddFile(const std::string &path, bool map)
{
   TTagmaStore::Layout layout;
   TTagmaSchema schema;
   std::string why;
   if (!TTagmaWriter::ReadStore(path.c_str(), &layout, &schema, &why))
      throw std::runtime_error("TTagmaDataset: " + path + ": " + why);

   auto store = std::make_shared<TTagmaStore>(layout);
   if (map && !store->MapFile(path.c_str()))
      throw std::runtime_error("TTagmaDataset: cannot map " + path);

   File file;
   file.fPath = path;
   file.fLayout = layout;
   file.fStore = store;
   file.fRunBase = fFiles.empty() ? 0 : fFiles.back().fRunBase + fFiles.back().fLayout.fRunMax;
   file.fRecordBase = fRecordCount;
   fRecordCount += store->RecordCount();
   fFiles.push_back(std::move(file));
   return fFiles.size() - 1;
}

std::uint64_t TTagmaDataset::RunMax() const
{
   if (fFiles.empty())
      return 0;
   return fFiles.back().fRunBase + fFiles.back().fLayout.fRunMax;
}

bool TTagmaDataset::Resolve(std::uint64_t run, std::uint64_t lumi, std::uint64_t event, std::size_t *file,
                            std::uint64_t *index) const
{
   for (std::size_t i = 0; i < fFiles.size(); ++i) {
      const File &f = fFiles[i];
      if (run < f.fRunBase || run >= f.fRunBase + f.fLayout.fRunMax)
         continue;
      const std::uint64_t localRun = run - f.fRunBase;
      if (!f.fStore->Contains(localRun, lumi, event))
         return false;
      if (file != nullptr)
         *file = i;
      if (index != nullptr)
         *index = f.fStore->Compose(localRun, lumi, event);
      return true;
   }
   return false;
}

bool TTagmaDataset::ResolveFlat(std::uint64_t run, std::uint64_t lumi, std::uint64_t event, std::uint64_t *flat) const
{
   std::size_t file = 0;
   std::uint64_t index = 0;
   if (!Resolve(run, lumi, event, &file, &index))
      return false;
   if (flat != nullptr)
      *flat = fFiles[file].fRecordBase + index;
   return true;
}

bool TTagmaDataset::DecomposeFlat(std::uint64_t flat, std::uint64_t *run, std::uint64_t *lumi,
                                  std::uint64_t *event) const
{
   for (const File &f : fFiles) {
      const std::uint64_t count = f.fStore->RecordCount();
      if (flat < f.fRecordBase || flat - f.fRecordBase >= count)
         continue;
      const auto decomposed = f.fStore->Decompose(flat - f.fRecordBase);
      if (run != nullptr)
         *run = f.fRunBase + std::get<0>(decomposed);
      if (lumi != nullptr)
         *lumi = std::get<1>(decomposed);
      if (event != nullptr)
         *event = std::get<2>(decomposed);
      return true;
   }
   return false;
}

bool TTagmaDataset::ReadRecord(std::uint64_t run, std::uint64_t lumi, std::uint64_t event, char *buf,
                               std::uint64_t len) const
{
   if (buf == nullptr)
      return false;
   std::size_t file = 0;
   std::uint64_t index = 0;
   if (!Resolve(run, lumi, event, &file, &index))
      return false;
   const File &f = fFiles[file];
   if (f.fStore == nullptr || !f.fStore->IsMapped() || len != f.fLayout.fRecordSize)
      return false;
   const std::uint64_t offset = index * f.fLayout.fRecordSize;
   if (!f.fStore->Covers(offset, len))
      return false;
   std::memcpy(buf, f.fStore->GetMapped() + offset, static_cast<std::size_t>(len));
   return true;
}

} // namespace ROOT
