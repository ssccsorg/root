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
// reader caches one block's table entry and the part of the block it needs, then
// reconstructs each of the block's events from them. A run start is relative to
// the block's data base, so a span is a range of the block, identified by the
// block-relative offset it begins at.

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
// `within` is the event's position inside that block. `firstField` and
// `lastField` are the fields to reconstruct, both inclusive, in the order the
// data region holds them: a reader passes the range its enabled branches fall in,
// which is what lets it read that range's region of the block instead of the
// whole block. The chunks of the other fields are left as they are, as the
// event-major reader leaves them.
//
// `span` holds the block's bytes from `spanStart` for `spanBytes`, both relative
// to the block's data base. Returns false when a reconstructed field's run falls
// outside the span or its chunk falls outside the slice, which a store written by
// TTagmaWriter cannot do, so a false here is a refusal rather than a partial fill.
inline bool ReconstructTagmaSlice(const TTagmaSchema &schema, const TTagmaStore::BlockTable &table,
                                  const unsigned char *entry, std::uint64_t within, const std::uint64_t *counts,
                                  std::size_t firstField, std::size_t lastField, const unsigned char *span,
                                  std::uint64_t spanStart, std::uint64_t spanBytes, char *slice)
{
   const auto &collections = schema.GetCollections();
   if (entry == nullptr || slice == nullptr || counts == nullptr || within >= table.fBlockEvents)
      return false;
   if (span == nullptr && spanBytes != 0)
      return false;
   if (collections.empty() || firstField > lastField || lastField >= schema.DataFieldCount())
      return false;
   const std::uint64_t sliceBytes = schema.SliceBytes(counts);
   const std::uint64_t spanEnd = spanStart + spanBytes;
   if (spanEnd < spanStart)
      return false;
   for (std::size_t c = 0; c < collections.size(); ++c) {
      const TTagmaSchema::Collection &collection = collections[c];
      const std::uint64_t fieldStart = schema.DataFieldStart(c);
      if (fieldStart > lastField || fieldStart + collection.fFields.size() <= firstField)
         continue;
      const std::uint64_t chunk = schema.ChunkOffset(c, counts);
      const std::uint64_t count = counts[c];
      // Objects of this collection before the event, so the prefix is the offset
      // of the event's first object inside the field's run.
      const std::uint32_t prefix = table.ObjectPrefixInEntry(entry, c, within);
      // The fields of a collection follow each other inside its chunk, so the
      // offset into the chunk accumulates along the field order instead of being
      // looked up by name for every field of every event.
      std::uint64_t inChunk = 0;
      for (std::size_t f = 0; f < collection.fFields.size(); ++f, inChunk += count * collection.fFields[f].Size()) {
         const std::size_t fieldIndex = fieldStart + f;
         if (fieldIndex < firstField || fieldIndex > lastField)
            continue;
         const TTagmaSchema::Field &field = collection.fFields[f];
         const std::uint64_t runStart = table.RunStartInEntry(entry, fieldIndex);
         const std::uint64_t length = count * field.Size();
         if (length != 0) {
            const std::uint64_t from = runStart + prefix * field.Size();
            const std::uint64_t into = chunk + inChunk;
            // Subtraction form, so no sum wraps before it is compared, and the
            // span bound, because the span may be one range's region.
            if (from < runStart || from < spanStart || from > spanEnd || length > spanEnd - from)
               return false;
            if (into > sliceBytes || length > sliceBytes - into)
               return false;
            std::memcpy(slice + into, span + (from - spanStart), static_cast<std::size_t>(length));
         }
      }
   }
   return true;
}

} // namespace ROOT

#endif // ROOT_TTagmaColumn
