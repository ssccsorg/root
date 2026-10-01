// tagma_harness.C
//
// The benchmark harness for the coordinate-indexed TTree read path. It is one
// of the benchmarks under benchmarks/tagma; the single entry point that names
// them all, carries the summary, and states the conclusion is tagma_bench.C.
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
// UPDATE THIS BLOCK AFTER EACH MEASURED RUN. The source is the per-run
// JSON under benchmarks/tagma/result/ (gitignored per-run artifact; the
// reference artifact is tracked there as reference.json). Keep it aligned
// with docs/works/cern/root-ttree/index.qmd.
//
// Workload: read every event of the CMS Run2016G DoubleMuon NanoAOD
// first file (tree Events, 2,315,223 events, 2,155,974,646 bytes), the
// M1 workload. The coordinate rows read the fixed-width store converted
// from the same events (2,560-byte records); of its 320 fields, 44 are
// scalar leaves and 276 are variable-length arrays the writer stored
// through their leading element.
//
// Protocol: the harness reads each local source through the page cache in an
// untimed pass before its timed pass (warm_cache, on by default). The rows
// below are three steady-state runs, and every range spans them. Row order
// within a run: baseline, baseline_uncomp, coordinate, coordinate+map.
//
// Full dataset, same medium (local disk), cache-disabled baseline:
//   path              wall_s        cpu_s         reads/ev  syscalls/ev  bytes/read    MB/s
//   baseline          187.0-188.2   183.6-185.9   0.20      0.20         4,573         11.5
//   baseline_uncomp    81.1-85.7     74.1-74.7   0.17      0.17        28,043        133.8
//   coordinate          2.71-2.81     2.69-2.73   1.00      1.00         2,560      2,190.1  (66.9-69.1x)
//   coordinate+map      1.55-1.61     1.54-1.58   1.00      0.00         2,560      3,819.1  (116.6-120.5x)
//   served_checksum: match (233262869086) in every run
//
// Decompression removal, baseline over baseline_uncomp: 2.20-2.31x.
// The payload-equal rows (uncompressed with the store's columns active,
// 35.935 s, 3.4 percent more bytes than the store) put the store at 13.1x,
// or 22.8x mapped. ROOT's best configuration for that payload (the same
// columns on the compressed file with a 32 MB cache, 111.111 s) puts it at
// 40.5x, or 70.5x mapped.
//
// One-time conversion of the dataset into the store: 226.3 s.
//
// Analysis workload (MET_pt above 100 GeV and at least one muon, MET_pt
// histogram), full dataset, same session as the rows above:
//   analysis_baseline    187.9 s, selected 20,861, histogram mean 132.696
//   analysis_coordinate    2.72 s, selected 20,861, histogram mean 132.696  (69.2x)
//   analysis_match: yes
// The store spends 2.715 s on those two columns against 2.698 s on the whole
// record, which is its column independence. RDataFrame on the same file, on
// a warm cache: entries only 0.030 s, one column 0.488 s, those two columns
// 0.558 s, the store's 44 scalar columns 10.849 s. ROOT's column-selective
// reader leads below about 11 columns, or about 6 with the store mapped. That
// is the store's weakest case and it is stated as such.
//
// Baseline with the TTreeCache enabled (10,000 events): 8 read calls,
// 1,943 KB per read, efficiency 0.917, miss rate 0.083. This is the
// ideal sequential case; the documented cache degradation under
// out-of-order multithreaded reads is not reproduced.
//
// M1 signature slice (2,000 events, same medium, three runs): baseline
// 1.37 reads per event at 2,635 bytes per read, wall 0.466-0.483 s,
// matching the documented 372,000 x 4.6 KB singular-read scale, and the
// coordinate walls at 0.002-0.003 s. The remote EOS baseline (2,000
// events) runs 100.1 s with about 98.8 percent I/O wait; that row was not
// re-measured. Both slice ratios sit at sub-millisecond scale and are
// indicative.
//
// Synthetic (20,000 events, 2,560-byte records): baseline 0.128-0.138 s at
// 3.00 reads and syscalls per event, coordinate 0.026-0.078 s at 1.00 and
// 1.00, coordinate+map 0.011-0.027 s at 0.00 syscalls. The spread is
// per-syscall cost at that scale, so the counts are the claim there.
//
// Collection store (mode 1, this fork's P5). The same harness against the
// self-describing store the conversion tool writes in mode 1: the whole
// event, 974 scalars and 19 collections, index record 1,280 bytes, data
// region 4,118,805,545 bytes, payload 7,082,290,985 bytes, 2,315,223
// events, one run on the same machine:
//   path              wall_s   reads        syscalls  bytes_moved     reads/ev  bytes/read  MB/s
//   baseline          176.7    470,131      470,131   2,149,961,076   0.20       4,573     12.2
//   baseline_uncomp    77.2    387,007      387,007  10,852,850,385   0.17      28,043    140.7
//   coordinate         10.2  4,630,378    4,630,378   7,082,290,985   2.00       1,530    697.2
//   coordinate+map      8.3  4,630,378            0   7,082,290,985   2.00       1,530    854.7
//
// The rows read the store's record and slice only, the shape of the
// fixed-width rows. Driving the branches a whole-event schema materializes
// adds about 50 microseconds per event: the same coordinate row with the
// branches active runs 117.6 s, and the analysis workload 123.4 s against
// the baseline's 180.3 s, so delivery, not the read path, decides that
// comparison. The store reads the whole slice whatever the selection, so it
// is column-independent: against RDataFrame on the same file with a warm
// cache, the column sum runs 0.132 s at one column, 1.716 s at eight,
// 7.411 s at 32, 16.806 s at 64 and 139.798 s at the whole 974 scalar
// columns. The store's fixed 10.2 s read path crosses that ramp near 42
// columns, against about 11 for the fixed-width store; the same first-order
// interpolation puts the whole-event row, 117.6 s, near 810 columns. Upstream
// therefore leads on any selection that reads a small fraction of the event,
// and the store leads from the whole-event read on.
//
// One-time conversion into the collection store: 1,963.3 s, against 226.3 s
// for the mode-0 projection.
//
// The same payload as a block-compressed store (tagma_compress.C, 256 KB
// blocks, zlib level 1): 7,082,290,985 to 2,897,372,833 bytes, 2.44 times, in
// 75.4 s over 27,017 blocks. The read path on its own (tagma_block_read.C, the
// whole payload through the byte source with no entry layer) is 12.4 s at
// 572 MB/s, against the plain store's 14.6 s and the baseline's 177.4 s, so
// compression keeps the read-path advantage and shrinks the file. The
// whole-event row with the branches active, 197.6 s, is the delivery layer and
// not the read path.
//
// Event-selected reads (tagma_scatter.C), the pattern the work is aimed at: the
// same events read in list order rather than a scan, 2,000 events, same medium,
// the 8 KB block store, one run. The first two store rows copy the addressing
// unit, the index record. The entry rows read the whole event through the tree
// interface, with the schema the descriptor carries and the field branches
// enabled, so they are layer-matched and payload-matched with the baseline
// rows. The entry rows are gated: before they are timed, the delivered event
// and MET_pt values are compared with the baseline file for 32 sampled
// entries, and the gate passed with no mismatch.
//   order       row                    seconds    reads       reads/event
//   sequential  store mapped             0.003    2,000       1.00
//   sequential  store block              0.005    2,000       1.00
//   sequential  store entry              0.109    4,000       2.00
//   sequential  store block entry        0.182    4,000       2.00
//   sequential  baseline                 0.453        7       0.004
//   sequential  baseline+cache           0.468       16       0.008
//   scattered   store mapped             0.000    2,000       1.00
//   scattered   store block              0.031    2,000       1.00
//   scattered   store entry              0.101    4,000       2.00
//   scattered   store block entry        0.586    4,000       2.00
//   scattered   baseline               202.209  1,365,153     682.6
//   scattered   baseline+cache         204.010       16       0.008
// The entry rows settle the claim: reading the whole event, the index record
// and the slice with the fields delivered, costs 0.101 s scattered against
// 0.109 s sequential, so the store's cost does not move with the order, while
// the baseline pays 682.6 requests per event scattered against 0.004 over a
// scan. That is 50 microseconds per event of delivery, the figure the scan rows
// measure too, and 2,000 times the baseline's 202.2 s over the same 2,000
// events, payload for payload. The compressed store, whose read decompresses
// the 8 KB block a record falls in, is at 0.586 s, 345 times. The cache is not
// the answer under scatter: a 64 MB TTreeCache collapses the read calls to 16
// and leaves the time where it was, 204.0 s against 202.2.
//
// The scatter penalty is a basket-boundary effect, not a slope. The first
// basket of the measured file covers entries 0 to 999, and a scattered list
// inside it reads like a scan, 6 requests at 500 and at 1,000 events, 0.076 to
// 0.093 s. A list that spans a second basket collapses: at 2,000 events the
// baseline issues 1,365,153 requests, because a jump between baskets
// invalidates the current basket of each of the 1,380 branches. The store's
// count is flat across the threshold, 1 or 2 reads per event, which is the
// addressing claim stated as a measurement.
//
// The compressed store paid for scatter until the block size became a parameter
// of Compress, carried in the descriptor so a reader needs no setting and
// tagma_compress.C writes a chosen size. A record read decompressed the whole
// 256 KB block it fell in, 205 times the bytes wanted; at 8 KB the scattered
// read falls to 16 microseconds per event against 222, and the file only grows
// from 2.897 to 2.990 GB, 2.44 to 2.37 times, while the scan gives up a little,
// 1.5 to 4 microseconds per event.
//
// Thread scaling (tagma_mt.C): every worker with its own file object and a
// disjoint range, the store row through the byte source and the baseline row
// through GetEntry with no branch addresses, on the same disk, one run.
//   threads  store_s  store_MB/s  base_s  base+cache_s  base/store
//   1         13.04       543      186.4     183.8        14.3
//   2          9.86       718       93.7      92.7         9.5
//   4          5.32      1331       50.3      53.7         9.4
//   8          2.86      2481       31.9      32.2        11.2
// Both scale, the store 4.6 times and the cache-less baseline 5.8 over eight
// threads, so the store's lead narrows from 14.3 to 11.2 rather than widening:
// the store is bandwidth bound and the baseline I/O bound. The cache neither
// helps nor hurts, 183.8 s against 186.4 at one thread and 32.2 against 31.9 at
// eight, because each worker reads a contiguous range and the cache is
// redundant under sequential access. This machine has ten cores, so the row
// ends at eight and O(128) is an extrapolation.
//
// Dataset level (tagma_shard.C, tagma_root_shard.C, tagma_dataset_bench.C),
// ssccs #121 step 2: whether a coordinate can address a dataset rather than one
// file. The collection store is sharded one file per run, and the same
// partition is held as ROOT files, read through a TChain, with the manifest
// both sides share. Measured on the M1 file, 38 runs, the largest run 281,515
// events, sources warmed:
//   subject   operation        seconds  note
//   store     attach            0.687   18.1 ms per file (descriptor read, parse, map)
//   chain     attach            0.002   0.05 ms per file (names; headers deferred)
//   chain     headers forced    0.355   9.3 ms per file
//   chain     select by run      11.1   no run index; the range comes from a scan of
//                                        the run branch
//   manifest  read              10.3   36 microseconds per event, one file, all branches
//   store     read path           1.6   942,333,903 bytes at 600 MB/s, no delivery
//   store     entry layer        29.0   78.9 microseconds per grid cell over 368,008
//                                        cells, 54.5 over the first 2,000
// The coordinate resolves a run to its shard with no scan, which is the dataset
// level claim. Two costs on the store side are recorded rather than argued. The
// descriptor is parsed per file and repeats the field table the shards share:
// 18.1 ms per file against the chain's forced 9.3 ms. The dense grid the
// partition lays out carries 2,862,780 cells for 2,315,223 events, 1.24 times,
// and the largest run 368,008 for 281,515, 1.31 times; the cells the events do
// not occupy, the padding, are 19.1 percent of the grid, and the walk pays them
// as well as the bytes. The entry layer is 2.8 times the chain's per-event read
// over the same run, 29.0 s against 10.3 s over 281,515 events, and its
// per-cell cost rises with the scale of the walk: 54.5 microseconds over the
// first 2,000 cells against 78.9 over 368,008, a rise this run does not
// attribute. The 50 microsecond delivery figure in this record is therefore a
// single-file figure, and that scale condition attaches to it. The dataset's
// run and lumi axes are slots while the record carries the physical values.
// Over the physical luminosity block values a dense axis would carry 14,710,139
// cells, 84.3 percent of them padding, 5.14 times the grid the slot axes lay
// out; the alternative is a block table of 2,348 entries, so where the
// slot-to-physical mapping lives is open.
//
// Boundaries: the fixed-width rows cover phase 1 and hold a scalar projection
// of the events; the collection-store rows carry the whole event and their
// advantage is on the read path, not on delivery or narrow selections; the
// scatter index rows carry the index record alone and the scatter entry rows
// carry the whole event, and every baseline row there has no branch addresses,
// so it delivers nothing into analysis variables; the scattered baseline count
// is bounded by what a workstation finishes, and the penalty behind it is the
// basket boundary measured above rather than a claim about scale; the dataset
// rows cover one partition of 38 runs, with the physical values carried in the
// record and the axes holding slots; the thread rows end at eight cores; and
// the documented 14-hour production workload, the remote medium, and O(128) are
// not reproduced end to end.
//
// Tools (benchmarks/tagma)
// -----------------------
//   tagma_bench.C            this harness, the entry layer over every regime
//                            it is given; the master record is above
//   tagma_block_read.C       a store's read path on its own, plain or
//                            block-compressed, no entry layer
//   tagma_mt.C               thread scaling, store against baseline, with and
//                            without a per-worker cache
//   tagma_scatter.C          sequential against event-selected access, with
//                            and without a baseline cache, over the index
//                            record and over the entry layer, the entry rows
//                            gated against the baseline's values
//   tagma_compress.C         writes a block-compressed store at a chosen
//                            block size.
//   tagma_shard.C            splits the collection store into one shard per run,
//                            each declaring its lumi and event axes.
//   tagma_root_shard.C       splits the ROOT tree into one file per run and
//                            writes the manifest both sides share.
//   tagma_dataset_bench.C    the dataset rows: the stores, a chain, and the
//                            manifest over the same partition.
//   tagma_make_store.C       converts a tree into a store, mode 0 fixed width
//                            or mode 1 collections
//   tagma_make_uncompressed.C  the uncompressed control file
//   tagma_rdf_columns.C      RDataFrame column selection, the upstream reader
//                            the crossover is measured against
// The library side is io/io/{inc/ROOT,src}/TTagma*.{hxx,cxx} and
// tree/tree/{inc,src}/TTree|TBranch, gated by the tagma gtests.
//
// Usage:
//   root -l -b -q 'tagma_bench.C()'
//   root -l -b -q
//   'tagma_bench.C("root://eospublic.cern.ch//eos/opendata/cms/Run2016G/DoubleMuon/NANOAOD/UL2016_MiniAODv2_NanoAODv9-v2/2430000/05DD095C-F6C3-9A4F-9FB3-348A5A6403D5.root",
//   "Events", -1, 2560)' root -l -b -q 'tagma_bench.C("/path/to/local.root", "Events", 2000, 2560, 3, 1)'
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
//                 sidecar checksum instead of generating a pattern store.
//                 A store written in mode 1 carries its own descriptor,
//                 which the harness reads back to address the index and
//                 data regions and to read the collections through the
//                 leaves, so it needs no sidecar
//   perf_entries entries to read for the cache-efficiency pass; 0 skips
//                 it. The pass opens the source fresh with the
//                 TTreeCache enabled and reports the cache efficiency
//                 and miss rate (requires treeplayer)
//   analyze       1 runs the analysis workload: the same selection
//                 (MET_pt above 100 GeV and at least one muon) and the
//                 same MET_pt histogram on both read paths, over the
//                 events the store covers; requires a converted store
//                 with its layout sidecar
//   warm_cache   1 reads each local source through the page cache before
//                 its timed pass, so every row measures the read path
//                 against resident data (default). 0 measures the medium
//                 instead, in which case the row order matters: a pass
//                 over the uncompressed rewrite evicts the store.
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

#include "ROOT/TTagmaSchema.hxx"
#include "ROOT/TTagmaStore.hxx"
#include "ROOT/TTagmaBlockSource.hxx"
#include "ROOT/TTagmaWriter.hxx"
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
const char *kScatterUncompressedFile = "tagma_bench_scatter_uncompressed.root";
const char *kStoreFile = "tagma_bench_store.bin";

// Populate the page cache for a local source, so the timed pass measures the
// read path against resident data instead of against the medium. A remote URL
// is left alone: its access cost is the subject of the remote row, and a warm
// pass would move the whole file over the network.
void WarmFile(const char *path)
{
   if (path == nullptr || path[0] == '\0')
      return;
   if (std::strstr(path, "://") != nullptr)
      return;
   FILE *in = std::fopen(path, "rb");
   if (in == nullptr)
      return;
   constexpr std::size_t kChunk = 4u << 20;
   std::vector<unsigned char> buffer(kChunk);
   std::uint64_t total = 0;
   std::size_t got = 0;
   while ((got = std::fread(buffer.data(), 1, kChunk, in)) == kChunk)
      total += got;
   total += got;
   std::fclose(in);
   std::fprintf(stderr, "tagma_bench: warmed %s (%llu bytes)\n", path, static_cast<unsigned long long>(total));
}

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
bool MakeScatteredTree(const char *path, Long64_t entries, Int_t nscatter, Long64_t recordSize,
                       Bool_t uncompressed = kFALSE)
{
   TFile file(path, "RECREATE");
   if (file.IsZombie()) {
      std::fprintf(stderr, "tagma_bench: cannot create %s\n", path);
      return false;
   }
   // The compression control writes the same tree with compression
   // disabled, so the pair holds the read path and the payload constant
   // and varies only the compression.
   if (uncompressed)
      file.SetCompressionLevel(0);
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

// A store that carries its own descriptor, read back with the writer's
// reader: the layout and the schema the writer laid out. A store without
// one is addressed as fixed-width records with a layout the harness
// synthesizes, the projection the earlier conversion writes.
struct StoreShape {
   ROOT::TTagmaStore::Layout layout;
   ROOT::TTagmaSchema schema;
   Bool_t described = kFALSE;
   // A block-compressed store is read through its own source, which the harness
   // installs in place of the one the hook builds from the mapping.
   std::shared_ptr<ROOT::TTagmaSource> source;
};

StoreShape ReadStoreShape(const char *path)
{
   StoreShape shape;
   std::string why;
   if (ROOT::TTagmaWriter::ReadStore(path, &shape.layout, &shape.schema, &why)) {
      shape.described = kTRUE;
      return shape;
   }
   auto block = std::make_shared<ROOT::TTagmaBlockSource>();
   if (block->Open(path, &why)) {
      shape.described = kTRUE;
      shape.layout = block->GetLayout();
      shape.schema = block->GetSchema();
      shape.source = std::make_shared<ROOT::TTagmaCachedSource>(block, block->BlockBytes(), 16, block->PayloadBytes());
   }
   return shape;
}

BenchResult MeasureCoordinate(const char *storePath, const char *mapPath, Long64_t limit, Long64_t recordSize,
                              const StoreShape *shape, Bool_t allFields = kFALSE)
{
   BenchResult r;
   if (shape != nullptr && shape->described)
      r.name =
         mapPath ? (allFields ? "coordinate+map" : "coord+map-scal") : (allFields ? "coordinate" : "coordinate-scal");
   else
      r.name = mapPath ? "coordinate+map" : "coordinate";
   r.entries = limit;

   TFile *file = TFile::Open(storePath);
   if (!file || file->IsZombie()) {
      std::fprintf(stderr, "tagma_bench: cannot open %s\n", storePath);
      r.ok = kFALSE;
      return r;
   }

   ROOT::TTagmaStore::Layout layout;
   if (shape != nullptr && shape->described) {
      layout = shape->layout;
   } else {
      layout.fRunMax = 1;
      layout.fLumiMax = 1;
      layout.fEventMax = static_cast<std::uint64_t>(limit);
      layout.fRecordSize = static_cast<std::uint64_t>(recordSize);
   }
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
   // The byte source serves the store's record requests, and the entry
   // layer resolves each event through the same store.
   file->SetTagmaStore(store);
   if (shape != nullptr && shape->source)
      file->SetTagmaSource(shape->source);

   TTree tree("Events", "Events");
   tree.SetDirectory(file);
   tree.SetEntries(limit);
   tree.SetTagmaStore(store);
   if (shape != nullptr && shape->described) {
      // The descriptor's schema resolves the collections as well as the
      // scalars, so the entry read serves the event's data slice after the
      // index record, the two reads the leaf gate measures.
      if (!tree.SetTagmaSchema(shape->schema)) {
         std::fprintf(stderr, "tagma_bench: cannot attach the store's schema\n");
         delete file;
         r.ok = kFALSE;
         return r;
      }
      // With the collection branches enabled the read serves the index record
      // and the event's slice, the whole event; with them disabled the read
      // stays in the index record, the column pruning the packed layout
      // admits. The analysis row reads the fields and covers the delivery.
      if (!allFields)
         tree.SetBranchStatus("*", 0);
   }

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

AnalysisResult AnalyzeCoordinate(const char *storePath, Long64_t limit, Long64_t recordSize,
                                 const std::map<std::string, Long64_t> &offsets, const StoreShape *shape)
{
   AnalysisResult r;
   r.name = "analysis_coordinate";
   r.entries = limit;
   const Bool_t described = (shape != nullptr && shape->described);
   const Bool_t haveOffsets = !described && offsets.count("MET_pt") != 0 && offsets.count("nMuon") != 0;
   if (!described && !haveOffsets) {
      std::fprintf(stderr,
                   "tagma_bench: analysis skipped, MET_pt/nMuon not in "
                   "the store layout\n");
      r.histEntries = -1;
      return r;
   }
   const Long64_t metOff = haveOffsets ? offsets.at("MET_pt") : 0;
   const Long64_t nmuonOff = haveOffsets ? offsets.at("nMuon") : 0;

   TFile *file = TFile::Open(storePath);
   if (!file || file->IsZombie()) {
      std::fprintf(stderr, "tagma_bench: analysis skipped, cannot open %s\n",
                   storePath);
      r.histEntries = -1;
      return r;
   }
   ROOT::TTagmaStore::Layout layout;
   if (described) {
      layout = shape->layout;
   } else {
      layout.fRunMax = 1;
      layout.fLumiMax = 1;
      layout.fEventMax = static_cast<std::uint64_t>(limit);
      layout.fRecordSize = static_cast<std::uint64_t>(recordSize);
   }
   auto store = std::make_shared<ROOT::TTagmaStore>(layout);
   file->SetTagmaStore(store);
   if (shape != nullptr && shape->source)
      file->SetTagmaSource(shape->source);
   TTree tree("Events", "Events");
   tree.SetDirectory(file);
   tree.SetEntries(limit);
   tree.SetTagmaStore(store);
   TLeaf *metLeaf = nullptr;
   TLeaf *nmuonLeaf = nullptr;
   if (described) {
      // Read the two selected scalars through the leaves, the ordinary
      // branch interface a store-backed analysis uses.
      if (!tree.SetTagmaSchema(shape->schema)) {
         std::fprintf(stderr, "tagma_bench: analysis skipped, cannot attach the "
                              "store's schema\n");
         delete file;
         r.histEntries = -1;
         return r;
      }
      metLeaf = tree.GetLeaf("MET_pt");
      nmuonLeaf = tree.GetLeaf("nMuon");
   }

   TH1F hist("met_pt", "MET_pt;MET_pt [GeV];events", 100, 0, 1000);
   TStopwatch watch;
   watch.Start();
   for (Long64_t i = 0; i < limit; ++i) {
      if (tree.GetEntry(i) > 0) {
         Double_t met = 0;
         Double_t nMuon = 0;
         if (described) {
            met = metLeaf ? metLeaf->GetValue(0) : 0;
            nMuon = nmuonLeaf ? nmuonLeaf->GetValue(0) : 0;
         } else {
            const char *buf = tree.GetTagmaRecordBuffer();
            std::memcpy(&met, buf + metOff, sizeof(met));
            std::memcpy(&nMuon, buf + nmuonOff, sizeof(nMuon));
         }
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

int tagma_harness(const char *url = "", const char *tree_name = "Events", Long64_t max_entries = -1,
                Long64_t record_size = 0, Int_t nscatter = 3, Bool_t disable_cache = kTRUE, const char *store_path = "",
                Long64_t perf_entries = 0, Bool_t analyze = kFALSE, const char *uncompressed_path = "",
                Bool_t warm_cache = kTRUE)
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

   // A self-describing store carries its layout and schema in a trailing
   // descriptor, so the harness reads them back instead of assuming
   // fixed-width records with a sidecar layout.
   StoreShape shape;
   if (realStore)
      shape = ReadStoreShape(store_path);

   TFile *baselineFile = nullptr;
   TTree *baselineTree = nullptr;
   Long64_t total = 0;

   if (synthetic) {
      if (!MakeScatteredTree(kScatterFile, kDefaultEntries, nscatter,
                             record_size))
         return 1;
      if (!MakeScatteredTree(kScatterUncompressedFile, kDefaultEntries, nscatter, record_size, kTRUE))
         return 1;
      baselineFile = TFile::Open(kScatterFile);
   } else {
      if (warm_cache)
         WarmFile(url);
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
      for (Int_t b = 0; b < nscatter; ++b)
         addresses[b].assign(sizes[b], 0);
   }
   auto BindPayload = [&](TTree *tree) {
      for (Int_t b = 0; b < nscatter; ++b)
         tree->SetBranchAddress(Form("b%02d", b), addresses[b].data());
   };
   if (synthetic)
      BindPayload(baselineTree);

   const BenchResult base = MeasureBaseline(baselineFile, baselineTree, limit,
                                            disable_cache,
                                            synthetic ? &addresses : nullptr);
   delete baselineFile;

   // The compression control: the same tree written with compression
   // disabled and read through the ordinary path. The row holds the read
   // path and the payload constant, so the gap to the baseline is the
   // decompression component of the measured ratio.
   BenchResult baseUncompressed;
   Bool_t haveUncompressed = kFALSE;
   const char *uncompressedSource = synthetic ? kScatterUncompressedFile : uncompressed_path;
   if (uncompressedSource != nullptr && uncompressedSource[0] != '\0') {
      if (warm_cache)
         WarmFile(uncompressedSource);
      TFile *ufile = TFile::Open(uncompressedSource);
      TTree *utree = nullptr;
      if (ufile && !ufile->IsZombie())
         ufile->GetObject(tree_name, utree);
      if (!utree) {
         std::fprintf(stderr,
                      "tagma_bench: uncompressed control skipped, cannot read "
                      "tree %s from %s\n",
                      tree_name, uncompressedSource);
      } else if (utree->GetEntries() < limit) {
         std::fprintf(stderr,
                      "tagma_bench: uncompressed control skipped, %s holds "
                      "%lld of %lld entries\n",
                      uncompressedSource, static_cast<long long>(utree->GetEntries()), static_cast<long long>(limit));
      } else {
         if (synthetic)
            BindPayload(utree);
         baseUncompressed = MeasureBaseline(ufile, utree, limit, disable_cache, synthetic ? &addresses : nullptr);
         baseUncompressed.name = "baseline_uncomp";
         haveUncompressed = kTRUE;
      }
      delete ufile;
   }

   std::uint64_t expectedChecksum = 0;
   Bool_t haveChecksum = kFALSE;
   if (realStore && !shape.described) {
      // The converted fixed-width store must hold exactly limit records of
      // record_size bytes; a self-describing store is checked against its
      // descriptor by ReadStore above.
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
   } else if (!realStore && !MakeStoreFile(kStoreFile, limit, record_size)) {
      return 1;
   }

   if (warm_cache)
      WarmFile(store);
   TString storeRaw(store);
   storeRaw += "?filetype=raw";
   const BenchResult coord = MeasureCoordinate(storeRaw.Data(), nullptr, limit, record_size, &shape, kTRUE);
   const BenchResult coordMap = MeasureCoordinate(storeRaw.Data(), store, limit, record_size, &shape, kTRUE);

   std::printf("tagma_bench: source=%s\n", source);
   const Long64_t reportedRecord = shape.described ? static_cast<Long64_t>(shape.layout.fRecordSize) : record_size;
   std::printf("tagma_bench: entries=%lld record_size=%lld nscatter=%d "
               "cache_disabled=%d store=%s\n",
               static_cast<long long>(limit), static_cast<long long>(reportedRecord), nscatter, disable_cache ? 1 : 0,
               store);
   if (shape.described)
      std::printf("tagma_bench: store self-describing data_size=%llu\n",
                  static_cast<unsigned long long>(shape.layout.fDataSize));
   std::printf(
       "tagma_bench: %-16s %8s %8s %7s %7s %8s %12s %7s %10s %10s %9s\n",
       "path", "wall_s", "cpu_s", "reads", "tagma", "syscalls",
       "bytes_moved", "reads/ev", "bytes/read", "syscalls/ev", "MB/s");
   PrintResult(base);
   if (haveUncompressed)
      PrintResult(baseUncompressed);
   PrintResult(coord);
   PrintResult(coordMap);
   if (shape.described) {
      // The scalar-only read: the entry is served from the index record and
      // the data region stays unread, the pruning the collection addressing
      // admits.
      PrintResult(MeasureCoordinate(storeRaw.Data(), nullptr, limit, record_size, &shape, kFALSE));
      PrintResult(MeasureCoordinate(storeRaw.Data(), store, limit, record_size, &shape, kFALSE));
   }

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
   if (synthetic) {
      gSystem->Unlink(kScatterFile);
      gSystem->Unlink(kScatterUncompressedFile);
   }

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
      if (!shape.described) {
         const std::string layoutPath = std::string(store_path) + ".layout";
         if (!ReadLayout(layoutPath.c_str(), &offsets)) {
            std::fprintf(stderr, "tagma_bench: analysis skipped, no layout %s\n", layoutPath.c_str());
            delete afile;
            return 1;
         }
      }

      const AnalysisResult base = AnalyzeBaseline(afile, atree, limit);
      delete afile;
      TString storeRaw(store_path);
      storeRaw += "?filetype=raw";
      const AnalysisResult coord = AnalyzeCoordinate(storeRaw.Data(), limit, record_size, offsets, &shape);

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
