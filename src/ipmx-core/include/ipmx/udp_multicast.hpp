#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace ipmx {
inline namespace v0 {

inline constexpr size_t kMaximumUdpDatagramBytes = 65'507U;

// Returns the SDP ts-refclk value for the selected IPv4 interface.
[[nodiscard]] std::string local_mac_reference(std::string interface_address = "0.0.0.0");
// Resolves 0.0.0.0 to the IPv4 source selected by the Windows route table.
[[nodiscard]] std::string resolve_ipv4_source_address(const std::string& destination_address,
                                                      uint16_t destination_port,
                                                      std::string interface_address = "0.0.0.0");

class MulticastSender {
public:
  MulticastSender(std::string group, uint16_t port, std::string interface_address = "0.0.0.0",
                  uint8_t dscp = 36U);
  ~MulticastSender();
  MulticastSender(const MulticastSender&) = delete;
  MulticastSender& operator=(const MulticastSender&) = delete;
  void send(std::span<const uint8_t> datagram) const;

private:
  struct State;
  std::unique_ptr<State> state_;
};

class MulticastReceiver {
public:
  MulticastReceiver(std::string group, uint16_t port, std::string interface_address = "0.0.0.0");
  ~MulticastReceiver();
  MulticastReceiver(const MulticastReceiver&) = delete;
  MulticastReceiver& operator=(const MulticastReceiver&) = delete;
  [[nodiscard]] std::optional<size_t> receive(std::span<uint8_t> storage,
                                              int timeout_ms = 10) const;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace v0
} // namespace ipmx
