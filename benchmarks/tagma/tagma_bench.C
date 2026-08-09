// tagma_bench.C
//
// M5 benchmark for the coordinate-indexed TTree read path (ssccs #103).
// Runs the same logical workload, read every event of a dataset, through
// the pre-replacement baseline and through the coordinate store, and
// reports wall-clock time, request count, bytes moved, and system call
// count for both paths.
//
// The baseline replicates the M1 workload: sequential TTree::GetEntry
// with the read cache disabled, which produces the scattered singular
// reads documented as 372,000 requests averaging 4.6 KB. The coordinate
// path serves the same number of events as fixed-width records through
// TTagmaStore, one read per event at the record size.
//
// Usage:
//   root -l -b -q 'tagma_bench.C()'
//   root -l -b -q 'tagma_bench.C("root://eospublic.cern.ch//eos/opendata/cms/Run2016G/DoubleMuon/NANOAOD/UL2016_MiniAODv2_NanoAODv9-v2/2430000/05DD095C-F6C3-9A4F-9FB3-348A5A6403D5.root", "Events", -1, 2560)'
//   root -l -b -q 'tagma_bench.C("/path/to/local.root", "Events", 2000, 2560, 3, 1)'
//
// Arguments:
//   url           baseline data source; empty generates a synthetic
//                 scattered tree with the same per-event payload
//   tree_name     tree to read in the baseline
//   max_entries   entries to read; -1 reads all
//   record_size   fixed-width record bytes per event for the coordinate
//                 store; 0 defaults to 2560
//   nscatter      synthetic baseline branches per event, the number of
//                 reads issued per event by the scattered path
//   disable_cache 1 disables the TTreeCache in the baseline (default)

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "ROOT/TTagmaStore.hxx"
#include "TFile.h"
#include "TStopwatch.h"
#include "TSystem.h"
#include "TTree.h"

namespace {

constexpr Long64_t kDefaultEntries = 20000;
constexpr Long64_t kDefaultRecordSize = 2560;
const char *kScatterFile = "tagma_bench_scatter.root";
const char *kStoreFile = "tagma_bench_store.bin";
const char *kStoreFileRaw = "tagma_bench_store.bin?filetype=raw";

struct BenchResult {
   const char *name = nullptr;
   Long64_t entries = 0;
   Double_t wall = 0;
   Double_t cpu = 0;
   Long64_t readCalls = 0;
   Long64_t tagmaReadCalls = 0;
   Long64_t sysReadCalls = 0;
   Long64_t bytesRead = 0;
   Bool_t ok = kTRUE;
};

// Writes a synthetic scattered tree: nscatter fixed-size branches per
// event, one entry per basket, so that a cache-disabled sequential read
// issues one small read per branch per event. The per-event payload is
// exactly record_size bytes across the branches.
bool MakeScatteredTree(const char *path, Long64_t entries, Int_t nscatter,
                       Long64_t recordSize)
{
   TFile file(path, "RECREATE");
   if (file.IsZombie()) {
      std::fprintf(stderr, "tagma_bench: cannot create %s\n", path);
      return false;
   }
   TTree tree("Events", "Events");
   tree.SetAutoFlush(1);

   std::vector<Long64_t> sizes(nscatter);
   const Long64_t chunk = recordSize / nscatter;
   for (Int_t b = 0; b < nscatter; ++b)
      sizes[b] = chunk;
   sizes[nscatter - 1] = recordSize - chunk * (nscatter - 1);

   std::vector<std::vector<unsigned char>> branches(nscatter);
   for (Int_t b = 0; b < nscatter; ++b) {
      branches[b].assign(sizes[b], 0);
      tree.Branch(Form("b%02d", b), branches[b].data(),
                  Form("b%02d[%lld]/b", b, static_cast<long long>(sizes[b])));
   }
   for (Long64_t i = 0; i < entries; ++i) {
      // Incompressible deterministic pattern, like real detector data.
      for (Int_t b = 0; b < nscatter; ++b) {
         for (Long64_t j = 0; j < sizes[b]; ++j)
            branches[b][j] =
                static_cast<unsigned char>((i * 31 + b * 7 + j * 13) % 251);
      }
      tree.Fill();
   }
   tree.Write();
   file.Close();
   return true;
}

// Computes the per-branch payload sizes for the synthetic tree so that
// the read side can allocate matching buffers and set branch addresses.
void ScatteredSizes(Int_t nscatter, Long64_t recordSize,
                    std::vector<Long64_t> *sizes)
{
   sizes->assign(nscatter, recordSize / nscatter);
   (*sizes)[nscatter - 1] =
       recordSize - (recordSize / nscatter) * (nscatter - 1);
}

// Writes the fixed-width coordinate store file: entries records of
// record_size bytes at consecutive offsets, record i at byte
// i * record_size. The bytes are a deterministic pattern; the read path
// measures the layout, and content correctness is covered by the unit
// tests.
bool MakeStoreFile(const char *path, Long64_t entries, Long64_t recordSize)
{
   if (recordSize <= 0 || recordSize > (1ll << 30)) {
      std::fprintf(stderr, "tagma_bench: record_size %lld out of range\n",
                   static_cast<long long>(recordSize));
      return false;
   }
   FILE *out = std::fopen(path, "wb");
   if (!out) {
      std::fprintf(stderr, "tagma_bench: cannot create %s\n", path);
      return false;
   }
   std::vector<unsigned char> record(static_cast<std::size_t>(recordSize));
   for (Long64_t i = 0; i < entries; ++i) {
      std::fill(record.begin(), record.end(),
                static_cast<unsigned char>(i % 251));
      if (std::fwrite(record.data(), 1, record.size(), out) != record.size()) {
         std::fprintf(stderr, "tagma_bench: short write to %s\n", path);
         std::fclose(out);
         return false;
      }
   }
   std::fclose(out);
   return true;
}

BenchResult MeasureBaseline(TFile *file, TTree *tree, Long64_t limit,
                            Bool_t disableCache,
                            std::vector<std::vector<unsigned char>> *addresses)
{
   BenchResult r;
   r.name = "baseline";
   r.entries = limit;
   if (disableCache)
      tree->SetCacheSize(0);

   const Int_t calls0 = file->GetReadCalls();
   const Int_t tagma0 = file->GetTagmaReadCalls();
   const Int_t sys0 = file->GetSysReadCalls();
   const Long64_t bytes0 = file->GetBytesRead();

   TStopwatch watch;
   watch.Start();
   Long64_t checksum = 0;
   for (Long64_t i = 0; i < limit; ++i) {
      tree->GetEntry(i);
      // Touch the payload buffers. A real analysis reads the data; the
      // synthetic baseline must force the basket payload transfer the
      // same way, otherwise ROOT defers the read to first access.
      if (addresses) {
         for (const auto &buf : *addresses)
            if (!buf.empty())
               checksum += buf[0];
      }
   }
   watch.Stop();

   r.wall = watch.RealTime();
   r.cpu = watch.CpuTime();
   r.readCalls = file->GetReadCalls() - calls0;
   r.tagmaReadCalls = file->GetTagmaReadCalls() - tagma0;
   r.sysReadCalls = file->GetSysReadCalls() - sys0;
   r.bytesRead = file->GetBytesRead() - bytes0;
   (void)checksum;
   return r;
}

BenchResult MeasureCoordinate(const char *storePath, Long64_t limit,
                              Long64_t recordSize)
{
   BenchResult r;
   r.name = "coordinate";
   r.entries = limit;

   TFile *file = TFile::Open(storePath);
   if (!file || file->IsZombie()) {
      std::fprintf(stderr, "tagma_bench: cannot open %s\n", storePath);
      r.ok = kFALSE;
      return r;
   }

   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1;
   layout.fLumiMax = 1;
   layout.fEventMax = static_cast<std::uint64_t>(limit);
   layout.fRecordSize = static_cast<std::uint64_t>(recordSize);
   std::shared_ptr<ROOT::TTagmaStore> store;
   try {
      store = std::make_shared<ROOT::TTagmaStore>(layout);
   } catch (const std::invalid_argument &e) {
      std::fprintf(stderr, "tagma_bench: %s\n", e.what());
      delete file;
      r.ok = kFALSE;
      return r;
   }
   // The byte source serves aligned fixed-width record requests, and the
   // entry layer resolves each event through the same store.
   file->SetTagmaStore(store);

   TTree tree("Events", "Events");
   tree.SetDirectory(file);
   tree.SetEntries(limit);
   tree.SetTagmaStore(store);

   const Int_t calls0 = file->GetReadCalls();
   const Int_t tagma0 = file->GetTagmaReadCalls();
   const Int_t sys0 = file->GetSysReadCalls();
   const Long64_t bytes0 = file->GetBytesRead();

   TStopwatch watch;
   watch.Start();
   Long64_t served = 0;
   for (Long64_t i = 0; i < limit; ++i) {
      if (tree.GetEntry(i) > 0)
         ++served;
   }
   watch.Stop();

   r.wall = watch.RealTime();
   r.cpu = watch.CpuTime();
   r.readCalls = file->GetReadCalls() - calls0;
   r.tagmaReadCalls = file->GetTagmaReadCalls() - tagma0;
   r.sysReadCalls = file->GetSysReadCalls() - sys0;
   r.bytesRead = file->GetBytesRead() - bytes0;

   if (served != limit) {
      std::fprintf(stderr,
                   "tagma_bench: coordinate path served %lld of %lld entries\n",
                   static_cast<long long>(served),
                   static_cast<long long>(limit));
      r.ok = kFALSE;
   }
   delete file;
   return r;
}

void PrintResult(const BenchResult &r)
{
   const Double_t readsPerEvent =
       r.entries > 0 ? Double_t(r.readCalls) / r.entries : 0;
   const Double_t bytesPerRead =
       r.readCalls > 0 ? Double_t(r.bytesRead) / r.readCalls : 0;
   const Double_t syscallsPerEvent =
       r.entries > 0 ? Double_t(r.sysReadCalls) / r.entries : 0;
   const Double_t mbs = r.wall > 0 ? Double_t(r.bytesRead) / 1e6 / r.wall : 0;
   std::printf(
       "tagma_bench: %-11s %8.3f %8.3f %7lld %7lld %8lld %12lld %7.2f "
       "%10.1f %10.2f %9.1f\n",
       r.name, r.wall, r.cpu, static_cast<long long>(r.readCalls),
       static_cast<long long>(r.tagmaReadCalls),
       static_cast<long long>(r.sysReadCalls),
       static_cast<long long>(r.bytesRead), readsPerEvent, bytesPerRead,
       syscallsPerEvent, mbs);
}

}  // namespace

int tagma_bench(const char *url = "", const char *tree_name = "Events",
                Long64_t max_entries = -1, Long64_t record_size = 0,
                Int_t nscatter = 3, Bool_t disable_cache = kTRUE)
{
   if (record_size <= 0)
      record_size = kDefaultRecordSize;
   if (nscatter < 1)
      nscatter = 1;

   const Bool_t synthetic = (url == nullptr || url[0] == '\0');
   const char *source = synthetic ? "<synthetic>" : url;

   TFile *baselineFile = nullptr;
   TTree *baselineTree = nullptr;
   Long64_t total = 0;

   if (synthetic) {
      if (!MakeScatteredTree(kScatterFile, kDefaultEntries, nscatter,
                             record_size))
         return 1;
      baselineFile = TFile::Open(kScatterFile);
   } else {
      baselineFile = TFile::Open(url);
   }
   if (!baselineFile || baselineFile->IsZombie()) {
      std::fprintf(stderr, "tagma_bench: cannot open the baseline source %s\n",
                   source);
      delete baselineFile;
      return 1;
   }
   baselineFile->GetObject(tree_name, baselineTree);
   if (!baselineTree) {
      std::fprintf(stderr, "tagma_bench: tree %s not found in %s\n",
                   tree_name, source);
      delete baselineFile;
      return 1;
   }
   total = baselineTree->GetEntries();
   const Long64_t limit =
       (max_entries > 0 && max_entries < total) ? max_entries : total;
   if (limit <= 0) {
      std::fprintf(stderr, "tagma_bench: nothing to read in %s\n", source);
      delete baselineFile;
      return 1;
   }

   // The synthetic baseline binds the payload to branch addresses, like
   // a real analysis reading the data, so the basket payload is
   // transferred on every GetEntry instead of being deferred.
   std::vector<std::vector<unsigned char>> addresses;
   if (synthetic) {
      std::vector<Long64_t> sizes;
      ScatteredSizes(nscatter, record_size, &sizes);
      addresses.resize(nscatter);
      for (Int_t b = 0; b < nscatter; ++b) {
         addresses[b].assign(sizes[b], 0);
         baselineTree->SetBranchAddress(Form("b%02d", b),
                                        addresses[b].data());
      }
   }

   const BenchResult base = MeasureBaseline(baselineFile, baselineTree, limit,
                                            disable_cache,
                                            synthetic ? &addresses : nullptr);
   delete baselineFile;

   if (!MakeStoreFile(kStoreFile, limit, record_size))
      return 1;
   const BenchResult coord =
       MeasureCoordinate(kStoreFileRaw, limit, record_size);

   std::printf("tagma_bench: source=%s\n", source);
   std::printf("tagma_bench: entries=%lld record_size=%lld nscatter=%d "
               "cache_disabled=%d\n",
               static_cast<long long>(limit),
               static_cast<long long>(record_size), nscatter,
               disable_cache ? 1 : 0);
   std::printf(
       "tagma_bench: %-11s %8s %8s %7s %7s %8s %12s %7s %10s %10s %9s\n",
       "path", "wall_s", "cpu_s", "reads", "tagma", "syscalls",
       "bytes_moved", "reads/ev", "bytes/read", "syscalls/ev", "MB/s");
   PrintResult(base);
   PrintResult(coord);

   gSystem->Unlink(kStoreFile);
   if (synthetic)
      gSystem->Unlink(kScatterFile);

   if (!coord.ok)
      return 1;
   std::printf("tagma_bench: comparison complete\n");
   return 0;
}
