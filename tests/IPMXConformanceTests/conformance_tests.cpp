#include "ipmx/annexb.hpp"
#include "ipmx/h264.hpp"
#include "ipmx/receiver/mf_h264_decoder.hpp"
#include "ipmx/rtp.hpp"
#include "ipmx/sender/x264_encoder.hpp"
#include "ipmx/types.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
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
              read_u32_le(bytes, 20U) == 101U,
          "golden PCAP header");
  size_t offset = 24U;
  size_t packets = 0U;
  bool saw_vcl = false;
  while (offset < bytes.size()) {
    require(offset + 16U <= bytes.size(), "complete PCAP record header");
    const uint32_t captured = read_u32_le(bytes, offset + 8U);
    offset += 16U;
    require(captured >= 28U && offset + captured <= bytes.size(), "complete PCAP packet");
    const std::span<const uint8_t> ip_packet(bytes.data() + offset, captured);
    require((ip_packet[0] >> 4U) == 4U && (ip_packet[0] & 0x0FU) == 5U,
            "golden packet is IPv4 without options");
    require(read_u16_be(ip_packet, 2U) == captured && captured <= 1488U,
            "golden packet avoids IPv4 fragmentation");
    require((read_u16_be(ip_packet, 6U) & 0x4000U) != 0U, "golden packet has DF set");
    require(ip_packet[9] == 17U && read_u16_be(ip_packet, 22U) == 5004U, "golden UDP destination");
    const auto rtp = ipmx::parse_rtp_packet(ip_packet.subspan(28U));
    require(rtp.has_value(), "golden RTP packet parses");
    const uint8_t packetization_type = static_cast<uint8_t>(rtp->payload.front() & 0x1FU);
    require((packetization_type >= 1U && packetization_type <= 23U) || packetization_type == 28U,
            "golden RTP uses only Single NAL or FU-A");
    saw_vcl = saw_vcl || packetization_type == 1U || packetization_type == 5U ||
              (packetization_type == 28U && (rtp->payload[1] & 0x1FU) <= 5U);
    ++packets;
    offset += captured;
  }
  require(packets > 0U && saw_vcl, "golden PCAP contains H.264 video RTP");
}

void test_live_x264_contract() {
  constexpr uint32_t width = 320U;
  constexpr uint32_t height = 180U;
  constexpr uint32_t fps = 30U;
  ipmx::sender::X264Encoder encoder({width, height, fps, 1U, 1'000U});
  const auto sps = ipmx::parse_h264_sps(encoder.sps());
  require(sps.has_value() && ipmx::validate_ipmx_sps(*sps, {width, height, fps, 1U}).empty(),
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
    test_live_x264_contract();
    test_receiver_main_and_high_profiles();
    std::cout << "ipmx_conformance_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ipmx_conformance_tests: FAIL: " << error.what() << '\n';
    return 1;
  }
}
