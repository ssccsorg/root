// Author: SSCCS Initiative 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_TTagmaHeader
#define ROOT_TTagmaHeader

// TTagmaHeader: the self-describing descriptor of a coordinate-indexed store.
//
// The descriptor names the axis maxima, the index record size, the data
// region size, and the field table of a store, so a store carries its own
// layout and schema and no sidecar file is needed.
//
// It is written after the payload, so the index region stays at offset zero
// and the address arithmetic of TTagmaStore keeps its closed form:
//
//   [ index region ][ data region ][ field table ][ descriptor ]
//
// The descriptor is a fixed block at the very end of the file, so a reader
// reads the last kSize bytes, validates the magic and the version, and reads
// the field table of fFieldTableBytes bytes immediately before it. The
// checksum covers the field table, the part of the store that has to agree
// with the payload the store addresses.
//
// The block grew once. Versions 1 and 2 are the descriptor an earlier build
// wrote, without the layout fields and kLegacySize bytes long; versions 3 and 4
// carry them and are kSize. The magic opens the block, so a reader that tries
// the sizes this build reads, newest first, tells the generations apart without
// a sidecar and without reading the version first.

#include "ROOT/TTagmaStore.hxx"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ROOT {

class TTagmaHeader {
public:
   static constexpr std::array<unsigned char, 8> kMagic = {'T', 'A', 'G', 'M', 'A', 'S', 'T', '0'};
   // The descriptor this build writes carries the layout fields and is kSize
   // bytes. Version 3 is the plain store and version 4 the block-compressed one,
   // so a reader that expects the plain form rejects the compressed one and the
   // decompressing source recognizes it, as the earlier pair did.
   static constexpr std::uint32_t kVersion = 3;
   static constexpr std::uint32_t kCompressedVersion = 4;
   // The versions an earlier build wrote, which this one still reads: the same
   // descriptor without the layout fields, and kLegacySize bytes.
   static constexpr std::uint32_t kLegacyVersion = 1;
   static constexpr std::uint32_t kLegacyCompressedVersion = 2;
   // magic (8) + version (4) + reserved (4) + run, lumi, event, record, data,
   // field table, checksum, layout kind, block events (9 * 8).
   static constexpr std::size_t kSize = 88;
   // The block an earlier build wrote: the same descriptor without the layout
   // fields, ending at the checksum.
   static constexpr std::size_t kLegacySize = 72;

   // The event-major layout every earlier store has: one index record per event
   // and its packed slice after it. The descriptor may name another kind; the
   // readers refuse a store their address arithmetic cannot reach, as they
   // already refuse a compressed store a plain reader cannot read.
   static constexpr std::uint64_t kEventMajorLayout = 0;
   // A data region transposed by field inside blocks of fBlockEvents events. A
   // store that carries one addresses it through the block table of
   // TTagmaStore::BlockTable, and a reader that only walks the event-major
   // layout refuses it.
   static constexpr std::uint64_t kColumnarBlockLayout = 1;

   // True when the version is one this build reads.
   static constexpr bool IsKnown(std::uint32_t version)
   {
      return version == kVersion || version == kCompressedVersion || version == kLegacyVersion ||
             version == kLegacyCompressedVersion;
   }

   // True when the version marks a block-compressed store.
   static constexpr bool IsCompressed(std::uint32_t version)
   {
      return version == kCompressedVersion || version == kLegacyCompressedVersion;
   }

   // The bytes a descriptor of this version occupies, or 0 when the version is
   // not one this build reads. A reader locates the block by trying kSize and
   // kLegacySize at the end of the file, because the magic tells the two apart.
   static constexpr std::size_t SizeOf(std::uint32_t version)
   {
      if (version == kVersion || version == kCompressedVersion)
         return kSize;
      if (version == kLegacyVersion || version == kLegacyCompressedVersion)
         return kLegacySize;
      return 0;
   }

   std::uint32_t fVersion = kVersion;  // kVersion, or kCompressedVersion for a block-compressed store
   std::uint32_t fBlockShift = 0;      // log2 of a block-compressed store's block size
   std::uint64_t fRunMax = 0;          // exclusive bound of the run axis
   std::uint64_t fLumiMax = 0;         // exclusive bound of the lumi axis
   std::uint64_t fEventMax = 0;        // exclusive bound of the event axis
   std::uint64_t fRecordSize = 0;      // bytes per index record
   std::uint64_t fDataSize = 0;        // bytes of the packed data region
   std::uint64_t fFieldTableBytes = 0; // bytes of the field table that precede the descriptor
   std::uint64_t fChecksum = 0;        // FNV-1a of the field table bytes
   std::uint64_t fLayoutKind = kEventMajorLayout; // how the data region is laid out across events
   std::uint64_t fBlockEvents = 0;                // events per columnar block, 0 for no block division

   // The layout the descriptor names.
   TTagmaStore::Layout Layout() const;

   // The 64-bit FNV-1a checksum of a byte range, the checksum the descriptor
   // carries of the field table.
   static std::uint64_t Checksum(const void *bytes, std::size_t len);

   // The descriptor as the kSize bytes a store ends with, little-endian, for the
   // version it carries. A legacy version is read, never written here.
   std::array<unsigned char, kSize> Serialize() const;

   // Parses a descriptor from the bytes of its block. `len` is the block the
   // caller read, so a legacy block and the current one are both accepted at
   // their own length and a block read at any other length is refused rather than
   // parsed from the wrong offsets. Returns false, with the reason in `why`, on a
   // bad magic, an unknown version, a mis-sized block, a zero axis or record
   // size, an unknown layout kind, or a block length that disagrees with the
   // kind.
   static bool Parse(const unsigned char *bytes, std::size_t len, TTagmaHeader *out, std::string *why = nullptr);
};

} // namespace ROOT

#endif // ROOT_TTagmaHeader
