#include "ipmx/h264.hpp"
#include "ipmx/metrics.hpp"
#include "ipmx/receiver/d3d11_renderer.hpp"
#include "ipmx/receiver/mf_h264_decoder.hpp"
#include "ipmx/rtp.hpp"
#include "ipmx/sdp.hpp"
#include "ipmx/udp_multicast.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::atomic_bool running{true};

BOOL WINAPI stop_handler(DWORD) {
  running = false;
  return TRUE;
}

struct Options {
  std::string group{"239.255.42.42"};
  std::string interface_address{"0.0.0.0"};
  uint16_t port{5004U};
  uint32_t width{1280U};
  uint32_t height{720U};
  uint32_t fps_numerator{60U};
  uint32_t fps_denominator{1U};
  uint32_t duration_seconds{};
  uint32_t maximum_latency_ms{250U};
  bool local_clock{};
  bool require_zero_loss{};
  std::filesystem::path sdp;
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
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::string_view(argv[index]) == "--sdp") {
      options.sdp = argv[index + 1];
      const auto description = ipmx::read_sdp_description(options.sdp);
      const auto validation = ipmx::validate_ipmx_sdp(description);
      if (!validation.empty()) {
        std::string message = "SDP does not conform to the IPMX H.264 transport profile";
        for (const auto& error : validation)
          message += "; " + error;
        throw std::runtime_error(message);
      }
      const auto& settings = description.settings;
      options.group = settings.multicast_group;
      options.port = settings.port;
      options.width = settings.width;
      options.height = settings.height;
      options.fps_numerator = settings.fps_numerator;
      options.fps_denominator = settings.fps_denominator;
      break;
    }
  }
  for (int index = 1; index < argc; ++index) {
    const std::string_view key(argv[index]);
    if (key == "--local-clock") {
      options.local_clock = true;
      continue;
    }
    if (key == "--help") {
      std::cout << "ipmx-receiver [--width N] [--height N] [--fps N] [--group A.B.C.D] "
                   "[--port N] [--interface A.B.C.D] [--sdp PATH] [--duration-seconds N] "
                   "[--local-clock] [--max-latency-ms N] [--require-zero-loss]\n";
      std::exit(0);
    }
    if (key == "--require-zero-loss") {
      options.require_zero_loss = true;
      continue;
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument("missing value after " + std::string(key));
    }
    const std::string value(argv[++index]);
    if (key == "--sdp")
      options.sdp = value;
    else if (key == "--width")
      options.width = parse_number<uint32_t>(value, "--width");
    else if (key == "--height")
      options.height = parse_number<uint32_t>(value, "--height");
    else if (key == "--fps") {
      options.fps_numerator = parse_number<uint32_t>(value, "--fps");
      options.fps_denominator = 1U;
    } else if (key == "--group")
      options.group = value;
    else if (key == "--port")
      options.port = parse_number<uint16_t>(value, "--port");
    else if (key == "--interface")
      options.interface_address = value;
    else if (key == "--duration-seconds")
      options.duration_seconds = parse_number<uint32_t>(value, "--duration-seconds");
    else if (key == "--max-latency-ms")
      options.maximum_latency_ms = parse_number<uint32_t>(value, "--max-latency-ms");
    else
      throw std::invalid_argument("unknown option: " + std::string(key));
  }
  if (options.maximum_latency_ms == 0U) {
    throw std::invalid_argument("--max-latency-ms must be greater than zero");
  }
  ipmx::validate_ipmx_media_port(options.port);
  if (!ipmx::is_recommended_ipmx_media_port(options.port))
    std::cerr << "warning: IPMX recommends an RTP media port greater than 5000\n";
  return options;
}

void print_stats(const ipmx::SequenceStats& sequence, const uint64_t access_units,
                 const uint64_t decoded_frames, const uint64_t late_frames,
                 const uint64_t invalid_packets, const ipmx::LatencyMetrics& latency) {
  std::cout << std::fixed << std::setprecision(3) << "packets=" << sequence.received
            << " loss=" << sequence.lost << " reordered=" << sequence.reordered
            << " invalid=" << invalid_packets << " AUs=" << access_units
            << " frames=" << decoded_frames << " late=" << late_frames
            << " latency_ms(mean/min/max/stddev)=" << latency.mean_ms() << '/'
            << latency.minimum_ms() << '/' << latency.maximum_ms() << '/'
            << latency.standard_deviation_ms() << '\n';
}

[[nodiscard]] bool is_recovery_access_unit(const std::span<const uint8_t> annex_b) {
  bool sps = false;
  bool pps = false;
  bool idr = false;
  for (const auto& nal : ipmx::parse_annex_b(annex_b)) {
    sps = sps || ipmx::h264_nal_type(nal) == ipmx::kH264NalSps;
    pps = pps || ipmx::h264_nal_type(nal) == ipmx::kH264NalPps;
    idr = idr || ipmx::h264_nal_type(nal) == ipmx::kH264NalIdr;
  }
  return sps && pps && idr;
}

} // namespace

int main(const int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    SetConsoleCtrlHandler(stop_handler, TRUE);
    ipmx::MulticastReceiver network(options.group, options.port, options.interface_address);
    ipmx::receiver::MfH264Decoder decoder(options.width, options.height, options.fps_numerator,
                                          options.fps_denominator);
    ipmx::receiver::D3d11Renderer renderer(options.width, options.height);
    ipmx::SequenceTracker sequence;
    ipmx::H264Depacketizer depacketizer;
    ipmx::LatencyMetrics latency;

    uint64_t invalid_packets = 0U;
    uint64_t access_units = 0U;
    uint64_t decoded_frames = 0U;
    uint64_t late_frames = 0U;
    bool synchronized = false;
    std::array<uint8_t, ipmx::kMaximumUdpDatagramBytes> datagram{};
    const uint64_t maximum_latency_ns =
        static_cast<uint64_t>(options.maximum_latency_ms) * 1'000'000ULL;
    const auto started = std::chrono::steady_clock::now();
    auto last_report = started;
    std::cout << "Receiver: " << options.group << ':' << options.port << " -> " << options.width
              << 'x' << options.height << '\n';

    while (running && renderer.pump_messages()) {
      const auto now = std::chrono::steady_clock::now();
      if (options.duration_seconds != 0U &&
          now - started >= std::chrono::seconds(options.duration_seconds)) {
        break;
      }
      if (const auto received = network.receive(datagram, 10)) {
        const auto packet =
            ipmx::parse_rtp_packet(std::span<const uint8_t>(datagram.data(), *received));
        if (!packet || packet->payload_type != ipmx::kH264PayloadType) {
          ++invalid_packets;
        } else {
          sequence.observe(packet->sequence);
          if (auto access_unit = depacketizer.push(*packet)) {
            ++access_units;
            synchronized = synchronized || is_recovery_access_unit(access_unit->annex_b);
            if (synchronized) {
              for (const auto& frame :
                   decoder.decode(access_unit->annex_b, access_unit->capture_time_ns)) {
                const uint64_t ready_time = ipmx::steady_now_ns();
                if (options.local_clock && frame.capture_time_ns != 0U && ready_time >= frame.capture_time_ns &&
                    ready_time - frame.capture_time_ns > maximum_latency_ns) {
                  latency.observe_ns(ready_time - frame.capture_time_ns);
                  ++late_frames;
                  continue;
                }
                const uint64_t presented = renderer.present(frame);
                if (options.local_clock && frame.capture_time_ns != 0U && presented >= frame.capture_time_ns) {
                  latency.observe_ns(presented - frame.capture_time_ns);
                }
                ++decoded_frames;
              }
            }
          }
        }
      }
      if (now - last_report >= std::chrono::seconds(1)) {
        print_stats(sequence.stats(), access_units, decoded_frames, late_frames, invalid_packets,
                    latency);
        last_report = now;
      }
    }

    print_stats(sequence.stats(), access_units, decoded_frames, late_frames, invalid_packets,
                latency);
    std::cout << "latency_domain=" << (options.local_clock ? "local-QPC" : "UNVERIFIABLE")
              << " presentation_event=graphics-call-completion\n";
    const bool clean = options.local_clock && sequence.stats().lost == 0U && sequence.stats().reordered == 0U &&
                       invalid_packets == 0U && late_frames == 0U && decoded_frames > 0U &&
                       latency.count() > 0U &&
                       latency.maximum_ms() <= static_cast<double>(options.maximum_latency_ms);
    std::cout << "Receiver stopped: " << (clean ? "PASS" : "FAIL") << '\n';
    return options.require_zero_loss && !clean ? 2 : 0;
  } catch (const std::exception& error) {
    std::cerr << "ipmx-receiver: " << error.what() << '\n';
    return 1;
  }
}
