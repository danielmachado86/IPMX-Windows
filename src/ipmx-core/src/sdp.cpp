#include "ipmx/phase0/sdp.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace phase0 {
namespace {

[[nodiscard]] std::string base64(const std::span<const uint8_t> input) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve((input.size() + 2U) / 3U * 4U);
  for (size_t offset = 0; offset < input.size(); offset += 3U) {
    const uint32_t a = input[offset];
    const uint32_t b = offset + 1U < input.size() ? input[offset + 1U] : 0U;
    const uint32_t c = offset + 2U < input.size() ? input[offset + 2U] : 0U;
    const uint32_t value = (a << 16U) | (b << 8U) | c;
    output.push_back(alphabet[(value >> 18U) & 0x3FU]);
    output.push_back(alphabet[(value >> 12U) & 0x3FU]);
    output.push_back(offset + 1U < input.size() ? alphabet[(value >> 6U) & 0x3FU] : '=');
    output.push_back(offset + 2U < input.size() ? alphabet[value & 0x3FU] : '=');
  }
  return output;
}

[[nodiscard]] std::string profile_level_id(const NalUnit& sps) {
  if (sps.size() < 4U || (sps[0] & 0x1FU) != 7U) {
    throw std::invalid_argument("invalid H.264 SPS");
  }
  std::ostringstream output;
  output << std::uppercase << std::hex << std::setfill('0')
         << std::setw(2) << static_cast<unsigned>(sps[1])
         << std::setw(2) << static_cast<unsigned>(sps[2])
         << std::setw(2) << static_cast<unsigned>(sps[3]);
  return output.str();
}

} // namespace

std::string make_phase0_sdp(const SdpSettings& settings, const NalUnit& sps, const NalUnit& pps) {
  if (settings.fps_numerator == 0U || settings.fps_denominator == 0U) {
    throw std::invalid_argument("invalid SDP frame rate");
  }
  std::ostringstream sdp;
  sdp << "v=0\r\n"
      << "o=- 0 0 IN IP4 127.0.0.1\r\n"
      << "s=IPMX Windows Phase 0 H.264 loopback\r\n"
      << "c=IN IP4 " << settings.multicast_group << "/1\r\n"
      << "t=0 0\r\n"
      << "m=video " << settings.port << " RTP/AVP " << static_cast<unsigned>(settings.payload_type)
      << "\r\n"
      << "b=AS:" << settings.target_bitrate_kbps << "\r\n"
      << "a=rtpmap:" << static_cast<unsigned>(settings.payload_type) << " H264/90000\r\n"
      << "a=fmtp:" << static_cast<unsigned>(settings.payload_type)
      << " packetization-mode=1;profile-level-id=" << profile_level_id(sps)
      << ";sprop-parameter-sets=" << base64(sps) << ',' << base64(pps) << "\r\n"
      << "a=framerate:" << std::fixed << std::setprecision(3)
      << static_cast<double>(settings.fps_numerator) / settings.fps_denominator << "\r\n"
      << "a=framesize:" << static_cast<unsigned>(settings.payload_type) << ' '
      << settings.width << '-' << settings.height << "\r\n"
      << "a=extmap:1 urn:ipmx-windows:phase0:capture-time-ns\r\n"
      << "a=sendonly\r\n";
  return sdp.str();
}

void write_phase0_sdp(const std::filesystem::path& path, const SdpSettings& settings,
                      const NalUnit& sps, const NalUnit& pps) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot create SDP file");
  }
  output << make_phase0_sdp(settings, sps, pps);
  if (!output) {
    throw std::runtime_error("cannot write SDP file");
  }
}

SdpSettings read_phase0_sdp(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open SDP file");
  }
  SdpSettings settings;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.starts_with("c=IN IP4 ")) {
      settings.multicast_group = line.substr(9U);
      if (const auto slash = settings.multicast_group.find('/'); slash != std::string::npos) {
        settings.multicast_group.resize(slash);
      }
    } else if (line.starts_with("m=video ")) {
      std::istringstream fields(line.substr(8U));
      unsigned port = 0U;
      std::string transport;
      unsigned payload_type = 0U;
      if (!(fields >> port >> transport >> payload_type) || port > std::numeric_limits<uint16_t>::max() ||
          payload_type > 127U) {
        throw std::runtime_error("invalid SDP media line");
      }
      settings.port = static_cast<uint16_t>(port);
      settings.payload_type = static_cast<uint8_t>(payload_type);
    } else if (line.starts_with("b=AS:")) {
      settings.target_bitrate_kbps = static_cast<uint32_t>(std::stoul(line.substr(5U)));
    } else if (line.starts_with("a=framesize:")) {
      const auto space = line.find(' ');
      const auto dash = line.find('-', space == std::string::npos ? 0U : space + 1U);
      if (space == std::string::npos || dash == std::string::npos) {
        throw std::runtime_error("invalid SDP framesize line");
      }
      settings.width = static_cast<uint32_t>(std::stoul(line.substr(space + 1U, dash - space - 1U)));
      settings.height = static_cast<uint32_t>(std::stoul(line.substr(dash + 1U)));
    } else if (line.starts_with("a=framerate:")) {
      const double fps = std::stod(line.substr(12U));
      if (fps <= 0.0 || fps > 1'000.0) {
        throw std::runtime_error("invalid SDP frame rate");
      }
      settings.fps_numerator = static_cast<uint32_t>(std::llround(fps * 1'000.0));
      settings.fps_denominator = 1'000U;
    }
  }
  return settings;
}

} // namespace phase0
