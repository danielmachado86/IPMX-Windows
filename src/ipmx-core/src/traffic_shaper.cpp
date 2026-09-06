#include "ipmx/traffic_shaper.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ipmx {
inline namespace v0 {

Tr107TrafficShaper::Tr107TrafficShaper(const uint32_t maximum_ip_bitrate_kbps,
                                       const uint16_t maximum_udp_bytes,
                                       const uint64_t maximum_drift_ns)
    : maximum_ip_bitrate_kbps_(maximum_ip_bitrate_kbps), maximum_udp_bytes_(maximum_udp_bytes),
      maximum_drift_ns_(maximum_drift_ns) {
  if (maximum_ip_bitrate_kbps_ == 0U || maximum_udp_bytes == 0U || maximum_drift_ns_ == 0U) {
    throw std::invalid_argument("TR-10-7 maximum IP bitrate must be non-zero");
  }
  const uint64_t packet_bits = (static_cast<uint64_t>(maximum_udp_bytes) + 28U) * 8U;
  maximum_packets_per_second_ =
      (static_cast<uint64_t>(maximum_ip_bitrate_kbps_) * 1'000U + packet_bits - 1U) / packet_bits;
  cmax_ = tr107_cmax(maximum_packets_per_second_);
  // ST 2110-21 uses beta=1.10 for the Network Compatibility Model drain rate.
  packet_drain_interval_ns_ = (10'000'000'000ULL + maximum_packets_per_second_ * 11U - 1U) /
                              (maximum_packets_per_second_ * 11U);
}

uint64_t Tr107TrafficShaper::schedule_ns(const uint64_t now_ns,
                                         const size_t udp_payload_bytes) noexcept {
  constexpr uint64_t ipv4_and_udp_header_bytes = 28U;
  const uint64_t ip_bytes =
      std::min<uint64_t>(static_cast<uint64_t>(udp_payload_bytes) + ipv4_and_udp_header_bytes,
                         std::numeric_limits<uint64_t>::max() / 8'000'000ULL);
  const uint64_t bit_spacing_ns =
      (ip_bytes * 8'000'000ULL + maximum_ip_bitrate_kbps_ - 1U) / maximum_ip_bitrate_kbps_;
  const uint64_t maximum_packet_bits = (static_cast<uint64_t>(maximum_udp_bytes_) + 28U) * 8U;
  const uint64_t maximum_packet_spacing_ns =
      (maximum_packet_bits * 1'000'000ULL + maximum_ip_bitrate_kbps_ - 1U) /
      maximum_ip_bitrate_kbps_;
  const uint64_t packet_tolerance = static_cast<uint64_t>(cmax_ - 1U) * packet_drain_interval_ns_;
  const uint64_t bitrate_tolerance = static_cast<uint64_t>(cmax_ - 1U) * maximum_packet_spacing_ns;

  if ((packet_tat_ns_ != 0U && now_ns > packet_tat_ns_ &&
       now_ns - packet_tat_ns_ > maximum_drift_ns_) ||
      packet_tat_ns_ == std::numeric_limits<uint64_t>::max()) {
    packet_tat_ns_ = now_ns;
    bitrate_tat_ns_ = now_ns;
  }
  if (packet_tat_ns_ == 0U) {
    packet_tat_ns_ = now_ns;
    bitrate_tat_ns_ = now_ns;
  }

  const uint64_t packet_earliest =
      packet_tat_ns_ > packet_tolerance ? packet_tat_ns_ - packet_tolerance : 0U;
  const uint64_t bitrate_earliest =
      bitrate_tat_ns_ > bitrate_tolerance ? bitrate_tat_ns_ - bitrate_tolerance : 0U;
  const uint64_t scheduled = std::max({now_ns, packet_earliest, bitrate_earliest});
  const auto saturated_add = [](const uint64_t value, const uint64_t increment) {
    return value > std::numeric_limits<uint64_t>::max() - increment
               ? std::numeric_limits<uint64_t>::max()
               : value + increment;
  };
  packet_tat_ns_ = saturated_add(std::max(packet_tat_ns_, scheduled), packet_drain_interval_ns_);
  bitrate_tat_ns_ = saturated_add(std::max(bitrate_tat_ns_, scheduled), bit_spacing_ns);
  return scheduled;
}

uint64_t Tr107TrafficShaper::schedule_burst_ns(const uint64_t now_ns,
                                               const size_t ip_bytes) noexcept {
  const uint64_t bytes = std::min<uint64_t>(static_cast<uint64_t>(ip_bytes),
                                            std::numeric_limits<uint64_t>::max() / 8'000'000ULL);
  const uint64_t numerator = bytes * 8'000'000ULL;
  const uint64_t spacing_ns =
      (numerator + maximum_ip_bitrate_kbps_ - 1U) / maximum_ip_bitrate_kbps_;
  if (bitrate_tat_ns_ != 0U && now_ns > bitrate_tat_ns_ &&
      now_ns - bitrate_tat_ns_ > maximum_drift_ns_)
    bitrate_tat_ns_ = now_ns;
  const uint64_t scheduled = std::max(now_ns, bitrate_tat_ns_);
  bitrate_tat_ns_ = scheduled > std::numeric_limits<uint64_t>::max() - spacing_ns
                        ? std::numeric_limits<uint64_t>::max()
                        : scheduled + spacing_ns;
  return scheduled;
}

uint32_t tr107_cmax(const uint64_t maximum_packets_per_second) noexcept {
  const uint64_t calculated = maximum_packets_per_second / 21'600U;
  return static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(16U, calculated),
                                                  std::numeric_limits<uint32_t>::max()));
}

NetworkCompatibilityTracker::NetworkCompatibilityTracker(const uint64_t maximum_packets_per_second,
                                                         const uint32_t cmax,
                                                         const uint32_t beta_numerator,
                                                         const uint32_t beta_denominator)
    : cmax_(cmax) {
  if (maximum_packets_per_second == 0U || cmax == 0U || beta_numerator == 0U ||
      beta_denominator == 0U) {
    throw std::invalid_argument("network compatibility parameters must be non-zero");
  }
  const uint64_t numerator = 1'000'000'000ULL * beta_denominator;
  if (maximum_packets_per_second > numerator / beta_numerator) {
    packet_drain_interval_ns_ = 1U;
  } else {
    const uint64_t denominator = maximum_packets_per_second * beta_numerator;
    packet_drain_interval_ns_ =
        std::max<uint64_t>(1U, (numerator + denominator - 1U) / denominator);
  }
}

void NetworkCompatibilityTracker::observe(const uint64_t packet_send_time_ns) noexcept {
  if (epoch_ns_ == 0U) {
    epoch_ns_ = packet_send_time_ns;
  } else if (packet_send_time_ns >= epoch_ns_) {
    const uint64_t drain_count = (packet_send_time_ns - epoch_ns_) / packet_drain_interval_ns_;
    const uint64_t drained = drain_count - last_drain_count_;
    cinst_ = drained >= cinst_ ? 0U : cinst_ - static_cast<uint32_t>(drained);
    last_drain_count_ = drain_count;
  }
  if (cinst_ != std::numeric_limits<uint32_t>::max())
    ++cinst_;
  maximum_cinst_ = std::max(maximum_cinst_, cinst_);
  if (cinst_ > cmax_)
    ++violations_;
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
  maximum_interval_spread_ns_ = std::max(maximum_interval_spread_ns_, maximum - minimum);
}

} // namespace v0
} // namespace ipmx
