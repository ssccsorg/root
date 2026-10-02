// Author: SSCCS Initiative 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_TTagmaBlockSource
#define ROOT_TTagmaBlockSource

// TTagmaBlockSource: the byte source of a block-compressed store.
//
// The store the writer produces is uncompressed, so its reads move the whole
// payload and the comparison against a compressed file overstates the store.
// This source keeps the addressing of the store and compresses the bytes under
// it: the payload is split into fixed blocks, each block is compressed on its
// own, and a read decompresses the blocks it touches. The address arithmetic
// is untouched, so the file hook, the branches, and the leaves do not know the
// difference.
//
// A block-compressed store is the compressed blocks, then the block table that
// names each block's offset and sizes, then the field table and the descriptor
// of the plain store, at TTagmaHeader::kCompressedVersion. The payload the
// descriptor names is the uncompressed size, so a reader computes the block
// count and the table's place from it and the fixed block size.

#include "ROOT/TTagmaSchema.hxx"
#include "ROOT/TTagmaSource.hxx"
#include "ROOT/TTagmaStore.hxx"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ROOT {

class TTagmaBlockSource final : public TTagmaSource {
public:
   // Bytes of the payload one block holds before compression, the default the
   // writer uses. A caller trades the ratio against the decode amplification of
   // a record read by passing a shift to Compress; the store records it, so a
   // reader needs no setting.
   static constexpr std::uint32_t kDefaultBlockShift = 18;
   static constexpr std::uint64_t kBlockBytes = 1ull << kDefaultBlockShift;

   TTagmaBlockSource() = default;
   ~TTagmaBlockSource() override;
   TTagmaBlockSource(const TTagmaBlockSource &) = delete;
   TTagmaBlockSource &operator=(const TTagmaBlockSource &) = delete;

   // Opens a block-compressed store, reading the descriptor, the block table,
   // and the field table. Returns false, with the reason in `why`, when the file
   // is not a block-compressed store or its tables disagree with the descriptor.
   bool Open(const std::string &path, std::string *why = nullptr);
   bool IsOpen() const { return fFile != nullptr; }
   void Close();

   std::int64_t Read(char *buf, std::uint64_t pos, std::uint64_t len) override;
   bool IsMapped() const override { return false; }

   const TTagmaStore::Layout &GetLayout() const { return fLayout; }
   const TTagmaSchema &GetSchema() const { return fSchema; }
   // Bytes of the payload one block holds, from the store's descriptor.
   std::uint64_t BlockBytes() const { return 1ull << fBlockShift; }
   // Bytes of the payload the source serves, uncompressed.
   std::uint64_t PayloadBytes() const { return fPayloadBytes; }
   std::uint64_t BlockCount() const { return fBlockCount; }
   // Bytes the file holds, against the payload it serves, so a caller sees what
   // the compression bought.
   std::uint64_t FileBytes() const { return fFileBytes; }

   // Writes `outPath` as the block-compressed form of the plain store `inPath`.
   // A block that does not shrink is stored whole, so the file is never larger
   // than the payload it carries plus the tables. Returns false, with the reason
   // in `why`, when the input cannot be read or the output cannot be written.
   static bool Compress(const std::string &inPath, const std::string &outPath, std::string *why = nullptr,
                        std::uint32_t blockShift = kDefaultBlockShift);

private:
   // One block: where its bytes are in the file, how many are stored there, and
   // how many they decompress to.
   struct Block {
      std::uint64_t fOffset = 0;
      std::uint64_t fStoredBytes = 0;
      std::uint64_t fUncompressedBytes = 0;
   };

   // The uncompressed bytes of a block, decompressed into a retained buffer
   // that is memoized, so reads that stay inside one block decode it once.
   // Returns nullptr when the block cannot be read.
   const char *Load(std::uint64_t index, const Block &block);

   std::FILE *fFile = nullptr;
   TTagmaStore::Layout fLayout;
   TTagmaSchema fSchema;
   std::uint64_t fPayloadBytes = 0;
   std::uint64_t fFileBytes = 0;
   std::uint64_t fBlockCount = 0;
   std::vector<Block> fBlocks;
   std::uint32_t fBlockShift = kDefaultBlockShift; // from the descriptor, log2 of the block size
   std::vector<char> fBlockData;           // the decoded block, retained
   std::uint64_t fLoadedBlock = kNoBlock;  // which block fBlockData holds
   std::vector<unsigned char> fCompressed; // the compressed bytes of a block, reused

   static constexpr std::uint64_t kNoBlock = ~0ull;
};

} // namespace ROOT

#endif // ROOT_TTagmaBlockSource
