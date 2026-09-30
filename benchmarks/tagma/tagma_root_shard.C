// tagma_root_shard.C
//
// Splits a ROOT tree into one file per run, so the ROOT side of the dataset
// comparison holds the same partition the tagma shards hold: one file per run,
// the runs contiguous in the tree's entry order.
//
// The run ranges are found by scanning the run branch alone, with every other
// branch switched off, which is what an analysis has to do to learn the
// partition from the data. The ranges are printed, and the manifest the
// catalogue row of the comparison reads is what this tool writes beside the
// files.
//
// Usage:
//   root -l -b -q 'tagma_root_shard.C("/path/open.root", "Events", "/path/root_shards/run")'

#include "TFile.h"
#include "TLeaf.h"
#include "TStopwatch.h"
#include "TTree.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

int tagma_root_shard(const char *url, const char *tree_name = "Events", const char *outPrefix = "run")
{
   TFile *file = TFile::Open(url);
   if (file == nullptr || file->IsZombie()) {
      std::fprintf(stderr, "tagma_root_shard: cannot open %s\n", url);
      return 1;
   }
   TTree *tree = nullptr;
   file->GetObject(tree_name, tree);
   if (tree == nullptr) {
      std::fprintf(stderr, "tagma_root_shard: %s holds no %s tree\n", url, tree_name);
      delete file;
      return 1;
   }
   const Long64_t entries = tree->GetEntries();

   // The partition, from the run branch alone.
   TStopwatch scan;
   scan.Start();
   tree->SetBranchStatus("*", 0);
   tree->SetBranchStatus("run", 1);
   TLeaf *runLeaf = tree->GetLeaf("run");
   if (runLeaf == nullptr) {
      std::fprintf(stderr, "tagma_root_shard: the tree carries no run leaf\n");
      delete file;
      return 1;
   }
   std::vector<std::uint32_t> runs;
   std::vector<Long64_t> firsts;
   std::vector<Long64_t> counts;
   for (Long64_t e = 0; e < entries; ++e) {
      if (tree->GetEntry(e) <= 0)
         break;
      const std::uint32_t run = static_cast<std::uint32_t>(runLeaf->GetValue(0));
      if (runs.empty() || runs.back() != run) {
         for (std::size_t i = 0; i + 1 < runs.size(); ++i) {
            if (runs[i] == run) {
               std::fprintf(stderr, "tagma_root_shard: run %u reappears out of order\n", run);
               delete file;
               return 1;
            }
         }
         runs.push_back(run);
         firsts.push_back(e);
         counts.push_back(0);
      }
      counts.back() += 1;
   }
   tree->SetBranchStatus("*", 1);
   scan.Stop();
   std::printf("tagma_root_shard: entries=%lld runs=%zu scan_s=%.1f\n", static_cast<long long>(entries), runs.size(),
               scan.RealTime());

   // One file per run, an entry range at a time.
   std::string manifest = std::string(outPrefix) + ".manifest";
   FILE *list = std::fopen(manifest.c_str(), "w");
   if (list == nullptr) {
      std::fprintf(stderr, "tagma_root_shard: cannot write %s\n", manifest.c_str());
      delete file;
      return 1;
   }
   std::fprintf(list, "# file run first entries\n");
   TStopwatch watch;
   watch.Start();
   Long64_t copied = 0;
   for (std::size_t r = 0; r < runs.size(); ++r) {
      char name[4096];
      std::snprintf(name, sizeof(name), "%s_%zu_%u.root", outPrefix, r, runs[r]);
      TFile *out = TFile::Open(name, "RECREATE");
      if (out == nullptr || out->IsZombie()) {
         std::fprintf(stderr, "tagma_root_shard: cannot write %s\n", name);
         std::fclose(list);
         delete file;
         return 1;
      }
      TTree *clone = tree->CopyTree("", "fast", counts[r], firsts[r]);
      if (clone == nullptr)
         clone = tree->CopyTree("", "", counts[r], firsts[r]);
      if (clone == nullptr || clone->GetEntries() != counts[r]) {
         std::fprintf(stderr, "tagma_root_shard: %s holds %lld of %lld entries\n", name,
                      clone ? static_cast<long long>(clone->GetEntries()) : -1, static_cast<long long>(counts[r]));
         out->Close();
         std::fclose(list);
         delete file;
         return 1;
      }
      out->Write();
      out->Close();
      delete out;
      copied += counts[r];
      std::fprintf(list, "%s %u %lld %lld\n", name, runs[r], static_cast<long long>(firsts[r]),
                   static_cast<long long>(counts[r]));
      std::printf("tagma_root_shard: file=%s run=%u first=%lld entries=%lld\n", name, runs[r],
                  static_cast<long long>(firsts[r]), static_cast<long long>(counts[r]));
   }
   std::fclose(list);
   watch.Stop();
   std::printf("tagma_root_shard: files=%zu entries_copied=%lld wall_s=%.1f manifest=%s\n", runs.size(),
               static_cast<long long>(copied), watch.RealTime(), manifest.c_str());
   delete file;
   return copied == entries ? 0 : 1;
}
