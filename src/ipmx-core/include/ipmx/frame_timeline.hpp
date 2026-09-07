#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
namespace ipmx {
struct FrameSlot { uint64_t index; uint64_t nominal_ns; uint64_t skipped; };
// Integer rational timeline; skip only inputs, never encoded access units.
inline FrameSlot select_frame_slot(uint64_t start, uint64_t now, uint64_t next,
                                   uint32_t numerator, uint32_t denominator) {
  if (!numerator || !denominator) throw std::invalid_argument("invalid frame rate");
  const uint64_t scale = 1'000'000'000ULL * denominator;
  const uint64_t elapsed = now > start ? now - start : 0U;
  const uint64_t latest = elapsed / scale * numerator + elapsed % scale * numerator / scale;
  const uint64_t index = std::max(next, latest);
  return {index, start + index / numerator * scale + index % numerator * scale / numerator,
          index - next};
}
}
