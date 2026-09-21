// tagma_arrays.C
//
// Design input for variable-length collections (ssccsorg/ssccs#121, the
// P2 phase). The store's fixed-width projection carries only the leading
// element of a variable-length array, so the addressing model has to
// change. Two models are candidates: a fixed slot per collection per
// event, which keeps offset = index * record size and pays padding, or a
// packed variable region addressed by a per-event offset, which is
// compact and costs one indirection.
//
// This macro measures what the padding would cost: how many distinct
// collectors the array fields belong to, how many fields each carries,
// and the count distribution of each.
//
// Usage:
//   root -l -b -q 'tagma_arrays.C("/path/to/source.root", "Events", "/path/to/store.bin.layout")'

#include <ROOT/RDataFrame.hxx>

#include "TFile.h"
#include "TLeaf.h"
#include "TTree.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

// The store layout sidecar: "<name> <offset> <type>" per field.
std::vector<std::string> ReadLayoutNames(const char *path)
{
   std::vector<std::string> names;
   std::ifstream in(path);
   if (!in)
      return names;
   std::string line;
   while (std::getline(in, line)) {
      std::istringstream probe(line);
      std::string name;
      if (!(probe >> name) || name.empty() || name[0] == '#')
         continue;
      names.push_back(name);
   }
   return names;
}

std::uint64_t ElementBytes(const TLeaf *leaf)
{
   const std::string type = leaf->GetTypeName() ? leaf->GetTypeName() : "";
   if (type == "Double_t" || type == "ULong64_t" || type == "Long64_t")
      return 8;
   if (type == "Float_t" || type == "UInt_t" || type == "Int_t")
      return 4;
   if (type == "UShort_t" || type == "Short_t")
      return 2;
   if (type == "UChar_t" || type == "Char_t" || type == "Bool_t")
      return 1;
   return 0;
}

struct Collection {
   std::string fCountBranch;          // the leaf count branch name
   std::vector<std::string> fFields;  // array fields indexed by that count
   std::uint64_t fBytesPerElement = 0;  // sum of the field element sizes
};

}  // namespace

int tagma_arrays(const char *url, const char *tree_name = "Events",
                 const char *layout_path = "")
{
   const std::vector<std::string> names = ReadLayoutNames(layout_path);
   if (names.empty()) {
      std::fprintf(stderr, "tagma_arrays: no columns in %s\n", layout_path);
      return 1;
   }

   TFile *file = TFile::Open(url);
   if (!file || file->IsZombie()) {
      std::fprintf(stderr, "tagma_arrays: cannot open %s\n", url);
      return 1;
   }
   TTree *tree = nullptr;
   file->GetObject(tree_name, tree);
   if (!tree) {
      std::fprintf(stderr, "tagma_arrays: tree %s not found\n", tree_name);
      return 1;
   }

   // Group the array fields by the branch their leaf count belongs to.
   std::map<std::string, Collection> collections;
   std::vector<std::string> scalars;
   for (const auto &name : names) {
      TLeaf *leaf = tree->GetLeaf(name.c_str());
      if (!leaf) {
         continue;
      }
      TLeaf *count = leaf->GetLeafCount();
      if (!count) {
         scalars.push_back(name);
         continue;
      }
      const std::string countName =
          (count->GetBranch() ? count->GetBranch()->GetName() : count->GetName());
      Collection &collection = collections[countName];
      collection.fCountBranch = countName;
      collection.fFields.push_back(name);
      collection.fBytesPerElement += ElementBytes(leaf);
   }

   std::uint64_t scalarBytes = 0;
   for (const auto &name : scalars) {
      TLeaf *leaf = tree->GetLeaf(name.c_str());
      scalarBytes += ElementBytes(leaf);
   }

   ROOT::RDataFrame df(*tree);
   std::printf("tagma_arrays: layout_fields=%d scalar=%d array=%d "
               "collections=%d scalar_bytes_per_event=%llu\n",
               (int)names.size(), (int)scalars.size(),
               (int)(names.size() - scalars.size()), (int)collections.size(),
               (unsigned long long)scalarBytes);
   std::printf("tagma_arrays: %-16s %7s %10s %8s %8s %14s\n", "count_branch",
               "fields", "bytes/elem", "mean", "max", "slot_bytes");

   std::uint64_t fixedSlotTotal = 0;
   std::uint64_t packedMeanTotal = 0;
   for (auto &entry : collections) {
      Collection &collection = entry.second;
      auto maxResult = df.Max(collection.fCountBranch);
      auto meanResult = df.Mean(collection.fCountBranch);
      const Double_t maxCount = maxResult.GetValue();
      const Double_t meanCount = meanResult.GetValue();
      const std::uint64_t slotBytes =
          static_cast<std::uint64_t>(maxCount) * collection.fBytesPerElement;
      fixedSlotTotal += slotBytes;
      packedMeanTotal +=
          static_cast<std::uint64_t>(meanCount) * collection.fBytesPerElement;
      std::printf("tagma_arrays: %-16s %7d %10llu %8.2f %8.0f %14llu\n",
                  collection.fCountBranch.c_str(),
                  (int)collection.fFields.size(),
                  (unsigned long long)collection.fBytesPerElement, meanCount,
                  maxCount, (unsigned long long)slotBytes);
   }

   const Long64_t entries = tree->GetEntries();
   std::printf(
       "tagma_arrays: entries=%lld fixed_slot_bytes_per_event=%llu "
       "mean_used_bytes_per_event=%llu fixed_slot_store=%.2f GB "
       "packed_store=%.2f GB\n",
       static_cast<long long>(entries),
       (unsigned long long)(scalarBytes + fixedSlotTotal),
       (unsigned long long)(scalarBytes + packedMeanTotal),
       1e-9 * (scalarBytes + fixedSlotTotal) * entries,
       1e-9 * (scalarBytes + packedMeanTotal) * entries);
   delete file;
   return 0;
}
