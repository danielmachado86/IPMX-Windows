#pragma once

#include "ipmx/annexb.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ipmx {
inline namespace v0 {

inline constexpr uint16_t kIpmxCompressedVideoMediaInfoType = 0x0005U;
inline constexpr uint16_t kIpmxH264MediaInfoType = 0x000AU;

struct IpmxVideoMediaInfo {
  uint32_t width{};
  uint32_t height{};
  uint32_t fps_numerator{};
  uint32_t fps_denominator{1U};
  std::string sampling{"YCbCr-4:2:0"};
  std::string range{"NARROW"};
  std::string colorimetry{"BT709"};
  std::string transfer_characteristic{"SDR"};
  uint8_t bit_depth{8U};
  uint64_t measured_pixel_clock{};
  uint16_t htotal{};
  uint16_t vtotal{};
};

struct IpmxH264MediaInfo {
  std::array<uint8_t, 3U> profile_level_id{};
  uint8_t packetization_mode{1U};
  std::string sprop_parameter_sets;
  std::string sprop_level_parameter_sets;
  std::string extra;
};

[[nodiscard]] IpmxH264MediaInfo make_ipmx_h264_media_info(const NalUnit& sps,
                                                           const NalUnit& pps);

struct IpmxRtcpSenderReport {
  uint32_t ssrc{};
  uint32_t ptp_seconds{};
  uint32_t ptp_nanoseconds{};
  uint32_t rtp_timestamp{};
  uint32_t packet_count{};
  uint32_t octet_count{};
  uint8_t block_version{1U};
  std::string ts_refclk;
  std::string media_clock{"direct=0"};
  std::string cname;
  IpmxVideoMediaInfo video;
  IpmxH264MediaInfo h264;
};

// TR-10-1 compound packet: IPMX Sender Report first, followed by SDES CNAME.
[[nodiscard]] std::vector<uint8_t>
make_ipmx_rtcp_compound(const IpmxRtcpSenderReport& report);

struct IpmxRtcpInspection {
  bool sender_report{};
  bool ipmx_info_block{};
  bool compressed_video_info{};
  bool h264_info{};
  bool sdes_cname{};
  uint8_t block_version{};
  uint32_t rtp_timestamp{};
  uint32_t packet_count{};
  uint32_t octet_count{};
};

[[nodiscard]] IpmxRtcpInspection inspect_ipmx_rtcp_compound(const std::vector<uint8_t>& packet);

} // namespace v0
} // namespace ipmx
