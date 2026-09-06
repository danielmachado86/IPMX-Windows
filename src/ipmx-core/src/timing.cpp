#include "ipmx/timing.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

namespace ipmx {
inline namespace v0 {
namespace {

[[nodiscard]] uint64_t qpc_frequency() noexcept {
  static const uint64_t frequency = [] {
    LARGE_INTEGER value{};
    return QueryPerformanceFrequency(&value) && value.QuadPart > 0
               ? static_cast<uint64_t>(value.QuadPart)
               : 1U;
  }();
  return frequency;
}

[[nodiscard]] uint64_t ticks_to_ns(const uint64_t ticks, const uint64_t frequency) noexcept {
  const uint64_t seconds = ticks / frequency;
  const uint64_t remainder = ticks % frequency;
  if (seconds > std::numeric_limits<uint64_t>::max() / 1'000'000'000ULL)
    return std::numeric_limits<uint64_t>::max();
  return seconds * 1'000'000'000ULL +
         static_cast<uint64_t>((static_cast<long double>(remainder) * 1'000'000'000.0L) /
                               static_cast<long double>(frequency));
}

} // namespace

uint64_t qpc_now_ns() noexcept {
  LARGE_INTEGER value{};
  if (!QueryPerformanceCounter(&value) || value.QuadPart < 0)
    return 0U;
  return ticks_to_ns(static_cast<uint64_t>(value.QuadPart), qpc_frequency());
}

struct PreciseWaiter::State {
  HANDLE timer{};
  uint64_t spin_window_ns{};
  bool high_resolution{};
};

PreciseWaiter::PreciseWaiter(const uint64_t spin_window_ns) : state_(std::make_unique<State>()) {
  state_->spin_window_ns = std::clamp<uint64_t>(spin_window_ns, 10'000U, 500'000U);
  state_->timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_MODIFY_STATE | SYNCHRONIZE);
  state_->high_resolution = state_->timer != nullptr;
  if (!state_->timer) {
    state_->timer = CreateWaitableTimerExW(nullptr, nullptr, 0U, TIMER_MODIFY_STATE | SYNCHRONIZE);
  }
}

PreciseWaiter::~PreciseWaiter() {
  if (state_ && state_->timer)
    CloseHandle(state_->timer);
}

void PreciseWaiter::wait_until(const uint64_t deadline_ns) const noexcept {
  for (;;) {
    const uint64_t now_ns = qpc_now_ns();
    if (now_ns >= deadline_ns)
      return;
    const uint64_t remaining_ns = deadline_ns - now_ns;
    if (state_->timer && remaining_ns > state_->spin_window_ns) {
      const uint64_t timer_wait_ns = remaining_ns - state_->spin_window_ns;
      LARGE_INTEGER due{};
      const uint64_t hundred_ns = std::max<uint64_t>(1U, timer_wait_ns / 100U);
      due.QuadPart = -static_cast<LONGLONG>(std::min<uint64_t>(
          hundred_ns, static_cast<uint64_t>(std::numeric_limits<LONGLONG>::max())));
      if (SetWaitableTimerEx(state_->timer, &due, 0, nullptr, nullptr, nullptr, 0U)) {
        static_cast<void>(WaitForSingleObject(state_->timer, INFINITE));
        continue;
      }
    }
    if (remaining_ns > state_->spin_window_ns + 1'000'000U) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(remaining_ns - state_->spin_window_ns));
      continue;
    }
    while (qpc_now_ns() < deadline_ns)
      YieldProcessor();
    return;
  }
}

bool PreciseWaiter::uses_high_resolution_timer() const noexcept { return state_->high_resolution; }

uint64_t PreciseWaiter::spin_window_ns() const noexcept { return state_->spin_window_ns; }

} // namespace v0
} // namespace ipmx
