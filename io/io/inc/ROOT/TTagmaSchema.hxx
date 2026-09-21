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

// TTagmaSchema: the field table of a coordinate-indexed store record.
//
// A fixed-width record is an array of typed fields at fixed byte offsets.
// The schema names those fields, so that the tree-level read path can
// materialize a branch per field over the record bytes and ordinary leaf
// access reads store-backed values. It is the in-library form of the
// layout sidecar written by the preparation tool.
//
// The text form is one field per line, "<name> <offset> <type>", where
// <type> is a single-token type name. Unknown types are rejected; the
// supported set is the fixed-width scalar set ROOT leaf codes cover.

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
      std::string fName;            // field name, unique within the schema
      std::uint64_t fOffset = 0;    // byte offset of the field in the record
      EType fType = EType::kDouble; // fixed-width scalar type
      std::uint64_t Size() const { return SizeOf(fType); }
   };

   // Storage size of a type in bytes.
   static std::uint64_t SizeOf(EType type);

   // ROOT leaflist code of a type: 'D', 'F', 'I', 'i', 'L', 'l', 'S',
   // 's', 'B', 'b', 'O'.
   static const char *LeafCode(EType type);

   // Maps a single-token type name to a type. Accepts the plain C++ names
   // and the ROOT typedef names.
   static bool ParseType(const std::string &name, EType *type);

   // Appends a field without validating it; Validate is the check.
   void AddField(const Field &field);

   const std::vector<Field> &GetFields() const { return fFields; }
   bool IsEmpty() const { return fFields.empty(); }

   // End of the last field in bytes.
   std::uint64_t Extent() const;

   // Parses one "<name> <offset> <type>" line. Returns false on a
   // malformed line, an unknown type, or a trailing token.
   bool AddLine(const std::string &line);

   // Reads the layout sidecar at `path`. Blank lines and lines whose
   // first non-blank character is '#' are ignored. Returns false when the
   // file cannot be read or holds no valid field.
   bool Read(const char *path);

   // Rejects a schema that is empty, whose fields overlap or fall outside
   // `recordSize`, which repeats a name, or which holds an empty name.
   // `why` receives the reason when the schema is rejected.
   bool Validate(std::uint64_t recordSize, std::string *why = nullptr) const;

private:
   std::vector<Field> fFields;
};

}  // namespace ROOT

#endif  // ROOT_TTagmaSchema
