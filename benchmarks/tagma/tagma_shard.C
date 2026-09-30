// tagma_shard.C
//
// Splits a self-describing coordinate store into one shard per run, so the
// dataset layer has a multi-file, multi-axis dataset to address instead of one
// flat store.
//
// Each shard carries one run and declares its own axes: one run, the luminosity
// blocks the run holds, and the greatest event count of any of them. The
// records are laid out as the dense grid those axes imply, lumi slot major and
// event slot minor, so a coordinate resolves by arithmetic inside the shard. A
// luminosity block with fewer events than the shard's maximum leaves the rest
// of its slots zero filled: that is the padding a dense lattice pays when its
// axes are populated unevenly, and the tool reports the fraction of the grid
// the padding takes. The padded slots carry zero counts and no data, so they
// cost the index record and nothing else.
//
// The payload is untouched. An event's scalar region and its data slice are
// copied byte for byte from the source store, so a leaf read through the shards
// returns the same values. The shard's coordinate is a slot over its axes while
// the record still carries the physical run, luminosity block, and event
// number, which is what the self-check at the end verifies.
//
// Usage:
//   root -l -b -q 'tagma_shard.C("/path/tagma_store_col.bin", "/path/shard")'

#include "ROOT/TTagmaDataset.hxx"
#include "ROOT/TTagmaSchema.hxx"
#include "ROOT/TTagmaStore.hxx"
#include "ROOT/TTagmaWriter.hxx"

#include "TStopwatch.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Lumi {
   std::uint64_t fLumi = 0;  // the luminosity block the run reports
   std::uint64_t fFirst = 0; // first record of that block in the source store
   std::uint64_t fCount = 0; // events in that block
};

std::uint64_t FieldOffset(const ROOT::TTagmaSchema &schema, const char *name)
{
   for (const ROOT::TTagmaSchema::Field &field : schema.GetFields())
      if (field.fName == name)
         return field.fOffset;
   return ~static_cast<std::uint64_t>(0);
}

// Bytes of the packed data region an event's counts imply.
std::uint64_t SliceBytesOf(const ROOT::TTagmaSchema &schema, const char *record)
{
   std::vector<std::uint64_t> counts;
   counts.reserve(schema.GetCollections().size());
   for (std::size_t i = 0; i < schema.GetCollections().size(); ++i)
      counts.push_back(schema.CountOf(i, record));
   return schema.SliceBytes(counts.data());
}

} // namespace

int tagma_shard(const char *sourceStore, const char *outPrefix)
{
   if (sourceStore == nullptr || outPrefix == nullptr) {
      std::fprintf(stderr, "tagma_shard: a source store and an output prefix are required\n");
      return 1;
   }

   ROOT::TTagmaStore::Layout layout;
   ROOT::TTagmaSchema schema;
   std::string why;
   if (!ROOT::TTagmaWriter::ReadStore(sourceStore, &layout, &schema, &why)) {
      std::fprintf(stderr, "tagma_shard: %s: %s\n", sourceStore, why.c_str());
      return 1;
   }
   auto store = std::make_shared<ROOT::TTagmaStore>(layout);
   if (!store->MapFile(sourceStore)) {
      std::fprintf(stderr, "tagma_shard: cannot map %s\n", sourceStore);
      return 1;
   }
   const char *base = store->GetMapped();
   const std::uint64_t recordSize = layout.fRecordSize;
   const std::uint64_t records = store->RecordCount();
   const std::uint64_t scalarBytes = schema.ScalarExtent();
   const std::uint64_t dataBaseOffset = schema.DataBaseOffset();
   const std::uint64_t runOffset = FieldOffset(schema, "run");
   const std::uint64_t lumiOffset = FieldOffset(schema, "luminosityBlock");
   if (runOffset == ~static_cast<std::uint64_t>(0) || lumiOffset == ~static_cast<std::uint64_t>(0) ||
       runOffset + sizeof(std::uint32_t) > scalarBytes || lumiOffset + sizeof(std::uint32_t) > scalarBytes) {
      std::fprintf(stderr, "tagma_shard: the schema carries no run or luminosityBlock scalar\n");
      return 1;
   }

   // Group the records by run and, inside a run, by luminosity block. The source
   // store is written in tree order, so a run's records are contiguous and its
   // blocks follow each other; a run that reappears is rejected rather than
   // silently split.
   std::vector<std::uint64_t> runs;
   std::vector<std::vector<Lumi>> blocks;
   for (std::uint64_t e = 0; e < records; ++e) {
      const char *record = base + e * recordSize;
      std::uint32_t run = 0;
      std::uint32_t lumi = 0;
      std::memcpy(&run, record + runOffset, sizeof(run));
      std::memcpy(&lumi, record + lumiOffset, sizeof(lumi));
      if (runs.empty() || runs.back() != run) {
         for (std::size_t i = 0; i + 1 < runs.size(); ++i) {
            if (runs[i] == run) {
               std::fprintf(stderr, "tagma_shard: run %u reappears out of order\n", run);
               return 1;
            }
         }
         runs.push_back(run);
         blocks.emplace_back();
      }
      std::vector<Lumi> &list = blocks.back();
      if (list.empty() || list.back().fLumi != lumi) {
         Lumi next;
         next.fLumi = lumi;
         next.fFirst = e;
         list.push_back(next);
      }
      list.back().fCount += 1;
   }

   std::printf("tagma_shard: source=%s records=%llu runs=%zu record_size=%llu\n", sourceStore,
               static_cast<unsigned long long>(records), runs.size(), static_cast<unsigned long long>(recordSize));

   // One shard per run: the run's blocks are the shard's lumi axis and the
   // greatest block count is its event axis.
   std::vector<char> empty(scalarBytes, 0);
   std::uint64_t gridCells = 0;
   std::uint64_t realEvents = 0;
   std::uint64_t written = 0;
   TStopwatch watch;
   watch.Start();
   for (std::size_t r = 0; r < runs.size(); ++r) {
      const std::vector<Lumi> &list = blocks[r];
      std::uint64_t eventMax = 0;
      for (const Lumi &lumi : list) {
         if (lumi.fCount > eventMax)
            eventMax = lumi.fCount;
      }
      const std::uint64_t cells = list.size() * eventMax;
      gridCells += cells;
      for (const Lumi &lumi : list)
         realEvents += lumi.fCount;

      char path[4096];
      std::snprintf(path, sizeof(path), "%s_%zu_%llu.tagma", outPrefix, r, static_cast<unsigned long long>(runs[r]));
      ROOT::TTagmaWriter writer(schema, 1, list.size(), eventMax);
      if (!writer.Open(path)) {
         std::fprintf(stderr, "tagma_shard: cannot write %s\n", path);
         return 1;
      }
      for (const Lumi &lumi : list) {
         for (std::uint64_t i = 0; i < eventMax; ++i) {
            if (i >= lumi.fCount) {
               // A padded slot: zero counts, no data.
               if (!writer.AddEvent(empty.data(), nullptr)) {
                  std::fprintf(stderr, "tagma_shard: cannot pad %s at lumi %llu slot %llu\n", path,
                               static_cast<unsigned long long>(lumi.fLumi), static_cast<unsigned long long>(i));
                  return 1;
               }
               continue;
            }
            const char *record = base + (lumi.fFirst + i) * recordSize;
            std::uint64_t dataBase = 0;
            std::memcpy(&dataBase, record + dataBaseOffset, sizeof(dataBase));
            const std::uint64_t slice = SliceBytesOf(schema, record);
            if (!writer.AddEvent(record, slice == 0 ? nullptr : base + dataBase)) {
               std::fprintf(stderr, "tagma_shard: cannot write %s at lumi %llu slot %llu\n", path,
                            static_cast<unsigned long long>(lumi.fLumi), static_cast<unsigned long long>(i));
               return 1;
            }
         }
      }
      if (!writer.Close()) {
         std::fprintf(stderr, "tagma_shard: %s did not close over a complete grid\n", path);
         return 1;
      }
      written += writer.IndexBytes() + writer.DataSize();
   }
   watch.Stop();

   const std::uint64_t padding = gridCells - realEvents;
   std::printf("tagma_shard: shards=%zu grid_cells=%llu events=%llu padding=%llu (%.1f%% of the grid)\n", runs.size(),
               static_cast<unsigned long long>(gridCells), static_cast<unsigned long long>(realEvents),
               static_cast<unsigned long long>(padding),
               gridCells == 0 ? 0.0 : 100.0 * static_cast<double>(padding) / static_cast<double>(gridCells));
   std::printf("tagma_shard: payload=%llu wall_s=%.1f\n", static_cast<unsigned long long>(written), watch.RealTime());

   // The self-check: the dataset resolves a slot coordinate to the shard that
   // owns it, and the record carries the physical values that slot stands for.
   ROOT::TTagmaDataset dataset;
   for (std::size_t r = 0; r < runs.size(); ++r)
      dataset.AddFile(std::string(outPrefix) + "_" + std::to_string(r) + "_" + std::to_string(runs[r]) + ".tagma", r, 0,
                      true);
   std::uint64_t checked = 0;
   std::uint64_t bad = 0;
   for (std::size_t r = 0; r < runs.size(); ++r) {
      const std::vector<Lumi> &list = blocks[r];
      for (std::size_t l = 0; l < list.size(); ++l) {
         // One coordinate per block, the middle event of the block.
         const std::uint64_t event = list[l].fCount / 2;
         std::size_t file = 0;
         std::uint64_t index = 0;
         std::vector<char> record(recordSize, 0);
         if (!dataset.Resolve(r, l, event, &file, &index) ||
             !dataset.ReadRecord(r, l, event, record.data(), recordSize)) {
            std::printf("tagma_shard: the dataset does not resolve run slot %zu lumi slot %zu event %llu\n", r, l,
                        static_cast<unsigned long long>(event));
            return 1;
         }
         std::uint32_t run = 0;
         std::uint32_t lumi = 0;
         std::memcpy(&run, record.data() + runOffset, sizeof(run));
         std::memcpy(&lumi, record.data() + lumiOffset, sizeof(lumi));
         ++checked;
         if (run != runs[r] || lumi != list[l].fLumi) {
            ++bad;
            std::printf("tagma_shard: slot (%zu,%zu,%llu) reads run %u lumi %u, expected %llu %llu\n", r, l,
                        static_cast<unsigned long long>(event), run, lumi, static_cast<unsigned long long>(runs[r]),
                        static_cast<unsigned long long>(list[l].fLumi));
         }
      }
   }
   std::printf("tagma_shard: self-check coordinates=%llu mismatches=%llu\n", static_cast<unsigned long long>(checked),
               static_cast<unsigned long long>(bad));
   return bad == 0 ? 0 : 1;
}
