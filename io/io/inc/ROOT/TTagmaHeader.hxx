// Author: SSCCS Foundation 2026

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

#include "ROOT/TTagmaStore.hxx"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ROOT {

class TTagmaHeader {
public:
   static constexpr std::array<unsigned char, 8> kMagic = {'T', 'A', 'G', 'M', 'A', 'S', 'T', '0'};
   static constexpr std::uint32_t kVersion = 1;
   // magic (8) + version (4) + reserved (4) + run, lumi, event, record, data,
   // field table, checksum (7 * 8).
   static constexpr std::size_t kSize = 72;

   std::uint64_t fRunMax = 0;          // exclusive bound of the run axis
   std::uint64_t fLumiMax = 0;         // exclusive bound of the lumi axis
   std::uint64_t fEventMax = 0;        // exclusive bound of the event axis
   std::uint64_t fRecordSize = 0;      // bytes per index record
   std::uint64_t fDataSize = 0;        // bytes of the packed data region
   std::uint64_t fFieldTableBytes = 0; // bytes of the field table that precede the descriptor
   std::uint64_t fChecksum = 0;        // FNV-1a of the field table bytes

   // The layout the descriptor names.
   TTagmaStore::Layout Layout() const;

   // The 64-bit FNV-1a checksum of a byte range, the checksum the descriptor
   // carries of the field table.
   static std::uint64_t Checksum(const void *bytes, std::size_t len);

   // The descriptor as the kSize bytes a store ends with, little-endian.
   std::array<unsigned char, kSize> Serialize() const;

   // Parses a descriptor from exactly kSize bytes. Returns false, with the
   // reason in `why`, on a bad magic, an unknown version, a zero axis or
   // record size, or a short block.
   static bool Parse(const unsigned char *bytes, std::size_t len, TTagmaHeader *out, std::string *why = nullptr);
};

} // namespace ROOT

#endif // ROOT_TTagmaHeader
