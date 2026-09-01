#pragma once

#include "ipmx/network_limits.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>

namespace ipmx {
inline namespace v0 {

class Tr107TrafficShaper {
public:
  explicit Tr107TrafficShaper(uint32_t maximum_ip_bitrate_kbps,
                              uint16_t maximum_udp_bytes = kMaximumStandardUdpPayloadBytes,
                              uint64_t maximum_drift_ns = 100'000'000U);

  [[nodiscard]] uint64_t schedule_ns(uint64_t now_ns, size_t udp_payload_bytes) noexcept;
  [[nodiscard]] uint64_t schedule_burst_ns(uint64_t now_ns, size_t ip_bytes) noexcept;
  [[nodiscard]] uint32_t maximum_ip_bitrate_kbps() const noexcept {
    return maximum_ip_bitrate_kbps_;
  }
  [[nodiscard]] uint32_t cmax() const noexcept { return cmax_; }

private:
  uint32_t maximum_ip_bitrate_kbps_{};
  uint32_t cmax_{};
  uint64_t maximum_drift_ns_{};
  uint64_t next_send_ns_{};
};

[[nodiscard]] uint32_t tr107_cmax(uint64_t maximum_packets_per_second) noexcept;

class FrameIntervalTracker {
public:
  explicit FrameIntervalTracker(uint64_t window_ns = 2'000'000'000U) : window_ns_(window_ns) {}
  void observe(uint64_t first_packet_time_ns);
  [[nodiscard]] bool window_observed() const noexcept { return window_observed_; }
  [[nodiscard]] uint64_t maximum_interval_spread_ns() const noexcept {
    return maximum_interval_spread_ns_;
  }

private:
  uint64_t window_ns_{};
  uint64_t maximum_interval_spread_ns_{};
  bool window_observed_{};
  std::deque<uint64_t> timestamps_;
};

} // namespace v0
} // namespace ipmx
