// tagma_make_store.C
//
// Preparation tool for the coordinate read-path benchmark (ssccs #103):
// converts a real dataset tree into a fixed-width coordinate store file.
// Each event becomes one record of record_size bytes holding the first
// (record_size / 8) scalar leaf values of the event, serialized as
// doubles in leaf order and zero-padded. The conversion is a one-time
// preparation step; its cost is reported separately from the read-path
// measurement.
//
// The sidecar file <out_path>.sum holds the 64-bit sum of every record
// byte. tagma_bench reads it and verifies the checksum against the bytes
// the coordinate read path serves, so the benchmark runs against real
// converted data, not a pattern.
//
// Usage:
//   root -l -b -q 'tagma_make_store.C("root://eospublic.cern.ch//eos/opendata/cms/Run2016G/DoubleMuon/NANOAOD/UL2016_MiniAODv2_NanoAODv9-v2/2430000/05DD095C-F6C3-9A4F-9FB3-348A5A6403D5.root", "Events", "tagma_store.bin", 2560)'
//   root -l -b -q 'tagma_make_store.C("/path/to/local.root", "Events", "tagma_store.bin", 2560, 200000)'
//
// Arguments:
//   url           dataset source (local path or root:// URL)
//   tree_name     tree to convert
//   out_path      coordinate store file to write
//   record_size   fixed-width record bytes per event, a multiple of 8
//   max_entries   events to convert; -1 converts all

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "TFile.h"
#include "TLeaf.h"
#include "TStopwatch.h"
#include "TTree.h"

namespace {

// True for scalar numeric leaves; array and string leaves are excluded
// from the fixed-width projection.
bool IsScalarNumeric(TLeaf *leaf)
{
   if (leaf->GetLenStatic() != 1)
      return false;
   const char *type = leaf->GetTypeName();
   return type != nullptr && std::strcmp(type, "TString") != 0 &&
          type[0] != '\0';
}

}  // namespace

int tagma_make_store(const char *url, const char *tree_name = "Events",
                     const char *out_path = "tagma_store.bin",
                     Long64_t record_size = 2560, Long64_t max_entries = -1)
{
   if (record_size <= 0 || record_size % 8 != 0) {
      std::fprintf(stderr,
                   "tagma_make_store: record_size must be positive and a "
                   "multiple of 8\n");
      return 1;
   }

   TFile *file = TFile::Open(url);
   if (!file || file->IsZombie()) {
      std::fprintf(stderr, "tagma_make_store: cannot open %s\n", url);
      return 1;
   }
   TTree *tree = nullptr;
   file->GetObject(tree_name, tree);
   if (!tree) {
      std::fprintf(stderr, "tagma_make_store: tree %s not found in %s\n",
                   tree_name, url);
      delete file;
      return 1;
   }
   const Long64_t total = tree->GetEntries();
   const Long64_t limit =
       (max_entries > 0 && max_entries < total) ? max_entries : total;
   if (limit <= 0) {
      std::fprintf(stderr, "tagma_make_store: nothing to convert in %s\n",
                   url);
      delete file;
      return 1;
   }

   // The fixed-width projection: scalar leaves in tree order, up to the
   // record capacity, serialized as doubles. Names are copied because
   // the leaves are owned by the tree, which is closed before the
   // layout is printed.
   const Long64_t capacity = record_size / 8;
   std::vector<TLeaf *> leaves;
   std::vector<std::string> leafNames;
   TIter next(tree->GetListOfLeaves());
   while (TObject *o = next()) {
      TLeaf *leaf = static_cast<TLeaf *>(o);
      if (IsScalarNumeric(leaf)) {
         leaves.push_back(leaf);
         leafNames.emplace_back(leaf->GetName());
         if (static_cast<Long64_t>(leaves.size()) >= capacity)
            break;
      }
   }

   FILE *out = std::fopen(out_path, "wb");
   if (!out) {
      std::fprintf(stderr, "tagma_make_store: cannot create %s\n", out_path);
      delete file;
      return 1;
   }

   std::vector<unsigned char> record(static_cast<std::size_t>(record_size), 0);
   std::uint64_t checksum = 0;
   TStopwatch watch;
   watch.Start();
   for (Long64_t i = 0; i < limit; ++i) {
      tree->GetEntry(i);
      std::memset(record.data(), 0, record.size());
      Long64_t offset = 0;
      for (const TLeaf *leaf : leaves) {
         const double v = leaf->GetValue(0);
         std::memcpy(record.data() + offset, &v, sizeof(v));
         offset += sizeof(v);
      }
      for (unsigned char b : record)
         checksum += b;
      if (std::fwrite(record.data(), 1, record.size(), out) != record.size()) {
         std::fprintf(stderr, "tagma_make_store: short write to %s\n",
                      out_path);
         std::fclose(out);
         delete file;
         return 1;
      }
   }
   watch.Stop();
   std::fclose(out);
   delete file;

   // The sidecar checksum tagma_bench verifies against served bytes.
   std::string sum_path = std::string(out_path) + ".sum";
   FILE *sum = std::fopen(sum_path.c_str(), "w");
   if (!sum) {
      std::fprintf(stderr, "tagma_make_store: cannot create %s\n",
                   sum_path.c_str());
      return 1;
   }
   std::fprintf(sum, "%llu\n",
                static_cast<unsigned long long>(checksum));
   std::fclose(sum);

   std::printf("tagma_make_store: source=%s\n", url);
   std::printf("tagma_make_store: entries=%lld record_size=%lld leaves=%lld\n",
               static_cast<long long>(limit),
               static_cast<long long>(record_size),
               static_cast<long long>(leaves.size()));
   const Long64_t shown = leaves.size() < 8 ? leaves.size() : 8;
   for (Long64_t i = 0; i < shown; ++i)
      std::printf("tagma_make_store: leaf[%lld]=%s offset=%lld\n",
                  static_cast<long long>(i), leafNames[i].c_str(),
                  static_cast<long long>(i * 8));
   std::printf("tagma_make_store: file_size=%lld checksum=%llu\n",
               static_cast<long long>(limit * record_size),
               static_cast<unsigned long long>(checksum));
   std::printf("tagma_make_store: wall_s=%.3f cpu_s=%.3f\n", watch.RealTime(),
               watch.CpuTime());
   std::printf("tagma_make_store: conversion complete\n");
   return 0;
}
