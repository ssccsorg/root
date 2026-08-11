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
// TTagmaStore, one read per event at the record size, and the mapped
// coordinate path serves the same records from the mmap'ed store file
// with no read system call at all, the byte source never reaching the
// medium for mapped data.
//
// Reference results (master document)
// -----------------------------------
// The measured comparison below is the master record for this
// benchmark. Absolute times are machine-specific: the runs were
// measured on a single Arm Firestorm 3.2 GHz core (macOS, out-of-source
// Release build, treeplayer=ON, testing=ON). The ratios are the claim;
// the request and system call counts are media-independent.
//
// Workload: read every event of the CMS Run2016G DoubleMuon NanoAOD
// first file (tree Events, 2,315,223 events, 2,155,974,646 bytes), the
// M1 workload. The coordinate rows read the fixed-width store converted
// from the same events (2,560-byte records, 320 scalar leaves).
//
// Full dataset, same medium (local disk), cache-disabled baseline:
//   path              wall_s   reads/ev  syscalls/ev  bytes/read    MB/s
//   baseline          195.2    0.20      0.20         4,573         11.0
//   coordinate          3.34   1.00      1.00         2,560        1,776   (58.5x)
//   coordinate+map      1.45   1.00      0.00         2,560        4,087   (134.6x)
//   served_checksum: match (233262869086)
//
// One-time conversion of the dataset into the store: 226.3 s.
//
// Analysis workload (MET_pt above 100 GeV and at least one muon, MET_pt
// histogram), full dataset:
//   analysis_baseline    188.9 s, selected 20,861, histogram mean 132.696
//   analysis_coordinate    3.10 s, selected 20,861, histogram mean 132.696  (60.9x)
//   analysis_match: yes
//
// Baseline with the TTreeCache enabled (10,000 events): 8 read calls,
// 1,943 KB per read, efficiency 0.917, miss rate 0.083. This is the
// ideal sequential case; the documented cache degradation under
// out-of-order multithreaded reads is not reproduced.
//
// M1 signature slice (2,000 events, same medium): baseline 1.37 reads
// per event at 2,635 bytes per read, wall 0.461 s, matching the
// documented 372,000 x 4.6 KB singular-read scale. The remote EOS
// baseline (2,000 events) runs 100.1 s with about 98.5 percent I/O wait.
//
// Synthetic (20,000 events, 2,560-byte records): baseline 0.124 s
// (3.00 reads and syscalls per event), coordinate 0.019 s,
// coordinate+map 0.004 s (zero syscalls).
//
// Boundaries: phase 1 covers fixed-width records; the store holds a
// scalar projection of the events; the documented 14-hour production
// workload is not reproduced end to end.
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
//   store_path   pre-built coordinate store file from tagma_make_store;
//                 when given, the benchmark serves the real converted
//                 records and verifies the served bytes against the
//                 sidecar checksum instead of generating a pattern store
//   perf_entries entries to read for the cache-efficiency pass; 0 skips
//                 it. The pass opens the source fresh with the
//                 TTreeCache enabled and reports the cache efficiency
//                 and miss rate (requires treeplayer)
//   analyze       1 runs the analysis workload: the same selection
//                 (MET_pt above 100 GeV and at least one muon) and the
//                 same MET_pt histogram on both read paths, over the
//                 events the store covers; requires a converted store
//                 with its layout sidecar
//
// The mapped coordinate row requires a Unix-like platform (mmap), like
// the canonical CoordSpaceM reference.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ROOT/TTagmaStore.hxx"
#include "TFile.h"
#include "TH1F.h"
#include "TStopwatch.h"
#include "TSystem.h"
#include "TTree.h"
#include "TTreeCache.h"
#include "TTreePerfStats.h"

namespace {

constexpr Long64_t kDefaultEntries = 20000;
constexpr Long64_t kDefaultRecordSize = 2560;
const char *kScatterFile = "tagma_bench_scatter.root";
const char *kStoreFile = "tagma_bench_store.bin";

struct BenchResult {
   const char *name = nullptr;
   Long64_t entries = 0;
   Double_t wall = 0;
   Double_t cpu = 0;
   Long64_t readCalls = 0;
   Long64_t tagmaReadCalls = 0;
   Long64_t sysReadCalls = 0;
   Long64_t bytesRead = 0;
   std::uint64_t servedChecksum = 0;
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

BenchResult MeasureCoordinate(const char *storePath, const char *mapPath,
                              Long64_t limit, Long64_t recordSize)
{
   BenchResult r;
   r.name = mapPath ? "coordinate+map" : "coordinate";
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
   if (mapPath && !store->MapFile(mapPath)) {
      std::fprintf(stderr, "tagma_bench: cannot map %s\n", mapPath);
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
   std::uint64_t servedChecksum = 0;
   for (Long64_t i = 0; i < limit; ++i) {
      if (tree.GetEntry(i) > 0) {
         ++served;
         const char *buf = tree.GetTagmaRecordBuffer();
         const Int_t size = tree.GetTagmaRecordSize();
         for (Int_t j = 0; j < size; ++j)
            servedChecksum += static_cast<unsigned char>(buf[j]);
      }
   }
   watch.Stop();

   r.wall = watch.RealTime();
   r.cpu = watch.CpuTime();
   r.readCalls = file->GetReadCalls() - calls0;
   r.tagmaReadCalls = file->GetTagmaReadCalls() - tagma0;
   r.sysReadCalls = file->GetSysReadCalls() - sys0;
   r.bytesRead = file->GetBytesRead() - bytes0;
   r.servedChecksum = servedChecksum;

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
       "tagma_bench: %-16s %8.3f %8.3f %7lld %7lld %8lld %12lld %7.2f "
       "%10.1f %10.2f %9.1f\n",
       r.name, r.wall, r.cpu, static_cast<long long>(r.readCalls),
       static_cast<long long>(r.tagmaReadCalls),
       static_cast<long long>(r.sysReadCalls),
       static_cast<long long>(r.bytesRead), readsPerEvent, bytesPerRead,
       syscallsPerEvent, mbs);
}

// Measures the baseline with the TTreeCache enabled on a fresh file
// open: the cache efficiency and miss rate that the M1 runbook records.
// The pass needs treeplayer (TTreePerfStats) and is skipped when the
// class is unavailable. The fresh open keeps the basket memory from the
// cache-disabled pass from hiding the cache behavior.
void MeasureCacheStats(const char *url, const char *treeName,
                       Long64_t entries)
{
   if (gROOT->GetClass("TTreePerfStats") == nullptr) {
      std::printf("tagma_bench: cache stats skipped, treeplayer not built\n");
      return;
   }
   TFile *file = TFile::Open(url);
   if (!file || file->IsZombie()) {
      std::printf("tagma_bench: cache stats skipped, cannot open %s\n", url);
      return;
   }
   TTree *tree = nullptr;
   file->GetObject(treeName, tree);
   if (!tree) {
      std::printf("tagma_bench: cache stats skipped, tree %s not found\n",
                  treeName);
      delete file;
      return;
   }
   const Long64_t total = tree->GetEntries();
   const Long64_t limit =
       (entries > 0 && entries < total) ? entries : total;

   TTreePerfStats perf("ioperf", tree);
   TStopwatch watch;
   watch.Start();
   for (Long64_t i = 0; i < limit; ++i)
      tree->GetEntry(i);
   watch.Stop();
   perf.Finish();

   auto *cache =
       dynamic_cast<TTreeCache *>(file->GetCacheRead(tree));
   const Double_t efficiency = cache ? cache->GetEfficiency() : 0;
   const Double_t disk = perf.GetDiskTime();
   const Double_t readSizeKb =
       perf.GetReadCalls() > 0
           ? 0.001 * perf.GetBytesRead() / perf.GetReadCalls()
           : 0;
   std::printf(
       "tagma_bench: cache entries=%lld read_calls=%d bytes=%lld "
       "cache_mb=%.1f efficiency=%.4f miss_rate=%.4f wall_s=%.3f "
       "cpu_s=%.3f disk_s=%.3f read_size_kb=%.1f\n",
       static_cast<long long>(limit), perf.GetReadCalls(),
       static_cast<long long>(perf.GetBytesRead()),
       1e-6 * perf.GetTreeCacheSize(), efficiency, 1 - efficiency,
       watch.RealTime(), perf.GetCpuTime(), disk, readSizeKb);
   delete file;
}

// Analysis result of one read path: the same selection applied to the
// same events, histogram of MET_pt filled identically on both paths.
struct AnalysisResult {
   const char *name = nullptr;
   Long64_t entries = 0;
   Long64_t selected = 0;
   Double_t wall = 0;
   Double_t histEntries = 0;
   Double_t histMean = 0;
};

// Parses the layout sidecar lines "<name> <offset> <type>" into a
// name-to-offset map.
bool ReadLayout(const char *path, std::map<std::string, Long64_t> *offsets)
{
   FILE *in = std::fopen(path, "r");
   if (!in)
      return false;
   char name[256];
   char type[64];
   long long offset = 0;
   while (std::fscanf(in, "%255s %lld %63s", name, &offset, type) == 3)
      (*offsets)[name] = static_cast<Long64_t>(offset);
   std::fclose(in);
   return !offsets->empty();
}

// The example analysis: select events with MET_pt above 100 GeV and at
// least one muon, fill a MET_pt histogram. The same selection runs on
// both paths; the coordinate path interprets the fixed-width record at
// the layout offsets.
constexpr Double_t kMetCut = 100.0;
constexpr Double_t kMinMuons = 1.0;

AnalysisResult AnalyzeBaseline(TFile *file, TTree *tree, Long64_t limit)
{
   AnalysisResult r;
   r.name = "analysis_baseline";
   r.entries = limit;
   tree->SetCacheSize(0);
   TLeaf *metLeaf = tree->GetLeaf("MET_pt");
   TLeaf *nmuonLeaf = tree->GetLeaf("nMuon");
   if (!metLeaf || !nmuonLeaf) {
      std::fprintf(stderr,
                   "tagma_bench: analysis skipped, MET_pt/nMuon not found\n");
      r.histEntries = -1;
      return r;
   }

   TH1F hist("met_pt", "MET_pt;MET_pt [GeV];events", 100, 0, 1000);
   TStopwatch watch;
   watch.Start();
   for (Long64_t i = 0; i < limit; ++i) {
      tree->GetEntry(i);
      const Double_t met = metLeaf->GetValue(0);
      const Double_t nMuon = nmuonLeaf->GetValue(0);
      if (met > kMetCut && nMuon >= kMinMuons) {
         ++r.selected;
         hist.Fill(met);
      }
   }
   watch.Stop();
   r.wall = watch.RealTime();
   r.histEntries = hist.GetEntries();
   r.histMean = hist.GetMean();
   return r;
}

AnalysisResult AnalyzeCoordinate(const char *storePath, Long64_t limit,
                                 Long64_t recordSize,
                                 const std::map<std::string, Long64_t> &offsets)
{
   AnalysisResult r;
   r.name = "analysis_coordinate";
   r.entries = limit;
   auto metIt = offsets.find("MET_pt");
   auto nmuonIt = offsets.find("nMuon");
   if (metIt == offsets.end() || nmuonIt == offsets.end()) {
      std::fprintf(stderr,
                   "tagma_bench: analysis skipped, MET_pt/nMuon not in "
                   "the store layout\n");
      r.histEntries = -1;
      return r;
   }
   const Long64_t metOff = metIt->second;
   const Long64_t nmuonOff = nmuonIt->second;

   TFile *file = TFile::Open(storePath);
   if (!file || file->IsZombie()) {
      std::fprintf(stderr, "tagma_bench: analysis skipped, cannot open %s\n",
                   storePath);
      r.histEntries = -1;
      return r;
   }
   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1;
   layout.fLumiMax = 1;
   layout.fEventMax = static_cast<std::uint64_t>(limit);
   layout.fRecordSize = static_cast<std::uint64_t>(recordSize);
   auto store = std::make_shared<ROOT::TTagmaStore>(layout);
   file->SetTagmaStore(store);
   TTree tree("Events", "Events");
   tree.SetDirectory(file);
   tree.SetEntries(limit);
   tree.SetTagmaStore(store);

   TH1F hist("met_pt", "MET_pt;MET_pt [GeV];events", 100, 0, 1000);
   TStopwatch watch;
   watch.Start();
   for (Long64_t i = 0; i < limit; ++i) {
      if (tree.GetEntry(i) > 0) {
         const char *buf = tree.GetTagmaRecordBuffer();
         Double_t met = 0;
         Double_t nMuon = 0;
         std::memcpy(&met, buf + metOff, sizeof(met));
         std::memcpy(&nMuon, buf + nmuonOff, sizeof(nMuon));
         if (met > kMetCut && nMuon >= kMinMuons) {
            ++r.selected;
            hist.Fill(met);
         }
      }
   }
   watch.Stop();
   r.wall = watch.RealTime();
   r.histEntries = hist.GetEntries();
   r.histMean = hist.GetMean();
   delete file;
   return r;
}

void PrintAnalysis(const AnalysisResult &r)
{
   std::printf("tagma_bench: %-20s entries=%lld selected=%lld wall_s=%.3f "
               "hist_entries=%.0f hist_mean=%.3f\n",
               r.name, static_cast<long long>(r.entries),
               static_cast<long long>(r.selected), r.wall, r.histEntries,
               r.histMean);
}

}  // namespace

int tagma_bench(const char *url = "", const char *tree_name = "Events",
                Long64_t max_entries = -1, Long64_t record_size = 0,
                Int_t nscatter = 3, Bool_t disable_cache = kTRUE,
                const char *store_path = "", Long64_t perf_entries = 0,
                Bool_t analyze = kFALSE)
{
   if (record_size <= 0)
      record_size = kDefaultRecordSize;
   if (nscatter < 1)
      nscatter = 1;

   const Bool_t synthetic = (url == nullptr || url[0] == '\0');
   const Bool_t realStore =
       !synthetic && store_path != nullptr && store_path[0] != '\0';
   const char *source = synthetic ? "<synthetic>" : url;
   const char *store = realStore ? store_path : kStoreFile;

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

   std::uint64_t expectedChecksum = 0;
   Bool_t haveChecksum = kFALSE;
   if (realStore) {
      // The converted store must hold exactly limit records of
      // record_size bytes.
      Long64_t size = 0;
      gSystem->GetPathInfo(store_path, nullptr, &size, nullptr, nullptr);
      if (size != limit * record_size) {
         std::fprintf(stderr,
                      "tagma_bench: store %s size %lld does not match "
                      "%lld records of %lld bytes\n",
                      store_path, static_cast<long long>(size),
                      static_cast<long long>(limit),
                      static_cast<long long>(record_size));
         return 1;
      }
      // The sidecar checksum from tagma_make_store, when present.
      const std::string sumPath = std::string(store_path) + ".sum";
      FILE *sum = std::fopen(sumPath.c_str(), "r");
      if (sum) {
         unsigned long long value = 0;
         if (std::fscanf(sum, "%llu", &value) == 1) {
            expectedChecksum = static_cast<std::uint64_t>(value);
            haveChecksum = kTRUE;
         }
         std::fclose(sum);
      }
   } else if (!MakeStoreFile(kStoreFile, limit, record_size)) {
      return 1;
   }

   TString storeRaw(store);
   storeRaw += "?filetype=raw";
   const BenchResult coord =
       MeasureCoordinate(storeRaw.Data(), nullptr, limit, record_size);
   const BenchResult coordMap =
       MeasureCoordinate(storeRaw.Data(), store, limit, record_size);

   std::printf("tagma_bench: source=%s\n", source);
   std::printf("tagma_bench: entries=%lld record_size=%lld nscatter=%d "
               "cache_disabled=%d store=%s\n",
               static_cast<long long>(limit),
               static_cast<long long>(record_size), nscatter,
               disable_cache ? 1 : 0, store);
   std::printf(
       "tagma_bench: %-16s %8s %8s %7s %7s %8s %12s %7s %10s %10s %9s\n",
       "path", "wall_s", "cpu_s", "reads", "tagma", "syscalls",
       "bytes_moved", "reads/ev", "bytes/read", "syscalls/ev", "MB/s");
   PrintResult(base);
   PrintResult(coord);
   PrintResult(coordMap);

   if (haveChecksum) {
      const Bool_t match = coord.servedChecksum == expectedChecksum &&
                           coordMap.servedChecksum == expectedChecksum;
      std::printf(
          "tagma_bench: served_checksum=%llu expected=%llu match=%s\n",
          static_cast<unsigned long long>(coord.servedChecksum),
          static_cast<unsigned long long>(expectedChecksum),
          match ? "yes" : "no");
      if (!match)
         return 1;
   }

   if (!realStore)
      gSystem->Unlink(kStoreFile);
   if (synthetic)
      gSystem->Unlink(kScatterFile);

   if (!coord.ok || !coordMap.ok)
      return 1;
   std::printf("tagma_bench: comparison complete\n");

   if (perf_entries > 0 && !synthetic)
      MeasureCacheStats(url, tree_name, perf_entries);

   if (analyze && realStore) {
      // The analysis workload: the same selection and histogram on both
      // read paths, over the same events the store covers.
      TFile *afile = TFile::Open(url);
      TTree *atree = nullptr;
      if (afile && !afile->IsZombie()) {
         afile->GetObject(tree_name, atree);
      }
      if (!atree) {
         std::fprintf(stderr,
                      "tagma_bench: analysis skipped, cannot open %s\n",
                      source);
         delete afile;
         return 1;
      }
      std::map<std::string, Long64_t> offsets;
      const std::string layoutPath = std::string(store_path) + ".layout";
      if (!ReadLayout(layoutPath.c_str(), &offsets)) {
         std::fprintf(stderr,
                      "tagma_bench: analysis skipped, no layout %s\n",
                      layoutPath.c_str());
         delete afile;
         return 1;
      }

      const AnalysisResult base = AnalyzeBaseline(afile, atree, limit);
      delete afile;
      TString storeRaw(store_path);
      storeRaw += "?filetype=raw";
      const AnalysisResult coord =
          AnalyzeCoordinate(storeRaw.Data(), limit, record_size, offsets);

      PrintAnalysis(base);
      PrintAnalysis(coord);
      const Bool_t match = base.histEntries >= 0 &&
                           base.histEntries == coord.histEntries &&
                           base.histMean == coord.histMean &&
                           base.selected == coord.selected;
      std::printf("tagma_bench: analysis_match=%s\n", match ? "yes" : "no");
      if (!match)
         return 1;
   }
   return 0;
}
