// Vendored from ssccsorg/syntagma sw/cpp/tagma_core (commit fa1b759,
// 2026-08-08). The Tagma C++ core is the canonical coordinate engine;
// the ROOT fork consumes it to verify the coordinate-indexed read path
// against the reference implementation. Upstream: ssccsorg/syntagma
// issue 38. Do not edit here; update from upstream instead.

#include "tagma_core/coord.h"

#include <cstdint>
#include <ostream>
#include <string>

namespace tagma {

std::string Coord::to_hangul_string() const {
  const uint32_t cp = code_point();
  std::string out;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return out;
}

std::ostream& operator<<(std::ostream& os, const Coord& coord) {
  return os << coord.to_hangul_string();
}

}  // namespace tagma
