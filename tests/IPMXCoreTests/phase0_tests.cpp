#include "ipmx/phase0/annexb.hpp"
#include "ipmx/phase0/bgra_to_nv12.hpp"
#include "ipmx/phase0/metrics.hpp"
#include "ipmx/phase0/rtp.hpp"
#include "ipmx/phase0/sdp.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void test_annex_b() {
  const std::vector<uint8_t> bytes{0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3, 0, 0};
  const auto nals = phase0::parse_annex_b(bytes);
  require(nals.size() == 2U, "Annex B NAL count");
  require(nals[0] == phase0::NalUnit({0x67, 1, 2}), "Annex B first NAL");
  require(nals[1] == phase0::NalUnit({0x68, 3}), "Annex B second NAL");
  const auto rebuilt = phase0::build_annex_b(nals);
  require(phase0::parse_annex_b(rebuilt) == nals, "Annex B round trip");
}

void test_bgra_to_nv12() {
  phase0::BgraFrame black;
  black.width = 2U;
  black.height = 2U;
  black.stride = 8U;
  black.pixels.assign(16U, 0U);
  for (size_t offset = 3U; offset < black.pixels.size(); offset += 4U) {
    black.pixels[offset] = 255U;
  }
  const auto black_nv12 = phase0::bgra_to_nv12(black);
  require(black_nv12.pixels == std::vector<uint8_t>({16, 16, 16, 16, 128, 128}),
          "black BGRA to NV12");

  auto white = black;
  for (size_t offset = 0U; offset < white.pixels.size(); offset += 4U) {
    white.pixels[offset] = 255U;
    white.pixels[offset + 1U] = 255U;
    white.pixels[offset + 2U] = 255U;
  }
  const auto white_nv12 = phase0::bgra_to_nv12(white);
  require(white_nv12.pixels == std::vector<uint8_t>({235, 235, 235, 235, 128, 128}),
          "white BGRA to NV12");
}

void test_rtp_single_and_fu_a() {
  phase0::NalUnit small{0x67, 0x64, 0x00, 0x2A};
  phase0::NalUnit large(333U);
  large[0] = 0x65U;
  for (size_t index = 1U; index < large.size(); ++index) {
    large[index] = static_cast<uint8_t>(index);
  }
  const std::vector<phase0::NalUnit> original{small, large};
  phase0::RtpPacketizer packetizer(100U, phase0::kH264PayloadType, 42U);
  const auto datagrams = packetizer.packetize(original, 123456U, 9'876'543'210ULL);
  require(datagrams.size() > 2U, "FU-A must use multiple datagrams");
  require(std::ranges::all_of(datagrams, [](const auto& packet) { return packet.size() <= 100U; }),
          "RTP datagram MTU");

  phase0::H264Depacketizer depacketizer;
  std::optional<phase0::CompletedAccessUnit> completed;
  size_t marker_count = 0U;
  for (const auto& datagram : datagrams) {
    const auto packet = phase0::parse_rtp_packet(datagram);
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
  require(phase0::parse_annex_b(completed->annex_b) == original, "FU-A round trip");
}

void test_sequence_and_clock() {
  phase0::SequenceTracker tracker;
  tracker.observe(65'535U);
  tracker.observe(0U);
  tracker.observe(2U);
  tracker.observe(1U);
  require(tracker.stats().received == 4U, "sequence received count");
  require(tracker.stats().lost == 1U, "sequence loss count");
  require(tracker.stats().reordered == 1U, "sequence reordered count");
  require(phase0::rtp_timestamp_for_frame(100U, 60U, 60U, 1U) == 90'100U,
          "90 kHz integer timestamp");
  require(phase0::rtp_timestamp_for_frame(100U, 30U, 30'000U, 1'001U) == 90'190U,
          "90 kHz fractional timestamp");
}

void test_sdp_and_metrics() {
  const phase0::NalUnit sps{0x67, 0x64, 0x00, 0x2A};
  const phase0::NalUnit pps{0x68, 0x01};
  const std::string sdp = phase0::make_phase0_sdp({}, sps, pps);
  require(sdp.find("a=rtpmap:96 H264/90000") != std::string::npos, "SDP RTP map");
  require(sdp.find("packetization-mode=1;profile-level-id=64002A") != std::string::npos,
          "SDP fmtp");
  require(sdp.find("a=extmap:1 urn:ipmx-windows:phase0:capture-time-ns") != std::string::npos,
          "SDP capture-time extension");
  phase0::LatencyMetrics latency;
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
    test_sdp_and_metrics();
    std::cout << "phase0_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "phase0_tests: FAIL: " << error.what() << '\n';
    return 1;
  }
}
