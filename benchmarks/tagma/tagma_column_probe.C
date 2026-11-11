// Author: SSCCS Initiative 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

// tagma_column_probe.C
//
// The read-side cost of a columnar block layout, measured before that layout
// exists. A whole-event read from a store touches one contiguous slice per
// event today. A block transposed by field would touch one run per field
// instead, with the same bytes and the same slice totals, so the only
// difference is the order: the prefetch breaks and the page walks that a
// transposed block costs.
//
// This is a probe, not a store benchmark. It isolates the access order from the
// entry layer and from the medium, neither of which a transposition changes,
// and it says nothing about a narrow selection, which is the side the columnar
// layout is for. The question it answers is only the one the design decision
// needs first: what does the transposition cost the row that wins today.
//
//   root -l -b -q 'tagma_column_probe.C()'

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// One event's slice of the data region: 36 field runs of 64 bytes, the shape a
// 2,560-byte record with collections has once the index record is set aside, and
// one run per cache line so the two orders touch the same number of lines.
constexpr std::uint64_t kSliceBytes = 2304;
constexpr std::uint64_t kFields = 36;
constexpr std::uint64_t kFieldBytes = kSliceBytes / kFields;
// Divisible by every block length below, so a block never runs past the end.
constexpr std::uint64_t kEvents = 393216;
constexpr std::uint64_t kBytes = kEvents * kSliceBytes;
constexpr int kRepeats = 5;

struct Sample {
   double fSeconds = 0;
   std::uint64_t fSink = 0;
};

// One contiguous run per event.
Sample EventMajor(const std::vector<unsigned char> &data)
{
   const auto start = std::chrono::steady_clock::now();
   std::uint64_t sink = 0;
   for (std::uint64_t event = 0; event < kEvents; ++event) {
      const std::uint64_t begin = event * kSliceBytes;
      const std::uint64_t end = begin + kSliceBytes;
      for (std::uint64_t at = begin; at < end; at += 64)
         sink += data[at];
   }
   const auto stop = std::chrono::steady_clock::now();
   return {std::chrono::duration<double>(stop - start).count(), sink};
}

// The whole-event read from a transposed block: for each event, one run per
// field, and the runs of one event are `blockEvents * kFieldBytes` apart. This
// is the pattern the risk is about, because the entry layer materialises one
// event at a time.
Sample Columnar(const std::vector<unsigned char> &data, std::uint64_t blockEvents)
{
   const auto start = std::chrono::steady_clock::now();
   std::uint64_t sink = 0;
   for (std::uint64_t event = 0; event < kEvents; ++event) {
      const std::uint64_t block = (event / blockEvents) * blockEvents;
      const std::uint64_t events = std::min(blockEvents, kEvents - block);
      const std::uint64_t base = block * kSliceBytes + (event - block) * kFieldBytes;
      for (std::uint64_t field = 0; field < kFields; ++field) {
         const std::uint64_t begin = base + field * events * kFieldBytes;
         const std::uint64_t end = begin + kFieldBytes;
         for (std::uint64_t at = begin; at < end; at += 64)
            sink += data[at];
      }
   }
   const auto stop = std::chrono::steady_clock::now();
   return {std::chrono::duration<double>(stop - start).count(), sink};
}

double Best(Sample (*run)(const std::vector<unsigned char> &), const std::vector<unsigned char> &data)
{
   double best = 0;
   for (int repeat = 0; repeat < kRepeats; ++repeat) {
      const Sample sample = run(data);
      if (repeat == 0 || sample.fSeconds < best)
         best = sample.fSeconds;
   }
   return best;
}

double BestColumnar(std::uint64_t blockEvents, const std::vector<unsigned char> &data)
{
   double best = 0;
   for (int repeat = 0; repeat < kRepeats; ++repeat) {
      const Sample sample = Columnar(data, blockEvents);
      if (repeat == 0 || sample.fSeconds < best)
         best = sample.fSeconds;
   }
   return best;
}

} // namespace

int tagma_column_probe()
{
   std::vector<unsigned char> data(kBytes);
   for (std::uint64_t at = 0; at < kBytes; at += 4096)
      data[at] = static_cast<unsigned char>(at >> 12);

   const double eventMajor = Best(EventMajor, data);

   std::printf("tagma_column_probe: events=%llu slice=%llu bytes field=%llu fields=%llu MB=%.1f\n",
               static_cast<unsigned long long>(kEvents), static_cast<unsigned long long>(kSliceBytes),
               static_cast<unsigned long long>(kFieldBytes), static_cast<unsigned long long>(kFields),
               static_cast<double>(kBytes) / 1e6);
   std::printf("tagma_column_probe: %-12s %-8s %-9s %-9s %-8s\n", "order", "block", "seconds", "MB/s", "vs event");
   std::printf("tagma_column_probe: %-12s %-8s %-9.3f %-9.1f %-8.2f\n", "event-major", "-", eventMajor,
               static_cast<double>(kBytes) / 1e6 / eventMajor, 1.0);

   for (const std::uint64_t blockEvents : {1ull, 16ull, 64ull, 256ull, 1024ull, 4096ull}) {
      const double columnar = BestColumnar(blockEvents, data);
      std::printf("tagma_column_probe: %-12s %-8llu %-9.3f %-9.1f %-8.2f\n", "columnar",
                  static_cast<unsigned long long>(blockEvents), columnar, static_cast<double>(kBytes) / 1e6 / columnar,
                  columnar / eventMajor);
   }

   return 0;
}
