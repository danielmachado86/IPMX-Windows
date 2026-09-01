#include "ipmx/rtcp.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

namespace ipmx {
inline namespace v0 {
namespace {

void append_u16(std::vector<uint8_t>& output, const uint16_t value) {
  output.push_back(static_cast<uint8_t>(value >> 8U));
  output.push_back(static_cast<uint8_t>(value));
}

void append_u32(std::vector<uint8_t>& output, const uint32_t value) {
  output.push_back(static_cast<uint8_t>(value >> 24U));
  output.push_back(static_cast<uint8_t>(value >> 16U));
  output.push_back(static_cast<uint8_t>(value >> 8U));
  output.push_back(static_cast<uint8_t>(value));
}

void append_u64(std::vector<uint8_t>& output, const uint64_t value) {
  append_u32(output, static_cast<uint32_t>(value >> 32U));
  append_u32(output, static_cast<uint32_t>(value));
}

void patch_u16(std::vector<uint8_t>& output, const size_t offset, const uint16_t value) {
  output.at(offset) = static_cast<uint8_t>(value >> 8U);
  output.at(offset + 1U) = static_cast<uint8_t>(value);
}

void append_fixed(std::vector<uint8_t>& output, const std::string_view value, const size_t size,
                  const char* field) {
  if (value.size() > size)
    throw std::invalid_argument(std::string("RTCP ") + field + " is too long");
  output.insert(output.end(), value.begin(), value.end());
  output.insert(output.end(), size - value.size(), 0U);
}

[[nodiscard]] uint16_t read_u16(const std::span<const uint8_t> input, const size_t offset) {
  if (offset + 2U > input.size())
    return 0U;
  return static_cast<uint16_t>((static_cast<uint16_t>(input[offset]) << 8U) | input[offset + 1U]);
}

[[nodiscard]] uint32_t read_u32(const std::span<const uint8_t> input, const size_t offset) {
  if (offset + 4U > input.size())
    return 0U;
  return (static_cast<uint32_t>(input[offset]) << 24U) |
         (static_cast<uint32_t>(input[offset + 1U]) << 16U) |
         (static_cast<uint32_t>(input[offset + 2U]) << 8U) | input[offset + 3U];
}

[[nodiscard]] std::string base64(const std::span<const uint8_t> input) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve((input.size() + 2U) / 3U * 4U);
  for (size_t offset = 0U; offset < input.size(); offset += 3U) {
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

void append_h264_media_info(std::vector<uint8_t>& output, const IpmxH264MediaInfo& info) {
  if (info.sprop_parameter_sets.size() > 255U ||
      info.sprop_level_parameter_sets.size() > 255U || info.extra.size() > 255U) {
    throw std::invalid_argument("RTCP H.264 variable field is too long");
  }
  uint32_t field_mask = 0x00000003U; // profile-level-id and packetization-mode.
  if (!info.sprop_parameter_sets.empty())
    field_mask |= 1U << 6U;
  if (!info.sprop_level_parameter_sets.empty())
    field_mask |= 1U << 7U;
  if (!info.extra.empty())
    field_mask |= 1U << 8U;

  const size_t block_start = output.size();
  append_u16(output, kIpmxH264MediaInfoType);
  append_u16(output, 0U);
  append_u32(output, field_mask);
  output.insert(output.end(), info.profile_level_id.begin(), info.profile_level_id.end());
  output.push_back(info.packetization_mode);
  append_u16(output, 0U); // sprop-max-don-diff.
  append_u16(output, 0U); // sprop-interleaving-depth.
  append_u32(output, 0U); // sprop-deint-buf-req.
  append_u32(output, 0U); // sprop-init-buf-time.
  output.push_back(static_cast<uint8_t>(info.sprop_parameter_sets.size()));
  output.push_back(static_cast<uint8_t>(info.sprop_level_parameter_sets.size()));
  output.push_back(static_cast<uint8_t>(info.extra.size()));
  output.push_back(0U);
  output.insert(output.end(), info.sprop_parameter_sets.begin(), info.sprop_parameter_sets.end());
  output.insert(output.end(), info.sprop_level_parameter_sets.begin(),
                info.sprop_level_parameter_sets.end());
  output.insert(output.end(), info.extra.begin(), info.extra.end());
  while ((output.size() - block_start) % 4U != 0U)
    output.push_back(0U);
  const size_t block_words = (output.size() - block_start) / 4U;
  if (block_words == 0U || block_words - 1U > std::numeric_limits<uint16_t>::max())
    throw std::length_error("RTCP H.264 Media Info Block is too large");
  patch_u16(output, block_start + 2U, static_cast<uint16_t>(block_words - 1U));
}

} // namespace

IpmxH264MediaInfo make_ipmx_h264_media_info(const NalUnit& sps, const NalUnit& pps) {
  if (sps.size() < 4U || (sps.front() & 0x1FU) != 7U || pps.empty() ||
      (pps.front() & 0x1FU) != 8U) {
    throw std::invalid_argument("invalid H.264 SPS/PPS for RTCP Media Info Block");
  }
  IpmxH264MediaInfo info;
  std::copy_n(sps.begin() + 1, info.profile_level_id.size(), info.profile_level_id.begin());
  info.sprop_parameter_sets = base64(sps) + ',' + base64(pps);
  return info;
}

std::vector<uint8_t> make_ipmx_rtcp_compound(const IpmxRtcpSenderReport& report) {
  if (report.ts_refclk.empty() || report.cname.empty() || report.video.width == 0U ||
      report.video.height == 0U || report.video.fps_numerator == 0U ||
      report.video.fps_denominator == 0U || report.video.fps_numerator >= (1U << 22U) ||
      report.video.fps_denominator >= (1U << 10U) || report.ptp_nanoseconds >= 1'000'000'000U ||
      report.h264.profile_level_id == std::array<uint8_t, 3U>{}) {
    throw std::invalid_argument("invalid IPMX RTCP sender report");
  }

  std::vector<uint8_t> output;
  output.reserve(320U);
  output.push_back(0x80U); // V=2, P=0, RC=0.
  output.push_back(200U);  // Sender Report.
  append_u16(output, 0U);
  append_u32(output, report.ssrc);
  append_u32(output, report.ptp_seconds);
  append_u32(output, report.ptp_nanoseconds);
  append_u32(output, report.rtp_timestamp);
  append_u32(output, report.packet_count);
  append_u32(output, report.octet_count);

  const size_t ipmx_start = output.size();
  append_u16(output, 0x5831U); // IPMX tag "X1".
  append_u16(output, 0U);
  output.push_back(report.block_version);
  output.insert(output.end(), 3U, 0U);
  append_fixed(output, report.ts_refclk, 64U, "ts-refclk");
  append_fixed(output, report.media_clock, 12U, "mediaclk");

  append_u16(output, kIpmxCompressedVideoMediaInfoType);
  append_u16(output, 22U); // 92 bytes / 4 - 1.
  append_fixed(output, report.video.sampling, 16U, "sampling");
  output.push_back(report.video.bit_depth); // F=0, bit depth in the low seven bits.
  output.push_back(0U);                     // M=0, progressive, segmented=0.
  output.push_back(1U);                     // PAR width.
  output.push_back(1U);                     // PAR height.
  append_fixed(output, report.video.range, 12U, "range");
  append_fixed(output, report.video.colorimetry, 20U, "colorimetry");
  append_fixed(output, report.video.transfer_characteristic, 16U, "TCS");
  if (report.video.width > std::numeric_limits<uint16_t>::max() ||
      report.video.height > std::numeric_limits<uint16_t>::max())
    throw std::invalid_argument("RTCP video dimensions are too large");
  append_u16(output, static_cast<uint16_t>(report.video.width));
  append_u16(output, static_cast<uint16_t>(report.video.height));
  append_u32(output, (report.video.fps_numerator << 10U) | report.video.fps_denominator);
  const uint64_t pixels_per_second = report.video.measured_pixel_clock != 0U
                                         ? report.video.measured_pixel_clock
                                         : (static_cast<uint64_t>(report.video.width) *
                                                report.video.height * report.video.fps_numerator +
                                            report.video.fps_denominator / 2U) /
                                               report.video.fps_denominator;
  append_u64(output, pixels_per_second);
  append_u16(output, report.video.htotal != 0U ? report.video.htotal
                                               : static_cast<uint16_t>(report.video.width));
  append_u16(output, report.video.vtotal != 0U ? report.video.vtotal
                                               : static_cast<uint16_t>(report.video.height));
  append_h264_media_info(output, report.h264);
  if ((output.size() - ipmx_start) % 4U != 0U)
    throw std::logic_error("internal IPMX Info Block alignment error");
  const size_t ipmx_words = (output.size() - ipmx_start) / 4U;
  const size_t sender_report_words = output.size() / 4U;
  if (ipmx_words - 1U > std::numeric_limits<uint16_t>::max() ||
      sender_report_words - 1U > std::numeric_limits<uint16_t>::max()) {
    throw std::length_error("RTCP Sender Report is too large");
  }
  patch_u16(output, ipmx_start + 2U, static_cast<uint16_t>(ipmx_words - 1U));
  patch_u16(output, 2U, static_cast<uint16_t>(sender_report_words - 1U));

  const size_t sdes_start = output.size();
  output.push_back(0x81U); // V=2, one source chunk.
  output.push_back(202U);  // SDES.
  append_u16(output, 0U);
  append_u32(output, report.ssrc);
  if (report.cname.size() > 255U)
    throw std::invalid_argument("RTCP CNAME is too long");
  output.push_back(1U);
  output.push_back(static_cast<uint8_t>(report.cname.size()));
  output.insert(output.end(), report.cname.begin(), report.cname.end());
  output.push_back(0U);
  while ((output.size() - sdes_start) % 4U != 0U)
    output.push_back(0U);
  const uint16_t sdes_length = static_cast<uint16_t>((output.size() - sdes_start) / 4U - 1U);
  output[sdes_start + 2U] = static_cast<uint8_t>(sdes_length >> 8U);
  output[sdes_start + 3U] = static_cast<uint8_t>(sdes_length);
  return output;
}

IpmxRtcpInspection inspect_ipmx_rtcp_compound(const std::vector<uint8_t>& packet) {
  IpmxRtcpInspection result;
  const std::span<const uint8_t> bytes(packet);
  if (bytes.size() < 112U || (bytes[0] >> 6U) != 2U || bytes[1] != 200U)
    return result;
  const size_t sender_report_bytes = (static_cast<size_t>(read_u16(bytes, 2U)) + 1U) * 4U;
  if (sender_report_bytes < 108U || sender_report_bytes > bytes.size())
    return result;
  result.sender_report = true;
  result.rtp_timestamp = read_u32(bytes, 16U);
  result.packet_count = read_u32(bytes, 20U);
  result.octet_count = read_u32(bytes, 24U);
  result.ipmx_info_block = read_u16(bytes, 28U) == 0x5831U;
  if (!result.ipmx_info_block)
    return result;
  result.block_version = bytes[32U];
  const size_t ipmx_bytes = (static_cast<size_t>(read_u16(bytes, 30U)) + 1U) * 4U;
  if (28U + ipmx_bytes != sender_report_bytes || ipmx_bytes < 84U)
    return result;
  size_t media_offset = 112U;
  while (media_offset + 4U <= sender_report_bytes) {
    const uint16_t type = read_u16(bytes, media_offset);
    const size_t block_bytes = (static_cast<size_t>(read_u16(bytes, media_offset + 2U)) + 1U) * 4U;
    if (block_bytes < 4U || media_offset + block_bytes > sender_report_bytes)
      return result;
    result.compressed_video_info = result.compressed_video_info || type == 0x0005U;
    result.h264_info = result.h264_info || type == 0x000AU;
    media_offset += block_bytes;
  }
  if (media_offset != sender_report_bytes)
    return result;
  if (bytes.size() >= sender_report_bytes + 10U &&
      (bytes[sender_report_bytes] >> 6U) == 2U && bytes[sender_report_bytes + 1U] == 202U &&
      bytes[sender_report_bytes + 8U] == 1U) {
    const size_t cname_size = bytes[sender_report_bytes + 9U];
    result.sdes_cname = sender_report_bytes + 10U + cname_size <= bytes.size();
  }
  return result;
}

} // namespace v0
} // namespace ipmx
