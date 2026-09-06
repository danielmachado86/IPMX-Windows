#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ipmx {
inline namespace v0 {

class PcapRtpWriter {
public:
  PcapRtpWriter(const std::filesystem::path& path, std::string source_address,
                std::string destination_address, uint16_t source_port, uint16_t destination_port,
                uint8_t dscp = 36U);
  ~PcapRtpWriter();
  PcapRtpWriter(const PcapRtpWriter&) = delete;
  PcapRtpWriter& operator=(const PcapRtpWriter&) = delete;

  void write(std::span<const uint8_t> udp_payload);
  void write(std::span<const uint8_t> udp_payload, uint64_t unix_time_ns);
  void write(std::span<const uint8_t> udp_payload, uint64_t unix_time_ns, uint16_t source_port,
             uint16_t destination_port);

private:
  struct State;
  std::unique_ptr<State> state_;
};

class AsyncPcapRtpWriter {
public:
  AsyncPcapRtpWriter(const std::filesystem::path& path, std::string source_address,
                     std::string destination_address, uint16_t source_port,
                     uint16_t destination_port, uint8_t dscp = 36U);
  ~AsyncPcapRtpWriter();
  AsyncPcapRtpWriter(const AsyncPcapRtpWriter&) = delete;
  AsyncPcapRtpWriter& operator=(const AsyncPcapRtpWriter&) = delete;
  void enqueue(std::vector<uint8_t> udp_payload);
  void enqueue(std::vector<uint8_t> udp_payload, uint16_t source_port, uint16_t destination_port);
  void enqueue(std::vector<uint8_t> udp_payload, uint64_t unix_time_ns, uint16_t source_port,
               uint16_t destination_port);
  void close();

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace v0
} // namespace ipmx
