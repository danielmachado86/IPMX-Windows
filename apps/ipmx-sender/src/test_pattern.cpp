#include "ipmx/sender/frame_source.hpp"
#include "ipmx/frame_timeline.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace ipmx::sender {
namespace {

class TestPatternSource final : public FrameSource {
public:
  TestPatternSource(const uint32_t width, const uint32_t height, const uint32_t fps_numerator,
                    const uint32_t fps_denominator, const bool stress)
      : width_(width), height_(height), fps_numerator_(fps_numerator),
        fps_denominator_(fps_denominator), start_ns_(steady_now_ns()), stress_(stress) {
    if (width == 0U || height == 0U || (width & 1U) != 0U || (height & 1U) != 0U ||
        fps_numerator == 0U || fps_denominator == 0U) {
      throw std::invalid_argument("test source needs even dimensions and a valid frame rate");
    }
  }

  [[nodiscard]] uint32_t width() const noexcept override { return width_; }
  [[nodiscard]] uint32_t height() const noexcept override { return height_; }

  bool next(BgraFrame& frame) override {
    if (frame_index_ == 0U) start_ns_ = steady_now_ns();
    const auto slot = select_frame_slot(start_ns_, steady_now_ns(), frame_index_,
                                         fps_numerator_, fps_denominator_);
    frame.skipped_intervals = slot.skipped;
    frame_index_ = slot.index;
    frame.frame_index = slot.index;
    const uint64_t deadline_ns = slot.nominal_ns;
    waiter_.wait_until(deadline_ns);

    frame.width = width_;
    frame.height = height_;
    frame.stride = width_ * 4U;
    frame.nominal_time_ns = deadline_ns;
    frame.capture_time_ns = steady_now_ns();
    frame.pixels.resize(static_cast<size_t>(frame.stride) * height_);

    constexpr std::array<std::array<uint8_t, 3>, 8> bars{{
        {191, 191, 191},
        {191, 191, 0},
        {0, 191, 191},
        {0, 191, 0},
        {191, 0, 191},
        {191, 0, 0},
        {0, 0, 191},
        {16, 16, 16},
    }};
    const uint32_t marker_x = static_cast<uint32_t>((frame_index_ * 7U) % width_);
    for (uint32_t y = 0; y < height_; ++y) {
      auto* row = frame.pixels.data() + static_cast<size_t>(y) * frame.stride;
      for (uint32_t x = 0; x < width_; ++x) {
        row[x * 4U + 3U] = 255U;
        if (stress_) {
          // Deterministic moving texture exercises VBV and packet pacing.
          uint32_t noise = x + y * width_ + static_cast<uint32_t>(frame_index_) * 0x9E3779B9U;
          noise ^= noise >> 16U;
          noise *= 0x7FEB352DU;
          noise ^= noise >> 15U;
          for (uint32_t channel = 0U; channel < 3U; ++channel)
            row[x * 4U + channel] = static_cast<uint8_t>(noise >> (channel * 8U));
        } else {
          const auto& rgb = bars[std::min<size_t>(7U, static_cast<size_t>(x) * bars.size() / width_)];
          const bool marker = x >= marker_x && x < std::min(width_, marker_x + 12U) &&
                              y > height_ / 3U && y < 2U * height_ / 3U;
          row[x * 4U + 0U] = marker ? 255U : rgb[2];
          row[x * 4U + 1U] = marker ? 255U : rgb[1];
          row[x * 4U + 2U] = marker ? 255U : rgb[0];
          row[x * 4U + 3U] = 255U;
        }
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
  uint64_t start_ns_{};
  bool stress_{};
  PreciseWaiter waiter_;
};

} // namespace

std::unique_ptr<FrameSource> make_test_pattern_source(const uint32_t width, const uint32_t height,
                                                      const uint32_t fps_numerator,
                                                      const uint32_t fps_denominator, const bool stress) {
  return std::make_unique<TestPatternSource>(width, height, fps_numerator, fps_denominator, stress);
}

} // namespace ipmx::sender
