#include "ipmx/bgra_to_nv12.hpp"
#include "ipmx/rtp.hpp"
#include "ipmx/sender/frame_transmitter.hpp"
#include "ipmx/sdp.hpp"
#include "ipmx/sender/frame_source.hpp"
#include "ipmx/sender/x264_encoder.hpp"
#include "ipmx/udp_multicast.hpp"

#include <windows.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <optional>

namespace {

std::atomic_bool running{true};

BOOL WINAPI stop_handler(DWORD) {
  running = false;
  return TRUE;
}

struct Options {
  std::string source{"test"};
  std::string group{"239.255.42.42"};
  std::string interface_address{"0.0.0.0"};
  uint16_t port{5004U};
  uint32_t width{1280U};
  uint32_t height{720U};
  uint32_t fps_numerator{60U};
  uint32_t fps_denominator{1U};
  uint32_t bitrate_kbps{4'000U};
  ipmx::sender::H264Profile profile{ipmx::sender::H264Profile::high};
  uint32_t maximum_ip_bitrate_kbps{};
  uint32_t duration_seconds{};
  uint16_t maximum_udp_bytes{1'200U};
  std::filesystem::path sdp{"ipmx.sdp"};
  std::filesystem::path dump_h264;
  std::filesystem::path dump_pcap;
  std::string ts_refclk;
  bool require_timing_compliance{};
};

template <typename T>
[[nodiscard]] T parse_number(const std::string_view text, const char* option) {
  unsigned long long value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      value > static_cast<unsigned long long>(std::numeric_limits<T>::max())) {
    throw std::invalid_argument(std::string("invalid value for ") + option);
  }
  return static_cast<T>(value);
}

[[nodiscard]] Options parse_options(const int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view key(argv[index]);
    if (key == "--help") {
      std::cout << "ipmx-sender [--source test|screen] [--width N] [--height N] "
                   "[--fps N] [--bitrate-kbps N] [--profile main|high] [--max-ip-bitrate-kbps N] "
                   "[--group A.B.C.D] [--port N] [--interface A.B.C.D] [--maxudp N] "
                   "[--ts-refclk VALUE] [--duration-seconds N] [--sdp PATH] "
                   "[--dump-h264 PATH] [--dump-pcap PATH] [--require-timing-compliance]\n";
      std::exit(0);
    }
    if (key == "--require-timing-compliance") {
      options.require_timing_compliance = true;
      continue;
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument("missing value after " + std::string(key));
    }
    const std::string value(argv[++index]);
    if (key == "--source")
      options.source = value;
    else if (key == "--width")
      options.width = parse_number<uint32_t>(value, "--width");
    else if (key == "--height")
      options.height = parse_number<uint32_t>(value, "--height");
    else if (key == "--fps")
      options.fps_numerator = parse_number<uint32_t>(value, "--fps");
    else if (key == "--bitrate-kbps")
      options.bitrate_kbps = parse_number<uint32_t>(value, "--bitrate-kbps");
    else if (key == "--profile") {
      if (value == "main")
        options.profile = ipmx::sender::H264Profile::main;
      else if (value == "high")
        options.profile = ipmx::sender::H264Profile::high;
      else
        throw std::invalid_argument("--profile must be main or high");
    } else if (key == "--max-ip-bitrate-kbps")
      options.maximum_ip_bitrate_kbps = parse_number<uint32_t>(value, "--max-ip-bitrate-kbps");
    else if (key == "--group")
      options.group = value;
    else if (key == "--port")
      options.port = parse_number<uint16_t>(value, "--port");
    else if (key == "--interface")
      options.interface_address = value;
    else if (key == "--maxudp" || key == "--mtu")
      options.maximum_udp_bytes = parse_number<uint16_t>(value, "--maxudp");
    else if (key == "--duration-seconds")
      options.duration_seconds = parse_number<uint32_t>(value, "--duration-seconds");
    else if (key == "--sdp")
      options.sdp = value;
    else if (key == "--dump-h264")
      options.dump_h264 = value;
    else if (key == "--dump-pcap")
      options.dump_pcap = value;
    else if (key == "--ts-refclk")
      options.ts_refclk = value;
    else
      throw std::invalid_argument("unknown option: " + std::string(key));
  }
  if (options.source != "test" && options.source != "screen") {
    throw std::invalid_argument("--source must be test or screen");
  }
  ipmx::validate_ipmx_media_port(options.port);
  if (!ipmx::is_recommended_ipmx_media_port(options.port))
    std::cerr << "warning: IPMX recommends an RTP media port greater than 5000\n";
  if (options.maximum_udp_bytes < 64U ||
      options.maximum_udp_bytes > ipmx::kMaximumStandardUdpPayloadBytes) {
    throw std::invalid_argument(
        "--maxudp must be between 64 and " +
        std::to_string(ipmx::kMaximumStandardUdpPayloadBytes) +
        " to avoid IPv4 fragmentation");
  }
  if (options.bitrate_kbps == 0U)
    throw std::invalid_argument("--bitrate-kbps must be non-zero");
  const uint32_t required_ip_bitrate =
      ipmx::minimum_ip_bitrate_kbps(options.bitrate_kbps, options.maximum_udp_bytes);
  if (options.maximum_ip_bitrate_kbps == 0U) {
    options.maximum_ip_bitrate_kbps = required_ip_bitrate;
  }
  if (options.maximum_ip_bitrate_kbps < required_ip_bitrate) {
    throw std::invalid_argument(
        "--max-ip-bitrate-kbps must include the encoded bitrate and IP overhead");
  }
  return options;
}

} // namespace

int main(const int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    SetConsoleCtrlHandler(stop_handler, TRUE);

    std::unique_ptr<ipmx::sender::FrameSource> source =
        options.source == "screen"
            ? ipmx::sender::make_primary_monitor_source(options.fps_numerator,
                                                        options.fps_denominator)
            : ipmx::sender::make_test_pattern_source(
                  options.width, options.height, options.fps_numerator, options.fps_denominator);
    const ipmx::sender::EncoderSettings encoder_settings{
        source->width(),         source->height(),     options.fps_numerator,
        options.fps_denominator, options.bitrate_kbps, options.profile};
    ipmx::sender::X264Encoder encoder(encoder_settings);
    ipmx::RtpPacketizer packetizer(options.maximum_udp_bytes);
    const std::string ts_refclk = options.ts_refclk.empty()
                                      ? ipmx::local_mac_reference(options.interface_address)
                                      : options.ts_refclk;
    ipmx::SdpSettings sdp_settings{options.group,
                                   options.port,
                                   ipmx::kH264PayloadType,
                                   source->width(),
                                   source->height(),
                                   options.fps_numerator,
                                   options.fps_denominator,
                                   options.bitrate_kbps,
                                   options.maximum_ip_bitrate_kbps,
                                   options.maximum_udp_bytes,
                                   ts_refclk,
                                   "direct=0"};
    ipmx::write_sdp(options.sdp, sdp_settings, encoder.sps(), encoder.pps());
    std::ofstream bitstream;
    if (!options.dump_h264.empty()) {
      bitstream.open(options.dump_h264, std::ios::binary | std::ios::trunc);
      if (!bitstream)
        throw std::runtime_error("cannot create H.264 dump file");
    }
    std::random_device random;
    const uint32_t initial_timestamp = (static_cast<uint32_t>(random()) << 16U) ^ random();
    ipmx::sender::FrameTransmitterSettings transmitter_settings;
    transmitter_settings.multicast_group = options.group;
    transmitter_settings.interface_address = options.interface_address;
    transmitter_settings.media_port = options.port;
    transmitter_settings.maximum_udp_bytes = options.maximum_udp_bytes;
    transmitter_settings.maximum_ip_bitrate_kbps = options.maximum_ip_bitrate_kbps;
    transmitter_settings.ssrc = packetizer.ssrc();
    transmitter_settings.ts_refclk = ts_refclk;
    transmitter_settings.cname = ts_refclk + "@ipmx-windows";
    transmitter_settings.video = {source->width(), source->height(), options.fps_numerator,
                                  options.fps_denominator};
    transmitter_settings.pcap_path = options.dump_pcap;
    ipmx::sender::FrameTransmitter transmitter(std::move(transmitter_settings));
    uint64_t frame_index = 0U;
    const auto started = std::chrono::steady_clock::now();
    auto last_report = started;
    std::cout << "Sender: " << options.source << ' ' << source->width() << 'x' << source->height()
              << " @ " << options.fps_numerator << " fps -> " << options.group << ':'
              << options.port << " SSRC=" << packetizer.ssrc() << " SDP=" << options.sdp.string()
              << '\n';

    ipmx::BgraFrame bgra;
    std::optional<ipmx::Nv12Frame> cached_nv12;
    while (running) {
      const auto now = std::chrono::steady_clock::now();
      if (options.duration_seconds != 0U &&
          now - started >= std::chrono::seconds(options.duration_seconds)) {
        break;
      }
      if (!source->next(bgra)) {
        continue;
      }
      if (!bgra.repeated) {
        cached_nv12 = ipmx::bgra_to_nv12(bgra);
      } else if (!cached_nv12) {
        continue;
      }
      cached_nv12->capture_time_ns = bgra.capture_time_ns;
      auto access_unit = encoder.encode(*cached_nv12, static_cast<int64_t>(frame_index));
      if (access_unit.nals.empty()) {
        ++frame_index;
        continue;
      }
      if (bitstream.is_open()) {
        const auto annex_b = ipmx::build_annex_b(access_unit.nals);
        bitstream.write(reinterpret_cast<const char*>(annex_b.data()),
                        static_cast<std::streamsize>(annex_b.size()));
        if (!bitstream)
          throw std::runtime_error("cannot write H.264 dump file");
      }
      const uint32_t timestamp = ipmx::rtp_timestamp_for_frame(
          initial_timestamp, frame_index, options.fps_numerator, options.fps_denominator);
      transmitter.enqueue({timestamp, cached_nv12->capture_time_ns,
                           packetizer.packetize(access_unit.nals, timestamp,
                                                cached_nv12->capture_time_ns)});
      ++frame_index;

      if (now - last_report >= std::chrono::seconds(1)) {
        const auto stats = transmitter.stats();
        const double seconds = std::chrono::duration<double>(now - started).count();
        std::cout << "frames=" << frame_index << " sent=" << stats.frames
                  << " packets=" << stats.packets
                  << " payload_rate=" << (stats.rtp_payload_octets * 8.0 / seconds / 1'000'000.0)
                  << " Mbit/s\n";
        last_report = now;
      }
    }
    transmitter.close();
    const auto stats = transmitter.stats();
    const double jitter_ms = static_cast<double>(stats.maximum_interval_spread_ns) / 1'000'000.0;
    const bool timing_compliant = stats.timing_window_observed && jitter_ms <= 2.0;
    std::cout << "Sender stopped: frames=" << frame_index << " sent=" << stats.frames
              << " packets=" << stats.packets << " rtcp=" << stats.rtcp_reports
              << " frame_interval_spread_ms=" << jitter_ms
              << " timing=" << (timing_compliant ? "PASS" : "FAIL")
              << " missing_picture_timing=" << encoder.missing_picture_timing_count()
              << " incomplete_recovery=" << encoder.incomplete_recovery_point_count() << '\n';
    return options.require_timing_compliance &&
                   (!timing_compliant || encoder.missing_picture_timing_count() != 0U ||
                    encoder.incomplete_recovery_point_count() != 0U)
               ? 2
               : 0;
  } catch (const std::exception& error) {
    std::cerr << "ipmx-sender: " << error.what() << '\n';
    return 1;
  }
}
