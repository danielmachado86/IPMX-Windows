#include "ipmx/bgra_to_nv12.hpp"
#include "ipmx/rtp.hpp"
#include "ipmx/sdp.hpp"
#include "ipmx/udp_multicast.hpp"
#include "ipmx/sender/frame_source.hpp"
#include "ipmx/sender/x264_encoder.hpp"

#include <windows.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

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
  uint32_t duration_seconds{};
  size_t mtu{1200U};
  std::filesystem::path sdp{"ipmx.sdp"};
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
                   "[--fps N] [--bitrate-kbps N] [--group A.B.C.D] [--port N] "
                   "[--interface A.B.C.D] [--mtu N] [--duration-seconds N] [--sdp PATH]\n";
      std::exit(0);
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument("missing value after " + std::string(key));
    }
    const std::string value(argv[++index]);
    if (key == "--source") options.source = value;
    else if (key == "--width") options.width = parse_number<uint32_t>(value, "--width");
    else if (key == "--height") options.height = parse_number<uint32_t>(value, "--height");
    else if (key == "--fps") options.fps_numerator = parse_number<uint32_t>(value, "--fps");
    else if (key == "--bitrate-kbps") options.bitrate_kbps = parse_number<uint32_t>(value, "--bitrate-kbps");
    else if (key == "--group") options.group = value;
    else if (key == "--port") options.port = parse_number<uint16_t>(value, "--port");
    else if (key == "--interface") options.interface_address = value;
    else if (key == "--mtu") options.mtu = parse_number<size_t>(value, "--mtu");
    else if (key == "--duration-seconds") options.duration_seconds = parse_number<uint32_t>(value, "--duration-seconds");
    else if (key == "--sdp") options.sdp = value;
    else throw std::invalid_argument("unknown option: " + std::string(key));
  }
  if (options.source != "test" && options.source != "screen") {
    throw std::invalid_argument("--source must be test or screen");
  }
  return options;
}

} // namespace

int main(const int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    SetConsoleCtrlHandler(stop_handler, TRUE);

    std::unique_ptr<ipmx::sender::FrameSource> source = options.source == "screen"
        ? ipmx::sender::make_primary_monitor_source(options.fps_numerator,
                                                    options.fps_denominator)
        : ipmx::sender::make_test_pattern_source(options.width, options.height,
                                                 options.fps_numerator,
                                                 options.fps_denominator);
    const ipmx::sender::EncoderSettings encoder_settings{
        source->width(), source->height(), options.fps_numerator, options.fps_denominator,
        options.bitrate_kbps};
    ipmx::sender::X264Encoder encoder(encoder_settings);
    ipmx::RtpPacketizer packetizer(options.mtu);
    ipmx::MulticastSender network(options.group, options.port, options.interface_address);
    ipmx::SdpSettings sdp_settings{options.group, options.port, ipmx::kH264PayloadType,
                                   source->width(), source->height(), options.fps_numerator,
                                   options.fps_denominator, options.bitrate_kbps};
    ipmx::write_sdp(options.sdp, sdp_settings, encoder.sps(), encoder.pps());

    std::random_device random;
    const uint32_t initial_timestamp = (static_cast<uint32_t>(random()) << 16U) ^ random();
    uint64_t frame_index = 0U;
    uint64_t packet_count = 0U;
    uint64_t byte_count = 0U;
    const auto started = std::chrono::steady_clock::now();
    auto last_report = started;
    std::cout << "Sender: " << options.source << ' ' << source->width() << 'x' << source->height()
              << " @ " << options.fps_numerator << " fps -> " << options.group << ':' << options.port
              << " SSRC=" << packetizer.ssrc() << " SDP=" << options.sdp.string() << '\n';

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
      const uint32_t timestamp = ipmx::rtp_timestamp_for_frame(
          initial_timestamp, frame_index, options.fps_numerator, options.fps_denominator);
      for (const auto& packet : packetizer.packetize(access_unit.nals, timestamp,
                                                      cached_nv12->capture_time_ns)) {
        network.send(packet);
        ++packet_count;
        byte_count += packet.size();
      }
      ++frame_index;

      if (now - last_report >= std::chrono::seconds(1)) {
        const double seconds = std::chrono::duration<double>(now - started).count();
        std::cout << "frames=" << frame_index << " packets=" << packet_count
                  << " rate=" << (byte_count * 8.0 / seconds / 1'000'000.0) << " Mbit/s\n";
        last_report = now;
      }
    }
    std::cout << "Sender stopped: frames=" << frame_index << " packets=" << packet_count << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ipmx-sender: " << error.what() << '\n';
    return 1;
  }
}
