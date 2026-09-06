#include "ipmx/annexb.hpp"
#include "ipmx/h264.hpp"
#include "ipmx/receiver/mf_h264_decoder.hpp"
#include "ipmx/rtcp.hpp"
#include "ipmx/rtp.hpp"
#include "ipmx/sender/frame_transmitter.hpp"
#include "ipmx/sender/x264_encoder.hpp"
#include "ipmx/types.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

void require(const bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

[[nodiscard]] std::vector<uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot open golden fixture: " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] uint16_t read_u16_be(const std::span<const uint8_t> bytes, const size_t offset) {
  return static_cast<uint16_t>((static_cast<uint16_t>(bytes[offset]) << 8U) | bytes[offset + 1U]);
}

[[nodiscard]] uint32_t read_u32_le(const std::span<const uint8_t> bytes, const size_t offset) {
  return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<uint32_t>(bytes[offset + 3U]) << 24U);
}

void validate_access_units(const std::vector<ipmx::NalUnit>& nals) {
  bool saw_sps = false;
  bool saw_pps = false;
  bool saw_idr = false;
  bool saw_buffering_period = false;
  size_t picture_timings = 0U;
  for (const auto& nal : nals) {
    switch (ipmx::h264_nal_type(nal)) {
    case ipmx::kH264NalSps: {
      const auto sps = ipmx::parse_h264_sps(nal);
      require(sps.has_value(), "golden SPS parses");
      require(ipmx::validate_ipmx_sps(*sps, {1280U, 720U, 60U, 1U}).empty(),
              "golden SPS conforms to the IPMX H.264 profile");
      saw_sps = true;
      break;
    }
    case ipmx::kH264NalPps:
      require(ipmx::parse_h264_pps(nal).has_value(), "golden PPS parses");
      saw_pps = true;
      break;
    case ipmx::kH264NalIdr:
      saw_idr = true;
      break;
    case ipmx::kH264NalSei: {
      const auto messages = ipmx::parse_h264_sei(nal);
      require(messages.has_value(), "golden SEI parses");
      saw_buffering_period =
          saw_buffering_period || ipmx::h264_sei_contains(*messages, ipmx::kH264SeiBufferingPeriod);
      picture_timings += static_cast<size_t>(
          std::count_if(messages->begin(), messages->end(), [](const auto& message) {
            return message.payload_type == ipmx::kH264SeiPictureTiming;
          }));
      break;
    }
    default:
      break;
    }
  }
  require(saw_sps && saw_pps && saw_idr && saw_buffering_period,
          "golden recovery point contains SPS/PPS/IDR/Buffering Period");
  require(picture_timings > 0U, "golden stream contains Picture Timing SEI");
}

void test_golden_bitstream() {
  const auto bytes =
      read_file(std::filesystem::path(IPMX_CONFORMANCE_TEST_DATA_DIR) / "ipmx_golden.h264");
  const auto nals = ipmx::parse_annex_b(bytes);
  require(!nals.empty(), "golden H.264 contains NAL units");
  validate_access_units(nals);
}

void test_golden_pcap() {
  const auto bytes =
      read_file(std::filesystem::path(IPMX_CONFORMANCE_TEST_DATA_DIR) / "ipmx_golden.pcap");
  require(bytes.size() >= 24U && read_u32_le(bytes, 0U) == 0xA1B2C3D4U &&
              read_u32_le(bytes, 20U) == 1U,
          "golden PCAP header");
  size_t offset = 24U;
  size_t packets = 0U;
  size_t reports = 0U;
  uint64_t payload_octets = 0U;
  bool saw_vcl = false;
  std::unordered_set<uint32_t> reported_timestamps;
  std::unordered_set<uint32_t> media_timestamps;
  while (offset < bytes.size()) {
    require(offset + 16U <= bytes.size(), "complete PCAP record header");
    const uint32_t captured = read_u32_le(bytes, offset + 8U);
    offset += 16U;
    require(captured >= 42U && offset + captured <= bytes.size(), "complete PCAP packet");
    const std::span<const uint8_t> ethernet(bytes.data() + offset, captured);
    require(read_u16_be(ethernet, 12U) == 0x0800U, "golden packet carries IPv4 Ethernet");
    require(ethernet[0] == 0x01U && ethernet[1] == 0x00U && ethernet[2] == 0x5EU,
            "golden packet uses multicast destination MAC");
    const std::span<const uint8_t> ip_packet = ethernet.subspan(14U);
    require((ip_packet[0] >> 4U) == 4U && (ip_packet[0] & 0x0FU) == 5U,
            "golden packet is IPv4 without options");
    require(read_u16_be(ip_packet, 2U) == ip_packet.size() && captured <= 1502U,
            "golden packet avoids IPv4 fragmentation");
    require((read_u16_be(ip_packet, 6U) & 0x4000U) != 0U, "golden packet has DF set");
    require(ip_packet[9] == 17U, "golden transport is UDP");
    const uint16_t destination_port = read_u16_be(ip_packet, 22U);
    if (destination_port == 5005U) {
      const std::vector<uint8_t> rtcp(ip_packet.begin() + 28, ip_packet.end());
      const auto inspection = ipmx::inspect_ipmx_rtcp_compound(rtcp);
      require(inspection.sender_report && inspection.ipmx_info_block &&
                  inspection.compressed_video_info && inspection.h264_info && inspection.sdes_cname,
              "golden RTCP has complete IPMX H.264 info");
      require(inspection.packet_count == packets && inspection.octet_count == payload_octets,
              "golden sender counters cover all preceding RTP packets");
      require(reported_timestamps.insert(inspection.rtp_timestamp).second,
              "golden has one RTCP sender report per frame timestamp");
      ++reports;
      offset += captured;
      continue;
    }
    require(destination_port == 5004U, "golden UDP destination is RTP or reserved RTCP");
    const auto rtp = ipmx::parse_rtp_packet(ip_packet.subspan(28U));
    require(rtp.has_value(), "golden RTP packet parses");
    require(reported_timestamps.contains(rtp->timestamp),
            "sender report precedes the first RTP packet of its access unit");
    media_timestamps.insert(rtp->timestamp);
    const uint8_t packetization_type = static_cast<uint8_t>(rtp->payload.front() & 0x1FU);
    require((packetization_type >= 1U && packetization_type <= 23U) || packetization_type == 28U,
            "golden RTP uses only Single NAL or FU-A");
    saw_vcl = saw_vcl || packetization_type == 1U || packetization_type == 5U ||
              (packetization_type == 28U && (rtp->payload[1] & 0x1FU) <= 5U);
    payload_octets += rtp->payload.size();
    ++packets;
    offset += captured;
  }
  require(packets > 0U && saw_vcl, "golden PCAP contains H.264 video RTP");
  require(reports == reported_timestamps.size() && reports >= media_timestamps.size(),
          "golden PCAP has a sender report for every progressive frame");
}

void test_h264_sender_report_schedule() {
  const auto defaults =
      ipmx::sender::resolve_ipmx_session_timing(60U, 1U, std::nullopt, std::nullopt, 1'000'000U);
  require(defaults.encoder_delay_ns == 50'000'001U &&
              defaults.sender_reports_delay_ns == defaults.encoder_delay_ns &&
              defaults.access_unit_offset_ns == 1'000'000U,
          "default encoder delay is three frame periods and remains constant for sender reports");
  const auto first = ipmx::sender::make_ipmx_frame_schedule(1'000U, 300U, 200U, 50U);
  const auto second = ipmx::sender::make_ipmx_frame_schedule(2'000U, 300U, 200U, 50U);
  require(first.sender_report_time_ns == 1'200U && first.encoder_cpb_insertion_time_ns == 1'300U &&
              first.first_rtp_time_ns == 1'350U,
          "H.264 frame schedule uses the configured session delays");
  require(second.sender_report_time_ns - first.sender_report_time_ns == 1'000U &&
              second.encoder_cpb_insertion_time_ns - first.encoder_cpb_insertion_time_ns == 1'000U,
          "encoder_delay and sender_reports_delay remain constant across frames");
  const auto zero_delay = ipmx::sender::resolve_ipmx_session_timing(60U, 1U, 0U, 0U, 0U);
  const auto immediate = ipmx::sender::make_ipmx_frame_schedule(1'000U, zero_delay.encoder_delay_ns,
                                                                zero_delay.sender_reports_delay_ns,
                                                                zero_delay.access_unit_offset_ns);
  require(immediate.sender_report_time_ns == 1'000U &&
              immediate.encoder_cpb_insertion_time_ns == 1'000U &&
              immediate.first_rtp_time_ns == 1'000U,
          "explicit zero delays are preserved");
  bool rejected = false;
  try {
    static_cast<void>(ipmx::sender::make_ipmx_frame_schedule(1U, 100U, 101U, 1U));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "sender_reports_delay cannot exceed encoder_delay");
}

void test_live_x264_contract() {
  constexpr uint32_t width = 320U;
  constexpr uint32_t height = 180U;
  constexpr uint32_t fps = 30U;
  ipmx::sender::X264Encoder encoder({width, height, fps, 1U, 1'000U});
  const auto sps = ipmx::parse_h264_sps(encoder.sps());
  require(sps.has_value() &&
              ipmx::validate_ipmx_vbr_sender_sps(*sps, {width, height, fps, 1U}).empty(),
          "live x264 SPS conforms");

  ipmx::Nv12Frame frame;
  frame.width = width;
  frame.height = height;
  frame.y_stride = width;
  frame.uv_stride = width;
  frame.pixels.assign(static_cast<size_t>(width) * height * 3U / 2U, 128U);
  std::fill_n(frame.y_plane(), static_cast<size_t>(width) * height, 16U);
  size_t keyframes = 0U;
  for (int64_t index = 0; index <= static_cast<int64_t>(fps); ++index) {
    frame.y_plane()[static_cast<size_t>(index) % width] = static_cast<uint8_t>(32 + index);
    const auto access_unit = encoder.encode(frame, index);
    require(access_unit.pts == access_unit.dts, "decode order equals output order");
    bool picture_timing = false;
    for (const auto& nal : access_unit.nals) {
      if (ipmx::h264_nal_type(nal) == ipmx::kH264NalSei) {
        const auto messages = ipmx::parse_h264_sei(nal);
        require(messages.has_value(), "live x264 SEI parses");
        picture_timing =
            picture_timing || ipmx::h264_sei_contains(*messages, ipmx::kH264SeiPictureTiming);
      }
    }
    require(picture_timing, "every live access unit has Picture Timing SEI");
    keyframes += access_unit.keyframe ? 1U : 0U;
  }
  require(keyframes >= 2U, "live x264 produces recovery point at most every second");
}

void test_receiver_main_and_high_profiles() {
  constexpr uint32_t width = 320U;
  constexpr uint32_t height = 180U;
  constexpr uint32_t fps = 30U;
  for (const auto profile : {ipmx::sender::H264Profile::main, ipmx::sender::H264Profile::high}) {
    ipmx::sender::X264Encoder encoder({width, height, fps, 1U, 1'000U, profile});
    const auto sps = ipmx::parse_h264_sps(encoder.sps());
    require(sps.has_value(), "profile SPS parses");
    require(sps->profile_idc == (profile == ipmx::sender::H264Profile::main ? 77U : 100U),
            "encoder emits requested Main/High profile");
    ipmx::receiver::MfH264Decoder decoder(width, height, fps, 1U);
    ipmx::Nv12Frame frame;
    frame.width = width;
    frame.height = height;
    frame.y_stride = width;
    frame.uv_stride = width;
    frame.pixels.assign(static_cast<size_t>(width) * height * 3U / 2U, 128U);
    std::fill_n(frame.y_plane(), static_cast<size_t>(width) * height, 16U);
    size_t decoded = 0U;
    for (int64_t index = 0; index < 8; ++index) {
      frame.y_plane()[static_cast<size_t>(index)] = static_cast<uint8_t>(48 + index);
      const auto access_unit = encoder.encode(frame, index);
      const auto annex_b = ipmx::build_annex_b(access_unit.nals);
      decoded += decoder.decode(annex_b, static_cast<uint64_t>(index + 1)).size();
    }
    require(decoded > 0U, "Media Foundation receiver decodes Main and High profiles");
  }
}

} // namespace

int main() {
  try {
    test_golden_bitstream();
    test_golden_pcap();
    test_h264_sender_report_schedule();
    test_live_x264_contract();
    test_receiver_main_and_high_profiles();
    std::cout << "ipmx_conformance_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ipmx_conformance_tests: FAIL: " << error.what() << '\n';
    return 1;
  }
}
