#include "ipmx/sender/frame_source.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace ipmx::sender {
namespace {

class TestPatternSource final : public FrameSource {
public:
  TestPatternSource(const uint32_t width, const uint32_t height, const uint32_t fps_numerator,
                    const uint32_t fps_denominator)
      : width_(width), height_(height), fps_numerator_(fps_numerator), fps_denominator_(fps_denominator),
        start_(std::chrono::steady_clock::now()) {
    if (width == 0U || height == 0U || (width & 1U) != 0U || (height & 1U) != 0U ||
        fps_numerator == 0U || fps_denominator == 0U) {
      throw std::invalid_argument("test source needs even dimensions and a valid frame rate");
    }
  }

  [[nodiscard]] uint32_t width() const noexcept override { return width_; }
  [[nodiscard]] uint32_t height() const noexcept override { return height_; }

  bool next(BgraFrame& frame) override {
    using namespace std::chrono;
    const auto deadline = start_ + nanoseconds((frame_index_ * 1'000'000'000ULL * fps_denominator_) /
                                                fps_numerator_);
    std::this_thread::sleep_until(deadline);

    frame.width = width_;
    frame.height = height_;
    frame.stride = width_ * 4U;
    frame.capture_time_ns = steady_now_ns();
    frame.pixels.resize(static_cast<size_t>(frame.stride) * height_);

    constexpr std::array<std::array<uint8_t, 3>, 8> bars{{
        {191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
        {191, 0, 191},   {191, 0, 0},   {0, 0, 191},   {16, 16, 16},
    }};
    const uint32_t marker_x = static_cast<uint32_t>((frame_index_ * 7U) % width_);
    for (uint32_t y = 0; y < height_; ++y) {
      auto* row = frame.pixels.data() + static_cast<size_t>(y) * frame.stride;
      for (uint32_t x = 0; x < width_; ++x) {
        const auto& rgb = bars[std::min<size_t>(7U, static_cast<size_t>(x) * bars.size() / width_)];
        const bool marker = x >= marker_x && x < std::min(width_, marker_x + 12U) &&
                            y > height_ / 3U && y < 2U * height_ / 3U;
        row[x * 4U + 0U] = marker ? 255U : rgb[2];
        row[x * 4U + 1U] = marker ? 255U : rgb[1];
        row[x * 4U + 2U] = marker ? 255U : rgb[0];
        row[x * 4U + 3U] = 255U;
      }
    }
    ++frame_index_;
    return true;
  }

private:
  uint32_t width_{};
  uint32_t height_{};
  uint32_t fps_numerator_{};
  uint32_t fps_denominator_{};
  uint64_t frame_index_{};
  std::chrono::steady_clock::time_point start_;
};

} // namespace

std::unique_ptr<FrameSource> make_test_pattern_source(const uint32_t width, const uint32_t height,
                                                      const uint32_t fps_numerator,
                                                      const uint32_t fps_denominator) {
  return std::make_unique<TestPatternSource>(width, height, fps_numerator, fps_denominator);
}

} // namespace ipmx::sender
