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
#include <fstream>
#include <sstream>

namespace ROOT {

std::uint64_t TTagmaSchema::SizeOf(EType type)
{
   switch (type) {
   case EType::kDouble:
   case EType::kInt64:
   case EType::kUInt64:
      return 8;
   case EType::kFloat:
   case EType::kInt32:
   case EType::kUInt32:
      return 4;
   case EType::kInt16:
   case EType::kUInt16:
      return 2;
   case EType::kInt8:
   case EType::kUInt8:
   case EType::kBool:
      return 1;
   }
   return 0;
}

const char *TTagmaSchema::LeafCode(EType type)
{
   switch (type) {
   case EType::kDouble:
      return "D";
   case EType::kFloat:
      return "F";
   case EType::kInt32:
      return "I";
   case EType::kUInt32:
      return "i";
   case EType::kInt64:
      return "L";
   case EType::kUInt64:
      return "l";
   case EType::kInt16:
      return "S";
   case EType::kUInt16:
      return "s";
   case EType::kInt8:
      return "B";
   case EType::kUInt8:
      return "b";
   case EType::kBool:
      return "O";
   }
   return "";
}

bool TTagmaSchema::ParseType(const std::string &name, EType *type)
{
   if (name == "double" || name == "Double_t")
      *type = EType::kDouble;
   else if (name == "float" || name == "Float_t")
      *type = EType::kFloat;
   else if (name == "int" || name == "Int_t" || name == "int32" ||
            name == "int32_t")
      *type = EType::kInt32;
   else if (name == "uint" || name == "UInt_t" || name == "uint32" ||
            name == "uint32_t" || name == "unsigned")
      *type = EType::kUInt32;
   else if (name == "long" || name == "Long64_t" || name == "longlong" ||
            name == "int64" || name == "int64_t")
      *type = EType::kInt64;
   else if (name == "ulong" || name == "ULong64_t" || name == "ulonglong" ||
            name == "uint64" || name == "uint64_t")
      *type = EType::kUInt64;
   else if (name == "short" || name == "Short_t" || name == "int16" ||
            name == "int16_t")
      *type = EType::kInt16;
   else if (name == "ushort" || name == "UShort_t" || name == "uint16" ||
            name == "uint16_t")
      *type = EType::kUInt16;
   else if (name == "char" || name == "Char_t" || name == "int8" ||
            name == "int8_t")
      *type = EType::kInt8;
   else if (name == "uchar" || name == "UChar_t" || name == "uint8" ||
            name == "uint8_t")
      *type = EType::kUInt8;
   else if (name == "bool" || name == "Bool_t")
      *type = EType::kBool;
   else
      return false;
   return true;
}

void TTagmaSchema::AddField(const Field &field)
{
   fFields.push_back(field);
}

std::uint64_t TTagmaSchema::Extent() const
{
   std::uint64_t extent = 0;
   for (const auto &field : fFields)
      extent = std::max(extent, field.fOffset + field.Size());
   return extent;
}

bool TTagmaSchema::AddLine(const std::string &line)
{
   std::istringstream stream(line);
   std::string name;
   std::string offset;
   std::string typeName;
   std::string trailing;
   if (!(stream >> name >> offset >> typeName))
      return false;
   if (stream >> trailing)
      return false;
   if (name.empty())
      return false;

   char *end = nullptr;
   const unsigned long long value = std::strtoull(offset.c_str(), &end, 10);
   if (end == offset.c_str() || *end != '\0')
      return false;

   Field field;
   if (!ParseType(typeName, &field.fType))
      return false;
   field.fName = name;
   field.fOffset = static_cast<std::uint64_t>(value);
   fFields.push_back(field);
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
   std::string line;
   while (std::getline(in, line)) {
      std::istringstream probe(line);
      std::string first;
      probe >> first;
      if (first.empty() || first[0] == '#')
         continue;
      if (!AddLine(line)) {
         fFields.clear();
         return false;
      }
   }
   return !fFields.empty();
}

bool TTagmaSchema::Validate(std::uint64_t recordSize, std::string *why) const
{
   if (fFields.empty()) {
      if (why)
         *why = "the schema holds no field";
      return false;
   }
   std::vector<Field> ordered(fFields);
   std::sort(ordered.begin(), ordered.end(),
             [](const Field &a, const Field &b) { return a.fOffset < b.fOffset; });
   std::uint64_t previousEnd = 0;
   for (const auto &field : ordered) {
      if (field.fName.empty()) {
         if (why)
            *why = "a field has an empty name";
         return false;
      }
      if (field.fOffset < previousEnd) {
         if (why)
            *why = "field " + field.fName + " overlaps the preceding field";
         return false;
      }
      const std::uint64_t end = field.fOffset + field.Size();
      if (end > recordSize) {
         if (why)
            *why = "field " + field.fName + " extends past the record size";
         return false;
      }
      previousEnd = end;
   }
   for (std::size_t i = 0; i < ordered.size(); ++i)
      for (std::size_t j = i + 1; j < ordered.size(); ++j)
         if (ordered[i].fName == ordered[j].fName) {
            if (why)
               *why = "field name " + ordered[i].fName + " repeats";
            return false;
         }
   return true;
}

}  // namespace ROOT
