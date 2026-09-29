// tagma_scatter: the access pattern the work is aimed at, event-selected reads
// in list order rather than a sequential scan.
//
// The store resolves an event in one step, so the requests an event costs do
// not depend on the order; a TTree re-traverses for every event, so a scattered
// order costs it more. The rows hold the layer and the medium constant: the
// mapped store copies a record from its mapping, the block source decompresses
// the block a record falls in, and the baseline drives TTree::GetEntry with no
// branch addresses.
//
//   root -l -b -q 'tagma_scatter.C("/path/store.bin", "/path/store.z.bin", "/path/open/data.root")'

#include "ROOT/TTagmaBlockSource.hxx"
#include "ROOT/TTagmaStore.hxx"
#include "ROOT/TTagmaWriter.hxx"

#include "TFile.h"
#include "TROOT.h"
#include "TTree.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

double Elapsed(const std::chrono::steady_clock::time_point &from)
{
   return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
}

void Report(const char *order, const char *row, double seconds, std::uint64_t requests, std::uint64_t events)
{
   std::printf("tagma_scatter: %-10s %-12s %-8.3f s  %-12llu requests  %-6.3f per event\n", order, row, seconds,
               static_cast<unsigned long long>(requests),
               events > 0 ? static_cast<double>(requests) / events : 0.0);
}

} // namespace

void tagma_scatter(const char *plainStore, const char *blockStore, const char *treePath, Long64_t count = 20000,
                   UInt_t seed = 12345, Int_t cacheMB = 64)
{
   ROOT::EnableThreadSafety();

   std::string why;
   ROOT::TTagmaStore::Layout layout;
   ROOT::TTagmaSchema schema;
   if (!ROOT::TTagmaWriter::ReadStore(plainStore, &layout, &schema, &why)) {
      std::printf("tagma_scatter: %s is not a plain store: %s\n", plainStore, why.c_str());
      return;
   }
   const std::uint64_t recordSize = layout.fRecordSize;
   auto mapped = std::make_shared<ROOT::TTagmaStore>(layout);
   if (!mapped->MapFile(plainStore)) {
      std::printf("tagma_scatter: cannot map %s\n", plainStore);
      return;
   }
   const std::uint64_t payload = mapped->SizeBytes();

   TFile *src = TFile::Open(treePath);
   if (src == nullptr || src->IsZombie()) {
      std::printf("tagma_scatter: cannot open %s\n", treePath);
      return;
   }
   TTree *tree = dynamic_cast<TTree *>(src->Get("Events"));
   const Long64_t entries = tree != nullptr ? tree->GetEntries() : 0;
   src->Close();
   delete src;
   if (entries <= 0) {
      std::printf("tagma_scatter: %s holds no Events tree\n", treePath);
      return;
   }
   count = std::min<Long64_t>(count, entries);

   std::vector<Long64_t> sequential(count);
   for (Long64_t i = 0; i < count; ++i)
      sequential[i] = i;
   std::vector<Long64_t> scattered = sequential;
   std::shuffle(scattered.begin(), scattered.end(), std::mt19937(seed));

   std::printf("tagma_scatter: events=%lld record=%llu payload=%llu\n", static_cast<long long>(count),
               static_cast<unsigned long long>(recordSize), static_cast<unsigned long long>(payload));

   const std::vector<std::pair<std::string, std::vector<Long64_t>>> orders = {{"sequential", sequential},
                                                                              {"scattered", scattered}};
   for (const auto &order : orders) {
      const std::vector<Long64_t> &list = order.second;

      // The mapped store: one copy of the record from the mapping per event.
      {
         const char *base = mapped->GetMapped();
         std::vector<char> rec(static_cast<std::size_t>(recordSize));
         const auto start = std::chrono::steady_clock::now();
         for (Long64_t e : list) {
            const std::uint64_t off = static_cast<std::uint64_t>(e) * recordSize;
            if (off + recordSize > payload)
               break;
            std::memcpy(rec.data(), base + off, static_cast<std::size_t>(recordSize));
         }
         Report(order.first.c_str(), "store mapped", Elapsed(start), list.size(), list.size());
      }

      // The block-compressed store: the block a record falls in is decompressed.
      if (blockStore != nullptr && blockStore[0] != '\0') {
         ROOT::TTagmaBlockSource source;
         if (!source.Open(blockStore, &why)) {
            std::printf("tagma_scatter: %s is not block-compressed: %s\n", blockStore, why.c_str());
         } else {
            std::vector<char> rec(static_cast<std::size_t>(recordSize));
            const auto start = std::chrono::steady_clock::now();
            for (Long64_t e : list) {
               const std::uint64_t off = static_cast<std::uint64_t>(e) * recordSize;
               if (off + recordSize > source.PayloadBytes() || source.Read(rec.data(), off, recordSize) < 0)
                  break;
            }
            Report(order.first.c_str(), "store block", Elapsed(start), list.size(), list.size());
         }
      }

      // The baseline: one GetEntry per event, no branch addresses, first with
      // no cache and then with a per-file cache, the configuration a
      // production job uses.
      for (const int withCache : {0, static_cast<int>(cacheMB)}) {
         TFile *file = TFile::Open(treePath);
         if (file == nullptr || file->IsZombie()) {
            std::printf("tagma_scatter: cannot open %s\n", treePath);
            return;
         }
         TTree *t2 = dynamic_cast<TTree *>(file->Get("Events"));
         if (withCache > 0) {
            t2->SetCacheSize(static_cast<Long64_t>(withCache) * 1024 * 1024);
            t2->AddBranchToCache("*", true);
         }
         const Long64_t before = file->GetReadCalls();
         const auto start = std::chrono::steady_clock::now();
         for (Long64_t e : list)
            t2->GetEntry(e);
         const double seconds = Elapsed(start);
         const Long64_t reads = file->GetReadCalls() - before;
         Report(order.first.c_str(), withCache > 0 ? "baseline+cache" : "baseline", seconds,
                static_cast<std::uint64_t>(reads), list.size());
         file->Close();
         delete file;
      }
   }
}
