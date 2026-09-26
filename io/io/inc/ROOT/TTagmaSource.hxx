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

#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>

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

} // namespace ROOT

#endif // ROOT_TTagmaSource
