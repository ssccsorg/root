// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_TTagmaDataset
#define ROOT_TTagmaDataset

// TTagmaDataset: several coordinate stores addressed as one.
//
// A production dataset is many store files, not one. This class holds those
// files and resolves a (run, luminosity block, event number) coordinate to the
// file that owns it and the record index inside that file, so the caller does
// not stitch the files itself.
//
// The files are laid out along the run axis, in the order they are added: the
// first file owns the runs [0, runMax[0]), the next file the runs after it, and
// so on, so the dataset's run maximum is the sum of the per-file run maxima.
// The lumi and event axes are each file's own, from its descriptor. The
// per-file ranges are therefore derived from the data: every file is a
// self-describing store, and its descriptor names the axes and the record size
// the dataset lays out.
//
//   TTagmaDataset dataset;
//   dataset.AddFile("file0.tagma", true);
//   dataset.AddFile("file1.tagma", true);
//   std::size_t file;
//   std::uint64_t index;
//   dataset.Resolve(run, lumi, event, &file, &index);

#include "ROOT/TTagmaStore.hxx"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ROOT {

class TTagmaDataset {
public:
   // One store file and the position it occupies in the dataset's axes.
   struct File {
      std::string fPath;                   // the store file
      TTagmaStore::Layout fLayout;         // the file's own axis maxima and sizes
      std::uint64_t fRunBase = 0;          // first run this file owns
      std::uint64_t fRecordBase = 0;       // first record of this file in the dataset's flat index
      std::shared_ptr<TTagmaStore> fStore; // the file's store, holding the mapping when one is attached
   };

   TTagmaDataset() = default;
   TTagmaDataset(const TTagmaDataset &) = delete;
   TTagmaDataset &operator=(const TTagmaDataset &) = delete;

   // Adds a store file, reading its trailing descriptor for the axis maxima and
   // the record size. `map` also maps the file, so ReadRecord serves it.
   // Returns the index of the added file. Throws std::runtime_error, with the
   // reason, when the descriptor cannot be read or a requested mapping fails.
   std::size_t AddFile(const std::string &path, bool map = false);

   std::size_t Size() const { return fFiles.size(); }
   bool Empty() const { return fFiles.empty(); }
   const File &GetFile(std::size_t index) const { return fFiles.at(index); }

   // Exclusive bound of the dataset's run axis: the sum of the per-file run
   // maxima.
   std::uint64_t RunMax() const;
   // Records over the files.
   std::uint64_t RecordCount() const { return fRecordCount; }

   // Resolves (run, lumi, event) to the file that owns it and the file's local
   // record index, whose byte offset is the index times the file's record size.
   // Returns false when the coordinate is outside the dataset.
   bool
   Resolve(std::uint64_t run, std::uint64_t lumi, std::uint64_t event, std::size_t *file, std::uint64_t *index) const;

   // Resolves to the flat dataset index: the owning file's record base plus the
   // local record index.
   bool ResolveFlat(std::uint64_t run, std::uint64_t lumi, std::uint64_t event, std::uint64_t *flat) const;

   // Decomposes a flat dataset index back into (run, lumi, event). Returns
   // false when the index is outside the dataset.
   bool DecomposeFlat(std::uint64_t flat, std::uint64_t *run, std::uint64_t *lumi, std::uint64_t *event) const;

   // Reads the index record of (run, lumi, event) from the file it resolves to,
   // through that file's mapping. `len` must be the file's record size. Returns
   // false when the coordinate is outside the dataset, the file was added
   // without a mapping, or `len` is not the record size.
   bool ReadRecord(std::uint64_t run, std::uint64_t lumi, std::uint64_t event, char *buf, std::uint64_t len) const;

   // Reads the record at the flat dataset index, so a caller that walks the
   // dataset as one sequence does not decompose the index itself. `len` must be
   // the owning file's record size. Returns false under the same conditions as
   // the coordinate overload, and when the index is outside the dataset.
   bool ReadRecord(std::uint64_t flat, char *buf, std::uint64_t len) const;

   // Reads `count` consecutive records from flat index `flat` into `buf`, each
   // of `recordSize` bytes, stitching the files the range touches at a file
   // boundary, so a read spans the dataset. Returns false when the range leaves
   // the dataset, a file was added without a mapping, or a file's record size
   // is not `recordSize`.
   bool ReadRecords(std::uint64_t flat, std::uint64_t count, char *buf, std::uint64_t recordSize) const;

private:
   std::vector<File> fFiles;
   std::uint64_t fRecordCount = 0;
};

} // namespace ROOT

#endif // ROOT_TTagmaDataset
