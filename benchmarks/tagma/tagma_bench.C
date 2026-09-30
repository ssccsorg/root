// tagma_bench.C
//
// The single entry point for the Tagma benchmark set (benchmarks/tagma). It
// names every tool, carries the measured summary and the conclusion, and
// forwards the run to tagma_harness.C, the benchmark harness. The full measured
// record, row by row, with the per-run protocol and the boundaries, is the
// master block at the top of tagma_harness.C; this file is the map and the
// summary.
//
// Usage:
//   root -l -b -q 'tagma_bench.C()'                        the entry-layer run
//   root -l -b -q 'tagma_bench.C("open.root","Events",-1,2560,3,1,"plain.bin")'
// and each tool on its own:
//   root -l -b -q 'tagma_harness.C()'                      the harness
//   root -l -b -q 'tagma_block_read.C("store.bin")'        a read path alone
//   root -l -b -q 'tagma_mt.C("store.z.bin","open.root")'  thread scaling
//   root -l -b -q 'tagma_scatter.C("plain.bin","store.z.bin","open.root",2000)'
//   root -l -b -q 'tagma_compress.C("plain.bin","plain.z.bin",13)'
//   root -l -b -q 'tagma_shard.C("plain.bin","shard")'
//   root -l -b -q 'tagma_root_shard.C("open.root","Events","shard")'
//   root -l -b -q 'tagma_dataset_bench.C("shard","shard","shard.manifest")'
//   root -l -b -q 'tagma_make_store.C("open.root","Events","plain.bin",2560,-1,1)'
//   root -l -b -q 'tagma_make_uncompressed.C("open.root","uncompressed.root")'
//   root -l -b -q 'tagma_rdf_columns.C("open.root","Events",-1,"plain.bin.layout")'
//
// Tools and what each measures
// ----------------------------
//   tagma_harness.C          the harness: the entry layer over every regime it
//                            is given, baseline, coordinate, the collection
//                            store, the analysis workload, the synthetic path.
//                            Holds the full measured record.
//   tagma_block_read.C       a store's read path alone, plain or compressed,
//                            with no entry layer.
//   tagma_mt.C               thread scaling, store against baseline, with and
//                            without a per-worker cache.
//   tagma_scatter.C          sequential against event-selected access, with
//                            and without a baseline cache.
//   tagma_compress.C         writes a block-compressed store at a chosen block
//                            size.
//   tagma_make_store.C       converts a tree into a store, mode 0 fixed width
//                            or mode 1 collections.
//   tagma_shard.C            splits a store into one shard per run, each
//                            declaring its own lumi and event axes, so the
//                            dataset layer has a multi-file dataset to address
//                            and the padding a dense lattice pays is measured.
//   tagma_root_shard.C       splits the ROOT tree into one file per run and
//                            writes the manifest both sides share.
//   tagma_dataset_bench.C    the dataset rows: the stores, a chain, and the
//                            manifest over the same partition.
//   tagma_make_uncompressed.C  the uncompressed control file.
//   tagma_rdf_columns.C      RDataFrame column selection, the upstream reader
//                            the crossover is measured against.
// The library side is io/io/{inc/ROOT,src}/TTagma*.{hxx,cxx} and
// tree/tree/{inc,src}/TTree|TBranch, gated by the tagma gtests.
//
// Measured summary (full record and protocol in tagma_harness.C)
// -----------------------------------------------------------
// The CMS Run2016G DoubleMuon NanoAOD first file, 2,315,223 events, one
// machine, the medium and the layer held constant within a comparison.
//
//   regime                tool                 result
//   scan, whole event     tagma_harness.C      1.5x, the delivery layer decides
//   scan, read path       tagma_harness.C      11-14x, branches off
//   scan, index only      tagma_harness.C      42x, mapped 54x
//   scan, compressed      tagma_block_read.C   12.4 s, 572 MB/s, 2.44x smaller
//   event-selected        tagma_scatter.C      1 request/event, the baseline 683
//   threads, 8 cores      tagma_mt.C           1 request/event, linear; 11.2x
//   block size            tagma_compress.C     8 KB blocks: scatter 222 to 16 us
//   crossover             tagma_rdf_columns.C  about 42 scalar columns
//   dataset resolve       tagma_dataset_bench.C  no scan; the chain 11.1 s
//
// Conclusion
// ----------
// The store's win is structural and it lives in event-selected access. A
// coordinate resolves to a byte offset in one step, so an event costs one
// request whatever the order, while a TTree re-traverses and pays 683 requests
// per event when the order is scattered, a superlinear cliff the baseline does
// not climb with scale: 0.210 s over 500 events against 206 s over 2,000.
// Compression keeps that and shrinks the file, a cache does not rescue the
// scattered baseline, and the block size trades the scan against scatter.
//
// What is not won, and is stated as such. A sequential scan reads 11 to 14
// times faster, not the 69 times of the scalar projection, because the
// whole-event read carries the collections and the entry layer costs about 50
// microseconds per event at single-file scale; over a shard at dataset scale it
// costs 2.8 times the chain's per-event read and its per-cell cost rises with
// the walk. A narrow column selection is upstream's to win below
// about 42 scalar columns. Thread scaling narrows the lead rather than widening
// it, 14.3 to 11.2 times over eight cores. And O(128), multi-TB, and the remote
// medium need infrastructure. The 128-core row is not measured but predicted:
// the per-event request count is media-independent, one for the store against
// hundreds for the baseline, and the baseline grows superlinearly with the
// events read, so the tendency holds and only the magnitude needs the machine.
//
// Open fronts (benchmarks/tagma/FRONTS.md): a column-major layout to move the
// crossover, the delivery layer, and the claims revision.

#include "tagma_harness.C"

int tagma_bench(const char *url = "", const char *tree_name = "Events", Long64_t max_entries = -1,
                Long64_t record_size = 0, Int_t nscatter = 3, Bool_t disable_cache = kTRUE, const char *store_path = "",
                Long64_t perf_entries = 0, Bool_t analyze = kFALSE, const char *uncompressed_path = "",
                Bool_t warm_cache = kTRUE)
{
   return tagma_harness(url, tree_name, max_entries, record_size, nscatter, disable_cache, store_path, perf_entries,
                        analyze, uncompressed_path, warm_cache);
}
