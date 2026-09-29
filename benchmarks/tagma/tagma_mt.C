// tagma_mt: thread scaling of the store read path against the baseline, with
// and without a per-thread read cache.
//
// Every worker owns its file object and reads a disjoint range, the model a
// production job uses, and the medium and the layer are held constant: the
// store row reads the whole payload through the byte source, the baseline rows
// drive TTree::GetEntry over the same events with no branch addresses, so all
// rows move the data with no field copy. The cached row sizes a TTreeCache per
// worker, the configuration a production job uses, and answers whether the
// cache degrades under threads as claimed.
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

void tagma_mt(const char *storePath, const char *treePath, Int_t maxThreads = 8, Int_t cacheMB = 32)
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

   // One baseline pass over a worker's range, with a TTreeCache when cacheBytes
   // is positive. Returns the seconds the pass took.
   const auto baseline = [treePath](const std::vector<std::pair<std::uint64_t, std::uint64_t>> &ranges,
                                    std::size_t cacheBytes) {
      const auto start = std::chrono::steady_clock::now();
      std::vector<std::thread> workers;
      for (std::size_t t = 0; t < ranges.size(); ++t) {
         workers.emplace_back([&ranges, t, treePath, cacheBytes]() {
            TFile *file = TFile::Open(treePath, "READ");
            if (file == nullptr || file->IsZombie())
               return;
            TTree *t2 = dynamic_cast<TTree *>(file->Get("Events"));
            if (t2 != nullptr) {
               if (cacheBytes > 0) {
                  t2->SetCacheSize(static_cast<Long64_t>(cacheBytes));
                  t2->AddBranchToCache("*", true);
               }
               for (std::uint64_t e = ranges[t].first; e < ranges[t].second; ++e)
                  t2->GetEntry(static_cast<Long64_t>(e));
            }
            file->Close();
            delete file;
         });
      }
      for (auto &w : workers)
         w.join();
      return Elapsed(start);
   };

   std::printf("tagma_mt: payload=%llu entries=%lld cache=%d MB\n", static_cast<unsigned long long>(payload),
               static_cast<long long>(entries), static_cast<int>(cacheMB));
   std::printf("tagma_mt: %-8s %-9s %-11s %-9s %-9s %-8s %-8s\n", "threads", "store_s", "store_MB/s", "base_s",
               "base+cache", "b/store", "bc/store");

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
      const double baseSeconds = baseline(entryRanges, 0);
      const double baseCacheSeconds = baseline(entryRanges, static_cast<std::size_t>(cacheMB) * 1024 * 1024);

      std::printf("tagma_mt: %-8d %-9.3f %-11.1f %-9.3f %-9.3f %-8.2f %-8.2f\n", threads, storeSeconds,
                  static_cast<double>(payload) / 1e6 / storeSeconds, baseSeconds, baseCacheSeconds,
                  storeSeconds > 0 ? baseSeconds / storeSeconds : 0.0,
                  storeSeconds > 0 ? baseCacheSeconds / storeSeconds : 0.0);
   }
}
