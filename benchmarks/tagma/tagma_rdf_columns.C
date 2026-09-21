// tagma_rdf_columns.C
//
// RDataFrame column-selected baseline (ssccsorg/ssccs#120). The
// SetBranchStatus rows of tagma_columns.C still pay the per-event branch
// walk of TTree::GetEntry, which ROOT's own analysis interface avoids:
// TTreeReader reads the active columns through their branches and never
// calls TTree::GetEntry. This macro measures that path on the same events,
// so the coordinate store is compared against ROOT's best column-selective
// reader rather than against a reader that walks every branch.
//
// Usage:
//   root -l -b -q 'tagma_rdf_columns.C("/path/to/source.root", "Events")'
//
// Each query runs twice and the second run is reported: the first run
// includes the one-time compilation of a jitted expression.

#include <ROOT/RDataFrame.hxx>

#include "TFile.h"
#include "TLeaf.h"
#include "TStopwatch.h"
#include "TTree.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// The store layout sidecar: "<name> <offset> <type>" per field. The names
// are the columns the store serves, which is the set the wide row reads.
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

std::string SumExpression(const std::vector<std::string> &names)
{
   std::string expression;
   for (std::size_t i = 0; i < names.size(); ++i) {
      if (i > 0)
         expression += " + ";
      expression += names[i];
   }
   return expression;
}

struct RdfRow {
   const char *name = nullptr;
   Long64_t entries = 0;
   Double_t wall = 0;
   Double_t cpu = 0;
   Long64_t readCalls = 0;
   Long64_t sysReadCalls = 0;
   Long64_t bytesRead = 0;
   Double_t result = 0;
};

void PrintRow(const RdfRow &row)
{
   std::printf(
       "tagma_rdf_columns: %-14s entries=%lld wall_s=%8.3f cpu_s=%8.3f "
       "reads=%8lld syscalls=%8lld bytes=%12lld result=%g\n",
       row.name, static_cast<long long>(row.entries), row.wall, row.cpu,
       static_cast<long long>(row.readCalls),
       static_cast<long long>(row.sysReadCalls),
       static_cast<long long>(row.bytesRead), row.result);
}

// Runs `query` twice on a fresh file open and reports the second run. The
// jitted expression compiles once, on the first run.
template <class Query>
RdfRow Measure(const char *name, const char *url, const char *tree_name,
               Long64_t limit, Query query)
{
   RdfRow row;
   row.name = name;

   TFile *file = TFile::Open(url);
   if (!file || file->IsZombie()) {
      std::fprintf(stderr, "tagma_rdf_columns: cannot open %s\n", url);
      delete file;
      return row;
   }
   TTree *tree = nullptr;
   file->GetObject(tree_name, tree);
   if (!tree) {
      std::fprintf(stderr, "tagma_rdf_columns: tree %s not found\n", tree_name);
      delete file;
      return row;
   }
   const Long64_t total = tree->GetEntries();
   const Long64_t entries = (limit > 0 && limit < total) ? limit : total;
   row.entries = entries;

   ROOT::RDataFrame df(*tree);
   query(df);  // warms the JIT and the page cache

   const Int_t calls0 = file->GetReadCalls();
   const Int_t sys0 = file->GetSysReadCalls();
   const Long64_t bytes0 = file->GetBytesRead();

   TStopwatch watch;
   watch.Start();
   row.result = query(df);
   watch.Stop();

   row.wall = watch.RealTime();
   row.cpu = watch.CpuTime();
   row.readCalls = file->GetReadCalls() - calls0;
   row.sysReadCalls = file->GetSysReadCalls() - sys0;
   row.bytesRead = file->GetBytesRead() - bytes0;
   delete file;
   return row;
}

}  // namespace

int tagma_rdf_columns(const char *url, const char *tree_name = "Events",
                      Long64_t max_entries = -1,
                      const char *layout_path = "")
{
   std::printf("tagma_rdf_columns: version=%s\n", gROOT->GetVersion());

   PrintRow(Measure("entries only", url, tree_name, max_entries, [](ROOT::RDataFrame &df) {
      return static_cast<Double_t>(df.Count().GetValue());
   }));

   PrintRow(Measure("one column", url, tree_name, max_entries, [](ROOT::RDataFrame &df) {
      return df.Histo1D("MET_pt")->GetEntries();
   }));

   PrintRow(Measure("two columns", url, tree_name, max_entries, [](ROOT::RDataFrame &df) {
      return df.Filter("MET_pt > 100 && nMuon >= 1").Histo1D("MET_pt")->GetEntries();
   }));

   if (layout_path != nullptr && layout_path[0] != '\0') {
      const std::vector<std::string> columns = ReadLayoutNames(layout_path);
      if (columns.empty()) {
         std::fprintf(stderr, "tagma_rdf_columns: no columns in %s\n",
                      layout_path);
         return 1;
      }
      // A leaf with a leaf count is a variable-length array that reports a
      // static length of one, so summing it with the others would add
      // vectors of different sizes. Only the leaves without a count are the
      // fixed-width scalars the store can hold faithfully.
      std::vector<std::string> scalarColumns;
      Long64_t arrays = 0;
      {
         TFile *probe = TFile::Open(url);
         TTree *probeTree = nullptr;
         if (probe && !probe->IsZombie())
            probe->GetObject(tree_name, probeTree);
         for (const auto &name : columns) {
            TLeaf *leaf = probeTree ? probeTree->GetLeaf(name.c_str()) : nullptr;
            if (leaf && leaf->GetLeafCount()) {
               ++arrays;
               continue;
            }
            scalarColumns.push_back(name);
         }
         delete probe;
      }
      std::printf(
          "tagma_rdf_columns: layout_columns=%d array_leaves=%lld "
          "scalar_columns=%d\n",
          (int)columns.size(), static_cast<long long>(arrays),
          (int)scalarColumns.size());

      // The wide row: every fixed-width column the store serves, read
      // through RDataFrame. Sum is the action that forces the definition,
      // since an action that needs no column would be pruned and read
      // nothing.
      const std::string expression = SumExpression(scalarColumns);
      PrintRow(Measure("store columns", url, tree_name, max_entries,
                       [&expression](ROOT::RDataFrame &df) {
                          return df.Define("tagma_sum", expression)
                              .Sum("tagma_sum")
                              .GetValue();
                       }));
   }

   return 0;
}
