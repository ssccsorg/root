// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_TTagmaSchema
#define ROOT_TTagmaSchema

// TTagmaSchema: the field table of a coordinate-indexed store.
//
// The store carries an event in two regions. The index record is fixed
// width and addressed arithmetically, at index times index record size. It
// holds the scalar fields and, at the end, the 64-bit offset of the event's
// slice of the data region. The data region packs the variable-length
// collections, each collection laid out field by field, so the elements of
// one field are contiguous and the offset of a collection inside the slice
// follows from the object counts by arithmetic.
//
// A fixed-width record cannot carry a variable-length collection without
// either padding a slot by the collection maximum or truncating the
// collection to its leading element. Measured on a CMS NanoAOD file, the
// padding route costs 11.1x the bytes of the packed route, since the mean
// object counts are a fifth to a tenth of the maximum.
//
// The text form is the layout sidecar, one field per line:
//
//   <name> <offset> <type>            a scalar field, offset in the index record
//   <name> <offset> <type> <count> [<maxCount>]   an array field, offset in one
//                                     object of the collection bound by <count>
//
// Fields of one collection are grouped by their count field, in order of
// first appearance, and the collection's object size is the end of its last
// field.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ROOT {

class TTagmaSchema {
public:
   enum class EType {
      kDouble,
      kFloat,
      kInt32,
      kUInt32,
      kInt64,
      kUInt64,
      kInt16,
      kUInt16,
      kInt8,
      kUInt8,
      kBool
   };

   struct Field {
      std::string fName;           // field name, unique within the schema
      std::string fCountField;     // empty for a scalar field; the count field for an array field
      std::uint64_t fOffset = 0;   // scalar: offset in the index record
                                   // array: offset in one object of the collection, which
                                   // fixes the field order and the overlap check; the store
                                   // packs the fields of a collection tightly in that order
      std::uint64_t fMaxCount = 0; // array: the greatest object count the store carries
      EType fType = EType::kDouble;
      bool IsArray() const { return !fCountField.empty(); }
      std::uint64_t Size() const { return SizeOf(fType); }
   };

   // One variable-length collection: the scalar field that bounds it, the
   // greatest object count the store carries for it, and the fields of one
   // object in schema order.
   struct Collection {
      std::string fCountField;
      std::uint64_t fMaxCount = 0;
      std::uint64_t fCountOffset = 0; // resolved from the count scalar field
      EType fCountType = EType::kUInt32;
      std::vector<Field> fFields;
      // Bytes of one object as the store packs it: the fields carry no
      // padding, so this is the sum of the field sizes.
      std::uint64_t ElementBytes() const;
   };

   static std::uint64_t SizeOf(EType type);
   static const char *LeafCode(EType type);
   static bool ParseType(const std::string &name, EType *type);
   // True for the types a count field may use.
   static bool IsIntegral(EType type);

   void AddField(const Field &field);

   const std::vector<Field> &GetFields() const { return fFields; }
   const std::vector<Field> &GetScalars() const { return fScalars; }
   const std::vector<Collection> &GetCollections() const { return fCollections; }
   bool IsEmpty() const { return fFields.empty(); }
   bool HasCollections() const { return !fCollections.empty(); }

   // End of the last scalar field in the index record, before the data base.
   std::uint64_t ScalarExtent() const;

   // Size of the index record: the scalar region plus the 64-bit data base,
   // aligned to eight bytes.
   std::uint64_t IndexRecordSize() const;

   // Offset of the data base field inside the index record.
   std::uint64_t DataBaseOffset() const;

   // Byte offset of a collection's chunk inside an event's data slice, from
   // the object counts in GetCollections() order. The layout is field by
   // field, so the fields of a collection follow each other and each one is
   // contiguous.
   std::uint64_t ChunkOffset(std::size_t collectionIndex, const std::uint64_t *counts) const;

   // Byte offset of a field's elements inside its collection's chunk, from
   // the object count of that collection.
   std::uint64_t FieldOffset(const Collection &collection, const std::string &fieldName, std::uint64_t count) const;

   // Total bytes of an event's data slice for the given counts.
   std::uint64_t SliceBytes(const std::uint64_t *counts) const;

   // Reads a collection's object count out of an index record.
   std::uint64_t CountOf(std::size_t collectionIndex, const void *indexRecord) const;

   // Parses one field line. Returns false on a malformed line, an unknown
   // type, or a trailing token.
   bool AddLine(const std::string &line);

   // Reads the layout sidecar at `path`. Blank lines and lines whose first
   // non-blank character is '#' are ignored. Returns false when the file
   // cannot be read or holds no valid field.
   bool Read(const char *path);

   // Rejects a schema that is empty, whose index record does not fit the
   // store, whose scalar or object fields overlap, whose collection names an
   // unknown or non-integral count field or carries no maximum object count,
   // which repeats a name, or which holds an empty name. `why` receives the
   // reason.
   bool Validate(std::uint64_t indexRecordSize, std::string *why = nullptr) const;

private:
   void Rebuild();

   std::vector<Field> fFields;
   std::vector<Field> fScalars;
   std::vector<Collection> fCollections;
};

} // namespace ROOT

#endif // ROOT_TTagmaSchema
