// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_TTagmaWriter
#define ROOT_TTagmaWriter

// TTagmaWriter: writes a coordinate-indexed store that carries collections.
//
// The store has two regions, both described by the schema (TTagmaSchema).
// The index region holds one fixed-width index record per event, addressed
// arithmetically, at index times index record size. Each record carries the
// scalar fields, the object count of every collection in its count field, and
// the 64-bit offset of the event's slice in the data region. The data region
// packs the collections field by field, so the elements of one field are
// contiguous and the offset of a collection inside a slice follows from the
// object counts.
//
// The writer produces both regions in a single pass over the caller's events
// and buffers neither: with the events in index order, the base of the current
// event's slice is the end of the data region so far, and the index record of
// the event is written at index times index record size. AddEvent therefore
// writes the record and the slice of one event at their respective offsets as
// the event arrives.
//
//   TTagmaWriter writer(schema, entries);
//   writer.Open(path);
//   for (event = 0; event < entries; ++event)
//      writer.AddEvent(scalars, slice);
//   writer.Close();
//
// `scalars` points at the scalar region of the event, with every scalar field
// at its schema offset, the count fields among them; the writer reads the
// counts and the slice size from it. `slice` points at the event's packed data
// region, sized by the counts. A schema that carries no collection has no data
// region and no base field, and AddEvent ignores `slice`.
//
// The store the writer produces is the store the reader consumes:
// TTagmaStore addresses the records, TFile serves them, and TTree materializes
// the schema over them.

#include "ROOT/TTagmaSchema.hxx"
#include "ROOT/TTagmaStore.hxx"

#include <cstdint>
#include <cstdio>
#include <string>

namespace ROOT {

class TTagmaWriter {
public:
   // `entries` events along a single run/lumi axis. Throws
   // std::invalid_argument when the count is zero, the schema is invalid, or
   // the index region would overflow.
   TTagmaWriter(const TTagmaSchema &schema, std::uint64_t entries);
   ~TTagmaWriter();

   TTagmaWriter(const TTagmaWriter &) = delete;
   TTagmaWriter &operator=(const TTagmaWriter &) = delete;

   std::uint64_t Entries() const { return fEntries; }
   std::uint64_t IndexRecordSize() const { return fIndexRecordSize; }
   std::uint64_t IndexBytes() const { return fEntries * fIndexRecordSize; }

   // Bytes of the data region written so far. Fixed once every event has been
   // added.
   std::uint64_t DataSize() const { return fDataSize; }

   // Creates `path` for writing. Returns false when the writer already holds
   // an open file, or when the file cannot be created.
   bool Open(const char *path);

   // Writes the next event, in index order. `scalars` supplies the scalar
   // region and `slice` the packed data region. Returns false, without
   // consuming the event, when no file is open, when the events are already
   // exhausted, when a count exceeds the collection maximum, when the slice
   // is null for a nonempty slice, or on a write failure.
   bool AddEvent(const void *scalars, const void *slice);

   // Flushes and closes the file. Returns false when the writer holds no open
   // file, when a write failed, or when fewer than `entries` events were
   // added. On the complete path the writer first appends the field table and
   // the descriptor TTagmaHeader, so the store is self-describing.
   bool Close();

   // Reads the descriptor a store written by the writer ends with, so a store
   // carries its own layout and schema and needs no sidecar file. Returns
   // false, with the reason in `why`, when the file is absent or short, the
   // descriptor is malformed, the field table does not match its checksum, or
   // the descriptor disagrees with the file size.
   static bool
   ReadStore(const char *path, TTagmaStore::Layout *layout, TTagmaSchema *schema, std::string *why = nullptr);

   // The layout the written store carries, for a TTagmaStore and the read
   // path. The record size is the schema's index record size and the data
   // size accumulates as events are added, so call it after the last
   // AddEvent.
   TTagmaStore::Layout GetLayout() const;

private:
   bool WriteDescriptor();

   TTagmaSchema fSchema;
   std::uint64_t fEntries = 0;
   std::uint64_t fIndexRecordSize = 0;
   std::uint64_t fDataSize = 0;
   std::uint64_t fAdded = 0;
   std::FILE *fFile = nullptr;
};

} // namespace ROOT

#endif // ROOT_TTagmaWriter
