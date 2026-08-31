#include "ipmx/phase0/bgra_to_nv12.hpp"

#include <algorithm>
#include <stdexcept>

namespace phase0 {
namespace {

[[nodiscard]] uint8_t clamp_byte(const int value) noexcept {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

[[nodiscard]] int y709(const int r, const int g, const int b) noexcept {
  return ((47 * r + 157 * g + 16 * b + 128) >> 8) + 16;
}

[[nodiscard]] int u709(const int r, const int g, const int b) noexcept {
  return ((-26 * r - 87 * g + 113 * b + 128) >> 8) + 128;
}

[[nodiscard]] int v709(const int r, const int g, const int b) noexcept {
  return ((112 * r - 102 * g - 10 * b + 128) >> 8) + 128;
}

} // namespace

Nv12Frame bgra_to_nv12(const BgraFrame& source) {
  if (source.width == 0 || source.height == 0 || (source.width & 1U) != 0 ||
      (source.height & 1U) != 0 || source.stride < source.width * 4U ||
      source.pixels.size() < static_cast<size_t>(source.stride) * source.height) {
    throw std::invalid_argument("BGRA frame must have valid even dimensions and stride");
  }

  Nv12Frame result;
  result.width = source.width;
  result.height = source.height;
  result.y_stride = source.width;
  result.uv_stride = source.width;
  result.capture_time_ns = source.capture_time_ns;
  result.pixels.resize(static_cast<size_t>(source.width) * source.height * 3U / 2U);

  for (uint32_t row = 0; row < source.height; ++row) {
    const auto* input = source.pixels.data() + static_cast<size_t>(row) * source.stride;
    auto* output = result.y_plane() + static_cast<size_t>(row) * result.y_stride;
    for (uint32_t column = 0; column < source.width; ++column) {
      const int b = input[column * 4U + 0U];
      const int g = input[column * 4U + 1U];
      const int r = input[column * 4U + 2U];
      output[column] = clamp_byte(y709(r, g, b));
    }
  }

  for (uint32_t row = 0; row < source.height; row += 2U) {
    auto* output = result.uv_plane() + static_cast<size_t>(row / 2U) * result.uv_stride;
    for (uint32_t column = 0; column < source.width; column += 2U) {
      int r = 0;
      int g = 0;
      int b = 0;
      for (uint32_t dy = 0; dy < 2U; ++dy) {
        const auto* input = source.pixels.data() + static_cast<size_t>(row + dy) * source.stride;
        for (uint32_t dx = 0; dx < 2U; ++dx) {
          b += input[(column + dx) * 4U + 0U];
          g += input[(column + dx) * 4U + 1U];
          r += input[(column + dx) * 4U + 2U];
        }
      }
      r = (r + 2) / 4;
      g = (g + 2) / 4;
      b = (b + 2) / 4;
      output[column] = clamp_byte(u709(r, g, b));
      output[column + 1U] = clamp_byte(v709(r, g, b));
    }
  }
  return result;
}

} // namespace phase0
