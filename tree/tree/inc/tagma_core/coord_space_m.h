// Vendored from ssccsorg/syntagma sw/cpp/tagma_core (commit fa1b759,
// 2026-08-08). The Tagma C++ core is the canonical coordinate engine;
// the ROOT fork consumes it to verify the coordinate-indexed read path
// against the reference implementation. Upstream: ssccsorg/syntagma
// issue 38. Do not edit here; update from upstream instead.

#pragma once

// CoordSpaceM: a dense, mmap-backed CoordSpace for N=3, the C++ port of
// the Rust CoordSpaceM in sw/rust/core/src/coord_space_m.rs. Like
// CoordSpace and CoordSpaceN, addressing is direct: no hashing, no
// collisions, O(1) place and lookup by linear index. The backing memory
// is reserved as virtual address space via anonymous mmap (with
// MAP_NORESERVE on Linux) and committed lazily by the kernel on first
// write, enabling Tagma at a scale (11172^3 slots) beyond single heap
// allocations.
//
// Unix only: the header requires <sys/mman.h> and fails to compile on
// non-Unix platforms, mirroring the Rust mmap feature gate.
//
// Slot invariant: anonymous mmap memory is zero-filled, and a slot whose
// bytes are all zero is disengaged. This mirrors the Rust reference
// reliance on Option<V> having an all-zero None bit pattern. clear()
// discards pages without invoking V destructors, and the destructor
// munmaps without invoking them either, mirroring the Rust reference;
// V should be trivially destructible or values are reclaimed only by
// the operating system.

// macOS defines __unix__ as well; __APPLE__ is listed for explicitness.
#if !defined(__unix__) && !defined(__APPLE__)
#error "tagma_core CoordSpaceM requires a Unix-like platform (mmap)"
#endif

#include "tagma_core/coord.h"
#include "tagma_core/coord_path.h"

#include <sys/mman.h>

#include <cassert>
#include <cstddef>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

namespace tagma {

// The slot count for a dense space at depth N. Only N=3 (11172^3) fits
// in size_t on 64-bit; other depths return 0 and the constructor throws,
// mirroring the Rust coord_slots helper.
constexpr std::size_t coord_space_m_slots(int n) {
  constexpr std::size_t kValid = static_cast<std::size_t>(Coord::kNValid);
  switch (n) {
    case 3:
      return kValid * kValid * kValid;
    default:
      return 0;
  }
}

template <int N, typename V>
class CoordSpaceM {
public:
  static constexpr std::size_t kSlotCount = coord_space_m_slots(N);

  // Creates an empty mmap-backed space. Throws std::invalid_argument for
  // an unsupported depth (only N=3) and std::runtime_error when mmap
  // fails, mirroring the Rust panics.
  CoordSpaceM() {
    if (kSlotCount == 0) {
      throw std::invalid_argument("CoordSpaceM: unsupported depth");
    }
    void* ptr = ::mmap(nullptr, alloc_size(), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS
#ifdef MAP_NORESERVE
                           | MAP_NORESERVE
#endif
                       ,
                       -1, 0);
    if (ptr == MAP_FAILED) {
      throw std::runtime_error("CoordSpaceM: mmap failed");
    }
    slots_ = static_cast<Slot*>(ptr);
  }

  CoordSpaceM(const CoordSpaceM&) = delete;
  CoordSpaceM& operator=(const CoordSpaceM&) = delete;

  CoordSpaceM(CoordSpaceM&& other) noexcept
      : slots_(other.slots_), len_(other.len_) {
    other.slots_ = nullptr;
    other.len_ = 0;
  }

  CoordSpaceM& operator=(CoordSpaceM&& other) noexcept {
    if (this != &other) {
      release();
      slots_ = other.slots_;
      len_ = other.len_;
      other.slots_ = nullptr;
      other.len_ = 0;
    }
    return *this;
  }

  // The Rust reference implements Clone via mmap plus memcpy; the C++
  // port omits it because a byte-wise copy would commit the entire
  // multi-terabyte virtual region page by page. PartialEq is likewise
  // omitted: slot-wise comparison would walk the same region.
  ~CoordSpaceM() { release(); }

  // The number of occupied slots.
  std::size_t len() const { return len_; }

  // True when no slots are occupied.
  bool is_empty() const { return len_ == 0; }

  // A pointer to the value at path, or nullptr when absent.
  const V* at_path(const CoordPath<N>& path) const {
    const std::size_t idx = linear_index(path);
    assert(idx < kSlotCount && "CoordSpaceM at_path: index out of bounds");
    const Slot& slot = slots_[idx];
    return slot.engaged ? reinterpret_cast<const V*>(slot.storage) : nullptr;
  }

  // Places value at path, returning the previous value when present.
  std::optional<V> place_path(const CoordPath<N>& path, V value) {
    const std::size_t idx = linear_index(path);
    assert(idx < kSlotCount && "CoordSpaceM place_path: index out of bounds");
    Slot& slot = slots_[idx];
    std::optional<V> previous;
    if (slot.engaged) {
      V* old = reinterpret_cast<V*>(slot.storage);
      previous.emplace(std::move(*old));
      old->~V();
      // Mirror the Rust slot.take(): the slot reads as disengaged while
      // the replacement value is constructed, so a throwing move leaves
      // a clean disengaged slot instead of a half-constructed object.
      // The entry count is decremented here and restored after a
      // successful construction, keeping len() consistent on the throw
      // path.
      slot.engaged = false;
      len_ -= 1;
    }
    ::new (static_cast<void*>(slot.storage)) V(std::move(value));
    slot.engaged = true;
    len_ += 1;
    return previous;
  }

  // Removes the value at path, returning it when present.
  std::optional<V> vacate_path(const CoordPath<N>& path) {
    const std::size_t idx = linear_index(path);
    assert(idx < kSlotCount && "CoordSpaceM vacate_path: index out of bounds");
    Slot& slot = slots_[idx];
    if (!slot.engaged) return std::nullopt;
    V* value = reinterpret_cast<V*>(slot.storage);
    std::optional<V> previous(std::move(*value));
    value->~V();
    slot.engaged = false;
    len_ -= 1;
    return previous;
  }

  // Removes all values. Retains the mmap allocation. On Linux, uses
  // madvise(MADV_DONTNEED) to discard pages immediately; on other
  // platforms, re-mmaps over the region with MAP_FIXED to zero it,
  // mirroring the Rust reference.
  void clear() {
    if (kSlotCount == 0) return;
#ifdef __linux__
    ::madvise(slots_, alloc_size(), MADV_DONTNEED);
#else
    void* ret = ::mmap(slots_, alloc_size(), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (ret == MAP_FAILED) {
      throw std::runtime_error("CoordSpaceM: MAP_FIXED remap failed on clear");
    }
#endif
    len_ = 0;
  }

private:
  // One slot per linear index. Zeroed memory reads as disengaged; values
  // are constructed in place on placement and destroyed on vacate.
  struct alignas(V) Slot {
    bool engaged;
    std::byte storage[sizeof(V)];
  };

  static std::size_t alloc_size() { return kSlotCount * sizeof(Slot); }

  static std::size_t linear_index(const CoordPath<N>& path) {
    std::size_t idx = 0;
    for (int i = 0; i < N; ++i) {
      idx = idx * static_cast<std::size_t>(Coord::kNValid) +
            path.coords()[i].index();
    }
    return idx;
  }

  void release() {
    if (slots_ != nullptr) {
      ::munmap(slots_, alloc_size());
      slots_ = nullptr;
    }
  }

  Slot* slots_ = nullptr;
  std::size_t len_ = 0;
};

// 3-character mmap-backed dense space, the only supported depth.
template <typename V>
using CoordSpaceM3 = CoordSpaceM<3, V>;

}  // namespace tagma
