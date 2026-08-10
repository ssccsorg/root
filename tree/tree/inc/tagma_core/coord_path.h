// Vendored from ssccsorg/syntagma sw/cpp/tagma_core (commit fa1b759,
// 2026-08-08). The Tagma C++ core is the canonical coordinate engine;
// the ROOT fork consumes it to verify the coordinate-indexed read path
// against the reference implementation. Upstream: ssccsorg/syntagma
// issue 38. Do not edit here; update from upstream instead.

#pragma once

// CoordPath: an index path, a compile-time N-element array of coordinates.
// The path is an address, not a hash key. Mirrors the Rust CoordPath<N>
// in sw/rust/core.

#include "tagma_core/coord.h"

#include <array>
#include <ostream>

namespace tagma {

template <int N>
class CoordPath {
public:
  static constexpr int kLength = N;

  // Default-constructs to a path of valid zero coordinates.
  CoordPath() = default;

  explicit CoordPath(const std::array<Coord, N>& coords) : coords_(coords) {}

  static CoordPath from_array(const std::array<Coord, N>& coords) {
    return CoordPath(coords);
  }

  // The internal coordinate array.
  const std::array<Coord, N>& coords() const { return coords_; }

  // The coordinate at index, or nullptr when out of range.
  const Coord* get(int index) const {
    return index >= 0 && index < N ? &coords_[index] : nullptr;
  }

  // The path length, always N.
  static constexpr int len() { return N; }

  static constexpr bool is_empty() { return N == 0; }

  auto begin() const { return coords_.begin(); }
  auto end() const { return coords_.end(); }

  // Equality and ordering by the coordinate array, mirroring the Rust
  // PartialEq and Eq impls.
  bool operator==(const CoordPath& other) const { return coords_ == other.coords_; }
  bool operator!=(const CoordPath& other) const { return !(*this == other); }

private:
  std::array<Coord, N> coords_;
};

// Display of the coordinate path, mirroring the Rust Display impl.
template <int N>
std::ostream& operator<<(std::ostream& os, const CoordPath<N>& path) {
  os << "CoordPath<" << N << ">(";
  for (int i = 0; i < N; ++i) {
    if (i > 0) os << ", ";
    os << path.coords()[i];
  }
  return os << ")";
}

}  // namespace tagma
