#include "ipmx/sdp.hpp"
#include "ipmx/h264.hpp"
#include "ipmx/rtp.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace ipmx {
inline namespace v0 {
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

[[nodiscard]] std::optional<NalUnit> decode_base64(const std::string_view input) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  NalUnit output;
  uint32_t accumulator = 0U;
  unsigned bits = 0U;
  bool padding = false;
  for (const char character : input) {
    if (character == '=') {
      padding = true;
      continue;
    }
    if (padding)
      return std::nullopt;
    const auto position = alphabet.find(character);
    if (position == std::string_view::npos)
      return std::nullopt;
    accumulator = static_cast<uint32_t>((accumulator << 6U) | position);
    bits += 6U;
    if (bits >= 8U) {
      bits -= 8U;
      output.push_back(static_cast<uint8_t>(accumulator >> bits));
      accumulator &= bits == 0U ? 0U : (1U << bits) - 1U;
    }
  }
  return output;
}

[[nodiscard]] std::string_view trim(const std::string_view value) noexcept {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string_view::npos)
    return {};
  const auto last = value.find_last_not_of(" \t");
  return value.substr(first, last - first + 1U);
}

[[nodiscard]] std::string lowercase(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

[[nodiscard]] bool iequals(const std::string_view left, const std::string_view right) noexcept {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), [](const char a, const char b) {
           return std::tolower(static_cast<unsigned char>(a)) ==
                  std::tolower(static_cast<unsigned char>(b));
         });
}

template <typename T>
[[nodiscard]] T parse_integer(const std::string_view text, const char* field) {
  unsigned long long value = 0U;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      value > static_cast<unsigned long long>(std::numeric_limits<T>::max())) {
    throw std::runtime_error(std::string("invalid SDP ") + field);
  }
  return static_cast<T>(value);
}

void parse_exact_frame_rate(const std::string_view text, SdpSettings& settings) {
  const auto slash = text.find('/');
  settings.fps_numerator = parse_integer<uint32_t>(text.substr(0U, slash), "exactframerate");
  settings.fps_denominator =
      slash == std::string_view::npos
          ? 1U
          : parse_integer<uint32_t>(text.substr(slash + 1U), "exactframerate");
  if (settings.fps_numerator == 0U || settings.fps_denominator == 0U) {
    throw std::runtime_error("invalid SDP exactframerate");
  }
}

[[nodiscard]] std::unordered_map<std::string, std::string> parse_fmtp(const std::string_view text) {
  std::unordered_map<std::string, std::string> values;
  size_t offset = 0U;
  while (offset <= text.size()) {
    const size_t end = text.find(';', offset);
    const std::string_view token = trim(
        text.substr(offset, end == std::string_view::npos ? text.size() - offset : end - offset));
    if (!token.empty()) {
      const size_t equals = token.find('=');
      const std::string key = lowercase(trim(token.substr(0U, equals)));
      const std::string value(equals == std::string_view::npos ? std::string_view{}
                                                               : trim(token.substr(equals + 1U)));
      values[key] = value;
    }
    if (end == std::string_view::npos)
      break;
    offset = end + 1U;
  }
  return values;
}

[[nodiscard]] std::string profile_level_id(const NalUnit& sps) {
  if (sps.size() < 4U || (sps[0] & 0x1FU) != 7U) {
    throw std::invalid_argument("invalid H.264 SPS");
  }
  std::ostringstream output;
  output << std::uppercase << std::hex << std::setfill('0') << std::setw(2)
         << static_cast<unsigned>(sps[1]) << std::setw(2) << static_cast<unsigned>(sps[2])
         << std::setw(2) << static_cast<unsigned>(sps[3]);
  return output.str();
}

} // namespace

bool is_valid_ipmx_media_port(const uint16_t port) noexcept {
  return port >= kMinimumIpmxMediaPort && port <= kMaximumIpmxMediaPort && (port & 1U) == 0U;
}

bool is_recommended_ipmx_media_port(const uint16_t port) noexcept {
  return is_valid_ipmx_media_port(port) && port >= kRecommendedMinimumIpmxMediaPort;
}

void validate_ipmx_media_port(const uint16_t port) {
  if (!is_valid_ipmx_media_port(port)) {
    throw std::invalid_argument(
        "IPMX RTP media port must be even, greater than 1024, and leave media+1 for RTCP");
  }
}

uint16_t reserved_rtcp_port(const uint16_t media_port) {
  validate_ipmx_media_port(media_port);
  return static_cast<uint16_t>(media_port + 1U);
}

uint32_t minimum_ip_bitrate_kbps(const uint32_t elementary_bitrate_kbps,
                                 const uint16_t maximum_udp_bytes) {
  if (elementary_bitrate_kbps == 0U || maximum_udp_bytes <= 30U ||
      maximum_udp_bytes > kMaximumStandardUdpPayloadBytes) {
    throw std::invalid_argument("invalid bitrate or MAXUDP");
  }
  const uint64_t fragment_overhead =
      (static_cast<uint64_t>(elementary_bitrate_kbps) * 58U + maximum_udp_bytes - 31U) /
      (maximum_udp_bytes - 30U);
  const uint64_t overhead =
      std::max<uint64_t>({128U, (elementary_bitrate_kbps + 9U) / 10U, fragment_overhead});
  const uint64_t result = static_cast<uint64_t>(elementary_bitrate_kbps) + overhead;
  if (result > std::numeric_limits<uint32_t>::max()) {
    throw std::invalid_argument("bitrate is too large");
  }
  return static_cast<uint32_t>(result);
}

std::string make_sdp(const SdpSettings& settings, const NalUnit& sps, const NalUnit& pps) {
  if (settings.fps_numerator == 0U || settings.fps_denominator == 0U) {
    throw std::invalid_argument("invalid SDP frame rate");
  }
  if (settings.ts_refclk.empty() || settings.media_clock.empty()) {
    throw std::invalid_argument("SDP ts-refclk and mediaclk are required");
  }
  validate_ipmx_media_port(settings.port);
  if (settings.maximum_udp_bytes < 64U ||
      settings.maximum_udp_bytes > kMaximumStandardUdpPayloadBytes) {
    throw std::invalid_argument("SDP MAXUDP must be between 64 and " +
                                std::to_string(kMaximumStandardUdpPayloadBytes) + " bytes");
  }
  if (settings.maximum_ip_bitrate_kbps <
      minimum_ip_bitrate_kbps(settings.target_bitrate_kbps, settings.maximum_udp_bytes)) {
    throw std::invalid_argument("SDP IP bitrate must include encoded media and IP overhead");
  }
  const auto parsed_sps = parse_h264_sps(sps);
  const auto parsed_pps = parse_h264_pps(pps);
  if (!parsed_sps || !parsed_pps || parsed_pps->sps_id != parsed_sps->sps_id) {
    throw std::invalid_argument("invalid or mismatched H.264 SPS/PPS");
  }
  const auto conformance =
      validate_ipmx_sps(*parsed_sps, {settings.width, settings.height, settings.fps_numerator,
                                      settings.fps_denominator});
  if (!conformance.empty())
    throw std::invalid_argument("H.264 SPS does not conform to the IPMX H.264 profile");
  std::ostringstream sdp;
  sdp << "v=0\r\n"
      << "o=- 0 0 IN IP4 127.0.0.1\r\n"
      << "s=IPMX Windows H.264 loopback\r\n"
      << "c=IN IP4 " << settings.multicast_group << "/1\r\n"
      << "t=0 0\r\n"
      << "m=video " << settings.port << " RTP/AVP " << static_cast<unsigned>(settings.payload_type)
      << "\r\n"
      << "b=AS:" << settings.maximum_ip_bitrate_kbps << "\r\n"
      << "a=rtcp:" << reserved_rtcp_port(settings.port) << " IN IP4 " << settings.multicast_group
      << "\r\n"
      << "a=ts-refclk:" << settings.ts_refclk << "\r\n"
      << "a=mediaclk:" << settings.media_clock << "\r\n"
      << "a=rtpmap:" << static_cast<unsigned>(settings.payload_type) << " H264/90000\r\n"
      << "a=fmtp:" << static_cast<unsigned>(settings.payload_type) << " width=" << settings.width
      << ";height=" << settings.height << ";depth=8;exactframerate=" << settings.fps_numerator
      << '/' << settings.fps_denominator
      << ";sampling=YCbCr-4:2:0;colorimetry=BT709;TP=2110TPW;MAXUDP=" << settings.maximum_udp_bytes
      << ";TCS=SDR;RANGE=NARROW;IPMX"
      << ";packetization-mode=1;profile-level-id=" << profile_level_id(sps)
      << ";sprop-parameter-sets=" << base64(sps) << ',' << base64(pps) << "\r\n"
      << "a=framerate:" << std::fixed << std::setprecision(3)
      << static_cast<double>(settings.fps_numerator) / settings.fps_denominator << "\r\n"
      << "a=framesize:" << static_cast<unsigned>(settings.payload_type) << ' ' << settings.width
      << '-' << settings.height << "\r\n"
      << "a=extmap:" << static_cast<unsigned>(kCaptureTimeExtensionId) << ' '
      << kCaptureTimeExtensionUri << "\r\n"
      << "a=sendonly\r\n";
  return sdp.str();
}

void write_sdp(const std::filesystem::path& path, const SdpSettings& settings, const NalUnit& sps,
               const NalUnit& pps) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot create SDP file");
  }
  output << make_sdp(settings, sps, pps);
  if (!output) {
    throw std::runtime_error("cannot write SDP file");
  }
}

SdpDescription read_sdp_description(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open SDP file");
  }
  SdpDescription description;
  auto& settings = description.settings;
  settings.width = 0U;
  settings.height = 0U;
  settings.fps_numerator = 0U;
  settings.fps_denominator = 0U;
  settings.target_bitrate_kbps = 0U; // Not represented by an IPMX SDP b=AS line.
  settings.maximum_ip_bitrate_kbps = 0U;
  settings.maximum_udp_bytes = 0U;
  settings.media_clock.clear();
  bool exact_frame_rate_seen = false;
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
      if (!(fields >> port >> transport >> payload_type) ||
          port > std::numeric_limits<uint16_t>::max() || payload_type > 127U) {
        throw std::runtime_error("invalid SDP media line");
      }
      settings.port = static_cast<uint16_t>(port);
      settings.payload_type = static_cast<uint8_t>(payload_type);
      description.media_seen = iequals(transport, "RTP/AVP");
    } else if (line.starts_with("b=AS:")) {
      settings.maximum_ip_bitrate_kbps = parse_integer<uint32_t>(line.substr(5U), "AS bitrate");
      if (settings.maximum_ip_bitrate_kbps == 0U)
        throw std::runtime_error("invalid SDP AS bitrate");
      description.bitrate_seen = true;
    } else if (line.starts_with("a=rtcp:")) {
      const auto space = line.find(' ');
      description.rtcp_port = parse_integer<uint16_t>(
          std::string_view(line).substr(7U,
                                        space == std::string::npos ? line.size() - 7U : space - 7U),
          "RTCP port");
    } else if (line.starts_with("a=rtpmap:")) {
      const auto space = line.find(' ');
      if (space == std::string::npos)
        throw std::runtime_error("invalid SDP rtpmap line");
      description.rtpmap_payload_type = parse_integer<uint8_t>(
          std::string_view(line).substr(9U, space - 9U), "RTP payload type");
      const std::string_view encoding = std::string_view(line).substr(space + 1U);
      const auto slash = encoding.find('/');
      if (slash == std::string_view::npos)
        throw std::runtime_error("invalid SDP rtpmap encoding");
      description.rtp_encoding = std::string(encoding.substr(0U, slash));
      description.rtp_clock_rate =
          parse_integer<uint32_t>(encoding.substr(slash + 1U), "RTP clock rate");
    } else if (line.starts_with("a=fmtp:")) {
      const auto space = line.find(' ');
      if (space == std::string::npos)
        throw std::runtime_error("invalid SDP fmtp line");
      description.fmtp_payload_type = parse_integer<uint8_t>(
          std::string_view(line).substr(7U, space - 7U), "fmtp payload type");
      const auto values = parse_fmtp(std::string_view(line).substr(space + 1U));
      description.fmtp_seen = true;
      if (const auto match = values.find("width"); match != values.end()) {
        description.fmtp_width = parse_integer<uint32_t>(match->second, "width");
        settings.width = *description.fmtp_width;
      }
      if (const auto match = values.find("height"); match != values.end()) {
        description.fmtp_height = parse_integer<uint32_t>(match->second, "height");
        settings.height = *description.fmtp_height;
      }
      if (const auto match = values.find("exactframerate"); match != values.end()) {
        parse_exact_frame_rate(match->second, settings);
        exact_frame_rate_seen = true;
        description.exact_frame_rate_seen = true;
      }
      if (const auto match = values.find("maxudp"); match != values.end()) {
        settings.maximum_udp_bytes = parse_integer<uint16_t>(match->second, "MAXUDP");
      }
      if (const auto match = values.find("tp"); match != values.end())
        description.traffic_profile = match->second;
      if (const auto match = values.find("sampling"); match != values.end())
        description.sampling = match->second;
      if (const auto match = values.find("colorimetry"); match != values.end())
        description.colorimetry = match->second;
      if (const auto match = values.find("tcs"); match != values.end())
        description.transfer_characteristic = match->second;
      if (const auto match = values.find("range"); match != values.end())
        description.range = match->second;
      if (const auto match = values.find("depth"); match != values.end())
        description.depth = parse_integer<uint8_t>(match->second, "depth");
      if (const auto match = values.find("packetization-mode"); match != values.end())
        description.packetization_mode =
            parse_integer<uint8_t>(match->second, "packetization-mode");
      description.ipmx = values.contains("ipmx");
      if (const auto match = values.find("profile-level-id"); match != values.end()) {
        description.profile_level_id = match->second;
      }
      if (const auto match = values.find("sprop-parameter-sets"); match != values.end()) {
        const auto comma = match->second.find(',');
        if (comma == std::string::npos)
          throw std::runtime_error("invalid SDP parameter sets");
        const auto decoded_sps = decode_base64(std::string_view(match->second).substr(0U, comma));
        const auto decoded_pps = decode_base64(std::string_view(match->second).substr(comma + 1U));
        if (!decoded_sps || !decoded_pps || !parse_h264_sps(*decoded_sps) ||
            !parse_h264_pps(*decoded_pps))
          throw std::runtime_error("invalid SDP SPS/PPS");
        description.sps = *decoded_sps;
        description.pps = *decoded_pps;
      }
    } else if (line.starts_with("a=framesize:")) {
      const auto space = line.find(' ');
      const auto dash = line.find('-', space == std::string::npos ? 0U : space + 1U);
      if (space == std::string::npos || dash == std::string::npos) {
        throw std::runtime_error("invalid SDP framesize line");
      }
      description.framesize_payload_type = parse_integer<uint8_t>(
          std::string_view(line).substr(12U, space - 12U), "framesize payload type");
      description.framesize_width = parse_integer<uint32_t>(
          std::string_view(line).substr(space + 1U, dash - space - 1U), "framesize width");
      description.framesize_height =
          parse_integer<uint32_t>(std::string_view(line).substr(dash + 1U), "framesize height");
      if (settings.width == 0U)
        settings.width = *description.framesize_width;
      if (settings.height == 0U)
        settings.height = *description.framesize_height;
    } else if (line.starts_with("a=framerate:") && !exact_frame_rate_seen) {
      const double fps = std::stod(line.substr(12U));
      if (fps <= 0.0 || fps > 1'000.0) {
        throw std::runtime_error("invalid SDP frame rate");
      }
      settings.fps_numerator = static_cast<uint32_t>(std::llround(fps * 1'000.0));
      settings.fps_denominator = 1'000U;
    } else if (line.starts_with("a=ts-refclk:")) {
      settings.ts_refclk = line.substr(12U);
    } else if (line.starts_with("a=mediaclk:")) {
      settings.media_clock = line.substr(11U);
    }
  }
  return description;
}

std::vector<std::string> validate_ipmx_sdp(const SdpDescription& description) {
  std::vector<std::string> errors;
  const auto& settings = description.settings;
  const auto require = [&errors](const bool condition, const char* message) {
    if (!condition)
      errors.emplace_back(message);
  };
  require(description.media_seen, "missing RTP/AVP video media line");
  require(description.bitrate_seen && settings.maximum_ip_bitrate_kbps != 0U,
          "missing b=AS IP bitrate");
  require(is_valid_ipmx_media_port(settings.port),
          "media port must be even, greater than 1024, and reserve media+1");
  require(description.rtcp_port && is_valid_ipmx_media_port(settings.port) &&
              *description.rtcp_port == static_cast<uint16_t>(settings.port + 1U),
          "missing RTCP reservation on media+1");
  require(description.rtpmap_payload_type == settings.payload_type &&
              iequals(description.rtp_encoding, "H264") && description.rtp_clock_rate == 90'000U,
          "RTP mapping must be H264/90000 and match the media payload type");
  require(description.fmtp_seen && description.fmtp_payload_type == settings.payload_type,
          "missing or mismatched H.264 fmtp line");
  require(description.fmtp_width && description.fmtp_height && *description.fmtp_width != 0U &&
              *description.fmtp_height != 0U,
          "missing fmtp width/height");
  require(description.exact_frame_rate_seen, "missing fmtp exactframerate");
  require(description.packetization_mode == 1U, "missing packetization-mode=1");
  require(iequals(description.traffic_profile, "2110TPW"), "missing TP=2110TPW");
  require(settings.maximum_udp_bytes >= 64U &&
              settings.maximum_udp_bytes <= kMaximumStandardUdpPayloadBytes,
          "missing or invalid MAXUDP");
  require(description.depth == 8U && iequals(description.sampling, "YCbCr-4:2:0"),
          "sender must signal 8-bit YCbCr-4:2:0");
  require(iequals(description.colorimetry, "BT709") &&
              iequals(description.transfer_characteristic, "SDR") &&
              iequals(description.range, "NARROW"),
          "missing or unsupported colorimetry, TCS, or range");
  require(description.ipmx, "missing IPMX fmtp keyword");
  require(!settings.ts_refclk.empty(), "missing a=ts-refclk");
  require(iequals(settings.media_clock, "direct=0") ||
              iequals(settings.media_clock, "sender/direct=0"),
          "missing synchronous a=mediaclk:direct=0");
  if (description.framesize_width && description.framesize_height) {
    require(description.framesize_payload_type == settings.payload_type &&
                description.fmtp_width && description.fmtp_height &&
                *description.framesize_width == *description.fmtp_width &&
                *description.framesize_height == *description.fmtp_height,
            "a=framesize disagrees with fmtp width/height");
  }
  require(!description.sps.empty() && !description.pps.empty(), "missing SPS/PPS parameter sets");
  if (!description.sps.empty()) {
    require(iequals(description.profile_level_id, profile_level_id(description.sps)),
            "profile-level-id does not match SPS");
    if (const auto parsed_sps = parse_h264_sps(description.sps)) {
      const auto h264_errors = validate_ipmx_sps(
          *parsed_sps,
          {settings.width, settings.height, settings.fps_numerator, settings.fps_denominator});
      errors.insert(errors.end(), h264_errors.begin(), h264_errors.end());
    } else {
      errors.emplace_back("invalid SPS");
    }
  }
  if (!description.pps.empty() && !parse_h264_pps(description.pps))
    errors.emplace_back("invalid PPS");
  return errors;
}

SdpSettings read_sdp(const std::filesystem::path& path) {
  return read_sdp_description(path).settings;
}

} // namespace v0
} // namespace ipmx
