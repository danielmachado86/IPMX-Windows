#pragma once

#include "ipmx/phase0/annexb.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace phase0 {

inline constexpr uint8_t kH264PayloadType = 96U;
inline constexpr uint32_t kRtpClockRate = 90'000U;
inline constexpr uint16_t kCaptureTimeExtensionProfile = 0xBEDEU;
inline constexpr uint8_t kCaptureTimeExtensionId = 1U;

struct ParsedRtpPacket {
  uint16_t sequence{};
  uint32_t timestamp{};
  uint32_t ssrc{};
  uint8_t payload_type{};
  bool marker{};
  uint64_t capture_time_ns{};
  std::span<const uint8_t> payload;
};

class RtpPacketizer {
public:
  explicit RtpPacketizer(size_t maximum_datagram_bytes = 1200U,
                         uint8_t payload_type = kH264PayloadType,
                         std::optional<uint32_t> deterministic_seed = std::nullopt);

  [[nodiscard]] std::vector<std::vector<uint8_t>> packetize(const std::vector<NalUnit>& nals,
                                                             uint32_t timestamp,
                                                             uint64_t capture_time_ns);
  [[nodiscard]] uint32_t ssrc() const noexcept { return ssrc_; }
  [[nodiscard]] uint16_t next_sequence() const noexcept { return sequence_; }

private:
  [[nodiscard]] std::vector<uint8_t> make_packet(std::span<const uint8_t> payload,
                                                 uint32_t timestamp,
                                                 uint64_t capture_time_ns,
                                                 bool marker);

  size_t maximum_datagram_bytes_{};
  uint8_t payload_type_{};
  uint16_t sequence_{};
  uint32_t ssrc_{};
};

[[nodiscard]] std::optional<ParsedRtpPacket> parse_rtp_packet(std::span<const uint8_t> datagram);

struct CompletedAccessUnit {
  uint32_t timestamp{};
  uint64_t capture_time_ns{};
  std::vector<uint8_t> annex_b;
};

class H264Depacketizer {
public:
  [[nodiscard]] std::optional<CompletedAccessUnit> push(const ParsedRtpPacket& packet);
  void reset() noexcept;

private:
  void start_timestamp(uint32_t timestamp, uint64_t capture_time_ns);

  bool active_{};
  bool fragmented_{};
  bool damaged_{};
  uint32_t timestamp_{};
  uint64_t capture_time_ns_{};
  uint16_t last_sequence_{};
  std::vector<NalUnit> nals_;
  NalUnit fragment_;
};

struct SequenceStats {
  uint64_t received{};
  uint64_t lost{};
  uint64_t reordered{};
};

class SequenceTracker {
public:
  void observe(uint16_t sequence) noexcept;
  [[nodiscard]] const SequenceStats& stats() const noexcept { return stats_; }

private:
  bool initialized_{};
  uint16_t expected_{};
  SequenceStats stats_{};
};

[[nodiscard]] uint32_t rtp_timestamp_for_frame(uint32_t initial_timestamp,
                                               uint64_t frame_index,
                                               uint32_t fps_numerator,
                                               uint32_t fps_denominator);

} // namespace phase0
