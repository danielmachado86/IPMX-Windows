#include "ipmx/sender/frame_transmitter.hpp"

#include "ipmx/pcap.hpp"
#include "ipmx/rtp.hpp"
#include "ipmx/sdp.hpp"
#include "ipmx/traffic_shaper.hpp"
#include "ipmx/udp_multicast.hpp"

#include <windows.h>
#include <avrt.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace ipmx::sender {
namespace {

[[nodiscard]] uint64_t steady_now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

class TimerResolution {
public:
  TimerResolution() : active_(timeBeginPeriod(1U) == TIMERR_NOERROR) {}
  ~TimerResolution() {
    if (active_)
      timeEndPeriod(1U);
  }

private:
  bool active_{};
};

class MultimediaScheduler {
public:
  MultimediaScheduler() {
    handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index_);
    if (handle_)
      static_cast<void>(AvSetMmThreadPriority(handle_, AVRT_PRIORITY_CRITICAL));
  }
  ~MultimediaScheduler() {
    if (handle_)
      static_cast<void>(AvRevertMmThreadCharacteristics(handle_));
  }

private:
  DWORD task_index_{};
  HANDLE handle_{};
};

class PreciseWaiter {
public:
  PreciseWaiter() {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_MODIFY_STATE | SYNCHRONIZE);
  }
  ~PreciseWaiter() {
    if (timer_)
      CloseHandle(timer_);
  }

  void wait_until(const uint64_t deadline_ns) const {
    constexpr uint64_t spin_window_ns = 3'000'000U;
    for (;;) {
      const uint64_t now_ns = steady_now_ns();
      if (now_ns >= deadline_ns)
        return;
      const uint64_t remaining_ns = deadline_ns - now_ns;
      if (remaining_ns > spin_window_ns) {
        const uint64_t wait_ns = remaining_ns - spin_window_ns;
        if (timer_) {
          LARGE_INTEGER due{};
          due.QuadPart = -static_cast<LONGLONG>(std::max<uint64_t>(1U, wait_ns / 100U));
          if (SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0U)) {
            static_cast<void>(WaitForSingleObject(timer_, INFINITE));
            continue;
          }
        }
        std::this_thread::sleep_for(std::chrono::nanoseconds(wait_ns));
      } else if (remaining_ns > 100'000U) {
        std::this_thread::yield();
      }
    }
  }

private:
  HANDLE timer_{};
};

} // namespace

struct FrameTransmitter::State {
  explicit State(FrameTransmitterSettings value) : settings(std::move(value)) {}
  FrameTransmitterSettings settings;
  mutable std::mutex mutex;
  std::condition_variable ready;
  std::condition_variable space;
  std::deque<TransmitFrame> queue;
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
  validate_ipmx_media_port(state_->settings.media_port);
  State* state = state_.get();
  state_->worker = std::thread([state] {
    try {
      TimerResolution timer_resolution;
      MultimediaScheduler multimedia_scheduler;
      PreciseWaiter waiter;
      static_cast<void>(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST));
      MulticastSender rtp_network(state->settings.multicast_group, state->settings.media_port,
                                  state->settings.interface_address);
      MulticastSender rtcp_network(state->settings.multicast_group,
                                   reserved_rtcp_port(state->settings.media_port),
                                   state->settings.interface_address);
      Tr107TrafficShaper shaper(state->settings.maximum_ip_bitrate_kbps,
                                state->settings.maximum_udp_bytes);
      FrameIntervalTracker intervals;
      std::unique_ptr<AsyncPcapRtpWriter> pcap;
      if (!state->settings.pcap_path.empty()) {
        const std::string source = state->settings.interface_address == "0.0.0.0"
                                       ? "127.0.0.1"
                                       : state->settings.interface_address;
        pcap = std::make_unique<AsyncPcapRtpWriter>(
            state->settings.pcap_path, source, state->settings.multicast_group,
            state->settings.media_port, state->settings.media_port);
      }
      uint64_t packet_count = 0U;
      uint64_t payload_octets = 0U;
      uint64_t frame_count = 0U;
      uint64_t rtcp_count = 0U;
      uint64_t schedule_origin_ns = 0U;
      for (;;) {
        TransmitFrame frame;
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
          state->space.notify_one();
        }
        if (frame.rtp_packets.empty())
          continue;

        const uint64_t frame_period_numerator =
            1'000'000'000ULL * state->settings.video.fps_denominator;
        if (schedule_origin_ns == 0U) {
          const uint64_t prebuffer_ns =
              (2U * frame_period_numerator + state->settings.video.fps_numerator - 1U) /
              state->settings.video.fps_numerator;
          schedule_origin_ns = steady_now_ns() + prebuffer_ns;
        }
        const uint64_t frame_deadline_ns =
            schedule_origin_ns +
            (frame_count * frame_period_numerator) / state->settings.video.fps_numerator;

        const auto wall = std::chrono::system_clock::now().time_since_epoch();
        const uint64_t wall_now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(wall).count());
        const uint64_t steady_reference_ns = steady_now_ns();
        const uint64_t wall_ns =
            frame.capture_time_ns != 0U && wall_now_ns >= steady_reference_ns
                ? wall_now_ns - steady_reference_ns + frame.capture_time_ns
                : wall_now_ns;
        IpmxRtcpSenderReport report;
        report.ssrc = state->settings.ssrc;
        report.ptp_seconds = static_cast<uint32_t>(wall_ns / 1'000'000'000U);
        report.ptp_nanoseconds = static_cast<uint32_t>(wall_ns % 1'000'000'000U);
        report.rtp_timestamp = frame.rtp_timestamp;
        report.packet_count = static_cast<uint32_t>(packet_count);
        report.octet_count = static_cast<uint32_t>(payload_octets);
        report.ts_refclk = state->settings.ts_refclk;
        report.media_clock = state->settings.media_clock;
        report.cname = state->settings.cname;
        report.video = state->settings.video;
        const auto compound = make_ipmx_rtcp_compound(report);

        size_t offset = 0U;
        bool first_burst = true;
        while (offset < frame.rtp_packets.size()) {
          const size_t count = std::min<size_t>(shaper.cmax(), frame.rtp_packets.size() - offset);
          size_t ip_bytes = 0U;
          for (size_t index = 0U; index < count; ++index)
            ip_bytes += frame.rtp_packets[offset + index].size() + 28U;
          const uint64_t scheduled = shaper.schedule_burst_ns(
              first_burst ? frame_deadline_ns : steady_now_ns(), ip_bytes);
          if (first_burst) {
            constexpr uint64_t rtcp_lead_ns = 1'000'000U;
            waiter.wait_until(scheduled > rtcp_lead_ns ? scheduled - rtcp_lead_ns : scheduled);
            rtcp_network.send(compound);
            ++rtcp_count;
          }
          waiter.wait_until(scheduled);
          for (size_t index = 0U; index < count; ++index) {
            auto& packet = frame.rtp_packets[offset + index];
            if (first_burst && index == 0U)
              intervals.observe(steady_now_ns());
            rtp_network.send(packet);
            if (const auto parsed = parse_rtp_packet(packet))
              payload_octets += parsed->payload.size();
            ++packet_count;
            if (pcap)
              pcap->enqueue(std::move(packet));
          }
          first_burst = false;
          offset += count;
        }
        ++frame_count;
        {
          std::lock_guard lock(state->mutex);
          state->stats = {frame_count, packet_count, payload_octets, rtcp_count,
                          intervals.maximum_interval_spread_ns(), intervals.window_observed()};
        }
      }
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
  std::unique_lock lock(state_->mutex);
  state_->space.wait(lock, [this] {
    return state_->stopping || state_->error ||
           state_->queue.size() < state_->settings.maximum_queued_frames;
  });
  if (state_->error)
    std::rethrow_exception(state_->error);
  if (state_->stopping)
    throw std::runtime_error("frame transmitter is closed");
  state_->queue.push_back(std::move(frame));
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

} // namespace ipmx::sender
