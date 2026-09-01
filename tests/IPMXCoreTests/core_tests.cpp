#include "ipmx/annexb.hpp"
#include "ipmx/bgra_to_nv12.hpp"
#include "ipmx/h264.hpp"
#include "ipmx/metrics.hpp"
#include "ipmx/pcap.hpp"
#include "ipmx/rtp.hpp"
#include "ipmx/rtcp.hpp"
#include "ipmx/sdp.hpp"
#include "ipmx/traffic_shaper.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

static_assert(std::is_same_v<ipmx::RtpPacketizer, ipmx::v0::RtpPacketizer>);

namespace {

void require(const bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Function> void require_throws(Function&& function, const char* message) {
  try {
    function();
  } catch (const std::exception&) {
    return;
  }
  throw std::runtime_error(message);
}

[[nodiscard]] const ipmx::NalUnit& reference_sps() {
  static const ipmx::NalUnit value{0x67, 0x64, 0x00, 0x20, 0xAC, 0xB4, 0x02, 0x80, 0x2D, 0xD8,
                                   0x0B, 0x50, 0x10, 0x10, 0x14, 0x00, 0x00, 0x03, 0x00, 0x04,
                                   0x00, 0x00, 0x03, 0x01, 0xE3, 0x90, 0x00, 0x03, 0xD0, 0x90,
                                   0x00, 0x82, 0xD9, 0x8B, 0x28, 0x0F, 0x8C, 0x19, 0x50};
  return value;
}

[[nodiscard]] const ipmx::NalUnit& reference_pps() {
  static const ipmx::NalUnit value{0x68, 0xEF, 0x3C, 0xB0};
  return value;
}

void test_annex_b() {
  const std::vector<uint8_t> bytes{0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3, 0, 0};
  const auto nals = ipmx::parse_annex_b(bytes);
  require(nals.size() == 2U, "Annex B NAL count");
  require(nals[0] == ipmx::NalUnit({0x67, 1, 2}), "Annex B first NAL");
  require(nals[1] == ipmx::NalUnit({0x68, 3}), "Annex B second NAL");
  const auto rebuilt = ipmx::build_annex_b(nals);
  require(ipmx::parse_annex_b(rebuilt) == nals, "Annex B round trip");

  const std::vector<uint8_t> trailing_start_code{0, 0, 1, 0x65, 0xAA, 0, 0, 1};
  const auto trailing = ipmx::parse_annex_b(trailing_start_code);
  require(trailing.size() == 1U && trailing.front() == ipmx::NalUnit({0x65, 0xAA}),
          "Annex B trailing three-byte start code");
}

void test_bgra_to_nv12() {
  ipmx::BgraFrame black;
  black.width = 2U;
  black.height = 2U;
  black.stride = 8U;
  black.pixels.assign(16U, 0U);
  for (size_t offset = 3U; offset < black.pixels.size(); offset += 4U) {
    black.pixels[offset] = 255U;
  }
  const auto black_nv12 = ipmx::bgra_to_nv12(black);
  require(black_nv12.pixels == std::vector<uint8_t>({16, 16, 16, 16, 128, 128}),
          "black BGRA to NV12");

  auto white = black;
  for (size_t offset = 0U; offset < white.pixels.size(); offset += 4U) {
    white.pixels[offset] = 255U;
    white.pixels[offset + 1U] = 255U;
    white.pixels[offset + 2U] = 255U;
  }
  const auto white_nv12 = ipmx::bgra_to_nv12(white);
  require(white_nv12.pixels == std::vector<uint8_t>({235, 235, 235, 235, 128, 128}),
          "white BGRA to NV12");
}

void test_rtp_single_and_fu_a() {
  ipmx::NalUnit small{0x67, 0x64, 0x00, 0x2A};
  ipmx::NalUnit large(333U);
  large[0] = 0x65U;
  for (size_t index = 1U; index < large.size(); ++index) {
    large[index] = static_cast<uint8_t>(index);
  }
  const std::vector<ipmx::NalUnit> original{small, large};
  ipmx::RtpPacketizer packetizer(100U, ipmx::kH264PayloadType, 42U);
  const auto datagrams = packetizer.packetize(original, 123456U, 9'876'543'210ULL);
  require(datagrams.size() > 2U, "FU-A must use multiple datagrams");
  require(std::ranges::all_of(datagrams, [](const auto& packet) { return packet.size() <= 100U; }),
          "RTP datagram MTU");

  ipmx::H264Depacketizer depacketizer;
  std::optional<ipmx::CompletedAccessUnit> completed;
  size_t marker_count = 0U;
  for (const auto& datagram : datagrams) {
    const auto packet = ipmx::parse_rtp_packet(datagram);
    require(packet.has_value(), "parse generated RTP packet");
    require(packet->timestamp == 123456U, "RTP timestamp");
    require(packet->capture_time_ns == 9'876'543'210ULL, "RTP capture extension");
    marker_count += packet->marker ? 1U : 0U;
    if (auto unit = depacketizer.push(*packet)) {
      completed = std::move(unit);
    }
  }
  require(marker_count == 1U, "one marker per access unit");
  require(completed.has_value(), "FU-A access unit completed");
  require(ipmx::parse_annex_b(completed->annex_b) == original, "FU-A round trip");
}

void test_sequence_and_clock() {
  ipmx::SequenceTracker tracker;
  tracker.observe(65'535U);
  tracker.observe(0U);
  tracker.observe(2U);
  tracker.observe(1U);
  require(tracker.stats().received == 4U, "sequence received count");
  require(tracker.stats().lost == 0U, "reordered packet is not permanently lost");
  require(tracker.stats().reordered == 1U, "sequence reordered count");
  ipmx::SequenceTracker permanent_loss;
  permanent_loss.observe(10U);
  permanent_loss.observe(12U);
  require(permanent_loss.stats().lost == 1U, "sequence permanent gap count");
  require(ipmx::rtp_timestamp_for_frame(100U, 60U, 60U, 1U) == 90'100U, "90 kHz integer timestamp");
  require(ipmx::rtp_timestamp_for_frame(100U, 30U, 30'000U, 1'001U) == 90'190U,
          "90 kHz fractional timestamp");
  require(ipmx::rtp_timestamp_for_frame(0xFFFFFFF0U, 32U, 90'000U, 1U) == 0x10U,
          "90 kHz timestamp wraparound");
}

void test_h264_syntax_and_conformance() {
  const auto sps = ipmx::parse_h264_sps(reference_sps());
  require(sps.has_value(), "parse reference SPS");
  require(sps->profile_idc == 100U && sps->chroma_format_idc == 1U && sps->bit_depth_luma == 8U &&
              sps->bit_depth_chroma == 8U,
          "SPS High 4:2:0 8-bit profile");
  require(sps->width == 1280U && sps->height == 720U, "SPS dimensions");
  require(sps->video_signal_type_present_flag && sps->colour_description_present_flag &&
              !sps->video_full_range_flag && sps->colour_primaries == 1U &&
              sps->transfer_characteristics == 1U && sps->matrix_coefficients == 1U,
          "SPS BT.709 narrow-range VUI");
  require(sps->timing_info_present_flag && sps->num_units_in_tick == 1U && sps->time_scale == 120U,
          "SPS exact VUI timing");
  require(sps->nal_hrd_parameters_present_flag && sps->nal_hrd &&
              sps->nal_hrd->cpb_cnt_minus1 == 0U && !sps->nal_hrd->cbr_flag,
          "SPS Type II VBR NAL HRD");
  require(ipmx::validate_ipmx_sps(*sps, {1280U, 720U, 60U, 1U}).empty(),
          "SPS IPMX H.264 profile conformance");
  require(!ipmx::validate_ipmx_sps(*sps, {1280U, 720U, 30U, 1U}).empty(),
          "SPS rejects mismatched timing");

  const auto pps = ipmx::parse_h264_pps(reference_pps());
  require(pps.has_value() && pps->sps_id == sps->sps_id, "parse matching PPS");
  const ipmx::NalUnit sei{0x06, 0x00, 0x01, 0x80, 0x01, 0x01, 0x40, 0x80};
  const auto messages = ipmx::parse_h264_sei(sei);
  require(messages.has_value() && messages->size() == 2U, "parse SEI messages");
  require(ipmx::h264_sei_contains(*messages, ipmx::kH264SeiBufferingPeriod) &&
              ipmx::h264_sei_contains(*messages, ipmx::kH264SeiPictureTiming),
          "Buffering Period and Picture Timing SEI");
}

void test_ipmx_transport_constraints() {
  require(ipmx::is_valid_ipmx_media_port(5002U) && ipmx::reserved_rtcp_port(5002U) == 5003U,
          "IPMX RTP/RTCP port pair");
  require_throws([] { ipmx::validate_ipmx_media_port(5001U); }, "reject odd media port");
  require(ipmx::is_valid_ipmx_media_port(2000U) &&
              !ipmx::is_recommended_ipmx_media_port(2000U),
          "port above 1024 is valid but below the IPMX recommendation");
  require_throws([] { ipmx::validate_ipmx_media_port(1024U); }, "reject reserved low port");
  require_throws([] { ipmx::RtpPacketizer packetizer(1461U); }, "reject fragmenting MAXUDP");

  ipmx::Tr107TrafficShaper shaper(8'000U);
  require(shaper.schedule_ns(100U, 972U) == 100U, "first shaped packet is immediate");
  require(shaper.schedule_ns(100U, 972U) == 1'000'100U, "TR-10-7 leaky-bucket pacing");
  require(ipmx::tr107_cmax(10'000U) == 16U && ipmx::tr107_cmax(1'080'000U) == 50U,
          "TR-10-7 CMAX calculation");
  require(shaper.cmax() == 16U, "traffic shaper applies TR-10-7 CMAX");
  require(shaper.schedule_ns(1'000'000'000U, 972U) == 1'000'000'000U,
          "traffic shaper resets excessive accumulated drift");
  ipmx::FrameIntervalTracker intervals;
  for (uint64_t index = 0U; index <= 120U; ++index)
    intervals.observe(index * 16'666'667U);
  require(intervals.window_observed() && intervals.maximum_interval_spread_ns() <= 1U,
          "two-second frame interval compliance metric");
  require(ipmx::minimum_ip_bitrate_kbps(4'000U, 1'200U) == 4'400U &&
              ipmx::minimum_ip_bitrate_kbps(4'000U, 64U) > 10'000U,
          "IP bitrate includes worst-case packet overhead");
}

void test_malformed_rtp_packets() {
  ipmx::RtpPacketizer packetizer(120U, ipmx::kH264PayloadType, 7U);
  const std::vector<ipmx::NalUnit> nals{{0x65, 1, 2, 3}};
  const auto valid = packetizer.packetize(nals, 100U, 200U).front();
  require(ipmx::parse_rtp_packet(valid).has_value(), "valid RTP baseline");

  require(!ipmx::parse_rtp_packet(std::span<const uint8_t>(valid.data(), 11U)),
          "reject truncated RTP header");
  auto bad_version = valid;
  bad_version[0] = static_cast<uint8_t>((bad_version[0] & 0x3FU) | 0x40U);
  require(!ipmx::parse_rtp_packet(bad_version), "reject non-v2 RTP");

  std::vector<uint8_t> bad_csrc(12U, 0U);
  bad_csrc[0] = 0x8FU;
  require(!ipmx::parse_rtp_packet(bad_csrc), "reject truncated CSRC list");

  auto bad_extension = valid;
  bad_extension[14U] = 0x7FU;
  bad_extension[15U] = 0xFFU;
  require(!ipmx::parse_rtp_packet(bad_extension), "reject oversized RTP extension");

  auto zero_padding = valid;
  zero_padding[0] |= 0x20U;
  zero_padding.back() = 0U;
  require(!ipmx::parse_rtp_packet(zero_padding), "reject zero RTP padding");

  auto oversized_padding = valid;
  oversized_padding[0] |= 0x20U;
  oversized_padding.back() = 0xFFU;
  require(!ipmx::parse_rtp_packet(oversized_padding), "reject oversized RTP padding");
}

void test_depacketizer_damage_paths() {
  ipmx::H264Depacketizer depacketizer;
  const std::array<uint8_t, 3U> fu_start{0x7CU, 0x85U, 0x11U};
  const std::array<uint8_t, 3U> fu_end{0x7CU, 0x45U, 0x22U};
  ipmx::ParsedRtpPacket start{1U, 10U, 1U, ipmx::kH264PayloadType, false, 100U, fu_start};
  ipmx::ParsedRtpPacket end{3U, 10U, 1U, ipmx::kH264PayloadType, true, 100U, fu_end};
  require(!depacketizer.push(start), "FU-A start is incomplete");
  require(!depacketizer.push(end), "drop FU-A with missing middle packet");

  depacketizer.reset();
  ipmx::ParsedRtpPacket orphan{4U, 11U, 1U, ipmx::kH264PayloadType, true, 100U, fu_end};
  require(!depacketizer.push(orphan), "drop FU-A without start bit");

  depacketizer.reset();
  const std::array<uint8_t, 2U> first_nal{0x61U, 0x10U};
  const std::array<uint8_t, 2U> second_nal{0x61U, 0x20U};
  ipmx::ParsedRtpPacket old_timestamp{5U, 12U, 1U, ipmx::kH264PayloadType, false, 100U, first_nal};
  ipmx::ParsedRtpPacket new_timestamp{6U, 13U, 1U, ipmx::kH264PayloadType, true, 200U, second_nal};
  require(!depacketizer.push(old_timestamp), "unmarked access unit remains incomplete");
  const auto completed = depacketizer.push(new_timestamp);
  require(completed.has_value() &&
              ipmx::parse_annex_b(completed->annex_b) == std::vector<ipmx::NalUnit>{{0x61U, 0x20U}},
          "timestamp change discards incomplete access unit");

  depacketizer.reset();
  const std::array<uint8_t, 1U> tiny_nal{0x61U};
  for (uint16_t sequence = 0U; sequence < 257U; ++sequence) {
    ipmx::ParsedRtpPacket packet{sequence, 20U, 1U, ipmx::kH264PayloadType, false, 300U, tiny_nal};
    require(!depacketizer.push(packet), "unterminated bounded access unit");
  }
  ipmx::ParsedRtpPacket marker{257U, 20U, 1U, ipmx::kH264PayloadType, true, 300U, tiny_nal};
  require(!depacketizer.push(marker), "drop access unit exceeding NAL limit");
}

void test_sdp_and_metrics() {
  ipmx::SdpSettings defaults;
  defaults.ts_refclk = "localmac=02-00-00-00-00-01";
  const std::string sdp = ipmx::make_sdp(defaults, reference_sps(), reference_pps());
  require(sdp.find("a=rtpmap:96 H264/90000") != std::string::npos, "SDP RTP map");
  require(sdp.find("packetization-mode=1;profile-level-id=640020") != std::string::npos,
          "SDP fmtp");
  require(sdp.find("TP=2110TPW;MAXUDP=1200") != std::string::npos,
          "SDP traffic profile and MAXUDP");
  require(sdp.find("a=rtcp:5005") != std::string::npos, "SDP reserves RTCP port");
  require(sdp.find("a=ts-refclk:localmac=02-00-00-00-00-01") != std::string::npos &&
              sdp.find("a=mediaclk:direct=0") != std::string::npos,
          "SDP synchronous clock signaling");
  require(sdp.find("a=extmap:1 " + std::string(ipmx::kCaptureTimeExtensionUri)) !=
              std::string::npos,
          "SDP capture-time extension");

  ipmx::SdpSettings settings;
  settings.multicast_group = "239.10.20.30";
  settings.port = 6000U;
  settings.payload_type = 110U;
  settings.width = 1280U;
  settings.height = 720U;
  settings.fps_numerator = 60U;
  settings.fps_denominator = 1U;
  settings.target_bitrate_kbps = 8'000U;
  settings.maximum_ip_bitrate_kbps = 8'800U;
  settings.maximum_udp_bytes = 1'200U;
  settings.ts_refclk = "localmac=02-00-00-00-00-01";
  const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path =
      std::filesystem::temp_directory_path() / ("ipmx-sdp-" + std::to_string(unique) + ".sdp");
  try {
    ipmx::write_sdp(path, settings, reference_sps(), reference_pps());
    const auto description = ipmx::read_sdp_description(path);
    const auto& parsed = description.settings;
    require(parsed.multicast_group == settings.multicast_group && parsed.port == settings.port &&
                parsed.payload_type == settings.payload_type,
            "SDP round-trip transport settings");
    require(parsed.width == settings.width && parsed.height == settings.height &&
                parsed.maximum_ip_bitrate_kbps == settings.maximum_ip_bitrate_kbps &&
                parsed.maximum_udp_bytes == settings.maximum_udp_bytes,
            "SDP round-trip video settings");
    require(description.sps == reference_sps() && description.pps == reference_pps(),
            "SDP round-trip parameter sets");
    require(ipmx::validate_ipmx_sdp(description).empty(),
            "generated SDP passes independent conformance validation");
    require(parsed.target_bitrate_kbps == 0U,
            "elementary target bitrate remains explicitly unknown after SDP parsing");
    const double parsed_fps = static_cast<double>(parsed.fps_numerator) / parsed.fps_denominator;
    const double expected_fps =
        static_cast<double>(settings.fps_numerator) / settings.fps_denominator;
    require(std::abs(parsed_fps - expected_fps) < 0.001, "SDP round-trip frame rate");
    auto invalid_sdp = ipmx::make_sdp(settings, reference_sps(), reference_pps());
    const auto traffic_profile = invalid_sdp.find("TP=2110TPW");
    require(traffic_profile != std::string::npos, "locate SDP traffic profile");
    invalid_sdp.replace(traffic_profile, std::string("TP=2110TPW").size(), "TP=INVALID");
    {
      std::ofstream invalid_output(path, std::ios::binary | std::ios::trunc);
      invalid_output << invalid_sdp;
    }
    const auto invalid_description = ipmx::read_sdp_description(path);
    require(!ipmx::validate_ipmx_sdp(invalid_description).empty(),
            "parser accepts but validator reports invalid traffic profile");
    auto incomplete_sdp = ipmx::make_sdp(settings, reference_sps(), reference_pps());
    const auto omitted = incomplete_sdp.find("TP=2110TPW;");
    require(omitted != std::string::npos, "locate optional SDP field");
    incomplete_sdp.erase(omitted, std::string("TP=2110TPW;").size());
    {
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output << incomplete_sdp;
    }
    require(!ipmx::validate_ipmx_sdp(ipmx::read_sdp_description(path)).empty(),
            "parser accepts omissions and validator reports them separately");
    auto mismatched_framesize = ipmx::make_sdp(settings, reference_sps(), reference_pps());
    const auto framesize = mismatched_framesize.find("1280-720");
    require(framesize != std::string::npos, "locate SDP framesize");
    mismatched_framesize.replace(framesize, std::string("1280-720").size(), "640-480");
    {
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output << mismatched_framesize;
    }
    const auto mismatch = ipmx::read_sdp_description(path);
    require(mismatch.settings.width == 1280U && mismatch.framesize_width == 640U &&
                !ipmx::validate_ipmx_sdp(mismatch).empty(),
            "framesize is cross-checked and cannot overwrite fmtp dimensions");
    auto lowercase_tokens = ipmx::make_sdp(settings, reference_sps(), reference_pps());
    for (const auto& [from, to] :
         std::array<std::pair<std::string_view, std::string_view>, 4U>{
             std::pair{"BT709", "bt709"}, std::pair{"SDR", "sdr"},
             std::pair{"NARROW", "narrow"}, std::pair{"2110TPW", "2110tpw"}}) {
      const auto token = lowercase_tokens.find(from);
      require(token != std::string::npos, "locate case-insensitive SDP token");
      lowercase_tokens.replace(token, from.size(), to);
    }
    {
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output << lowercase_tokens;
    }
    require(ipmx::validate_ipmx_sdp(ipmx::read_sdp_description(path)).empty(),
            "known SDP tokens validate case-insensitively");
    auto insufficient_bitrate = settings;
    insufficient_bitrate.maximum_ip_bitrate_kbps = settings.target_bitrate_kbps;
    require_throws(
        [&insufficient_bitrate] {
          static_cast<void>(ipmx::make_sdp(insufficient_bitrate, reference_sps(), reference_pps()));
        },
        "reject SDP bitrate without IP overhead");
    std::filesystem::remove(path);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    throw;
  }
  ipmx::LatencyMetrics latency;
  latency.observe_ns(1'000'000U);
  latency.observe_ns(3'000'000U);
  require(latency.mean_ms() == 2.0, "latency mean");
  require(latency.minimum_ms() == 1.0 && latency.maximum_ms() == 3.0, "latency range");
}

void test_ipmx_rtcp() {
  ipmx::IpmxRtcpSenderReport report;
  report.ssrc = 0x11223344U;
  report.ptp_seconds = 1'700'000'000U;
  report.ptp_nanoseconds = 123'456'789U;
  report.rtp_timestamp = 90'000U;
  report.packet_count = 42U;
  report.octet_count = 12'345U;
  report.ts_refclk = "localmac=02-00-00-00-00-01";
  report.cname = "conformance@ipmx-windows";
  report.video = {1280U, 720U, 60U, 1U};
  const auto compound = ipmx::make_ipmx_rtcp_compound(report);
  const auto inspected = ipmx::inspect_ipmx_rtcp_compound(compound);
  require(inspected.sender_report && inspected.ipmx_info_block &&
              inspected.compressed_video_info && inspected.sdes_cname,
          "compound RTCP contains SR, IPMX block 0x0005, and SDES CNAME");
}

} // namespace

int main() {
  try {
    test_annex_b();
    test_bgra_to_nv12();
    test_rtp_single_and_fu_a();
    test_sequence_and_clock();
    test_h264_syntax_and_conformance();
    test_ipmx_transport_constraints();
    test_malformed_rtp_packets();
    test_depacketizer_damage_paths();
    test_sdp_and_metrics();
    test_ipmx_rtcp();
    std::cout << "ipmx_core_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ipmx_core_tests: FAIL: " << error.what() << '\n';
    return 1;
  }
}
