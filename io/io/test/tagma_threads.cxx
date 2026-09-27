// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

// Verifies the concurrency of the reader backend's file hook
// (ssccsorg/ssccs#121): the coordinate read path serves one store from several
// threads at once. The standard implicit multi-threading model gives each
// thread its own file object over the same store bytes, and the tagma read
// path holds no per-read shared state, a mapped record being a copy from the
// mapping and a positioned one a pread that leaves the file offset alone, so
// the values do not cross the threads and the per-file counts stay exact.

#include "ROOT/TTagmaStore.hxx"

#include "TFile.h"

#include "gtest/gtest.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kRecordSize = 64;
constexpr std::uint64_t kEntries = 512;
constexpr int kThreads = 8;

// A store file whose record i names itself in its first bytes, so a record
// read back identifies the coordinate it was read for.
void WriteRecords(const char *path)
{
   FILE *out = std::fopen(path, "wb");
   ASSERT_NE(out, nullptr);
   std::vector<char> record(kRecordSize);
   for (std::uint64_t i = 0; i < kEntries; ++i) {
      std::memcpy(record.data(), &i, sizeof(i));
      ASSERT_EQ(std::fwrite(record.data(), 1, record.size(), out), record.size());
   }
   std::fclose(out);
}

} // namespace

TEST(TTagmaThreads, MappedRecordsReadCorrectlyFromEveryThread)
{
   const char *path = "tagma_threads_store.bin";
   WriteRecords(path);

   ROOT::TTagmaStore::Layout layout;
   layout.fRunMax = 1;
   layout.fLumiMax = 1;
   layout.fEventMax = kEntries;
   layout.fRecordSize = kRecordSize;

   // One file, mapping, and store per thread, over the same store bytes: the
   // standard implicit multi-threading model. The files are created on this
   // thread, so the workers only read.
   std::vector<std::unique_ptr<TFile>> files;
   std::vector<std::shared_ptr<ROOT::TTagmaStore>> stores;
   std::vector<Int_t> before;
   for (int t = 0; t < kThreads; ++t) {
      const std::string url = std::string(path) + "?filetype=raw";
      files.push_back(std::make_unique<TFile>(url.c_str()));
      ASSERT_FALSE(files.back()->IsZombie());
      auto store = std::make_shared<ROOT::TTagmaStore>(layout);
      ASSERT_TRUE(store->MapFile(path));
      files.back()->SetTagmaStore(store);
      stores.push_back(store);
      before.push_back(files.back()->GetSysReadCalls());
   }

   std::vector<int> failures(kThreads, 0);
   std::vector<std::thread> workers;
   for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([&, t]() {
         TFile *file = files[t].get();
         std::vector<char> buf(kRecordSize);
         for (std::uint64_t i = 0; i < kEntries; ++i) {
            if (file->ReadBuffer(buf.data(), static_cast<Long64_t>(i * kRecordSize), static_cast<Int_t>(kRecordSize)))
               ++failures[t];
            std::uint64_t seen = 0;
            std::memcpy(&seen, buf.data(), sizeof(seen));
            if (seen != i)
               ++failures[t];
         }
      });
   }
   for (auto &worker : workers)
      worker.join();

   for (int t = 0; t < kThreads; ++t) {
      EXPECT_EQ(failures[t], 0) << "thread " << t;
      // Served from the mapping: no read system call, one coordinate read per
      // record, and the counts survive the concurrent threads.
      EXPECT_EQ(files[t]->GetSysReadCalls(), before[t]);
      EXPECT_EQ(files[t]->GetTagmaReadCalls(), static_cast<Int_t>(kEntries));
   }

   for (auto &store : stores)
      store->Unmap();
   std::remove(path);
}
