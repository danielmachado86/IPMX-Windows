#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ipmx {
inline namespace v0 {

inline constexpr uint16_t kIpmxCompressedVideoMediaInfoType = 0x0005U;

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
};

struct IpmxRtcpSenderReport {
  uint32_t ssrc{};
  uint32_t ptp_seconds{};
  uint32_t ptp_nanoseconds{};
  uint32_t rtp_timestamp{};
  uint32_t packet_count{};
  uint32_t octet_count{};
  std::string ts_refclk;
  std::string media_clock{"direct=0"};
  std::string cname;
  IpmxVideoMediaInfo video;
};

// TR-10-1 compound packet: IPMX Sender Report first, followed by SDES CNAME.
[[nodiscard]] std::vector<uint8_t>
make_ipmx_rtcp_compound(const IpmxRtcpSenderReport& report);

struct IpmxRtcpInspection {
  bool sender_report{};
  bool ipmx_info_block{};
  bool compressed_video_info{};
  bool sdes_cname{};
};

[[nodiscard]] IpmxRtcpInspection inspect_ipmx_rtcp_compound(const std::vector<uint8_t>& packet);

} // namespace v0
} // namespace ipmx
