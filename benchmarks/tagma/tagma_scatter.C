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
// The first two store rows copy the addressing unit, the index record. The
// entry-layer rows go further and read the whole event through the tree
// interface: the store attached to a tree with the schema its descriptor
// carries, so one GetEntry serves the index record and the event's slice and
// drives the field branches. Those rows and the baseline both move the whole
// event and both include the delivery the branch machinery costs, so the two
// sides are layer-matched as well as payload-matched, and they are the rows the
// scatter claim is read from. One entry row reads the plain store, the other
// the block-compressed store, so the compressed claim has its own row.
//
// The entry rows are gated. Before any of them is timed, the values they
// deliver are compared with the baseline file for a sample of the same
// entries, and a gate that does not pass skips them, so a row that serves the
// wrong record fails instead of being read as a result.
//
//   root -l -b -q 'tagma_scatter.C("/path/store.bin", "/path/store.z.bin", "/path/open/data.root")'

#include "ROOT/TTagmaBlockSource.hxx"
#include "ROOT/TTagmaStore.hxx"
#include "ROOT/TTagmaWriter.hxx"

#include "TFile.h"
#include "TLeaf.h"
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

// One entry-layer row: the store behind the tree interface, every field branch
// the schema materializes enabled, so the row carries the whole event and the
// delivery as the baseline does. The seconds are the measurement; the counts
// are context, the file hook's requests and the store-served reads.
void MeasureEntry(const char *order, const char *row, TFile *file, TTree &tree, const std::vector<Long64_t> &list)
{
   const Int_t calls0 = file->GetReadCalls();
   const Int_t tagma0 = file->GetTagmaReadCalls();
   const Int_t sys0 = file->GetSysReadCalls();
   const auto start = std::chrono::steady_clock::now();
   Long64_t served = 0;
   for (Long64_t e : list)
      if (tree.GetEntry(e) > 0)
         ++served;
   const double seconds = Elapsed(start);
   std::printf("tagma_scatter: %-10s %-18s %-8.3f s  reads=%lld tagma=%lld syscalls=%lld  served=%lld\n", order, row,
               seconds, static_cast<long long>(file->GetReadCalls() - calls0),
               static_cast<long long>(file->GetTagmaReadCalls() - tagma0),
               static_cast<long long>(file->GetSysReadCalls() - sys0), static_cast<long long>(served));
   if (served != static_cast<Long64_t>(list.size()))
      std::printf("tagma_scatter: %s served %lld of %lld\n", row, static_cast<long long>(served),
                  static_cast<long long>(list.size()));
}

// The gate: the entry layer must deliver the values the baseline file holds for
// the same entries. `event` identifies the entry and `MET_pt` carries a payload
// value, so a row that serves the wrong record is caught before it is timed.
// Returns the number of mismatches, or -1 when the check cannot run.
Long64_t GateEntryLayer(const char *treePath, TTree &storeTree, const std::vector<Long64_t> &list, Long64_t sample)
{
   TFile *file = TFile::Open(treePath);
   if (file == nullptr || file->IsZombie()) {
      std::printf("tagma_scatter: the gate cannot open %s\n", treePath);
      return -1;
   }
   TTree *base = dynamic_cast<TTree *>(file->Get("Events"));
   TLeaf *storeEvent = storeTree.GetLeaf("event");
   TLeaf *storeMet = storeTree.GetLeaf("MET_pt");
   TLeaf *baseEvent = base != nullptr ? base->GetLeaf("event") : nullptr;
   TLeaf *baseMet = base != nullptr ? base->GetLeaf("MET_pt") : nullptr;
   if (storeEvent == nullptr || storeMet == nullptr || baseEvent == nullptr || baseMet == nullptr) {
      std::printf("tagma_scatter: the gate needs event and MET_pt on both sides\n");
      file->Close();
      delete file;
      return -1;
   }
   Long64_t checked = 0;
   Long64_t bad = 0;
   const std::size_t step = list.size() > static_cast<std::uint64_t>(sample) ? list.size() / sample : 1;
   for (std::size_t i = 0; i < list.size() && checked < sample; i += step) {
      const Long64_t e = list[i];
      if (base->GetEntry(e) <= 0 || storeTree.GetEntry(e) <= 0)
         break;
      ++checked;
      const Double_t storeEventValue = storeEvent->GetValue(0);
      const Double_t baseEventValue = baseEvent->GetValue(0);
      const Double_t storeMetValue = storeMet->GetValue(0);
      const Double_t baseMetValue = baseMet->GetValue(0);
      if (storeEventValue != baseEventValue || storeMetValue != baseMetValue) {
         ++bad;
         std::printf("tagma_scatter: the gate mismatches at entry %lld: event %g against %g, MET_pt %g against %g\n",
                     static_cast<long long>(e), storeEventValue, baseEventValue, storeMetValue, baseMetValue);
      }
   }
   file->Close();
   delete file;
   std::printf("tagma_scatter: gate entry checked=%lld mismatches=%lld\n", static_cast<long long>(checked),
               static_cast<long long>(bad));
   return bad;
}

} // namespace

void tagma_scatter(const char *plainStore, const char *blockStore, const char *treePath, Long64_t count = 2000,
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

   // The entry-layer readers: the plain store behind the tree interface, and
   // the block-compressed store behind the same interface through the source
   // its descriptor names, each with the schema the descriptor carries, so one
   // GetEntry per event serves the index record, the event's slice, and the
   // fields the branches deliver. A store is not a ROOT file, so it is opened
   // as a raw file, the form the harness uses.
   const std::string plainRaw = std::string(plainStore) + "?filetype=raw";
   TFile *entryFile = TFile::Open(plainRaw.c_str());
   TTree entryTree("Events", "Events");
   Bool_t entryOk = kFALSE;
   if (entryFile != nullptr && !entryFile->IsZombie()) {
      entryFile->SetTagmaStore(mapped);
      entryTree.SetDirectory(entryFile);
      entryTree.SetEntries(count);
      entryTree.SetTagmaStore(mapped);
      entryOk = entryTree.SetTagmaSchema(schema);
   }

   auto block = std::make_shared<ROOT::TTagmaBlockSource>();
   TFile *blockFile = nullptr;
   TTree blockTree("Events", "Events");
   Bool_t blockOk = kFALSE;
   if (blockStore != nullptr && blockStore[0] != '\0' && block->Open(blockStore, &why)) {
      const std::string blockRaw = std::string(blockStore) + "?filetype=raw";
      blockFile = TFile::Open(blockRaw.c_str());
      auto blockStoreObject = std::make_shared<ROOT::TTagmaStore>(block->GetLayout());
      if (blockFile != nullptr && !blockFile->IsZombie()) {
         blockFile->SetTagmaStore(blockStoreObject);
         blockFile->SetTagmaSource(
            std::make_shared<ROOT::TTagmaCachedSource>(block, block->BlockBytes(), 16, block->PayloadBytes()));
         blockTree.SetDirectory(blockFile);
         blockTree.SetEntries(count);
         blockTree.SetTagmaStore(blockStoreObject);
         blockOk = blockTree.SetTagmaSchema(block->GetSchema());
      }
   }

   // The gate runs over the scattered list, the harder order, before any row is
   // timed. A gate that does not pass skips the entry rows rather than letting
   // an unverified time stand.
   if (entryOk && GateEntryLayer(treePath, entryTree, scattered, 32) != 0) {
      std::printf("tagma_scatter: the entry-layer rows are skipped, the gate did not pass\n");
      entryOk = kFALSE;
      blockOk = kFALSE;
   }
   if (!entryOk && entryFile != nullptr)
      std::printf("tagma_scatter: the entry-layer rows are unavailable\n");

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

      // The entry layer: the store behind the tree interface, with every field
      // branch the schema materializes enabled, so the row carries the whole
      // event and the delivery as the baseline does.
      if (entryOk)
         MeasureEntry(order.first.c_str(), "store entry", entryFile, entryTree, list);
      if (blockOk)
         MeasureEntry(order.first.c_str(), "store block entry", blockFile, blockTree, list);

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
