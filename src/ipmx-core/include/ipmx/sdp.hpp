#pragma once

#include "ipmx/annexb.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

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
};

[[nodiscard]] std::string make_sdp(const SdpSettings& settings,
                                   const NalUnit& sps,
                                   const NalUnit& pps);
void write_sdp(const std::filesystem::path& path,
               const SdpSettings& settings,
               const NalUnit& sps,
               const NalUnit& pps);
[[nodiscard]] SdpSettings read_sdp(const std::filesystem::path& path);

} // namespace v0
} // namespace ipmx
