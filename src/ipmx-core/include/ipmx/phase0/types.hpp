#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace phase0 {

struct BgraFrame {
  uint32_t width{};
  uint32_t height{};
  uint32_t stride{};
  uint64_t capture_time_ns{};
  std::vector<uint8_t> pixels;
};

struct Nv12Frame {
  uint32_t width{};
  uint32_t height{};
  uint32_t y_stride{};
  uint32_t uv_stride{};
  uint64_t capture_time_ns{};
  std::vector<uint8_t> pixels;

  [[nodiscard]] uint8_t* y_plane() noexcept { return pixels.data(); }
  [[nodiscard]] uint8_t* uv_plane() noexcept {
    return pixels.data() + static_cast<size_t>(y_stride) * height;
  }
  [[nodiscard]] const uint8_t* y_plane() const noexcept { return pixels.data(); }
  [[nodiscard]] const uint8_t* uv_plane() const noexcept {
    return pixels.data() + static_cast<size_t>(y_stride) * height;
  }
};

struct DecodedFrame {
  uint32_t width{};
  uint32_t height{};
  uint32_t y_stride{};
  uint32_t uv_stride{};
  uint64_t capture_time_ns{};
  std::vector<uint8_t> pixels;
};

[[nodiscard]] inline uint64_t steady_now_ns() noexcept {
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace phase0
