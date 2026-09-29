// tagma_mt: thread scaling of the store read path against the baseline.
//
// Every worker owns its file object and reads a disjoint range, the model a
// production job uses, and the medium and the layer are held constant: the
// store row reads the whole payload through the byte source, the baseline row
// drives TTree::GetEntry over the same events with no branch addresses, so both
// move all the data with no field copy.
//
//   root -l -b -q 'tagma_mt.C("/path/to/store.bin", "/path/to/open/data.root")'

#include "ROOT/TTagmaBlockSource.hxx"
#include "ROOT/TTagmaStore.hxx"
#include "ROOT/TTagmaWriter.hxx"

#include "TFile.h"
#include "TROOT.h"
#include "TTree.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

double Elapsed(const std::chrono::steady_clock::time_point &from)
{
   return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
}

// Splits `total` into `parts` contiguous ranges that cover it exactly.
std::vector<std::pair<std::uint64_t, std::uint64_t>> Ranges(std::uint64_t total, int parts)
{
   std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
   const std::uint64_t each = total / parts;
   const std::uint64_t extra = total % parts;
   std::uint64_t lo = 0;
   for (int i = 0; i < parts; ++i) {
      const std::uint64_t hi = lo + each + (static_cast<std::uint64_t>(i) < extra ? 1 : 0);
      ranges.emplace_back(lo, hi);
      lo = hi;
   }
   return ranges;
}

} // namespace

void tagma_mt(const char *storePath, const char *treePath, Int_t maxThreads = 8)
{
   ROOT::EnableThreadSafety();

   std::string why;
   ROOT::TTagmaBlockSource probe;
   if (!probe.Open(storePath, &why)) {
      std::printf("tagma_mt: %s is not block-compressed: %s\n", storePath, why.c_str());
      return;
   }
   const std::uint64_t payload = probe.PayloadBytes();
   probe.Close();

   TFile *src = TFile::Open(treePath);
   if (src == nullptr || src->IsZombie()) {
      std::printf("tagma_mt: cannot open %s\n", treePath);
      return;
   }
   TTree *tree = dynamic_cast<TTree *>(src->Get("Events"));
   if (tree == nullptr) {
      std::printf("tagma_mt: %s holds no Events tree\n", treePath);
      src->Close();
      delete src;
      return;
   }
   const Long64_t entries = tree->GetEntries();
   src->Close();
   delete src;

   std::printf("tagma_mt: payload=%llu entries=%lld\n", static_cast<unsigned long long>(payload),
               static_cast<long long>(entries));
   std::printf("tagma_mt: %-8s %-10s %-11s %-10s %-6s\n", "threads", "store_s", "store_MB/s", "base_s", "base/store");

   for (int threads = 1; threads <= maxThreads; threads *= 2) {
      const auto ranges = Ranges(payload, threads);
      const auto startStore = std::chrono::steady_clock::now();
      {
         std::vector<std::thread> workers;
         for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&ranges, t, storePath]() {
               ROOT::TTagmaBlockSource source;
               std::string w;
               if (!source.Open(storePath, &w))
                  return;
               std::vector<char> buf(1u << 20);
               for (std::uint64_t at = ranges[t].first; at < ranges[t].second;) {
                  const std::uint64_t take = std::min<std::uint64_t>(buf.size(), ranges[t].second - at);
                  if (source.Read(buf.data(), at, take) < 0)
                     return;
                  at += take;
               }
            });
         }
         for (auto &w : workers)
            w.join();
      }
      const double storeSeconds = Elapsed(startStore);

      const auto entryRanges = Ranges(static_cast<std::uint64_t>(entries), threads);
      const auto startBase = std::chrono::steady_clock::now();
      {
         std::vector<std::thread> workers;
         for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&entryRanges, t, treePath]() {
               TFile *file = TFile::Open(treePath, "READ");
               if (file == nullptr || file->IsZombie())
                  return;
               TTree *t2 = dynamic_cast<TTree *>(file->Get("Events"));
               if (t2 != nullptr) {
                  for (std::uint64_t e = entryRanges[t].first; e < entryRanges[t].second; ++e)
                     t2->GetEntry(static_cast<Long64_t>(e));
               }
               file->Close();
               delete file;
            });
         }
         for (auto &w : workers)
            w.join();
      }
      const double baseSeconds = Elapsed(startBase);

      std::printf("tagma_mt: %-8d %-10.3f %-11.1f %-10.3f %-6.2f\n", threads, storeSeconds,
                  static_cast<double>(payload) / 1e6 / storeSeconds, baseSeconds,
                  baseSeconds > 0 ? baseSeconds / storeSeconds : 0.0);
   }
}
