#pragma once

#include <cstdint>
#include <memory>

namespace ipmx {
inline namespace v0 {

// QueryPerformanceCounter-backed monotonic clock expressed in nanoseconds.
[[nodiscard]] uint64_t qpc_now_ns() noexcept;

class PreciseWaiter {
public:
  explicit PreciseWaiter(uint64_t spin_window_ns = 100'000U);
  ~PreciseWaiter();
  PreciseWaiter(const PreciseWaiter&) = delete;
  PreciseWaiter& operator=(const PreciseWaiter&) = delete;

  void wait_until(uint64_t deadline_ns) const noexcept;
  [[nodiscard]] bool uses_high_resolution_timer() const noexcept;
  [[nodiscard]] uint64_t spin_window_ns() const noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace v0
} // namespace ipmx
