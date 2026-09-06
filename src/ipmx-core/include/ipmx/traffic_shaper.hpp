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
  [[nodiscard]] uint64_t maximum_packets_per_second() const noexcept {
    return maximum_packets_per_second_;
  }
  [[nodiscard]] uint32_t cmax() const noexcept { return cmax_; }
  [[nodiscard]] uint64_t packet_drain_interval_ns() const noexcept {
    return packet_drain_interval_ns_;
  }

private:
  uint32_t maximum_ip_bitrate_kbps_{};
  uint16_t maximum_udp_bytes_{};
  uint64_t maximum_packets_per_second_{};
  uint32_t cmax_{};
  uint64_t maximum_drift_ns_{};
  uint64_t packet_drain_interval_ns_{};
  uint64_t packet_tat_ns_{};
  uint64_t bitrate_tat_ns_{};
};

[[nodiscard]] uint32_t tr107_cmax(uint64_t maximum_packets_per_second) noexcept;

class NetworkCompatibilityTracker {
public:
  NetworkCompatibilityTracker(uint64_t maximum_packets_per_second, uint32_t cmax,
                              uint32_t beta_numerator = 11U, uint32_t beta_denominator = 10U);
  void observe(uint64_t packet_send_time_ns) noexcept;
  [[nodiscard]] uint32_t maximum_cinst() const noexcept { return maximum_cinst_; }
  [[nodiscard]] uint64_t violations() const noexcept { return violations_; }
  [[nodiscard]] uint64_t packet_drain_interval_ns() const noexcept {
    return packet_drain_interval_ns_;
  }

private:
  uint32_t cmax_{};
  uint32_t cinst_{};
  uint32_t maximum_cinst_{};
  uint64_t violations_{};
  uint64_t packet_drain_interval_ns_{};
  uint64_t epoch_ns_{};
  uint64_t last_drain_count_{};
};

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
