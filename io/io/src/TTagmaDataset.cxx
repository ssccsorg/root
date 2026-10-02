// Author: SSCCS Initiative 2026

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

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ROOT {

std::size_t TTagmaDataset::AddFile(const std::string &path, bool map)
{
   const std::uint64_t runBase = fFiles.empty() ? 0 : fFiles.back().fRunBase + fFiles.back().fLayout.fRunMax;
   return AddFile(path, runBase, 0, map);
}

std::size_t TTagmaDataset::AddFile(const std::string &path, std::uint64_t runBase, std::uint64_t lumiBase, bool map)
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
   file.fRunBase = runBase;
   file.fLumiBase = lumiBase;
   file.fRecordBase = fRecordCount;
   fRecordCount += store->RecordCount();
   fFiles.push_back(std::move(file));
   return fFiles.size() - 1;
}

std::uint64_t TTagmaDataset::RunMax() const
{
   std::uint64_t runMax = 0;
   for (const File &f : fFiles)
      runMax = std::max(runMax, f.fRunBase + f.fLayout.fRunMax);
   return runMax;
}

bool TTagmaDataset::Resolve(std::uint64_t run, std::uint64_t lumi, std::uint64_t event, std::size_t *file,
                            std::uint64_t *index) const
{
   for (std::size_t i = 0; i < fFiles.size(); ++i) {
      const File &f = fFiles[i];
      if (run < f.fRunBase || run >= f.fRunBase + f.fLayout.fRunMax)
         continue;
      if (lumi < f.fLumiBase || lumi >= f.fLumiBase + f.fLayout.fLumiMax)
         continue;
      const std::uint64_t localRun = run - f.fRunBase;
      const std::uint64_t localLumi = lumi - f.fLumiBase;
      if (!f.fStore->Contains(localRun, localLumi, event))
         return false;
      if (file != nullptr)
         *file = i;
      if (index != nullptr)
         *index = f.fStore->Compose(localRun, localLumi, event);
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
         *lumi = f.fLumiBase + std::get<1>(decomposed);
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

bool TTagmaDataset::ReadRecord(std::uint64_t flat, char *buf, std::uint64_t len) const
{
   if (buf == nullptr)
      return false;
   for (const File &f : fFiles) {
      const std::uint64_t count = f.fStore->RecordCount();
      if (flat < f.fRecordBase || flat - f.fRecordBase >= count)
         continue;
      if (f.fStore == nullptr || !f.fStore->IsMapped() || len != f.fLayout.fRecordSize)
         return false;
      const std::uint64_t offset = (flat - f.fRecordBase) * f.fLayout.fRecordSize;
      if (!f.fStore->Covers(offset, len))
         return false;
      std::memcpy(buf, f.fStore->GetMapped() + offset, static_cast<std::size_t>(len));
      return true;
   }
   return false;
}

bool TTagmaDataset::ReadRecords(std::uint64_t flat, std::uint64_t count, char *buf, std::uint64_t recordSize) const
{
   if (buf == nullptr || count == 0)
      return false;
   std::uint64_t remaining = count;
   std::uint64_t at = flat;
   char *out = buf;
   for (const File &f : fFiles) {
      if (remaining == 0)
         break;
      const std::uint64_t fileCount = f.fStore->RecordCount();
      if (at >= f.fRecordBase + fileCount)
         continue;
      if (at < f.fRecordBase)
         return false;
      if (f.fStore == nullptr || !f.fStore->IsMapped() || recordSize != f.fLayout.fRecordSize)
         return false;
      const std::uint64_t local = at - f.fRecordBase;
      const std::uint64_t take = std::min(remaining, fileCount - local);
      const std::uint64_t bytes = take * recordSize;
      const std::uint64_t offset = local * recordSize;
      if (!f.fStore->Covers(offset, bytes))
         return false;
      std::memcpy(out, f.fStore->GetMapped() + offset, static_cast<std::size_t>(bytes));
      out += bytes;
      at += take;
      remaining -= take;
   }
   return remaining == 0;
}

} // namespace ROOT
