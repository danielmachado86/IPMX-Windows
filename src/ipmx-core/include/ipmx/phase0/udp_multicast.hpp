#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <winsock2.h>

namespace phase0 {

class WinsockRuntime {
public:
  WinsockRuntime();
  ~WinsockRuntime();
  WinsockRuntime(const WinsockRuntime&) = delete;
  WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};

class MulticastSender {
public:
  MulticastSender(std::string group, uint16_t port, std::string interface_address = "0.0.0.0");
  ~MulticastSender();
  MulticastSender(const MulticastSender&) = delete;
  MulticastSender& operator=(const MulticastSender&) = delete;
  void send(std::span<const uint8_t> datagram) const;

private:
  WinsockRuntime winsock_;
  SOCKET socket_{INVALID_SOCKET};
  sockaddr_in destination_{};
};

class MulticastReceiver {
public:
  MulticastReceiver(std::string group, uint16_t port, std::string interface_address = "0.0.0.0");
  ~MulticastReceiver();
  MulticastReceiver(const MulticastReceiver&) = delete;
  MulticastReceiver& operator=(const MulticastReceiver&) = delete;
  [[nodiscard]] bool receive(std::vector<uint8_t>& datagram, int timeout_ms = 10) const;

private:
  WinsockRuntime winsock_;
  SOCKET socket_{INVALID_SOCKET};
};

} // namespace phase0

