#include "ipmx/traffic_shaper.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ipmx {
inline namespace v0 {

Tr107TrafficShaper::Tr107TrafficShaper(const uint32_t maximum_ip_bitrate_kbps,
                                       const uint16_t maximum_udp_bytes,
                                       const uint64_t maximum_drift_ns)
    : maximum_ip_bitrate_kbps_(maximum_ip_bitrate_kbps), maximum_drift_ns_(maximum_drift_ns) {
  if (maximum_ip_bitrate_kbps_ == 0U || maximum_udp_bytes == 0U || maximum_drift_ns_ == 0U) {
    throw std::invalid_argument("TR-10-7 maximum IP bitrate must be non-zero");
  }
  const uint64_t packet_bits = (static_cast<uint64_t>(maximum_udp_bytes) + 28U) * 8U;
  const uint64_t maximum_packets_per_second =
      (static_cast<uint64_t>(maximum_ip_bitrate_kbps_) * 1'000U + packet_bits - 1U) / packet_bits;
  cmax_ = tr107_cmax(maximum_packets_per_second);
}

uint64_t Tr107TrafficShaper::schedule_ns(const uint64_t now_ns,
                                         const size_t udp_payload_bytes) noexcept {
  constexpr uint64_t ipv4_and_udp_header_bytes = 28U;
  return schedule_burst_ns(now_ns, static_cast<uint64_t>(udp_payload_bytes) +
                                       ipv4_and_udp_header_bytes);
}

uint64_t Tr107TrafficShaper::schedule_burst_ns(const uint64_t now_ns,
                                                const size_t ip_bytes) noexcept {
  const uint64_t bytes = std::min<uint64_t>(static_cast<uint64_t>(ip_bytes),
                                             std::numeric_limits<uint64_t>::max() / 8'000'000ULL);
  const uint64_t numerator = bytes * 8'000'000ULL;
  const uint64_t spacing_ns =
      (numerator + maximum_ip_bitrate_kbps_ - 1U) / maximum_ip_bitrate_kbps_;
  if (next_send_ns_ != 0U && now_ns > next_send_ns_ &&
      now_ns - next_send_ns_ > maximum_drift_ns_)
    next_send_ns_ = now_ns;
  const uint64_t scheduled = std::max(now_ns, next_send_ns_);
  next_send_ns_ = scheduled > std::numeric_limits<uint64_t>::max() - spacing_ns
                      ? std::numeric_limits<uint64_t>::max()
                      : scheduled + spacing_ns;
  return scheduled;
}

uint32_t tr107_cmax(const uint64_t maximum_packets_per_second) noexcept {
  const uint64_t calculated = maximum_packets_per_second / 21'600U;
  return static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(16U, calculated),
                                                  std::numeric_limits<uint32_t>::max()));
}

void FrameIntervalTracker::observe(const uint64_t first_packet_time_ns) {
  if (!timestamps_.empty() && first_packet_time_ns < timestamps_.back())
    return;
  timestamps_.push_back(first_packet_time_ns);
  while (timestamps_.size() > 2U && first_packet_time_ns - timestamps_[1U] >= window_ns_)
    timestamps_.pop_front();
  if (timestamps_.size() < 3U || first_packet_time_ns - timestamps_.front() < window_ns_)
    return;
  uint64_t minimum = std::numeric_limits<uint64_t>::max();
  uint64_t maximum = 0U;
  for (size_t index = 1U; index < timestamps_.size(); ++index) {
    const uint64_t interval = timestamps_[index] - timestamps_[index - 1U];
    minimum = std::min(minimum, interval);
    maximum = std::max(maximum, interval);
  }
  window_observed_ = true;
  maximum_interval_spread_ns_ =
      std::max(maximum_interval_spread_ns_, maximum - minimum);
}

} // namespace v0
} // namespace ipmx
