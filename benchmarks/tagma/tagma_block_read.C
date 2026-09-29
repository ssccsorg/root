// tagma_block_read: the store's read path on its own.
//
// The harness rows report the entry layer, which the branch machinery
// dominates; this reads the whole payload of a store through the byte source
// the file hook would use, so the read path is measured, and reproduced,
// without the entry layer. A block-compressed store is read through
// TTagmaBlockSource, a plain store from its mapping.
//
//   root -l -b -q 'tagma_block_read.C("/path/to/store.bin")'

#include "ROOT/TTagmaBlockSource.hxx"
#include "ROOT/TTagmaStore.hxx"
#include "ROOT/TTagmaWriter.hxx"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

void Report(const char *what, std::uint64_t bytes, double seconds)
{
   std::printf("tagma_block_read: %-8s %llu bytes  %.3f s  %.1f MB/s\n", what, static_cast<unsigned long long>(bytes),
               seconds, static_cast<double>(bytes) / 1e6 / seconds);
}

} // namespace

void tagma_block_read(const char *store, Long64_t chunk = 1 << 24)
{
   std::string why;
   ROOT::TTagmaBlockSource block;
   if (block.Open(store, &why)) {
      std::printf("tagma_block_read: %s blocks=%llu file=%llu payload=%llu\n", store,
                  static_cast<unsigned long long>(block.BlockCount()),
                  static_cast<unsigned long long>(block.FileBytes()),
                  static_cast<unsigned long long>(block.PayloadBytes()));
      const std::uint64_t payload = block.PayloadBytes();
      std::vector<char> buf(static_cast<std::size_t>(chunk));
      const auto start = std::chrono::steady_clock::now();
      for (std::uint64_t at = 0; at < payload;) {
         const std::uint64_t take = std::min<std::uint64_t>(buf.size(), payload - at);
         if (block.Read(buf.data(), at, take) < 0) {
            std::printf("tagma_block_read: read failed at %llu\n", static_cast<unsigned long long>(at));
            return;
         }
         at += take;
      }
      Report("block", payload, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
      return;
   }

   // Not block-compressed: the plain store, read from its mapping.
   ROOT::TTagmaStore::Layout layout;
   ROOT::TTagmaSchema schema;
   if (!ROOT::TTagmaWriter::ReadStore(store, &layout, &schema, &why)) {
      std::printf("tagma_block_read: %s is neither a plain nor a block-compressed store: %s\n", store, why.c_str());
      return;
   }
   auto plain = std::make_shared<ROOT::TTagmaStore>(layout);
   if (!plain->MapFile(store)) {
      std::printf("tagma_block_read: cannot map %s\n", store);
      return;
   }
   const std::uint64_t payload = plain->SizeBytes();
   const char *base = plain->GetMapped();
   std::vector<char> buf(static_cast<std::size_t>(chunk));
   const auto start = std::chrono::steady_clock::now();
   for (std::uint64_t at = 0; at < payload;) {
      const std::uint64_t take = std::min<std::uint64_t>(buf.size(), payload - at);
      std::memcpy(buf.data(), base + at, static_cast<std::size_t>(take));
      at += take;
   }
   Report("mapped", payload, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
}
