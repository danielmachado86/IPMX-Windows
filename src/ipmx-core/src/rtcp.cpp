#include "ipmx/rtcp.hpp"

#include <algorithm>
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

} // namespace

std::vector<uint8_t> make_ipmx_rtcp_compound(const IpmxRtcpSenderReport& report) {
  if (report.ts_refclk.empty() || report.cname.empty() || report.video.width == 0U ||
      report.video.height == 0U || report.video.fps_numerator == 0U ||
      report.video.fps_denominator == 0U || report.video.fps_numerator >= (1U << 22U) ||
      report.video.fps_denominator >= (1U << 10U) || report.ptp_nanoseconds >= 1'000'000'000U) {
    throw std::invalid_argument("invalid IPMX RTCP sender report");
  }

  std::vector<uint8_t> output;
  output.reserve(256U);
  output.push_back(0x80U); // V=2, P=0, RC=0.
  output.push_back(200U);  // Sender Report.
  append_u16(output, 50U); // 204 bytes / 4 - 1.
  append_u32(output, report.ssrc);
  append_u32(output, report.ptp_seconds);
  append_u32(output, report.ptp_nanoseconds);
  append_u32(output, report.rtp_timestamp);
  append_u32(output, report.packet_count);
  append_u32(output, report.octet_count);

  append_u16(output, 0x5831U); // IPMX tag "X1".
  append_u16(output, 43U);     // 176 bytes / 4 - 1.
  output.push_back(1U);        // IPMX Info Block version.
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
  const uint64_t pixels_per_second =
      (static_cast<uint64_t>(report.video.width) * report.video.height *
           report.video.fps_numerator +
       report.video.fps_denominator / 2U) /
      report.video.fps_denominator;
  append_u64(output, pixels_per_second);
  append_u16(output, static_cast<uint16_t>(report.video.width));
  append_u16(output, static_cast<uint16_t>(report.video.height));
  if (output.size() != 204U)
    throw std::logic_error("internal RTCP Sender Report size error");

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
  if (bytes.size() < 204U || (bytes[0] >> 6U) != 2U || bytes[1] != 200U)
    return result;
  const size_t sender_report_bytes = (static_cast<size_t>(read_u16(bytes, 2U)) + 1U) * 4U;
  if (sender_report_bytes != 204U || sender_report_bytes > bytes.size())
    return result;
  result.sender_report = true;
  result.ipmx_info_block = read_u16(bytes, 28U) == 0x5831U;
  result.compressed_video_info = result.ipmx_info_block && read_u16(bytes, 112U) == 0x0005U;
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
