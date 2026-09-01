#pragma once

#include "ipmx/rtcp.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
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
  IpmxVideoMediaInfo video;
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
  uint64_t maximum_interval_spread_ns{};
  bool timing_window_observed{};
};

class FrameTransmitter {
public:
  explicit FrameTransmitter(FrameTransmitterSettings settings);
  ~FrameTransmitter();
  FrameTransmitter(const FrameTransmitter&) = delete;
  FrameTransmitter& operator=(const FrameTransmitter&) = delete;
  void enqueue(TransmitFrame frame);
  void close();
  [[nodiscard]] FrameTransmitterStats stats() const;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace ipmx::sender
