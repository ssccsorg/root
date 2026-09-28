// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

// Verifies the dataset addressing of the reader backend (ssccsorg/ssccs#121):
// TTagmaDataset lays several self-describing stores out along the run axis and
// resolves a (run, lumi, event) coordinate to the file that owns it and the
// record inside it, so a coordinate read spans the dataset with no per-file
// stitching by the caller.

#include "ROOT/TTagmaDataset.hxx"
#include "ROOT/TTagmaSchema.hxx"
#include "ROOT/TTagmaWriter.hxx"

#include "gtest/gtest.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::uint32_t kMaxMuon = 200;
const char *kFile0 = "tagma_dataset_0.bin";
const char *kFile1 = "tagma_dataset_1.bin";

ROOT::TTagmaSchema MakeSchema()
{
   ROOT::TTagmaSchema schema;
   ROOT::TTagmaSchema::Field nMuon;
   nMuon.fName = "nMuon";
   nMuon.fOffset = 0;
   nMuon.fType = ROOT::TTagmaSchema::EType::kUInt32;
   schema.AddField(nMuon);

   ROOT::TTagmaSchema::Field muonPt;
   muonPt.fName = "Muon_pt";
   muonPt.fCountField = "nMuon";
   muonPt.fMaxCount = kMaxMuon;
   muonPt.fOffset = 0;
   muonPt.fType = ROOT::TTagmaSchema::EType::kFloat;
   schema.AddField(muonPt);
   return schema;
}

// Writes a store whose events carry `nMuon = base + entry` muons, so a record
// read back identifies both the file and the entry.
void WriteStore(const char *path, std::uint32_t entries, std::uint32_t base)
{
   ROOT::TTagmaWriter writer(MakeSchema(), entries);
   ASSERT_TRUE(writer.Open(path));
   for (std::uint32_t entry = 0; entry < entries; ++entry) {
      const std::uint32_t nMuon = base + entry;
      char scalars[4];
      std::memcpy(scalars, &nMuon, sizeof(nMuon));
      std::vector<char> slice(nMuon * sizeof(float), 0);
      ASSERT_TRUE(writer.AddEvent(scalars, slice.data())) << "entry " << entry;
   }
   ASSERT_TRUE(writer.Close());
}

std::uint32_t RecordMuonCount(const char *record)
{
   std::uint32_t nMuon = 0;
   std::memcpy(&nMuon, record, sizeof(nMuon));
   return nMuon;
}

} // namespace

TEST(TTagmaDataset, ResolvesCoordinatesAcrossFiles)
{
   WriteStore(kFile0, 4, 10);  // runs [0], events 0..3, nMuon 10..13
   WriteStore(kFile1, 3, 110); // runs [1], events 0..2, nMuon 110..112

   ROOT::TTagmaDataset dataset;
   EXPECT_EQ(dataset.AddFile(kFile0), 0u);
   EXPECT_EQ(dataset.AddFile(kFile1), 1u);
   EXPECT_EQ(dataset.Size(), 2u);
   EXPECT_EQ(dataset.RunMax(), 2u);
   EXPECT_EQ(dataset.RecordCount(), 7u);

   // The run axis selects the file; the lumi and event axes resolve inside it.
   std::size_t file = 0;
   std::uint64_t index = 0;
   ASSERT_TRUE(dataset.Resolve(0, 0, 2, &file, &index));
   EXPECT_EQ(file, 0u);
   EXPECT_EQ(index, 2u);
   ASSERT_TRUE(dataset.Resolve(1, 0, 0, &file, &index));
   EXPECT_EQ(file, 1u);
   EXPECT_EQ(index, 0u);
   ASSERT_TRUE(dataset.Resolve(1, 0, 2, &file, &index));
   EXPECT_EQ(file, 1u);
   EXPECT_EQ(index, 2u);

   // Outside the dataset: a run past the last file, an event past the file's
   // events, and a lumi past the file's lumi axis.
   EXPECT_FALSE(dataset.Resolve(2, 0, 0, &file, &index));
   EXPECT_FALSE(dataset.Resolve(0, 0, 4, &file, &index));
   EXPECT_FALSE(dataset.Resolve(0, 1, 0, &file, &index));

   // The flat index is contiguous across the files, and decomposes back.
   std::uint64_t flat = 0;
   ASSERT_TRUE(dataset.ResolveFlat(1, 0, 1, &flat));
   EXPECT_EQ(flat, 5u);
   std::uint64_t run = 0;
   std::uint64_t lumi = 0;
   std::uint64_t event = 0;
   ASSERT_TRUE(dataset.DecomposeFlat(5, &run, &lumi, &event));
   EXPECT_EQ(run, 1u);
   EXPECT_EQ(lumi, 0u);
   EXPECT_EQ(event, 1u);
   ASSERT_TRUE(dataset.DecomposeFlat(3, &run, &lumi, &event));
   EXPECT_EQ(run, 0u);
   EXPECT_EQ(event, 3u);
   EXPECT_FALSE(dataset.DecomposeFlat(7, &run, &lumi, &event));

   std::remove(kFile0);
   std::remove(kFile1);
}

TEST(TTagmaDataset, ReadRecordServesTheResolvedFile)
{
   WriteStore(kFile0, 4, 10);
   WriteStore(kFile1, 3, 110);

   ROOT::TTagmaDataset dataset;
   dataset.AddFile(kFile0, true);
   dataset.AddFile(kFile1, true);

   const std::uint64_t recordSize = dataset.GetFile(0).fLayout.fRecordSize;
   std::vector<char> record(recordSize);

   // (run 1, lumi 0, event 2) is the third record of file 1.
   ASSERT_TRUE(dataset.ReadRecord(1, 0, 2, record.data(), recordSize));
   EXPECT_EQ(RecordMuonCount(record.data()), 112u);
   // (run 0, lumi 0, event 1) is the second record of file 0.
   ASSERT_TRUE(dataset.ReadRecord(0, 0, 1, record.data(), recordSize));
   EXPECT_EQ(RecordMuonCount(record.data()), 11u);

   // A coordinate outside the dataset and a length that is not the record size
   // are rejected.
   EXPECT_FALSE(dataset.ReadRecord(2, 0, 0, record.data(), recordSize));
   EXPECT_FALSE(dataset.ReadRecord(0, 0, 0, record.data(), recordSize - 1));

   // A file added without a mapping cannot serve a record.
   ROOT::TTagmaDataset unmapped;
   unmapped.AddFile(kFile0);
   std::vector<char> other(recordSize);
   EXPECT_FALSE(unmapped.ReadRecord(0, 0, 0, other.data(), recordSize));

   std::remove(kFile0);
   std::remove(kFile1);
}

TEST(TTagmaDataset, RejectsAFileWithoutADescriptor)
{
   FILE *out = std::fopen(kFile0, "wb");
   ASSERT_NE(out, nullptr);
   const std::vector<char> junk(256, 'j');
   ASSERT_EQ(std::fwrite(junk.data(), 1, junk.size(), out), junk.size());
   std::fclose(out);

   ROOT::TTagmaDataset dataset;
   EXPECT_THROW(dataset.AddFile(kFile0), std::runtime_error);
   EXPECT_EQ(dataset.Size(), 0u);
   std::remove(kFile0);
}

TEST(TTagmaDataset, ReadsTheDatasetAsOneSequence)
{
   WriteStore(kFile0, 4, 10);  // records 0..3
   WriteStore(kFile1, 3, 110); // records 4..6

   ROOT::TTagmaDataset dataset;
   dataset.AddFile(kFile0, true);
   dataset.AddFile(kFile1, true);

   const std::uint64_t recordSize = dataset.GetFile(0).fLayout.fRecordSize;
   std::vector<char> record(recordSize);

   // The flat index walks the dataset as one sequence: the first file's records,
   // then the next file's, with no per-file stitching by the caller.
   const std::uint32_t expected[] = {10, 11, 12, 13, 110, 111, 112};
   for (std::uint64_t flat = 0; flat < dataset.RecordCount(); ++flat) {
      ASSERT_TRUE(dataset.ReadRecord(flat, record.data(), recordSize)) << "flat " << flat;
      EXPECT_EQ(RecordMuonCount(record.data()), expected[flat]);
   }
   EXPECT_FALSE(dataset.ReadRecord(dataset.RecordCount(), record.data(), recordSize));

   // A range read spans the boundary between the two files.
   std::vector<char> range(5 * recordSize);
   ASSERT_TRUE(dataset.ReadRecords(2, 5, range.data(), recordSize));
   for (std::uint64_t i = 0; i < 5; ++i)
      EXPECT_EQ(RecordMuonCount(range.data() + i * recordSize), expected[2 + i]);

   // A range that leaves the dataset and a wrong record size are rejected.
   EXPECT_FALSE(dataset.ReadRecords(5, 3, range.data(), recordSize));
   EXPECT_FALSE(dataset.ReadRecords(0, 2, range.data(), recordSize - 1));

   std::remove(kFile0);
   std::remove(kFile1);
}
