#pragma once

#include "ipmx/rtcp.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ipmx::sender {

struct FrameTransmitterSettings {
  std::string multicast_group;
  std::string interface_address;
  uint16_t media_port{};
  uint16_t maximum_udp_bytes{};
  uint32_t maximum_ip_bitrate_kbps{};
  uint32_t ssrc{};
  std::string ts_refclk;
  std::string media_clock{"direct=0"};
  std::string cname;
  uint8_t block_version{1U};
  IpmxVideoMediaInfo video;
  IpmxH264MediaInfo h264;
  std::optional<uint64_t> encoder_delay_ns;
  std::optional<uint64_t> sender_reports_delay_ns;
  uint64_t access_unit_offset_ns{1'000'000U};
  std::filesystem::path pcap_path;
  size_t maximum_queued_frames{8U};
};

struct TransmitFrame {
  uint32_t rtp_timestamp{};
  uint64_t capture_time_ns{};
  std::vector<std::vector<uint8_t>> rtp_packets;
};

struct FrameTransmitterStats {
  uint64_t frames{};
  uint64_t packets{};
  uint64_t rtp_payload_octets{};
  uint64_t rtcp_reports{};
  uint64_t maximum_sender_report_lateness_ns{};
  uint64_t maximum_encoder_cpb_lateness_ns{};
  uint64_t maximum_interval_spread_ns{};
  bool timing_window_observed{};
};

struct IpmxFrameSchedule {
  uint64_t sender_report_time_ns{};
  uint64_t encoder_cpb_insertion_time_ns{};
  uint64_t first_rtp_time_ns{};
};

struct IpmxSessionTiming {
  uint64_t encoder_delay_ns{};
  uint64_t sender_reports_delay_ns{};
  uint64_t access_unit_offset_ns{};
};

[[nodiscard]] IpmxSessionTiming
resolve_ipmx_session_timing(uint32_t fps_numerator, uint32_t fps_denominator,
                            std::optional<uint64_t> encoder_delay_ns,
                            std::optional<uint64_t> sender_reports_delay_ns,
                            uint64_t access_unit_offset_ns);

[[nodiscard]] IpmxFrameSchedule make_ipmx_frame_schedule(uint64_t capture_time_ns,
                                                          uint64_t encoder_delay_ns,
                                                          uint64_t sender_reports_delay_ns,
                                                          uint64_t access_unit_offset_ns);

class FrameTransmitter {
public:
  explicit FrameTransmitter(FrameTransmitterSettings settings);
  ~FrameTransmitter();
  FrameTransmitter(const FrameTransmitter&) = delete;
  FrameTransmitter& operator=(const FrameTransmitter&) = delete;
  void enqueue(TransmitFrame frame);
  void close();
  [[nodiscard]] FrameTransmitterStats stats() const;
  [[nodiscard]] IpmxSessionTiming timing() const;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace ipmx::sender
