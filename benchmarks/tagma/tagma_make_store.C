// tagma_make_store.C
//
// Preparation tool for the coordinate read-path benchmark (ssccs #103):
// converts a real dataset tree into a coordinate store file.
//
// Two modes:
//
//   mode 0 (default)  the fixed-width projection. Each event becomes one
//                     record of record_size bytes holding the first
//                     (record_size / 8) scalar leaf values of the event,
//                     serialized as doubles in leaf order and zero-padded.
//                     This is the store the M5 harness measures, and the
//                     shape the sidecar layout describes.
//
//   mode 1            the collection-carrying, self-describing store. A
//                     TTagmaSchema is derived from the tree, one scalar
//                     field per scalar numeric leaf and one collection per
//                     variable-length array leaf bounded by its count leaf,
//                     with the leaf types preserved instead of widened to
//                     double. The store is written with ROOT::TTagmaWriter:
//                     an index record per event holding the scalars, the
//                     object counts, and the data-base offset, then the
//                     packed data region, then the field table and the
//                     trailing descriptor. Reading it needs no sidecar;
//                     TTree::SetTagmaStore(path) recovers the layout and the
//                     schema from the descriptor.
//
// The sidecar file <out_path>.sum holds the sum of the payload bytes the
// read path serves (the index and data regions). tagma_bench reads it and
// verifies the checksum against the bytes the coordinate read path serves,
// so the benchmark runs against real converted data, not a pattern.
//
// The sidecar file <out_path>.layout holds the layout in the schema text
// form: one field per line, `<name> <offset> <type>` for a scalar and
// `<name> <offset> <type> <count> <maxCount>` for an array field.
//
// Usage:
//   root -l -b -q 'tagma_make_store.C("/path/to/local.root", "Events", "tagma_store.bin", 2560)'
//   root -l -b -q 'tagma_make_store.C("/path/to/local.root", "Events", "tagma_store.bin", 2560, 200000, 1)'
//
// Arguments:
//   url           dataset source (local path or root:// URL)
//   tree_name     tree to convert
//   out_path      coordinate store file to write
//   record_size   fixed-width record bytes per event, a multiple of 8,
//                 used by mode 0 only
//   max_entries   events to convert; -1 converts all
//   mode          0 fixed-width projection, 1 collection-carrying store

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ROOT/TTagmaSchema.hxx"
#include "ROOT/TTagmaWriter.hxx"

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

// The schema type a leaf serializes as, from the leaf type name. Returns
// false for a leaf whose type the schema does not carry.
bool SchemaType(TLeaf *leaf, ROOT::TTagmaSchema::EType *type)
{
   const char *name = leaf->GetTypeName();
   if (name == nullptr)
      return false;
   static const std::map<std::string, ROOT::TTagmaSchema::EType> known = {
      {"Float_t", ROOT::TTagmaSchema::EType::kFloat},  {"Double_t", ROOT::TTagmaSchema::EType::kDouble},
      {"Int_t", ROOT::TTagmaSchema::EType::kInt32},    {"UInt_t", ROOT::TTagmaSchema::EType::kUInt32},
      {"Short_t", ROOT::TTagmaSchema::EType::kInt16},  {"UShort_t", ROOT::TTagmaSchema::EType::kUInt16},
      {"Long64_t", ROOT::TTagmaSchema::EType::kInt64}, {"ULong64_t", ROOT::TTagmaSchema::EType::kUInt64},
      {"Char_t", ROOT::TTagmaSchema::EType::kInt8},    {"UChar_t", ROOT::TTagmaSchema::EType::kUInt8},
      {"Bool_t", ROOT::TTagmaSchema::EType::kBool}};
   const auto found = known.find(std::string(name));
   if (found == known.end())
      return false;
   *type = found->second;
   return true;
}

// Sum of the first `bytes` bytes of `path`, the payload checksum the
// harness verifies. Returns false when the file cannot be read.
bool PayloadChecksum(const char *path, std::uint64_t bytes, std::uint64_t *sum)
{
   FILE *in = std::fopen(path, "rb");
   if (!in)
      return false;
   std::uint64_t total = 0;
   std::uint64_t left = bytes;
   unsigned char buffer[65536];
   while (left > 0) {
      const std::size_t want = left < sizeof(buffer) ? left : sizeof(buffer);
      const std::size_t got = std::fread(buffer, 1, want, in);
      if (got == 0)
         break;
      for (std::size_t i = 0; i < got; ++i)
         total += buffer[i];
      left -= got;
   }
   std::fclose(in);
   *sum = total;
   return true;
}

// Writes the collection-carrying, self-describing store (mode 1): a schema
// derived from the tree, an index region of scalars, counts, and data bases,
// and a packed data region, all through ROOT::TTagmaWriter.
int MakeCollectionStore(const char *url, TTree *tree, const char *out_path, Long64_t limit)
{
   // Classify the leaves. A leaf bounded by a count leaf is a collection;
   // a length-one numeric leaf is a scalar.
   std::vector<TLeaf *> scalars;
   std::vector<TLeaf *> collections;
   TIter next(tree->GetListOfLeaves());
   while (TObject *o = next()) {
      TLeaf *leaf = static_cast<TLeaf *>(o);
      if (leaf->GetLeafCount() != nullptr)
         collections.push_back(leaf);
      else if (IsScalarNumeric(leaf))
         scalars.push_back(leaf);
   }

   // The first pass: the greatest object count per collection, from the
   // count leaves. The bound has to be known before the schema is built.
   std::map<std::string, std::uint64_t> maxCounts;
   for (TLeaf *leaf : collections)
      maxCounts[leaf->GetLeafCount()->GetName()] = 0;
   for (Long64_t i = 0; i < limit; ++i) {
      tree->GetEntry(i);
      for (TLeaf *leaf : collections) {
         const std::string name = leaf->GetLeafCount()->GetName();
         const std::uint64_t count = static_cast<std::uint64_t>(leaf->GetLeafCount()->GetValue());
         if (count > maxCounts[name])
            maxCounts[name] = count;
      }
   }

   // The schema: the scalars first, in tree order, then the collection
   // fields, each object's fields following each other from offset zero.
   ROOT::TTagmaSchema schema;
   std::map<std::string, TLeaf *> leaves;
   std::uint64_t scalarOffset = 0;
   for (TLeaf *leaf : scalars) {
      ROOT::TTagmaSchema::Field field;
      field.fName = leaf->GetName();
      field.fOffset = scalarOffset;
      if (!SchemaType(leaf, &field.fType)) {
         std::fprintf(stderr, "tagma_make_store: unsupported scalar type %s\n", leaf->GetTypeName());
         return 1;
      }
      schema.AddField(field);
      leaves[field.fName] = leaf;
      scalarOffset += field.Size();
   }
   std::map<std::string, std::uint64_t> objectOffset;
   for (TLeaf *leaf : collections) {
      const std::string countName = leaf->GetLeafCount()->GetName();
      ROOT::TTagmaSchema::Field field;
      field.fName = leaf->GetName();
      field.fCountField = countName;
      field.fOffset = objectOffset[countName];
      field.fMaxCount = maxCounts[countName];
      if (!SchemaType(leaf, &field.fType)) {
         std::fprintf(stderr, "tagma_make_store: unsupported array type %s\n", leaf->GetTypeName());
         return 1;
      }
      schema.AddField(field);
      leaves[field.fName] = leaf;
      objectOffset[countName] += field.Size();
   }

   std::string why;
   if (!schema.Validate(schema.IndexRecordSize(), &why)) {
      std::fprintf(stderr, "tagma_make_store: invalid schema: %s\n", why.c_str());
      return 1;
   }
   if (!schema.HasCollections()) {
      std::fprintf(stderr, "tagma_make_store: mode 1 needs a collection; the tree has "
                           "none\n");
      return 1;
   }

   ROOT::TTagmaWriter writer(schema, static_cast<std::uint64_t>(limit));
   if (!writer.Open(out_path)) {
      std::fprintf(stderr, "tagma_make_store: cannot create %s\n", out_path);
      return 1;
   }

   const std::vector<ROOT::TTagmaSchema::Collection> &groups = schema.GetCollections();
   std::vector<char> scalarRegion(schema.ScalarExtent(), 0);
   std::vector<char> slice;
   std::vector<std::uint64_t> counts(groups.size(), 0);
   TStopwatch watch;
   watch.Start();
   for (Long64_t i = 0; i < limit; ++i) {
      tree->GetEntry(i);

      // The scalar region: every scalar field at its offset, the count
      // fields among them, copied at the leaf's own width.
      std::memset(scalarRegion.data(), 0, scalarRegion.size());
      for (const ROOT::TTagmaSchema::Field &field : schema.GetScalars()) {
         TLeaf *leaf = leaves[field.fName];
         std::memcpy(scalarRegion.data() + field.fOffset, leaf->GetValuePointer(), field.Size());
      }

      // The packed slice: the collections in schema order, each field
      // contiguous, at the offsets the schema arithmetic resolves.
      for (std::size_t c = 0; c < groups.size(); ++c)
         counts[c] = schema.CountOf(c, scalarRegion.data());
      slice.assign(schema.SliceBytes(counts.data()), 0);
      for (std::size_t c = 0; c < groups.size(); ++c) {
         const std::uint64_t chunk = schema.ChunkOffset(c, counts.data());
         for (const ROOT::TTagmaSchema::Field &field : groups[c].fFields) {
            TLeaf *leaf = leaves[field.fName];
            const std::uint64_t at = chunk + schema.FieldOffset(groups[c], field.fName, counts[c]);
            std::memcpy(slice.data() + at, leaf->GetValuePointer(), counts[c] * field.Size());
         }
      }

      if (!writer.AddEvent(scalarRegion.data(), slice.empty() ? nullptr : slice.data())) {
         std::fprintf(stderr, "tagma_make_store: event %lld rejected\n", static_cast<long long>(i));
         return 1;
      }
   }
   const std::uint64_t payload = writer.IndexBytes() + writer.DataSize();
   if (!writer.Close()) {
      std::fprintf(stderr, "tagma_make_store: cannot close %s\n", out_path);
      return 1;
   }
   watch.Stop();

   // The sidecars: the payload checksum and the schema text form.
   std::uint64_t checksum = 0;
   if (!PayloadChecksum(out_path, payload, &checksum)) {
      std::fprintf(stderr, "tagma_make_store: cannot checksum %s\n", out_path);
      return 1;
   }
   std::string sum_path = std::string(out_path) + ".sum";
   FILE *sum = std::fopen(sum_path.c_str(), "w");
   if (!sum) {
      std::fprintf(stderr, "tagma_make_store: cannot create %s\n", sum_path.c_str());
      return 1;
   }
   std::fprintf(sum, "%llu\n", static_cast<unsigned long long>(checksum));
   std::fclose(sum);

   std::string layout_path = std::string(out_path) + ".layout";
   FILE *layout = std::fopen(layout_path.c_str(), "w");
   if (!layout) {
      std::fprintf(stderr, "tagma_make_store: cannot create %s\n", layout_path.c_str());
      return 1;
   }
   std::fputs(schema.Text().c_str(), layout);
   std::fclose(layout);

   std::printf("tagma_make_store: mode=1 source=%s\n", url);
   std::printf("tagma_make_store: entries=%lld scalars=%zu collections=%zu\n", static_cast<long long>(limit),
               schema.GetScalars().size(), groups.size());
   std::printf("tagma_make_store: index_record_size=%llu data_size=%llu\n",
               static_cast<unsigned long long>(schema.IndexRecordSize()),
               static_cast<unsigned long long>(writer.DataSize()));
   for (const ROOT::TTagmaSchema::Collection &group : groups)
      std::printf("tagma_make_store: collection %s max_count=%llu fields=%zu\n", group.fCountField.c_str(),
                  static_cast<unsigned long long>(group.fMaxCount), group.fFields.size());
   std::printf("tagma_make_store: payload=%llu checksum=%llu\n", static_cast<unsigned long long>(payload),
               static_cast<unsigned long long>(checksum));
   std::printf("tagma_make_store: wall_s=%.3f cpu_s=%.3f\n", watch.RealTime(), watch.CpuTime());
   std::printf("tagma_make_store: conversion complete\n");
   return 0;
}

}  // namespace

int tagma_make_store(const char *url, const char *tree_name = "Events", const char *out_path = "tagma_store.bin",
                     Long64_t record_size = 2560, Long64_t max_entries = -1, Int_t mode = 0)
{
   if (mode == 0 && (record_size <= 0 || record_size % 8 != 0)) {
      std::fprintf(stderr,
                   "tagma_make_store: record_size must be positive and a "
                   "multiple of 8\n");
      return 1;
   }
   if (mode != 0 && mode != 1) {
      std::fprintf(stderr, "tagma_make_store: mode must be 0 or 1\n");
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
      std::fprintf(stderr, "tagma_make_store: nothing to convert in %s\n", url);
      delete file;
      return 1;
   }

   if (mode == 1) {
      const int rc = MakeCollectionStore(url, tree, out_path, limit);
      delete file;
      return rc;
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
         std::fprintf(stderr, "tagma_make_store: short write to %s\n", out_path);
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
   std::fprintf(sum, "%llu\n", static_cast<unsigned long long>(checksum));
   std::fclose(sum);

   // The layout sidecar: leaf name and byte offset per record field, in
   // serialization order. The analysis mode of tagma_bench reads it to
   // interpret the fixed-width records.
   std::string layout_path = std::string(out_path) + ".layout";
   FILE *layout = std::fopen(layout_path.c_str(), "w");
   if (!layout) {
      std::fprintf(stderr, "tagma_make_store: cannot create %s\n",
                   layout_path.c_str());
      return 1;
   }
   for (std::size_t i = 0; i < leaves.size(); ++i)
      std::fprintf(layout, "%s %zu double\n", leafNames[i].c_str(), i * 8);
   std::fclose(layout);

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
