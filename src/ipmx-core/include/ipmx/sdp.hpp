#pragma once

#include "ipmx/annexb.hpp"
#include "ipmx/network_limits.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ipmx {
inline namespace v0 {

struct SdpSettings {
  std::string multicast_group{"239.255.42.42"};
  uint16_t port{5004U};
  uint8_t payload_type{96U};
  uint32_t width{1280U};
  uint32_t height{720U};
  uint32_t fps_numerator{60U};
  uint32_t fps_denominator{1U};
  uint32_t target_bitrate_kbps{4'000U};
  uint32_t maximum_ip_bitrate_kbps{4'400U};
  uint16_t maximum_udp_bytes{1'200U};
  std::string ts_refclk;
  std::string media_clock{"direct=0"};
};

struct SdpDescription {
  SdpSettings settings;
  NalUnit sps;
  NalUnit pps;
  std::optional<uint16_t> rtcp_port;
  std::optional<uint32_t> framesize_width;
  std::optional<uint32_t> framesize_height;
  std::optional<uint8_t> framesize_payload_type;
  std::optional<uint32_t> fmtp_width;
  std::optional<uint32_t> fmtp_height;
  std::string rtp_encoding;
  uint32_t rtp_clock_rate{};
  std::optional<uint8_t> rtpmap_payload_type;
  std::optional<uint8_t> fmtp_payload_type;
  std::string traffic_profile;
  std::string sampling;
  std::string colorimetry;
  std::string transfer_characteristic;
  std::string range;
  std::string profile_level_id;
  std::optional<uint8_t> depth;
  std::optional<uint8_t> packetization_mode;
  bool exact_frame_rate_seen{};
  bool ipmx{};
  bool media_seen{};
  bool bitrate_seen{};
  bool fmtp_seen{};
};

inline constexpr uint16_t kMinimumIpmxMediaPort = 1'026U;
inline constexpr uint16_t kRecommendedMinimumIpmxMediaPort = 5'002U;
inline constexpr uint16_t kMaximumIpmxMediaPort = 65'534U;

[[nodiscard]] bool is_valid_ipmx_media_port(uint16_t port) noexcept;
[[nodiscard]] bool is_recommended_ipmx_media_port(uint16_t port) noexcept;
void validate_ipmx_media_port(uint16_t port);
[[nodiscard]] uint16_t reserved_rtcp_port(uint16_t media_port);
[[nodiscard]] uint32_t minimum_ip_bitrate_kbps(uint32_t elementary_bitrate_kbps,
                                               uint16_t maximum_udp_bytes);

[[nodiscard]] std::string make_sdp(const SdpSettings& settings, const NalUnit& sps,
                                   const NalUnit& pps);
void write_sdp(const std::filesystem::path& path, const SdpSettings& settings, const NalUnit& sps,
               const NalUnit& pps);
[[nodiscard]] SdpSettings read_sdp(const std::filesystem::path& path);
[[nodiscard]] SdpDescription read_sdp_description(const std::filesystem::path& path);
[[nodiscard]] std::vector<std::string>
validate_ipmx_sdp(const SdpDescription& description);

} // namespace v0
} // namespace ipmx
