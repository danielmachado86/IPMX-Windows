#include "ipmx/sender/frame_transmitter.hpp"

#include "ipmx/pcap.hpp"
#include "ipmx/rtp.hpp"
#include "ipmx/sdp.hpp"
#include "ipmx/timing.hpp"
#include "ipmx/traffic_shaper.hpp"
#include "ipmx/udp_multicast.hpp"

// clang-format off: avrt.h requires Windows base types.
#include <windows.h>
#include <avrt.h>
// clang-format on

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace ipmx::sender {
namespace {

class MultimediaScheduler {
public:
  MultimediaScheduler() {
    handle_ = AvSetMmThreadCharacteristicsW(L"Distribution", &task_index_);
    if (handle_)
      static_cast<void>(AvSetMmThreadPriority(handle_, AVRT_PRIORITY_HIGH));
  }
  [[nodiscard]] bool registered() const noexcept { return handle_ != nullptr; }
  ~MultimediaScheduler() {
    if (handle_)
      static_cast<void>(AvRevertMmThreadCharacteristics(handle_));
  }

private:
  DWORD task_index_{};
  HANDLE handle_{};
};

} // namespace

IpmxFrameSchedule make_ipmx_frame_schedule(const uint64_t capture_time_ns,
                                           const uint64_t encoder_delay_ns,
                                           const uint64_t sender_reports_delay_ns,
                                           const uint64_t access_unit_offset_ns) {
  if (capture_time_ns == 0U || sender_reports_delay_ns > encoder_delay_ns ||
      capture_time_ns > std::numeric_limits<uint64_t>::max() - encoder_delay_ns ||
      capture_time_ns + encoder_delay_ns >
          std::numeric_limits<uint64_t>::max() - access_unit_offset_ns) {
    throw std::invalid_argument("invalid IPMX H.264 frame timing");
  }
  return {capture_time_ns + sender_reports_delay_ns, capture_time_ns + encoder_delay_ns,
          capture_time_ns + encoder_delay_ns + access_unit_offset_ns};
}

IpmxSessionTiming resolve_ipmx_session_timing(const uint32_t fps_numerator,
                                              const uint32_t fps_denominator,
                                              const std::optional<uint64_t> encoder_delay_ns,
                                              const std::optional<uint64_t> sender_reports_delay_ns,
                                              const uint64_t access_unit_offset_ns) {
  if (fps_numerator == 0U || fps_denominator == 0U)
    throw std::invalid_argument("frame rate must be non-zero");
  const uint64_t frame_period_ns =
      (1'000'000'000ULL * fps_denominator + fps_numerator - 1U) / fps_numerator;
  const uint64_t default_encoder_delay = frame_period_ns > std::numeric_limits<uint64_t>::max() / 3U
                                             ? std::numeric_limits<uint64_t>::max()
                                             : frame_period_ns * 3U;
  const uint64_t resolved_encoder_delay = encoder_delay_ns.value_or(default_encoder_delay);
  const uint64_t resolved_sender_reports_delay =
      sender_reports_delay_ns.value_or(resolved_encoder_delay);
  static_cast<void>(make_ipmx_frame_schedule(1U, resolved_encoder_delay,
                                             resolved_sender_reports_delay, access_unit_offset_ns));
  return {resolved_encoder_delay, resolved_sender_reports_delay, access_unit_offset_ns};
}

struct FrameTransmitter::State {
  explicit State(FrameTransmitterSettings value) : settings(std::move(value)) {}
  FrameTransmitterSettings settings;
  IpmxSessionTiming timing;
  mutable std::mutex mutex;
  std::condition_variable ready;
  std::condition_variable space;
  std::deque<TransmitFrame> queue;
  uint64_t queued_bytes{};
  uint64_t maximum_queued_bytes{};
  uint64_t maximum_queue_depth{};
  std::thread worker;
  std::exception_ptr error;
  FrameTransmitterStats stats;
  bool stopping{};
  bool closed{};
};

FrameTransmitter::FrameTransmitter(FrameTransmitterSettings settings)
    : state_(std::make_unique<State>(std::move(settings))) {
  if (state_->settings.maximum_queued_frames == 0U)
    throw std::invalid_argument("transmit queue must contain at least one frame");
  if (state_->settings.late_packet_threshold_ns == 0U || state_->settings.dscp > 63U)
    throw std::invalid_argument("invalid transmitter instrumentation settings");
  validate_ipmx_media_port(state_->settings.media_port);
  if (state_->settings.video.fps_numerator == 0U || state_->settings.video.fps_denominator == 0U) {
    throw std::invalid_argument("transmitter needs a valid frame rate");
  }
  state_->timing = resolve_ipmx_session_timing(
      state_->settings.video.fps_numerator, state_->settings.video.fps_denominator,
      state_->settings.encoder_delay_ns, state_->settings.sender_reports_delay_ns,
      state_->settings.access_unit_offset_ns);
  State* state = state_.get();
  state_->worker = std::thread([state] {
    try {
      MultimediaScheduler multimedia_scheduler;
      // Windows can resume a high-resolution waitable timer a few hundred
      // microseconds late under load. Keep only the final 500 us as an active
      // wait so the network cadence stays deterministic without busy-waiting
      // for an entire packet interval.
      PreciseWaiter waiter(500'000U);
      static_cast<void>(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL));
      MulticastSender rtp_network(state->settings.multicast_group, state->settings.media_port,
                                  state->settings.interface_address, state->settings.dscp);
      MulticastSender rtcp_network(state->settings.multicast_group,
                                   reserved_rtcp_port(state->settings.media_port),
                                   state->settings.interface_address, state->settings.dscp);
      Tr107TrafficShaper shaper(state->settings.maximum_ip_bitrate_kbps,
                                state->settings.maximum_udp_bytes);
      NetworkCompatibilityTracker network_compatibility(shaper.maximum_packets_per_second(),
                                                        shaper.cmax());
      FrameIntervalTracker intervals;
      std::unique_ptr<AsyncPcapRtpWriter> pcap;
      if (!state->settings.pcap_path.empty()) {
        const std::string source = state->settings.interface_address == "0.0.0.0"
                                       ? "127.0.0.1"
                                       : state->settings.interface_address;
        pcap = std::make_unique<AsyncPcapRtpWriter>(
            state->settings.pcap_path, source, state->settings.multicast_group,
            state->settings.media_port, state->settings.media_port, state->settings.dscp);
      }
      const auto wall_reference = std::chrono::system_clock::now().time_since_epoch();
      const uint64_t wall_reference_ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(wall_reference).count());
      const uint64_t qpc_reference_ns = qpc_now_ns();
      const auto to_unix_ns = [wall_reference_ns, qpc_reference_ns](const uint64_t qpc_ns) {
        if (qpc_ns >= qpc_reference_ns &&
            wall_reference_ns <=
                std::numeric_limits<uint64_t>::max() - (qpc_ns - qpc_reference_ns)) {
          return wall_reference_ns + (qpc_ns - qpc_reference_ns);
        }
        if (qpc_ns < qpc_reference_ns && qpc_reference_ns - qpc_ns <= wall_reference_ns)
          return wall_reference_ns - (qpc_reference_ns - qpc_ns);
        return wall_reference_ns;
      };
      uint64_t packet_count = 0U;
      uint64_t payload_octets = 0U;
      uint64_t frame_count = 0U;
      uint64_t rtcp_count = 0U;
      uint64_t maximum_sender_report_lateness_ns = 0U;
      uint64_t maximum_encoder_cpb_lateness_ns = 0U;
      uint64_t maximum_packet_lateness_ns = 0U;
      uint64_t maximum_send_jitter_ns = 0U;
      uint64_t late_packets = 0U;
      uint64_t previous_packet_actual_ns = 0U;
      uint64_t previous_packet_scheduled_ns = 0U;
      uint32_t rtp_clock_alignment{};
      bool rtp_clock_aligned{};
      const auto update_stats = [&] {
        std::lock_guard lock(state->mutex);
        state->stats.frames = frame_count;
        state->stats.packets = packet_count;
        state->stats.rtp_payload_octets = payload_octets;
        state->stats.rtcp_reports = rtcp_count;
        state->stats.maximum_sender_report_lateness_ns = maximum_sender_report_lateness_ns;
        state->stats.maximum_encoder_cpb_lateness_ns = maximum_encoder_cpb_lateness_ns;
        state->stats.maximum_packet_lateness_ns = maximum_packet_lateness_ns;
        state->stats.maximum_send_jitter_ns = maximum_send_jitter_ns;
        state->stats.maximum_interval_spread_ns = intervals.maximum_interval_spread_ns();
        state->stats.late_packets = late_packets;
        state->stats.ncm_violations = network_compatibility.violations();
        state->stats.current_cpb_occupancy_bytes = state->queued_bytes;
        state->stats.maximum_cpb_occupancy_bytes = state->maximum_queued_bytes;
        state->stats.current_queued_frames = state->queue.size();
        state->stats.maximum_queued_frames = state->maximum_queue_depth;
        state->stats.configured_cmax = shaper.cmax();
        state->stats.maximum_cinst_observed = network_compatibility.maximum_cinst();
        state->stats.mmcss_registered = multimedia_scheduler.registered();
        state->stats.high_resolution_timer = waiter.uses_high_resolution_timer();
        state->stats.timing_window_observed = intervals.window_observed();
      };
      for (;;) {
        TransmitFrame frame;
        uint64_t frame_bytes = 0U;
        {
          std::unique_lock lock(state->mutex);
          state->ready.wait(lock, [state] { return state->stopping || !state->queue.empty(); });
          if (state->queue.empty()) {
            if (state->stopping)
              break;
            continue;
          }
          frame = std::move(state->queue.front());
          state->queue.pop_front();
          for (const auto& packet : frame.rtp_packets)
            frame_bytes += packet.size();
          state->stats.current_queued_frames = state->queue.size();
          state->space.notify_one();
        }
        const uint64_t capture_time_ns =
            frame.capture_time_ns != 0U ? frame.capture_time_ns : qpc_now_ns();
        const auto schedule = make_ipmx_frame_schedule(
            capture_time_ns, state->timing.encoder_delay_ns, state->timing.sender_reports_delay_ns,
            state->timing.access_unit_offset_ns);

        const uint64_t wall_ns = to_unix_ns(capture_time_ns);
        if (!rtp_clock_aligned) {
          const uint64_t wall_seconds = wall_ns / 1'000'000'000U;
          const uint64_t wall_nanoseconds = wall_ns % 1'000'000'000U;
          const uint32_t clock_timestamp = static_cast<uint32_t>(
              wall_seconds * kRtpClockRate + wall_nanoseconds * kRtpClockRate / 1'000'000'000U);
          rtp_clock_alignment = clock_timestamp - frame.rtp_timestamp;
          rtp_clock_aligned = true;
        }
        frame.rtp_timestamp += rtp_clock_alignment;
        for (auto& packet : frame.rtp_packets) {
          if (packet.size() < 8U)
            throw std::runtime_error("invalid RTP packet queued for transmission");
          packet[4U] = static_cast<uint8_t>(frame.rtp_timestamp >> 24U);
          packet[5U] = static_cast<uint8_t>(frame.rtp_timestamp >> 16U);
          packet[6U] = static_cast<uint8_t>(frame.rtp_timestamp >> 8U);
          packet[7U] = static_cast<uint8_t>(frame.rtp_timestamp);
        }
        IpmxRtcpSenderReport report;
        report.ssrc = state->settings.ssrc;
        report.ptp_seconds = static_cast<uint32_t>(wall_ns / 1'000'000'000U);
        report.ptp_nanoseconds = static_cast<uint32_t>(wall_ns % 1'000'000'000U);
        report.rtp_timestamp = frame.rtp_timestamp;
        report.packet_count = static_cast<uint32_t>(packet_count);
        report.octet_count = static_cast<uint32_t>(payload_octets);
        report.block_version = state->settings.block_version;
        report.ts_refclk = state->settings.ts_refclk;
        report.media_clock = state->settings.media_clock;
        report.cname = state->settings.cname;
        report.video = state->settings.video;
        report.h264 = state->settings.h264;
        const auto compound = make_ipmx_rtcp_compound(report);

        waiter.wait_until(schedule.sender_report_time_ns);
        const uint64_t sender_report_actual_ns = qpc_now_ns();
        maximum_sender_report_lateness_ns =
            std::max(maximum_sender_report_lateness_ns,
                     sender_report_actual_ns > schedule.sender_report_time_ns
                         ? sender_report_actual_ns - schedule.sender_report_time_ns
                         : 0U);
        rtcp_network.send(compound);
        if (pcap) {
          const uint16_t rtcp_port = reserved_rtcp_port(state->settings.media_port);
          pcap->enqueue(compound, to_unix_ns(sender_report_actual_ns), rtcp_port, rtcp_port);
        }
        ++rtcp_count;

        if (frame.rtp_packets.empty()) {
          ++frame_count;
          {
            std::lock_guard lock(state->mutex);
            state->queued_bytes =
                frame_bytes >= state->queued_bytes ? 0U : state->queued_bytes - frame_bytes;
          }
          update_stats();
          continue;
        }

        waiter.wait_until(schedule.encoder_cpb_insertion_time_ns);
        const uint64_t encoder_cpb_actual_ns = qpc_now_ns();
        maximum_encoder_cpb_lateness_ns =
            std::max(maximum_encoder_cpb_lateness_ns,
                     encoder_cpb_actual_ns > schedule.encoder_cpb_insertion_time_ns
                         ? encoder_cpb_actual_ns - schedule.encoder_cpb_insertion_time_ns
                         : 0U);

        bool first_packet = true;
        for (auto& packet : frame.rtp_packets) {
          const uint64_t scheduled = shaper.schedule_ns(
              first_packet ? std::max(schedule.first_rtp_time_ns, qpc_now_ns()) : qpc_now_ns(),
              packet.size());
          waiter.wait_until(scheduled);
          const uint64_t actual = qpc_now_ns();
          const uint64_t lateness = actual > scheduled ? actual - scheduled : 0U;
          maximum_packet_lateness_ns = std::max(maximum_packet_lateness_ns, lateness);
          if (lateness > state->settings.late_packet_threshold_ns)
            ++late_packets;
          if (previous_packet_actual_ns != 0U && actual >= previous_packet_actual_ns &&
              scheduled >= previous_packet_scheduled_ns) {
            const uint64_t actual_interval = actual - previous_packet_actual_ns;
            const uint64_t scheduled_interval = scheduled - previous_packet_scheduled_ns;
            const uint64_t jitter = actual_interval >= scheduled_interval
                                        ? actual_interval - scheduled_interval
                                        : scheduled_interval - actual_interval;
            maximum_send_jitter_ns = std::max(maximum_send_jitter_ns, jitter);
          }
          previous_packet_actual_ns = actual;
          previous_packet_scheduled_ns = scheduled;
          if (first_packet)
            intervals.observe(actual);
          network_compatibility.observe(actual);
          if (const auto parsed = parse_rtp_packet(packet))
            payload_octets += parsed->payload.size();
          rtp_network.send(packet);
          ++packet_count;
          if (pcap)
            pcap->enqueue(std::move(packet), to_unix_ns(actual), state->settings.media_port,
                          state->settings.media_port);
          first_packet = false;
        }
        ++frame_count;
        {
          std::lock_guard lock(state->mutex);
          state->queued_bytes =
              frame_bytes >= state->queued_bytes ? 0U : state->queued_bytes - frame_bytes;
        }
        update_stats();
      }
      update_stats();
      if (pcap)
        pcap->close();
    } catch (...) {
      std::lock_guard lock(state->mutex);
      state->error = std::current_exception();
      state->stopping = true;
      state->space.notify_all();
      state->ready.notify_all();
    }
  });
}

FrameTransmitter::~FrameTransmitter() {
  try {
    close();
  } catch (...) {
  }
}

void FrameTransmitter::enqueue(TransmitFrame frame) {
  uint64_t frame_bytes = 0U;
  for (const auto& packet : frame.rtp_packets) {
    if (frame_bytes > std::numeric_limits<uint64_t>::max() - packet.size()) {
      frame_bytes = std::numeric_limits<uint64_t>::max();
      break;
    }
    frame_bytes += packet.size();
  }
  std::unique_lock lock(state_->mutex);
  state_->space.wait(lock, [this] {
    return state_->stopping || state_->error ||
           state_->queue.size() < state_->settings.maximum_queued_frames;
  });
  if (state_->error)
    std::rethrow_exception(state_->error);
  if (state_->stopping)
    throw std::runtime_error("frame transmitter is closed");
  state_->queued_bytes = state_->queued_bytes > std::numeric_limits<uint64_t>::max() - frame_bytes
                             ? std::numeric_limits<uint64_t>::max()
                             : state_->queued_bytes + frame_bytes;
  state_->maximum_queued_bytes = std::max(state_->maximum_queued_bytes, state_->queued_bytes);
  state_->queue.push_back(std::move(frame));
  state_->maximum_queue_depth =
      std::max<uint64_t>(state_->maximum_queue_depth, state_->queue.size());
  state_->stats.current_cpb_occupancy_bytes = state_->queued_bytes;
  state_->stats.maximum_cpb_occupancy_bytes = state_->maximum_queued_bytes;
  state_->stats.current_queued_frames = state_->queue.size();
  state_->stats.maximum_queued_frames = state_->maximum_queue_depth;
  state_->ready.notify_one();
}

void FrameTransmitter::close() {
  {
    std::lock_guard lock(state_->mutex);
    if (state_->closed) {
      if (state_->error)
        std::rethrow_exception(state_->error);
      return;
    }
    state_->stopping = true;
    state_->closed = true;
    state_->ready.notify_all();
    state_->space.notify_all();
  }
  if (state_->worker.joinable())
    state_->worker.join();
  if (state_->error)
    std::rethrow_exception(state_->error);
}

FrameTransmitterStats FrameTransmitter::stats() const {
  std::lock_guard lock(state_->mutex);
  if (state_->error)
    std::rethrow_exception(state_->error);
  return state_->stats;
}

IpmxSessionTiming FrameTransmitter::timing() const { return state_->timing; }

} // namespace ipmx::sender
