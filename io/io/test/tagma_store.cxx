// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

// Verifies the coordinate-indexed store extension: composition and
// decomposition round trip, byte offsets, record count, extent, axis
// bounds, and the TFile read-path hook that serves aligned fixed-width
// records through the coordinate path. The store itself is
// self-contained C++17 and does not depend on ROOT.

#include "ROOT/TTagmaStore.hxx"

#include "TFile.h"
#include "TNamed.h"

#include "gtest/gtest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>

namespace {

ROOT::TTagmaStore MakeStore()
{
   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1000;
   layout.fLumiMax = 128;
   layout.fEventMax = 10000;
   layout.fRecordSize = 128;
   return ROOT::TTagmaStore(layout);
}

using Axes = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;

}  // namespace

TEST(TTagmaStore, ComposeDecomposeRoundTrip)
{
   const ROOT::TTagmaStore store = MakeStore();
   const std::uint64_t runs[] = {0, 1, 42, 999};
   const std::uint64_t lumis[] = {0, 7, 127};
   const std::uint64_t events[] = {0, 1, 4096, 9999};

   for (const std::uint64_t run : runs) {
      for (const std::uint64_t lumi : lumis) {
         for (const std::uint64_t event : events) {
            const std::uint64_t index = store.Compose(run, lumi, event);
            EXPECT_EQ(store.Decompose(index), Axes(run, lumi, event));
         }
      }
   }
}

TEST(TTagmaStore, OffsetIsClosedForm)
{
   const ROOT::TTagmaStore store = MakeStore();
   EXPECT_EQ(store.Offset(0, 0, 0), 0u);
   EXPECT_EQ(store.Offset(999, 127, 9999),
             store.Compose(999, 127, 9999) * 128u);
   EXPECT_EQ(store.Offset(42, 63, 2048),
             (std::uint64_t(42) * 128u + 63u) * 10000u * 128u +
                 std::uint64_t(2048) * 128u);
}

TEST(TTagmaStore, RecordCountAndExtent)
{
   const ROOT::TTagmaStore store = MakeStore();
   EXPECT_EQ(store.RecordCount(), 1000ull * 128ull * 10000ull);
   EXPECT_EQ(store.SizeBytes(), 1000ull * 128ull * 10000ull * 128ull);
}

TEST(TTagmaStore, AxisBounds)
{
   const ROOT::TTagmaStore store = MakeStore();
   EXPECT_TRUE(store.Contains(0, 0, 0));
   EXPECT_TRUE(store.Contains(999, 127, 9999));
   EXPECT_FALSE(store.Contains(1000, 0, 0));
   EXPECT_FALSE(store.Contains(0, 128, 0));
   EXPECT_FALSE(store.Contains(0, 0, 10000));
}

TEST(TTagmaStore, LayoutValidation)
{
   ROOT::TTagmaStore::Layout base;
   base.fRunMax = 1000;
   base.fLumiMax = 128;
   base.fEventMax = 10000;
   base.fRecordSize = 128;
   EXPECT_NO_THROW(ROOT::TTagmaStore store(base));

   ROOT::TTagmaStore::Layout zero = base;
   zero.fRunMax = 0;
   EXPECT_THROW(ROOT::TTagmaStore store(zero), std::invalid_argument);

   zero = base;
   zero.fRecordSize = 0;
   EXPECT_THROW(ROOT::TTagmaStore store(zero), std::invalid_argument);

   ROOT::TTagmaStore::Layout overflow = base;
   overflow.fRunMax = std::numeric_limits<std::uint64_t>::max();
   EXPECT_THROW(ROOT::TTagmaStore store(overflow), std::invalid_argument);

   overflow = base;
   overflow.fLumiMax = std::numeric_limits<std::uint64_t>::max();
   EXPECT_THROW(ROOT::TTagmaStore store(overflow), std::invalid_argument);

   overflow = base;
   overflow.fEventMax = std::numeric_limits<std::uint64_t>::max();
   EXPECT_THROW(ROOT::TTagmaStore store(overflow), std::invalid_argument);

   overflow = base;
   overflow.fRecordSize = std::numeric_limits<std::uint64_t>::max();
   EXPECT_THROW(ROOT::TTagmaStore store(overflow), std::invalid_argument);
}

TEST(TTagmaStore, SysReadCounterTracksEveryByteSourceRead)
{
   // The M5 benchmark reports the system call count per path. Every
   // byte-source read, ordinary or coordinate-served, issues exactly one
   // read system call, and GetSysReadCalls must count them all.
   {
      TFile file("tagma_sysread_test.root", "RECREATE");
      TNamed first("key", "value");
      first.Write();
      file.Close();
   }

   TFile file("tagma_sysread_test.root");
   const Int_t sys0 = file.GetSysReadCalls();

   // Ordinary path: one system call per ReadBuffer request.
   char buf[64];
   EXPECT_FALSE(file.ReadBuffer(buf, 0, 64));
   EXPECT_EQ(file.GetSysReadCalls(), sys0 + 1);

   // Coordinate path: each aligned fixed-width record read issues one
   // system call as well, so the per-event count is exactly one.
   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1;
   layout.fLumiMax = 1;
   layout.fEventMax = 2;
   layout.fRecordSize = 64;
   file.SetTagmaStore(std::make_shared<ROOT::TTagmaStore>(layout));
   EXPECT_FALSE(file.ReadBuffer(buf, 64, 64));
   EXPECT_EQ(file.GetSysReadCalls(), sys0 + 2);

   // The counter is cumulative and monotonic across both paths.
   EXPECT_FALSE(file.ReadBuffer(buf, 0, 32));  // misaligned: ordinary path
   EXPECT_EQ(file.GetSysReadCalls(), sys0 + 3);
}

TEST(TTagmaStore, MemoryBackedReadServesRecordsWithoutSyscalls)
{
   // A mapped store is the byte source: covered fixed-width record
   // requests are copied from the mapping, the payload bytes are
   // delivered, and no read system call is issued. The read path never
   // reaches the medium for mapped data.
   const char *path = "tagma_mmap_test.bin";
   {
      FILE *out = std::fopen(path, "wb");
      ASSERT_NE(out, nullptr);
      unsigned char record[64];
      for (int i = 0; i < 4; ++i) {
         for (int j = 0; j < 64; ++j)
            record[j] = static_cast<unsigned char>(i * 64 + j);
         ASSERT_EQ(std::fwrite(record, 1, sizeof(record), out),
                   sizeof(record));
      }
      std::fclose(out);
   }

   TFile file("tagma_mmap_test.bin?filetype=raw");
   ASSERT_FALSE(file.IsZombie());
   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1;
   layout.fLumiMax = 1;
   layout.fEventMax = 4;
   layout.fRecordSize = 64;
   auto store = std::make_shared<ROOT::TTagmaStore>(layout);
   ASSERT_TRUE(store->MapFile(path));
   file.SetTagmaStore(store);

   const Int_t sys0 = file.GetSysReadCalls();
   char buf[64];
   EXPECT_FALSE(file.ReadBuffer(buf, 0, 64));   // record 0
   EXPECT_EQ(std::memcmp(buf, "\x00\x01\x02\x03", 4), 0);
   EXPECT_FALSE(file.ReadBuffer(buf, 64, 64));  // record 1
   EXPECT_EQ(std::memcmp(buf, "\x40\x41\x42\x43", 4), 0);
   EXPECT_FALSE(file.ReadBuffer(buf, 192, 64)); // record 3
   EXPECT_EQ(std::memcmp(buf, "\xc0\xc1\xc2\xc3", 4), 0);

   // Served from the mapping: zero system calls, counted as coordinate
   // reads, and the bytes moved equal the record bytes.
   EXPECT_EQ(file.GetSysReadCalls(), sys0);
   EXPECT_EQ(file.GetTagmaReadCalls(), 3);
   EXPECT_EQ(file.GetBytesRead(), 192);

   store->Unmap();
   std::remove(path);
}

TEST(TTagmaStore, MapFileRejectsShortFile)
{
   // A file smaller than the store extent is not mapped: the read path
   // would otherwise read past the end of the byte source.
   const char *path = "tagma_mmap_short.bin";
   {
      FILE *out = std::fopen(path, "wb");
      ASSERT_NE(out, nullptr);
      const char one = 0;
      ASSERT_EQ(std::fwrite(&one, 1, 1, out), 1u);
      std::fclose(out);
   }

   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1;
   layout.fLumiMax = 1;
   layout.fEventMax = 4;
   layout.fRecordSize = 64;
   ROOT::TTagmaStore store(layout);
   EXPECT_FALSE(store.MapFile(path));
   EXPECT_FALSE(store.IsMapped());
   std::remove(path);
}

TEST(TTagmaStore, TFileReadBufferServesAlignedRecords)
{
   // A ROOT file provides the byte source; the hook serves aligned
   // fixed-width record requests through the coordinate path regardless
   // of the payload bytes, and everything else falls through to the
   // ordinary read path.
   {
      TFile file("tagma_file_hook.root", "RECREATE");
      TNamed first("key", "value");
      first.Write();
      TNamed second("key2", "value2");
      second.Write();
      file.Close();
   }

   TFile file("tagma_file_hook.root");
   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1;
   layout.fLumiMax = 1;
   layout.fEventMax = 2;
   layout.fRecordSize = 64;
   file.SetTagmaStore(std::make_shared<ROOT::TTagmaStore>(layout));

   char buf[64];
   const Int_t calls0 = file.GetReadCalls();
   EXPECT_FALSE(file.ReadBuffer(buf, 0, 64));    // record 0, covered
   EXPECT_EQ(file.GetTagmaReadCalls(), 1);
   EXPECT_EQ(file.GetReadCalls(), calls0 + 1);
   EXPECT_FALSE(file.ReadBuffer(buf, 64, 64));   // record 1, covered
   EXPECT_EQ(file.GetTagmaReadCalls(), 2);
   EXPECT_EQ(file.GetReadCalls(), calls0 + 2);

   EXPECT_FALSE(file.ReadBuffer(buf, 100, 64));  // misaligned: ordinary path
   EXPECT_EQ(file.GetTagmaReadCalls(), 2);
   EXPECT_EQ(file.GetReadCalls(), calls0 + 3);
   EXPECT_FALSE(file.ReadBuffer(buf, 128, 64));  // index 2 outside layout
   EXPECT_EQ(file.GetTagmaReadCalls(), 2);
   EXPECT_EQ(file.GetReadCalls(), calls0 + 4);
   EXPECT_FALSE(file.ReadBuffer(buf, 0, 32));    // wrong size: ordinary path
   EXPECT_EQ(file.GetTagmaReadCalls(), 2);
   EXPECT_EQ(file.GetReadCalls(), calls0 + 5);
}
