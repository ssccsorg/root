// tagma_make_uncompressed.C
//
// Preparation tool for the read-path control experiment (ssccs #103):
// rewrites a dataset into a second ROOT file holding the same tree with
// compression disabled. The benchmark reads both files through the same
// ordinary path, so the pair holds the read path and the payload constant
// and separates decompression removal from the addressing change.
//
// The rewrite is not a byte copy. CloneTree without the "fast" option
// unzips each basket and re-streams it at the destination file's
// compression level, which is zero. Branch definitions, entry counts, and
// basket sizes are preserved; the on-disk basket grouping can differ from
// the source.
//
// Usage:
//   root -l -b -q 'tagma_make_uncompressed.C("/path/to/local.root", "Events", "tagma_uncompressed.root")'
//
// Arguments:
//   url           dataset source (local path or root:// URL)
//   tree_name     tree to rewrite
//   out_path      uncompressed ROOT file to write
//   max_entries   events to rewrite; -1 rewrites all

#include <cstdio>

#include "TFile.h"
#include "TStopwatch.h"
#include "TTree.h"

int tagma_make_uncompressed(const char *url, const char *tree_name = "Events",
                            const char *out_path = "tagma_uncompressed.root", Long64_t max_entries = -1)
{
   TFile *in = TFile::Open(url);
   if (!in || in->IsZombie()) {
      std::fprintf(stderr, "tagma_make_uncompressed: cannot open %s\n", url);
      return 1;
   }
   TTree *tree = nullptr;
   in->GetObject(tree_name, tree);
   if (!tree) {
      std::fprintf(stderr, "tagma_make_uncompressed: tree %s not found in %s\n", tree_name, url);
      delete in;
      return 1;
   }
   const Long64_t total = tree->GetEntries();
   const Long64_t limit = (max_entries > 0 && max_entries < total) ? max_entries : total;
   if (limit <= 0) {
      std::fprintf(stderr, "tagma_make_uncompressed: nothing to rewrite in %s\n", url);
      delete in;
      return 1;
   }

   TFile *out = TFile::Open(out_path, "RECREATE");
   if (!out || out->IsZombie()) {
      std::fprintf(stderr, "tagma_make_uncompressed: cannot create %s\n", out_path);
      delete in;
      delete out;
      return 1;
   }
   out->SetCompressionLevel(0);

   TStopwatch watch;
   watch.Start();
   // Without "fast", every basket is unzipped and re-streamed at the
   // destination compression level. The "fast" option would copy the
   // stored bytes verbatim and keep the source compression.
   TTree *clone = tree->CloneTree(limit);
   if (!clone) {
      std::fprintf(stderr, "tagma_make_uncompressed: clone failed for %s\n", url);
      delete in;
      delete out;
      return 1;
   }
   out->Write();
   watch.Stop();

   const Long64_t written = clone->GetEntries();
   Long64_t outSize = 0;
   gSystem->GetPathInfo(out_path, nullptr, &outSize, nullptr, nullptr);
   std::printf("tagma_make_uncompressed: source=%s\n", url);
   std::printf("tagma_make_uncompressed: out=%s entries=%lld branches=%lld "
               "file_size=%lld compression=%d\n",
               out_path, static_cast<long long>(written),
               static_cast<long long>(clone->GetListOfBranches()->GetEntries()), static_cast<long long>(outSize),
               out->GetCompressionLevel());
   std::printf("tagma_make_uncompressed: wall_s=%.3f cpu_s=%.3f\n", watch.RealTime(), watch.CpuTime());
   if (written != limit) {
      std::fprintf(stderr, "tagma_make_uncompressed: wrote %lld of %lld entries\n", static_cast<long long>(written),
                   static_cast<long long>(limit));
      delete out;
      delete in;
      return 1;
   }
   std::printf("tagma_make_uncompressed: conversion complete\n");
   delete out;
   delete in;
   return 0;
}
