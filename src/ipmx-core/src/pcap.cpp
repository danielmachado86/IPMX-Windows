#include "ipmx/pcap.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <vector>

namespace ipmx {
inline namespace v0 {
namespace {

void write_u16_be(std::span<uint8_t> bytes, const size_t offset, const uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value >> 8U);
  bytes[offset + 1U] = static_cast<uint8_t>(value);
}

void write_u32_le(std::ostream& output, const uint32_t value) {
  const std::array<char, 4U> bytes{static_cast<char>(value), static_cast<char>(value >> 8U),
                                   static_cast<char>(value >> 16U),
                                   static_cast<char>(value >> 24U)};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u16_le(std::ostream& output, const uint16_t value) {
  const std::array<char, 2U> bytes{static_cast<char>(value), static_cast<char>(value >> 8U)};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::array<uint8_t, 4U> parse_ipv4(const std::string& text) {
  std::array<uint8_t, 4U> bytes{};
  size_t offset = 0U;
  for (size_t index = 0U; index < bytes.size(); ++index) {
    const size_t dot = text.find('.', offset);
    const size_t end = dot == std::string::npos ? text.size() : dot;
    if (end == offset || (index < 3U) != (dot != std::string::npos)) {
      throw std::invalid_argument("invalid IPv4 address for PCAP");
    }
    unsigned value = 0U;
    for (size_t position = offset; position < end; ++position) {
      if (text[position] < '0' || text[position] > '9') {
        throw std::invalid_argument("invalid IPv4 address for PCAP");
      }
      value = value * 10U + static_cast<unsigned>(text[position] - '0');
      if (value > 255U)
        throw std::invalid_argument("invalid IPv4 address for PCAP");
    }
    bytes[index] = static_cast<uint8_t>(value);
    offset = end + 1U;
  }
  return bytes;
}

[[nodiscard]] uint16_t ipv4_checksum(const std::span<const uint8_t> header) noexcept {
  uint32_t sum = 0U;
  for (size_t offset = 0U; offset + 1U < header.size(); offset += 2U) {
    sum +=
        static_cast<uint32_t>((static_cast<uint16_t>(header[offset]) << 8U) | header[offset + 1U]);
  }
  while (sum >> 16U)
    sum = (sum & 0xFFFFU) + (sum >> 16U);
  return static_cast<uint16_t>(~sum);
}

} // namespace

struct PcapRtpWriter::State {
  std::ofstream output;
  std::array<uint8_t, 4U> source{};
  std::array<uint8_t, 4U> destination{};
  uint16_t source_port{};
  uint16_t destination_port{};
  uint16_t identification{};
};

PcapRtpWriter::PcapRtpWriter(const std::filesystem::path& path, std::string source_address,
                             std::string destination_address, const uint16_t source_port,
                             const uint16_t destination_port)
    : state_(std::make_unique<State>()) {
  state_->source = parse_ipv4(source_address);
  state_->destination = parse_ipv4(destination_address);
  state_->source_port = source_port;
  state_->destination_port = destination_port;
  state_->output.open(path, std::ios::binary | std::ios::trunc);
  if (!state_->output)
    throw std::runtime_error("cannot create PCAP file");
  write_u32_le(state_->output, 0xA1B2C3D4U);
  write_u16_le(state_->output, 2U);
  write_u16_le(state_->output, 4U);
  write_u32_le(state_->output, 0U);
  write_u32_le(state_->output, 0U);
  write_u32_le(state_->output, 65'535U);
  write_u32_le(state_->output, 101U); // LINKTYPE_RAW: packet starts with an IPv4 header.
}

PcapRtpWriter::~PcapRtpWriter() = default;

void PcapRtpWriter::write(const std::span<const uint8_t> udp_payload) {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  write(udp_payload,
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
}

void PcapRtpWriter::write(const std::span<const uint8_t> udp_payload, const uint64_t unix_time_ns) {
  write(udp_payload, unix_time_ns, state_->source_port, state_->destination_port);
}

void PcapRtpWriter::write(const std::span<const uint8_t> udp_payload, const uint64_t unix_time_ns,
                          const uint16_t source_port, const uint16_t destination_port) {
  if (udp_payload.size() > std::numeric_limits<uint16_t>::max() - 28U) {
    throw std::length_error("UDP packet is too large for IPv4 PCAP");
  }
  std::vector<uint8_t> packet(28U + udp_payload.size(), 0U);
  packet[0] = 0x45U;
  write_u16_be(packet, 2U, static_cast<uint16_t>(packet.size()));
  write_u16_be(packet, 4U, state_->identification++);
  write_u16_be(packet, 6U, 0x4000U); // Don't Fragment.
  packet[8] = 64U;
  packet[9] = 17U;
  std::copy(state_->source.begin(), state_->source.end(), packet.begin() + 12);
  std::copy(state_->destination.begin(), state_->destination.end(), packet.begin() + 16);
  write_u16_be(packet, 10U, ipv4_checksum(std::span<const uint8_t>(packet.data(), 20U)));
  write_u16_be(packet, 20U, source_port);
  write_u16_be(packet, 22U, destination_port);
  write_u16_be(packet, 24U, static_cast<uint16_t>(udp_payload.size() + 8U));
  std::copy(udp_payload.begin(), udp_payload.end(), packet.begin() + 28);

  write_u32_le(state_->output, static_cast<uint32_t>(unix_time_ns / 1'000'000'000U));
  write_u32_le(state_->output, static_cast<uint32_t>((unix_time_ns % 1'000'000'000U) / 1'000U));
  write_u32_le(state_->output, static_cast<uint32_t>(packet.size()));
  write_u32_le(state_->output, static_cast<uint32_t>(packet.size()));
  state_->output.write(reinterpret_cast<const char*>(packet.data()),
                       static_cast<std::streamsize>(packet.size()));
  if (!state_->output)
    throw std::runtime_error("cannot write PCAP file");
}

struct AsyncPcapRtpWriter::State {
  struct Entry {
    std::vector<uint8_t> payload;
    uint64_t unix_time_ns{};
    uint16_t source_port{};
    uint16_t destination_port{};
  };
  PcapRtpWriter writer;
  uint16_t source_port{};
  uint16_t destination_port{};
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<Entry> entries;
  std::thread worker;
  std::exception_ptr error;
  uint64_t dropped{};
  bool stopping{};

  State(const std::filesystem::path& path, std::string source_address,
        std::string destination_address, const uint16_t source_port,
        const uint16_t destination_port)
      : writer(path, std::move(source_address), std::move(destination_address), source_port,
               destination_port),
        source_port(source_port), destination_port(destination_port) {}
};

AsyncPcapRtpWriter::AsyncPcapRtpWriter(const std::filesystem::path& path,
                                       std::string source_address,
                                       std::string destination_address,
                                       const uint16_t source_port,
                                       const uint16_t destination_port)
    : state_(std::make_unique<State>(path, std::move(source_address),
                                     std::move(destination_address), source_port,
                                     destination_port)) {
  State* state = state_.get();
  state_->worker = std::thread([state] {
    try {
      for (;;) {
        State::Entry entry;
        {
          std::unique_lock lock(state->mutex);
          state->ready.wait(lock, [state] { return state->stopping || !state->entries.empty(); });
          if (state->entries.empty()) {
            if (state->stopping)
              break;
            continue;
          }
          entry = std::move(state->entries.front());
          state->entries.pop_front();
        }
        state->writer.write(entry.payload, entry.unix_time_ns, entry.source_port,
                            entry.destination_port);
      }
    } catch (...) {
      std::lock_guard lock(state->mutex);
      state->error = std::current_exception();
      state->stopping = true;
      state->ready.notify_all();
    }
  });
}

AsyncPcapRtpWriter::~AsyncPcapRtpWriter() {
  try {
    close();
  } catch (...) {
  }
}

void AsyncPcapRtpWriter::enqueue(std::vector<uint8_t> udp_payload) {
  enqueue(std::move(udp_payload), state_->source_port, state_->destination_port);
}

void AsyncPcapRtpWriter::enqueue(std::vector<uint8_t> udp_payload, const uint16_t source_port,
                                 const uint16_t destination_port) {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const uint64_t unix_time_ns =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  std::lock_guard lock(state_->mutex);
  if (state_->error)
    std::rethrow_exception(state_->error);
  if (state_->stopping)
    throw std::runtime_error("PCAP writer is closed");
  constexpr size_t maximum_queued_packets = 8'192U;
  if (state_->entries.size() >= maximum_queued_packets) {
    ++state_->dropped;
    return;
  }
  state_->entries.push_back(
      {std::move(udp_payload), unix_time_ns, source_port, destination_port});
  state_->ready.notify_one();
}

void AsyncPcapRtpWriter::close() {
  if (!state_)
    return;
  {
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
    state_->ready.notify_all();
  }
  if (state_->worker.joinable())
    state_->worker.join();
  if (state_->error)
    std::rethrow_exception(state_->error);
  if (state_->dropped != 0U)
    throw std::runtime_error("PCAP writer could not keep up and dropped queued packets");
}

} // namespace v0
} // namespace ipmx
