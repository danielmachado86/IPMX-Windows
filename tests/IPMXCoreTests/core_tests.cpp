#include "ipmx/annexb.hpp"
#include "ipmx/bgra_to_nv12.hpp"
#include "ipmx/metrics.hpp"
#include "ipmx/rtp.hpp"
#include "ipmx/sdp.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
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
  require(ipmx::rtp_timestamp_for_frame(100U, 60U, 60U, 1U) == 90'100U,
          "90 kHz integer timestamp");
  require(ipmx::rtp_timestamp_for_frame(100U, 30U, 30'000U, 1'001U) == 90'190U,
          "90 kHz fractional timestamp");
  require(ipmx::rtp_timestamp_for_frame(0xFFFFFFF0U, 32U, 90'000U, 1U) == 0x10U,
          "90 kHz timestamp wraparound");
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
  ipmx::ParsedRtpPacket old_timestamp{5U, 12U, 1U, ipmx::kH264PayloadType, false, 100U,
                                        first_nal};
  ipmx::ParsedRtpPacket new_timestamp{6U, 13U, 1U, ipmx::kH264PayloadType, true, 200U,
                                        second_nal};
  require(!depacketizer.push(old_timestamp), "unmarked access unit remains incomplete");
  const auto completed = depacketizer.push(new_timestamp);
  require(completed.has_value() && ipmx::parse_annex_b(completed->annex_b) ==
                                       std::vector<ipmx::NalUnit>{{0x61U, 0x20U}},
          "timestamp change discards incomplete access unit");

  depacketizer.reset();
  const std::array<uint8_t, 1U> tiny_nal{0x61U};
  for (uint16_t sequence = 0U; sequence < 257U; ++sequence) {
    ipmx::ParsedRtpPacket packet{sequence, 20U, 1U, ipmx::kH264PayloadType, false, 300U,
                                   tiny_nal};
    require(!depacketizer.push(packet), "unterminated bounded access unit");
  }
  ipmx::ParsedRtpPacket marker{257U, 20U, 1U, ipmx::kH264PayloadType, true, 300U, tiny_nal};
  require(!depacketizer.push(marker), "drop access unit exceeding NAL limit");
}

void test_sdp_and_metrics() {
  const ipmx::NalUnit sps{0x67, 0x64, 0x00, 0x2A};
  const ipmx::NalUnit pps{0x68, 0x01};
  const std::string sdp = ipmx::make_sdp({}, sps, pps);
  require(sdp.find("a=rtpmap:96 H264/90000") != std::string::npos, "SDP RTP map");
  require(sdp.find("packetization-mode=1;profile-level-id=64002A") != std::string::npos,
          "SDP fmtp");
  require(sdp.find("a=extmap:1 " + std::string(ipmx::kCaptureTimeExtensionUri)) !=
              std::string::npos,
          "SDP capture-time extension");

  ipmx::SdpSettings settings;
  settings.multicast_group = "239.10.20.30";
  settings.port = 6000U;
  settings.payload_type = 110U;
  settings.width = 1920U;
  settings.height = 1080U;
  settings.fps_numerator = 60'000U;
  settings.fps_denominator = 1'001U;
  settings.target_bitrate_kbps = 8'000U;
  const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("ipmx-sdp-" + std::to_string(unique) + ".sdp");
  try {
    ipmx::write_sdp(path, settings, sps, pps);
    const auto parsed = ipmx::read_sdp(path);
    require(parsed.multicast_group == settings.multicast_group && parsed.port == settings.port &&
                parsed.payload_type == settings.payload_type,
            "SDP round-trip transport settings");
    require(parsed.width == settings.width && parsed.height == settings.height &&
                parsed.target_bitrate_kbps == settings.target_bitrate_kbps,
            "SDP round-trip video settings");
    const double parsed_fps = static_cast<double>(parsed.fps_numerator) / parsed.fps_denominator;
    const double expected_fps = static_cast<double>(settings.fps_numerator) / settings.fps_denominator;
    require(std::abs(parsed_fps - expected_fps) < 0.001, "SDP round-trip frame rate");
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

} // namespace

int main() {
  try {
    test_annex_b();
    test_bgra_to_nv12();
    test_rtp_single_and_fu_a();
    test_sequence_and_clock();
    test_malformed_rtp_packets();
    test_depacketizer_damage_paths();
    test_sdp_and_metrics();
    std::cout << "ipmx_core_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ipmx_core_tests: FAIL: " << error.what() << '\n';
    return 1;
  }
}
