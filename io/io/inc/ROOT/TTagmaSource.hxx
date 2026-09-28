// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_TTagmaSource
#define ROOT_TTagmaSource

// TTagmaSource: the byte source behind the coordinate read path.
//
// The read path turns a record index into a byte range with TTagmaStore and
// asks the source for the range. The source decides how the bytes are
// obtained, so a mapping, a positioned read, and a later block-compressed or
// block-cached source are implementations rather than branches inside the file
// hook: the hook delegates, and a new byte source is a new implementation.
//
// TTagmaMappedSource copies from a read-only mapping another owner holds, so a
// read is a memory copy and issues no read system call.
// TTagmaPositionedSource delegates to a caller-supplied primitive, so the file
// keeps the system call and the accounting that goes with it.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ROOT {

class TTagmaSource {
public:
   virtual ~TTagmaSource() = default;

   // Fills `buf` with `len` bytes at store offset `pos`. Returns `len` on
   // success, or -1 when the source cannot serve the range.
   virtual std::int64_t Read(char *buf, std::uint64_t pos, std::uint64_t len) = 0;

   // True when a read copies from memory and issues no read system call.
   virtual bool IsMapped() const = 0;
};

// Copies from a read-only mapping another owner holds. The mapping covers the
// store extent, so every in-bounds range resolves to a plain memory copy.
class TTagmaMappedSource final : public TTagmaSource {
public:
   explicit TTagmaMappedSource(const char *base) : fBase(base) {}

   std::int64_t Read(char *buf, std::uint64_t pos, std::uint64_t len) override
   {
      std::memcpy(buf, fBase + pos, static_cast<std::size_t>(len));
      return static_cast<std::int64_t>(len);
   }

   bool IsMapped() const override { return true; }

private:
   const char *fBase = nullptr;
};

// Delegates to a caller-supplied primitive, so the owner of the file keeps the
// positioned read, the system call, and the retry on an interrupted call.
class TTagmaPositionedSource final : public TTagmaSource {
public:
   using ReadFn = std::function<std::int64_t(char *buf, std::uint64_t pos, std::uint64_t len)>;

   explicit TTagmaPositionedSource(ReadFn read) : fRead(std::move(read)) {}

   std::int64_t Read(char *buf, std::uint64_t pos, std::uint64_t len) override { return fRead(buf, pos, len); }

   bool IsMapped() const override { return false; }

private:
   ReadFn fRead;
};

// Wraps another source and holds the blocks it has read, so a range another
// read already served is a memory copy. A caller sizes the cache: the source
// holds at most `blocks` blocks of `blockBytes` bytes and evicts the least
// recently used one when it is full, so a bounded cache holds the reuse the
// caller expects and no more. `payloadBytes` bounds the last block. A miss asks
// the delegate for the whole block, so the reads the delegate sees are the
// blocks the store is read in, not the ranges the caller asks for.
class TTagmaCachedSource final : public TTagmaSource {
public:
   TTagmaCachedSource(std::shared_ptr<TTagmaSource> delegate, std::uint64_t blockBytes, std::size_t blocks,
                      std::uint64_t payloadBytes)
      : fDelegate(std::move(delegate)), fBlockBytes(blockBytes), fCapacity(blocks), fPayloadBytes(payloadBytes)
   {
      if (fDelegate == nullptr || fBlockBytes == 0 || fCapacity == 0 || fPayloadBytes == 0)
         throw std::invalid_argument("TTagmaCachedSource: a missing delegate, a zero block size or capacity, or an "
                                     "empty payload");
   }

   std::int64_t Read(char *buf, std::uint64_t pos, std::uint64_t len) override
   {
      if (buf == nullptr || pos >= fPayloadBytes)
         return -1;
      len = std::min(len, fPayloadBytes - pos);
      std::uint64_t at = pos;
      std::uint64_t left = len;
      char *out = buf;
      while (left > 0) {
         const std::uint64_t within = at % fBlockBytes;
         const Block *cached = Load(at / fBlockBytes);
         if (cached == nullptr)
            return -1;
         const std::uint64_t take = std::min(left, cached->fData.size() - within);
         if (take == 0)
            return -1;
         std::memcpy(out, cached->fData.data() + within, static_cast<std::size_t>(take));
         out += take;
         at += take;
         left -= take;
      }
      return static_cast<std::int64_t>(len);
   }

   bool IsMapped() const override { return false; }

   std::uint64_t Hits() const { return fHits; }
   std::uint64_t Misses() const { return fMisses; }
   std::size_t Cached() const { return fBlocks.size(); }

private:
   struct Block {
      std::vector<char> fData;
      std::list<std::uint64_t>::iterator fLru;
   };

   // The cached block, loaded from the delegate on a miss. Moves the block to
   // the front of the recency list and evicts the back when the cache is full.
   const Block *Load(std::uint64_t block)
   {
      const auto found = fBlocks.find(block);
      if (found != fBlocks.end()) {
         ++fHits;
         fLru.splice(fLru.begin(), fLru, found->second.fLru);
         return &found->second;
      }
      ++fMisses;
      const std::uint64_t start = block * fBlockBytes;
      const std::uint64_t size = std::min(fBlockBytes, fPayloadBytes - start);
      std::vector<char> data(static_cast<std::size_t>(size));
      const std::int64_t got = fDelegate->Read(data.data(), start, size);
      if (got < 0 || static_cast<std::uint64_t>(got) != size)
         return nullptr;
      if (fBlocks.size() >= fCapacity) {
         fBlocks.erase(fLru.back());
         fLru.pop_back();
      }
      fLru.push_front(block);
      const auto inserted = fBlocks.emplace(block, Block{std::move(data), fLru.begin()});
      return &inserted.first->second;
   }

   std::shared_ptr<TTagmaSource> fDelegate;
   std::uint64_t fBlockBytes = 0;
   std::size_t fCapacity = 0;
   std::uint64_t fPayloadBytes = 0;
   std::list<std::uint64_t> fLru; // front is the most recently used block
   std::unordered_map<std::uint64_t, Block> fBlocks;
   std::uint64_t fHits = 0;
   std::uint64_t fMisses = 0;
};

} // namespace ROOT

#endif // ROOT_TTagmaSource
