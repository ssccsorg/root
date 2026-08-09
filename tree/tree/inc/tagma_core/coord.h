// Vendored from ssccsorg/syntagma sw/cpp/tagma_core (commit fa1b759,
// 2026-08-08). The Tagma C++ core is the canonical coordinate engine;
// the ROOT fork consumes it to verify the coordinate-indexed read path
// against the reference implementation. Upstream: ssccsorg/syntagma
// issue 38. Do not edit here; update from upstream instead.

#pragma once

// tagma_core: Tagma coordinate engine, C++ core.
//
// A single 16-bit structural coordinate in the Unicode closed-form
// composition block. Composition and decomposition are closed-form
// arithmetic, with no hash function:
//
//   C(i,m,f) = 0xAC00 + 588i + 28m + f
//              for 0 <= i < 19, 0 <= m < 21, 0 <= f < 28
//
// The lattice holds 11,172 valid coordinates. The remaining 54,364 of
// the 65,536 representable 16-bit states are structurally invalid and
// detectable in constant time. This mirrors the Rust tagma-core Coord
// in sw/rust/core.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <tuple>

namespace tagma {

class Coord {
public:
  static constexpr uint16_t kBase = 0xAC00;

  // Last code point of the Unicode compositional characters block
  // (U+D7AF). U+D7A4..U+D7AF are filler positions; the last valid
  // character is at U+D7A3 (offset 11171).
  static constexpr uint16_t kLast = 0xD7AF;
  static constexpr int kInitialMax = 19;
  static constexpr int kMedialMax = 21;
  static constexpr int kFinalMax = 28;
  static constexpr int kNValid = 11172;

  // Number of representable 16-bit states.
  static constexpr int kTotal = 65536;

  // Number of structurally invalid states (kTotal - kNValid).
  static constexpr int kInvalidMargin = 54364;

  // Compose from the three axes. Returns nullopt when any axis is out of
  // range.
  static constexpr std::optional<Coord> from_axes(int initial, int medial,
                                                  int final) {
    if (initial < 0 || initial >= kInitialMax || medial < 0 ||
        medial >= kMedialMax || final < 0 || final >= kFinalMax) {
      return std::nullopt;
    }
    return Coord(static_cast<uint16_t>(initial * (kMedialMax * kFinalMax) +
                                       medial * kFinalMax + final));
  }

  // Construct from the linear index 0..11171.
  static constexpr std::optional<Coord> from_index(uint16_t index) {
    if (index >= kNValid) return std::nullopt;
    return Coord(index);
  }

  // Construct from the Unicode code point U+AC00..U+D7AF. Filler
  // positions U+D7A4..U+D7AF are rejected.
  static constexpr std::optional<Coord> from_code_point(uint16_t code_point) {
    if (code_point < kBase || code_point > kLast) return std::nullopt;
    const uint16_t offset = static_cast<uint16_t>(code_point - kBase);
    if (offset >= kNValid) return std::nullopt;  // filler positions
    return Coord(offset);
  }

  // Construct from a code point carried as a 32-bit character.
  static constexpr std::optional<Coord> from_char(char32_t ch) {
    if (ch < kBase || ch > kLast) return std::nullopt;
    const uint16_t offset = static_cast<uint16_t>(ch - kBase);
    if (offset >= kNValid) return std::nullopt;
    return Coord(offset);
  }

  constexpr Coord() = default;

  // Structural validity. Every constructed Coord is valid; the predicate
  // mirrors the hardware range check for completeness.
  constexpr bool valid() const { return index_ < kNValid; }

  // Linear index in 0..11171, the offset from the block base.
  constexpr uint16_t index() const { return index_; }

  // Unicode code point U+AC00..U+D7AF.
  constexpr uint16_t code_point() const {
    return static_cast<uint16_t>(kBase + index_);
  }

  // Compositional character display of the code point.
  constexpr char32_t to_char() const {
    return static_cast<char32_t>(code_point());
  }

  // The compositional character as a UTF-8 string.
  std::string to_hangul_string() const;

  // Little-endian and big-endian bytes of the raw index (Rust
  // to_le_bytes/to_be_bytes).
  constexpr std::array<uint8_t, 2> to_le_bytes() const {
    return {static_cast<uint8_t>(index_ & 0xFF),
            static_cast<uint8_t>(index_ >> 8)};
  }

  constexpr std::array<uint8_t, 2> to_be_bytes() const {
    return {static_cast<uint8_t>(index_ >> 8),
            static_cast<uint8_t>(index_ & 0xFF)};
  }

  // Construct from little-endian or big-endian bytes of the raw index.
  // Returns nullopt when the decoded index is invalid.
  static constexpr std::optional<Coord> from_le_bytes(
      const std::array<uint8_t, 2>& bytes) {
    return from_index(static_cast<uint16_t>(bytes[0]) |
                      (static_cast<uint16_t>(bytes[1]) << 8));
  }

  static constexpr std::optional<Coord> from_be_bytes(
      const std::array<uint8_t, 2>& bytes) {
    return from_index((static_cast<uint16_t>(bytes[0]) << 8) |
                      static_cast<uint16_t>(bytes[1]));
  }

  // Decompose into (initial, medial, final).
  constexpr std::tuple<int, int, int> axes() const {
    const int index = index_;
    return {index / (kMedialMax * kFinalMax),
            (index / kFinalMax) % kMedialMax, index % kFinalMax};
  }

  // Field-wise Hamming distance: per-axis absolute differences as
  // (d_initial, d_medial, d_final).
  constexpr std::tuple<int, int, int> hamming_distance(
      const Coord& other) const {
    const auto [i1, m1, f1] = axes();
    const auto [i2, m2, f2] = other.axes();
    const auto abs_diff = [](int a, int b) { return a > b ? a - b : b - a; };
    return {abs_diff(i1, i2), abs_diff(m1, m2), abs_diff(f1, f2)};
  }

  // Ordering by linear index, mirroring the Rust Ord derive.
  constexpr bool operator==(const Coord& other) const {
    return index_ == other.index_;
  }
  constexpr bool operator!=(const Coord& other) const {
    return !(*this == other);
  }
  constexpr bool operator<(const Coord& other) const {
    return index_ < other.index_;
  }
  constexpr bool operator<=(const Coord& other) const {
    return !(other < *this);
  }
  constexpr bool operator>(const Coord& other) const {
    return other < *this;
  }
  constexpr bool operator>=(const Coord& other) const {
    return !(*this < other);
  }

private:
  constexpr explicit Coord(uint16_t index) : index_(index) {}
  uint16_t index_ = 0;
};

// Display of the compositional character, mirroring the Rust Display impl.
// Defined in namespace tagma so argument-dependent lookup finds it.
std::ostream& operator<<(std::ostream& os, const Coord& coord);

}  // namespace tagma

// Hash by linear index, mirroring the Rust Hash derive.
namespace std {
template <>
struct hash<tagma::Coord> {
  std::size_t operator()(const tagma::Coord& coord) const {
    return static_cast<std::size_t>(coord.index());
  }
};
}  // namespace std
