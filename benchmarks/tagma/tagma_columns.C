// tagma_columns.C
//
// Column-selected baseline for the coordinate read-path comparison
// (ssccsorg/ssccs#120). The harness baseline reads every branch, while an
// analysis reads a few columns and the store serves a fixed-width record.
// This macro measures the same events with the branch set restricted, so the
// store's cost can be compared against a baseline that does the work its
// payload actually requires.
//
// Rows: every branch, the store layout columns, and the two columns of the
// analysis workload, each on the compressed source, plus the store layout
// columns on the uncompressed rewrite where neither side decompresses.
//
// Usage:
//   root -l -b -q 'tagma_columns.C("/path/to/source.root", "Events", "/path/to/store.bin.layout", -1, "/path/to/uncompressed.root")'
//
// Arguments:
//   url          compressed dataset source
//   tree_name    tree to read
//   layout_path  the store layout sidecar, whose field names select the
//                store's columns
//   max_entries  events to read; -1 reads all
//   uncomp_url   uncompressed rewrite, or "" to skip that row

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "TFile.h"
#include "TLeaf.h"
#include "TStopwatch.h"
#include "TTree.h"

namespace {

constexpr Long64_t kCacheMb = 32;

struct ColumnRow {
   const char *name = nullptr;
   const char *url = nullptr;
   Long64_t entries = 0;
   Double_t wall = 0;
   Double_t cpu = 0;
   Long64_t readCalls = 0;
   Long64_t sysReadCalls = 0;
   Long64_t bytesRead = 0;
   Long64_t branches = 0;
   Long64_t found = 0;
   Long64_t checksum = 0;
   Bool_t ok = kFALSE;
};

// The store layout sidecar: "<name> <offset> <type>" per field.
bool ReadLayoutNames(const char *path, std::vector<std::string> *names)
{
   std::ifstream in(path);
   if (!in)
      return false;
   std::string line;
   while (std::getline(in, line)) {
      std::istringstream probe(line);
      std::string name;
      if (!(probe >> name) || name.empty() || name[0] == '#')
         continue;
      names->push_back(name);
   }
   return !names->empty();
}

void PrintRow(const ColumnRow &row)
{
   const Double_t readsPerEvent =
       row.entries > 0 ? static_cast<Double_t>(row.readCalls) / row.entries : 0;
   const Double_t bytesPerRead =
       row.readCalls > 0 ? static_cast<Double_t>(row.bytesRead) / row.readCalls : 0;
   const Double_t syscallsPerEvent =
       row.entries > 0 ? static_cast<Double_t>(row.sysReadCalls) / row.entries : 0;
   const Double_t mbPerSecond =
       row.wall > 0 ? 1e-6 * row.bytesRead / row.wall : 0;
   std::printf(
       "tagma_columns: %-22s %9.3f %8.3f %9lld %8lld %12lld %8.2f %11.1f "
       "%9.2f %8lld/%lld\n",
       row.name, row.wall, row.cpu, static_cast<long long>(row.readCalls),
       static_cast<long long>(row.sysReadCalls),
       static_cast<long long>(row.bytesRead), readsPerEvent, bytesPerRead,
       syscallsPerEvent, static_cast<long long>(row.found),
       static_cast<long long>(row.branches));
}

// One row: a fresh file open, the branch set restricted to `columns` when it
// is not empty, the cache at `cacheMb`, and a full pass over the entries.
// The two analysis columns are read when present, so the leaf payload is
// transferred rather than deferred.
ColumnRow MeasureColumnRow(const char *name, const char *url,
                           const char *tree_name,
                           const std::vector<std::string> &columns,
                           Long64_t limit, Long64_t cacheMb)
{
   ColumnRow row;
   row.name = name;
   row.url = url;

   TFile *file = TFile::Open(url);
   if (!file || file->IsZombie()) {
      std::fprintf(stderr, "tagma_columns: cannot open %s\n", url);
      delete file;
      return row;
   }
   TTree *tree = nullptr;
   file->GetObject(tree_name, tree);
   if (!tree) {
      std::fprintf(stderr, "tagma_columns: tree %s not found in %s\n", tree_name,
                   url);
      delete file;
      return row;
   }
   row.branches = tree->GetListOfBranches()->GetEntries();
   const Long64_t total = tree->GetEntries();
   const Long64_t entries =
       (limit > 0 && limit < total) ? limit : total;
   row.entries = entries;

   tree->SetCacheSize(cacheMb > 0 ? cacheMb * 1024 * 1024 : 0);
   if (!columns.empty()) {
      tree->SetBranchStatus("*", 0);
      for (const auto &column : columns) {
         UInt_t found = 0;
         tree->SetBranchStatus(column.c_str(), 1, &found);
         row.found += found;
      }
   } else {
      row.found = row.branches;
   }

   TLeaf *metLeaf = tree->GetLeaf("MET_pt");
   TLeaf *nMuonLeaf = tree->GetLeaf("nMuon");

   const Int_t calls0 = file->GetReadCalls();
   const Int_t sys0 = file->GetSysReadCalls();
   const Long64_t bytes0 = file->GetBytesRead();

   TStopwatch watch;
   watch.Start();
   Long64_t checksum = 0;
   for (Long64_t i = 0; i < entries; ++i) {
      tree->GetEntry(i);
      if (metLeaf)
         checksum += static_cast<Long64_t>(metLeaf->GetValue(0));
      if (nMuonLeaf)
         checksum += static_cast<Long64_t>(nMuonLeaf->GetValue(0));
   }
   watch.Stop();

   row.wall = watch.RealTime();
   row.cpu = watch.CpuTime();
   row.readCalls = file->GetReadCalls() - calls0;
   row.sysReadCalls = file->GetSysReadCalls() - sys0;
   row.bytesRead = file->GetBytesRead() - bytes0;
   row.checksum = checksum;
   row.ok = kTRUE;
   delete file;
   return row;
}

}  // namespace

int tagma_columns(const char *url, const char *tree_name = "Events",
                  const char *layout_path = "", Long64_t max_entries = -1,
                  const char *uncomp_url = "")
{
   std::vector<std::string> storeColumns;
   if (!ReadLayoutNames(layout_path, &storeColumns)) {
      std::fprintf(stderr, "tagma_columns: cannot read the layout %s\n",
                   layout_path);
      return 1;
   }
   const std::vector<std::string> analysisColumns{"MET_pt", "nMuon"};

   std::printf(
       "tagma_columns: %-22s %9s %8s %9s %8s %12s %8s %11s %9s %9s\n", "row",
       "wall_s", "cpu_s", "reads", "syscalls", "bytes_moved", "reads/ev",
       "bytes/read", "syscalls/ev", "found");

   const ColumnRow all =
       MeasureColumnRow("compressed all", url, tree_name, {}, max_entries, 0);
   PrintRow(all);

   const ColumnRow storeOff = MeasureColumnRow("compressed store-cols", url,
                                               tree_name, storeColumns,
                                               max_entries, 0);
   PrintRow(storeOff);

   const ColumnRow storeOn = MeasureColumnRow("compressed store-cols cache",
                                              url, tree_name, storeColumns,
                                              max_entries, kCacheMb);
   PrintRow(storeOn);

   const ColumnRow narrowOff = MeasureColumnRow("compressed 2-cols", url,
                                                tree_name, analysisColumns,
                                                max_entries, 0);
   PrintRow(narrowOff);

   const ColumnRow narrowOn = MeasureColumnRow("compressed 2-cols cache", url,
                                               tree_name, analysisColumns,
                                               max_entries, kCacheMb);
   PrintRow(narrowOn);

   if (uncomp_url != nullptr && uncomp_url[0] != '\0') {
      const ColumnRow uncompStore = MeasureColumnRow(
          "uncompressed store-cols", uncomp_url, tree_name, storeColumns,
          max_entries, 0);
      PrintRow(uncompStore);
   }

   std::printf("tagma_columns: store_columns=%d\n", (int)storeColumns.size());
   std::printf("tagma_columns: comparison complete\n");
   return 0;
}
