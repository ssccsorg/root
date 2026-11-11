// Author: SSCCS Initiative 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_TTagmaColumn
#define ROOT_TTagmaColumn

// TTagmaColumn: the addressing a columnar store's data region needs.
//
// A block of a columnar store holds one run per field, so the values of one
// event are not contiguous. Reconstructing an event's slice therefore takes two
// lookups per field: the run's start inside the block, and the objects of that
// collection that precede the event, both from the block's table entry. The
// result is written where the event-major layout would have put it, so the field
// copy that consumes the slice does not change with the layout.
//
// The file reading is the caller's: this header addresses bytes it is given. A
// reader caches one block's table entry and data span, then reconstructs each of
// the block's events from them. A run start is relative to the block's data base,
// so a span is the block's data region alone rather than the whole data region.

#include "ROOT/TTagmaSchema.hxx"
#include "ROOT/TTagmaStore.hxx"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ROOT {

// Writes the data slice of one event of a columnar store into `slice`, which must
// hold `schema.SliceBytes(counts)` bytes.
//
// `table` is the geometry the block table was written with and `entry` is the
// table entry of the block the event falls in, which a reader caches per block.
// `within` is the event's position inside that block. `span` is the block's data
// region in memory and `spanBytes` its length; the table's run starts are offsets
// into it. Returns false when a field's run falls outside the span or the slice,
// which a store written by TTagmaWriter cannot do, so a false here is a refusal
// rather than a partial fill.
inline bool ReconstructTagmaSlice(const TTagmaSchema &schema, const TTagmaStore::BlockTable &table,
                                  const unsigned char *entry, std::uint64_t within, const std::uint64_t *counts,
                                  const unsigned char *span, std::uint64_t spanBytes, char *slice)
{
   if (entry == nullptr || span == nullptr || slice == nullptr || counts == nullptr ||
       within >= table.fBlockEvents)
      return false;
   const auto &collections = schema.GetCollections();
   const std::uint64_t sliceBytes = schema.SliceBytes(counts);
   std::uint64_t fieldIndex = 0;
   for (std::size_t c = 0; c < collections.size(); ++c) {
      const TTagmaSchema::Collection &collection = collections[c];
      const std::uint64_t chunk = schema.ChunkOffset(c, counts);
      const std::uint64_t count = counts[c];
      // Objects of this collection before the event, so the prefix is the offset
      // of the event's first object inside the field's run.
      const std::uint32_t prefix = table.ObjectPrefixInEntry(entry, c, within);
      for (const TTagmaSchema::Field &field : collection.fFields) {
         const std::uint64_t runStart = table.RunStartInEntry(entry, fieldIndex);
         const std::uint64_t length = count * field.Size();
         if (length != 0) {
            const std::uint64_t from = runStart + prefix * field.Size();
            const std::uint64_t into = chunk + schema.FieldOffset(collection, field.fName, count);
            // Subtraction form, so neither sum can wrap before it is compared.
            if (from < runStart || from > spanBytes || length > spanBytes - from)
               return false;
            if (into > sliceBytes || length > sliceBytes - into)
               return false;
            std::memcpy(slice + into, span + from, static_cast<std::size_t>(length));
         }
         ++fieldIndex;
      }
   }
   return true;
}

} // namespace ROOT

#endif // ROOT_TTagmaColumn
