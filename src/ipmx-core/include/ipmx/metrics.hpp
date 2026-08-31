#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ipmx {
inline namespace v0 {

class LatencyMetrics {
public:
  void observe_ns(const uint64_t latency_ns) noexcept {
    const double milliseconds = static_cast<double>(latency_ns) / 1'000'000.0;
    ++count_;
    minimum_ms_ = std::min(minimum_ms_, milliseconds);
    maximum_ms_ = std::max(maximum_ms_, milliseconds);
    const double delta = milliseconds - mean_ms_;
    mean_ms_ += delta / static_cast<double>(count_);
    squared_delta_ += delta * (milliseconds - mean_ms_);
  }

  [[nodiscard]] uint64_t count() const noexcept { return count_; }
  [[nodiscard]] double minimum_ms() const noexcept { return count_ == 0U ? 0.0 : minimum_ms_; }
  [[nodiscard]] double maximum_ms() const noexcept { return maximum_ms_; }
  [[nodiscard]] double mean_ms() const noexcept { return mean_ms_; }
  [[nodiscard]] double standard_deviation_ms() const noexcept {
    return count_ < 2U ? 0.0 : std::sqrt(squared_delta_ / static_cast<double>(count_ - 1U));
  }

private:
  uint64_t count_{};
  double minimum_ms_{std::numeric_limits<double>::max()};
  double maximum_ms_{};
  double mean_ms_{};
  double squared_delta_{};
};

} // namespace v0
} // namespace ipmx
