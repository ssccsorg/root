// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ROOT/TTagmaSchema.hxx"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace ROOT {

namespace {

// Parses a decimal unsigned integer. Rejects an empty string, a non-digit,
// and an overflow.
bool ParseUint(const std::string &text, std::uint64_t *value)
{
   if (text.empty())
      return false;
   std::uint64_t result = 0;
   for (char c : text) {
      if (c < '0' || c > '9')
         return false;
      const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
      if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
         return false;
      result = result * 10 + digit;
   }
   *value = result;
   return true;
}

} // namespace

std::uint64_t TTagmaSchema::SizeOf(EType type)
{
   switch (type) {
   case EType::kDouble:
   case EType::kInt64:
   case EType::kUInt64: return 8;
   case EType::kFloat:
   case EType::kInt32:
   case EType::kUInt32: return 4;
   case EType::kInt16:
   case EType::kUInt16: return 2;
   case EType::kInt8:
   case EType::kUInt8:
   case EType::kBool: return 1;
   }
   return 0;
}

const char *TTagmaSchema::LeafCode(EType type)
{
   switch (type) {
   case EType::kDouble: return "D";
   case EType::kFloat: return "F";
   case EType::kInt32: return "I";
   case EType::kUInt32: return "i";
   case EType::kInt64: return "L";
   case EType::kUInt64: return "l";
   case EType::kInt16: return "S";
   case EType::kUInt16: return "s";
   case EType::kInt8: return "B";
   case EType::kUInt8: return "b";
   case EType::kBool: return "O";
   }
   return "";
}

bool TTagmaSchema::ParseType(const std::string &name, EType *type)
{
   if (name == "double" || name == "Double_t")
      *type = EType::kDouble;
   else if (name == "float" || name == "Float_t")
      *type = EType::kFloat;
   else if (name == "int" || name == "Int_t" || name == "int32" || name == "int32_t")
      *type = EType::kInt32;
   else if (name == "uint" || name == "UInt_t" || name == "uint32" || name == "uint32_t" || name == "unsigned")
      *type = EType::kUInt32;
   else if (name == "long" || name == "Long64_t" || name == "longlong" || name == "int64" || name == "int64_t")
      *type = EType::kInt64;
   else if (name == "ulong" || name == "ULong64_t" || name == "ulonglong" || name == "uint64" || name == "uint64_t")
      *type = EType::kUInt64;
   else if (name == "short" || name == "Short_t" || name == "int16" || name == "int16_t")
      *type = EType::kInt16;
   else if (name == "ushort" || name == "UShort_t" || name == "uint16" || name == "uint16_t")
      *type = EType::kUInt16;
   else if (name == "char" || name == "Char_t" || name == "int8" || name == "int8_t")
      *type = EType::kInt8;
   else if (name == "uchar" || name == "UChar_t" || name == "uint8" || name == "uint8_t")
      *type = EType::kUInt8;
   else if (name == "bool" || name == "Bool_t")
      *type = EType::kBool;
   else
      return false;
   return true;
}

bool TTagmaSchema::IsIntegral(EType type)
{
   switch (type) {
   case EType::kInt32:
   case EType::kUInt32:
   case EType::kInt64:
   case EType::kUInt64:
   case EType::kInt16:
   case EType::kUInt16:
   case EType::kInt8:
   case EType::kUInt8: return true;
   case EType::kDouble:
   case EType::kFloat:
   case EType::kBool: return false;
   }
   return false;
}

std::uint64_t TTagmaSchema::Collection::ElementBytes() const
{
   std::uint64_t bytes = 0;
   for (const auto &field : fFields)
      bytes += field.Size();
   return bytes;
}

void TTagmaSchema::AddField(const Field &field)
{
   fFields.push_back(field);
   Rebuild();
}

void TTagmaSchema::Rebuild()
{
   fScalars.clear();
   fCollections.clear();
   for (const auto &field : fFields) {
      if (!field.IsArray()) {
         fScalars.push_back(field);
         continue;
      }
      auto match = std::find_if(fCollections.begin(), fCollections.end(),
                                [&field](const Collection &c) { return c.fCountField == field.fCountField; });
      if (match == fCollections.end()) {
         Collection collection;
         collection.fCountField = field.fCountField;
         fCollections.push_back(collection);
         match = fCollections.end() - 1;
      }
      match->fFields.push_back(field);
      match->fMaxCount = std::max(match->fMaxCount, field.fMaxCount);
   }
   // A count field can be declared after the array fields it bounds, so the
   // resolution runs on every rebuild.
   for (auto &collection : fCollections) {
      auto count = std::find_if(fScalars.begin(), fScalars.end(),
                                [&collection](const Field &field) { return field.fName == collection.fCountField; });
      if (count != fScalars.end()) {
         collection.fCountOffset = count->fOffset;
         collection.fCountType = count->fType;
      }
   }
}

std::uint64_t TTagmaSchema::ScalarExtent() const
{
   std::uint64_t extent = 0;
   for (const auto &field : fScalars)
      extent = std::max(extent, field.fOffset + field.Size());
   return extent;
}

std::uint64_t TTagmaSchema::IndexRecordSize() const
{
   // The data base field is present only when the schema carries a
   // collection, so a scalar-only schema keeps the flat record of the
   // fixed-width store.
   const std::uint64_t payload = ScalarExtent() + (HasCollections() ? sizeof(std::uint64_t) : 0);
   return (payload + 7) & ~static_cast<std::uint64_t>(7);
}

std::uint64_t TTagmaSchema::DataBaseOffset() const
{
   return HasCollections() ? IndexRecordSize() - sizeof(std::uint64_t) : IndexRecordSize();
}

std::uint64_t TTagmaSchema::ChunkOffset(std::size_t collectionIndex, const std::uint64_t *counts) const
{
   std::uint64_t offset = 0;
   for (std::size_t i = 0; i < collectionIndex && i < fCollections.size(); ++i)
      offset += counts[i] * fCollections[i].ElementBytes();
   return offset;
}

std::uint64_t
TTagmaSchema::FieldOffset(const Collection &collection, const std::string &fieldName, std::uint64_t count) const
{
   std::uint64_t base = 0;
   for (const auto &field : collection.fFields) {
      if (field.fName == fieldName)
         return base * count;
      base += field.Size();
   }
   return 0;
}

std::uint64_t TTagmaSchema::SliceBytes(const std::uint64_t *counts) const
{
   std::uint64_t bytes = 0;
   for (std::size_t i = 0; i < fCollections.size(); ++i)
      bytes += counts[i] * fCollections[i].ElementBytes();
   return bytes;
}

std::uint64_t TTagmaSchema::CountOf(std::size_t collectionIndex, const void *indexRecord) const
{
   if (collectionIndex >= fCollections.size() || indexRecord == nullptr)
      return 0;
   const Collection &collection = fCollections[collectionIndex];
   const char *base = static_cast<const char *>(indexRecord) + collection.fCountOffset;
   switch (collection.fCountType) {
   case EType::kUInt32: {
      std::uint32_t value = 0;
      std::memcpy(&value, base, sizeof(value));
      return value;
   }
   case EType::kInt32: {
      std::int32_t value = 0;
      std::memcpy(&value, base, sizeof(value));
      return value < 0 ? 0 : static_cast<std::uint64_t>(value);
   }
   case EType::kUInt64: {
      std::uint64_t value = 0;
      std::memcpy(&value, base, sizeof(value));
      return value;
   }
   case EType::kInt64: {
      std::int64_t value = 0;
      std::memcpy(&value, base, sizeof(value));
      return value < 0 ? 0 : static_cast<std::uint64_t>(value);
   }
   case EType::kUInt16: {
      std::uint16_t value = 0;
      std::memcpy(&value, base, sizeof(value));
      return value;
   }
   case EType::kInt16: {
      std::int16_t value = 0;
      std::memcpy(&value, base, sizeof(value));
      return value < 0 ? 0 : static_cast<std::uint64_t>(value);
   }
   case EType::kUInt8: {
      std::uint8_t value = 0;
      std::memcpy(&value, base, sizeof(value));
      return value;
   }
   case EType::kInt8: {
      std::int8_t value = 0;
      std::memcpy(&value, base, sizeof(value));
      return value < 0 ? 0 : static_cast<std::uint64_t>(value);
   }
   default: return 0;
   }
}

bool TTagmaSchema::AddLine(const std::string &line)
{
   std::istringstream stream(line);
   std::vector<std::string> tokens;
   std::string token;
   while (stream >> token)
      tokens.push_back(token);
   if (tokens.size() < 3 || tokens.size() > 5)
      return false;
   if (tokens[0].empty())
      return false;

   Field field;
   if (!ParseUint(tokens[1], &field.fOffset))
      return false;
   if (!ParseType(tokens[2], &field.fType))
      return false;
   field.fName = tokens[0];
   if (tokens.size() >= 4) {
      field.fCountField = tokens[3];
      if (field.fCountField.empty())
         return false;
      if (tokens.size() == 5 && !ParseUint(tokens[4], &field.fMaxCount))
         return false;
   }
   AddField(field);
   return true;
}

bool TTagmaSchema::Read(const char *path)
{
   if (path == nullptr || path[0] == '\0')
      return false;
   std::ifstream in(path);
   if (!in)
      return false;
   fFields.clear();
   Rebuild();
   std::string line;
   while (std::getline(in, line)) {
      std::istringstream probe(line);
      std::string first;
      probe >> first;
      if (first.empty() || first[0] == '#')
         continue;
      if (!AddLine(line)) {
         fFields.clear();
         Rebuild();
         return false;
      }
   }
   return !fFields.empty();
}

bool TTagmaSchema::Validate(std::uint64_t indexRecordSize, std::string *why) const
{
   const auto reject = [why](const std::string &reason) {
      if (why)
         *why = reason;
      return false;
   };
   if (fFields.empty())
      return reject("the schema holds no field");
   if (IndexRecordSize() > indexRecordSize)
      return reject("the index record needs " + std::to_string(IndexRecordSize()) + " bytes, the store holds " +
                    std::to_string(indexRecordSize));

   // The scalars live in the index record, which reserves the data base.
   std::vector<Field> ordered(fScalars);
   std::sort(ordered.begin(), ordered.end(), [](const Field &a, const Field &b) { return a.fOffset < b.fOffset; });
   std::uint64_t previousEnd = 0;
   for (const auto &field : ordered) {
      if (field.fName.empty())
         return reject("a field has an empty name");
      if (field.fOffset < previousEnd)
         return reject("scalar field " + field.fName + " overlaps the preceding field");
      // The index record size follows from this extent, so a field can only
      // exceed it by making the record larger than the store holds, which the
      // size check above already reports.
      previousEnd = field.fOffset + field.Size();
   }

   // Each collection is bound by a scalar field of integral type, and its
   // object fields do not overlap.
   for (const auto &collection : fCollections) {
      auto count = std::find_if(fScalars.begin(), fScalars.end(),
                                [&collection](const Field &field) { return field.fName == collection.fCountField; });
      if (count == fScalars.end())
         return reject("collection count field " + collection.fCountField + " is not a scalar field of the schema");
      if (!IsIntegral(count->fType))
         return reject("collection count field " + collection.fCountField + " is not integral");
      if (collection.fMaxCount == 0)
         return reject("collection " + collection.fCountField + " carries no maximum object count");
      std::vector<Field> fields(collection.fFields);
      std::sort(fields.begin(), fields.end(), [](const Field &a, const Field &b) { return a.fOffset < b.fOffset; });
      std::uint64_t objectEnd = 0;
      for (const auto &field : fields) {
         if (field.fOffset < objectEnd)
            return reject("array field " + field.fName + " overlaps the preceding field of its object");
         objectEnd = field.fOffset + field.Size();
      }
   }

   // Names are unique across the whole schema.
   for (std::size_t i = 0; i < fFields.size(); ++i)
      for (std::size_t j = i + 1; j < fFields.size(); ++j)
         if (fFields[i].fName == fFields[j].fName)
            return reject("field name " + fFields[i].fName + " repeats");
   return true;
}

} // namespace ROOT
