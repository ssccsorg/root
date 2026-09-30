// tagma_dataset_bench.C
//
// The dataset comparison of ssccs #121 step 2: the same partition, one file per
// run, held three ways, and the same questions asked of each.
//
//   the tagma shards    one coordinate store per run, from tagma_shard.C, each
//                       carrying its own lumi and event axes
//   the ROOT shards     one ROOT file per run, from tagma_root_shard.C, the
//                       same entry ranges, with the manifest both sides share
//   the manifest        file, run, first, entries: the catalogue a production
//                       analysis keeps beside its files
//
// Rows. Attach: what it costs to make the dataset addressable, which for the
// chain is a header read per file and for the store is a descriptor read and a
// mapping per file. Select: the largest run's events, asked for by run number.
// On the chain side the run number is not in the chain's index, so the row
// scans the run branch to find the range, which is what an analysis has to do
// today; the manifest row is the same read with the range declared. On the
// store side the coordinate resolves to a shard and a record offset with no
// scan, and the row is measured twice: the payload through the mapping alone,
// and the whole event with the schema attached and the branches delivered,
// which is the form the chain rows are in.
//
// Usage:
//   root -l -b -q 'tagma_dataset_bench.C("/path/shards/run", "/path/root_shards/run",
//   "/path/root_shards/run.manifest")'

#include "ROOT/TTagmaDataset.hxx"
#include "ROOT/TTagmaSchema.hxx"
#include "ROOT/TTagmaStore.hxx"
#include "ROOT/TTagmaWriter.hxx"

#include "TChain.h"
#include "TFile.h"
#include "TLeaf.h"
#include "TStopwatch.h"
#include "TTree.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Shard {
   std::string fRoot;  // the ROOT file of the run
   std::string fTagma; // the coordinate store of the run
   std::uint64_t fRun = 0;
   Long64_t fFirst = 0;
   Long64_t fEntries = 0;
};

double Seconds(const std::chrono::steady_clock::time_point &from)
{
   return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
}

// Reads a source through the page cache in an untimed pass, so the timed rows
// measure the read path against resident data rather than against the medium,
// which is the protocol every other tool in this set uses.
void WarmFile(const char *path)
{
   if (path == nullptr || path[0] == '\0')
      return;
   FILE *in = std::fopen(path, "rb");
   if (in == nullptr)
      return;
   std::vector<unsigned char> buffer(4u << 20);
   while (std::fread(buffer.data(), 1, buffer.size(), in) == buffer.size()) {
   }
   std::fclose(in);
}

} // namespace

int tagma_dataset_bench(const char *shardPrefix, const char *rootPrefix, const char *manifestPath)
{
   if (shardPrefix == nullptr || rootPrefix == nullptr || manifestPath == nullptr) {
      std::fprintf(stderr, "tagma_dataset_bench: a shard prefix, a root prefix, and a manifest are required\n");
      return 1;
   }

   // The tagma shards carry the same slot order as the manifest, so the paths
   // are derived from the runs the manifest names.
   std::vector<Shard> shards;
   {
      const auto start = std::chrono::steady_clock::now();
      std::vector<std::string> placeholder; // filled below, so the parse cost is separated
      std::ifstream in(manifestPath);
      if (!in) {
         std::fprintf(stderr, "tagma_dataset_bench: cannot read %s\n", manifestPath);
         return 1;
      }
      std::string line;
      std::size_t slot = 0;
      while (std::getline(in, line)) {
         if (line.empty() || line[0] == '#')
            continue;
         Shard shard;
         std::istringstream probe(line);
         if (!(probe >> shard.fRoot >> shard.fRun >> shard.fFirst >> shard.fEntries))
            return 1;
         char path[4096];
         std::snprintf(path, sizeof(path), "%s_%zu_%llu.tagma", shardPrefix, slot,
                       static_cast<unsigned long long>(shard.fRun));
         shard.fTagma = path;
         shards.push_back(shard);
         ++slot;
      }
      const double parse = Seconds(start);
      std::printf("tagma_dataset_bench: manifest=%s shards=%zu parse_s=%.6f entries=%lld\n", manifestPath,
                  shards.size(), parse, static_cast<long long>(shards.empty() ? 0 : shards.back().fEntries));
   }

   // The largest run is the case the comparison is read from.
   std::size_t target = 0;
   for (std::size_t i = 1; i < shards.size(); ++i)
      if (shards[i].fEntries > shards[target].fEntries)
         target = i;
   std::printf("tagma_dataset_bench: target slot=%zu run=%llu entries=%lld\n", target,
               static_cast<unsigned long long>(shards[target].fRun), static_cast<long long>(shards[target].fEntries));

   // Attach: the store's descriptors against the chain's headers, both over the
   // whole dataset.
   {
      const auto start = std::chrono::steady_clock::now();
      ROOT::TTagmaDataset dataset;
      for (std::size_t i = 0; i < shards.size(); ++i)
         dataset.AddFile(shards[i].fTagma, i, 0, true);
      const double seconds = Seconds(start);
      std::printf("tagma_dataset_bench: attach store    %.6f s  %.3f ms per file  files=%zu\n", seconds,
                  1000.0 * seconds / shards.size(), shards.size());
   }
   {
      const auto start = std::chrono::steady_clock::now();
      TChain chain("Events");
      for (const Shard &shard : shards)
         chain.Add(shard.fRoot.c_str());
      const double seconds = Seconds(start);
      std::printf("tagma_dataset_bench: attach chain    %.6f s  %.3f ms per file  files=%zu entries=%lld\n", seconds,
                  1000.0 * seconds / shards.size(), shards.size(), static_cast<long long>(chain.GetEntries()));
      // The chain records the names and defers the headers, so the cost of
      // making every file addressable is paid here: loading the tree of each
      // file is what the store's AddFile pays up front.
      const auto forceStart = std::chrono::steady_clock::now();
      Long64_t at = 0;
      for (const Shard &s : shards) {
         chain.LoadTree(at);
         at += s.fEntries;
      }
      const double forced = Seconds(forceStart);
      std::printf("tagma_dataset_bench: chain headers  %.6f s  %.3f ms per file  (deferred until now)\n", forced,
                  1000.0 * forced / shards.size());
   }

   // Select: the largest run's events, by run number. Both sources are warmed
   // first, so the rows measure the read path and not the medium.
   const Shard &shard = shards[target];
   WarmFile(shard.fTagma.c_str());
   WarmFile(shard.fRoot.c_str());

   // The chain row: the run number is not in the chain's index, so the range is
   // found by scanning the run branch over the whole dataset.
   {
      TChain chain("Events");
      for (const Shard &s : shards)
         chain.Add(s.fRoot.c_str());
      chain.SetBranchStatus("*", 0);
      chain.SetBranchStatus("run", 1);
      const auto start = std::chrono::steady_clock::now();
      Long64_t first = -1;
      Long64_t count = 0;
      for (Long64_t e = 0; e < chain.GetEntries(); ++e) {
         chain.GetEntry(e);
         const auto run = static_cast<std::uint64_t>(chain.GetLeaf("run")->GetValue(0));
         if (run == shard.fRun) {
            if (first < 0)
               first = e;
            ++count;
         } else if (first >= 0) {
            break;
         }
      }
      const double scan = Seconds(start);
      chain.SetBranchStatus("*", 1);
      const auto readStart = std::chrono::steady_clock::now();
      for (Long64_t i = first; i < first + count; ++i)
         chain.GetEntry(i);
      const double read = Seconds(readStart);
      std::printf("tagma_dataset_bench: chain scan+read %.6f s  scan=%.6f read=%.6f  found=%lld entries=%lld\n",
                  scan + read, scan, read, static_cast<long long>(first), static_cast<long long>(count));
   }

   // The manifest row: the range is declared, so the read opens one file.
   {
      TFile *file = TFile::Open(shard.fRoot.c_str());
      if (file == nullptr || file->IsZombie()) {
         std::fprintf(stderr, "tagma_dataset_bench: cannot open %s\n", shard.fRoot.c_str());
         return 1;
      }
      TTree *tree = nullptr;
      file->GetObject("Events", tree);
      const Int_t reads0 = file->GetReadCalls();
      const auto start = std::chrono::steady_clock::now();
      for (Long64_t i = 0; i < shard.fEntries; ++i)
         tree->GetEntry(i);
      const double seconds = Seconds(start);
      std::printf("tagma_dataset_bench: manifest read   %.6f s  entries=%lld reads=%d  MB/s=%.1f  us per event=%.1f\n",
                  seconds, static_cast<long long>(shard.fEntries), file->GetReadCalls() - reads0,
                  seconds > 0 ? file->GetBytesRead() / 1e6 / seconds : 0.0,
                  seconds > 0 ? 1e6 * seconds / static_cast<double>(shard.fEntries) : 0.0);
      file->Close();
      delete file;
   }

   // The store rows: the payload through the mapping, then the whole event with
   // the schema attached, the layer the chain rows are in.
   {
      ROOT::TTagmaStore::Layout layout;
      ROOT::TTagmaSchema schema;
      std::string why;
      if (!ROOT::TTagmaWriter::ReadStore(shard.fTagma.c_str(), &layout, &schema, &why)) {
         std::fprintf(stderr, "tagma_dataset_bench: %s: %s\n", shard.fTagma.c_str(), why.c_str());
         return 1;
      }
      auto mapped = std::make_shared<ROOT::TTagmaStore>(layout);
      if (!mapped->MapFile(shard.fTagma.c_str())) {
         std::fprintf(stderr, "tagma_dataset_bench: cannot map %s\n", shard.fTagma.c_str());
         return 1;
      }
      const std::uint64_t size = mapped->SizeBytes();
      const char *base = mapped->GetMapped();
      std::vector<char> buffer(1u << 22);
      const auto start = std::chrono::steady_clock::now();
      for (std::uint64_t at = 0; at < size;) {
         const std::uint64_t take = std::min<std::uint64_t>(buffer.size(), size - at);
         std::memcpy(buffer.data(), base + at, static_cast<std::size_t>(take));
         at += take;
      }
      const double seconds = Seconds(start);
      std::printf("tagma_dataset_bench: store mapped    %.6f s  bytes=%llu  MB/s=%.1f  (read path, no delivery)\n",
                  seconds, static_cast<unsigned long long>(size), seconds > 0 ? size / 1e6 / seconds : 0.0);

      const std::string raw = shard.fTagma + "?filetype=raw";
      TFile *file = TFile::Open(raw.c_str());
      TTree tree("Events", "Events");
      if (file == nullptr || file->IsZombie()) {
         std::fprintf(stderr, "tagma_dataset_bench: cannot open %s as raw\n", shard.fTagma.c_str());
         return 1;
      }
      file->SetTagmaStore(mapped);
      tree.SetDirectory(file);
      tree.SetEntries(mapped->RecordCount());
      tree.SetTagmaStore(mapped);
      if (!tree.SetTagmaSchema(schema)) {
         std::fprintf(stderr, "tagma_dataset_bench: cannot attach the schema of %s\n", shard.fTagma.c_str());
         delete file;
         return 1;
      }
      const auto entryStart = std::chrono::steady_clock::now();
      Long64_t served = 0;
      for (std::uint64_t i = 0; i < mapped->RecordCount(); ++i)
         if (tree.GetEntry(static_cast<Long64_t>(i)) > 0)
            ++served;
      const double entrySeconds = Seconds(entryStart);
      std::printf("tagma_dataset_bench: store entry     %.6f s  records=%llu served=%lld  us per grid cell=%.1f  "
                  "us per event=%.1f\n",
                  entrySeconds, static_cast<unsigned long long>(mapped->RecordCount()), static_cast<long long>(served),
                  served > 0 ? 1e6 * entrySeconds / served : 0.0,
                  served > 0 ? 1e6 * entrySeconds / static_cast<double>(shard.fEntries) : 0.0);
      // The same row over the first 2,000 cells, which is the scale the scatter
      // tool measured on the single store, so the two can be compared.
      const auto smallStart = std::chrono::steady_clock::now();
      Long64_t smallServed = 0;
      for (std::uint64_t i = 0; i < 2000 && i < mapped->RecordCount(); ++i)
         if (tree.GetEntry(static_cast<Long64_t>(i)) > 0)
            ++smallServed;
      const double smallSeconds = Seconds(smallStart);
      std::printf("tagma_dataset_bench: store entry 2k  %.6f s  served=%lld  us per grid cell=%.1f\n", smallSeconds,
                  static_cast<long long>(smallServed), smallServed > 0 ? 1e6 * smallSeconds / smallServed : 0.0);
      file->Close();
      delete file;
   }
   return 0;
}
