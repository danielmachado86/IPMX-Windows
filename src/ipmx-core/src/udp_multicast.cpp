#include "ipmx/phase0/udp_multicast.hpp"

#include <ws2tcpip.h>

#include <limits>
#include <stdexcept>
#include <system_error>

namespace phase0 {
namespace {

[[noreturn]] void throw_winsock(const char* message) {
  throw std::system_error(WSAGetLastError(), std::system_category(), message);
}

[[nodiscard]] in_addr parse_ipv4(const std::string& address) {
  in_addr result{};
  if (InetPtonA(AF_INET, address.c_str(), &result) != 1) {
    throw std::invalid_argument("invalid IPv4 address: " + address);
  }
  return result;
}

} // namespace

WinsockRuntime::WinsockRuntime() {
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    throw_winsock("WSAStartup failed");
  }
}

WinsockRuntime::~WinsockRuntime() { WSACleanup(); }

MulticastSender::MulticastSender(std::string group, const uint16_t port,
                                 std::string interface_address) {
  socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_ == INVALID_SOCKET) {
    throw_winsock("sender socket failed");
  }
  const BOOL loopback = TRUE;
  const int ttl = 1;
  if (setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loopback),
                 sizeof(loopback)) == SOCKET_ERROR ||
      setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl),
                 sizeof(ttl)) == SOCKET_ERROR) {
    throw_winsock("multicast sender option failed");
  }
  if (interface_address != "0.0.0.0") {
    const in_addr interface_ip = parse_ipv4(interface_address);
    if (setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_IF,
                   reinterpret_cast<const char*>(&interface_ip), sizeof(interface_ip)) == SOCKET_ERROR) {
      throw_winsock("IP_MULTICAST_IF failed");
    }
  }
  destination_.sin_family = AF_INET;
  destination_.sin_port = htons(port);
  destination_.sin_addr = parse_ipv4(group);
}

MulticastSender::~MulticastSender() {
  if (socket_ != INVALID_SOCKET) {
    closesocket(socket_);
  }
}

void MulticastSender::send(const std::span<const uint8_t> datagram) const {
  if (datagram.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("datagram is too large");
  }
  const int sent = sendto(socket_, reinterpret_cast<const char*>(datagram.data()),
                          static_cast<int>(datagram.size()), 0,
                          reinterpret_cast<const sockaddr*>(&destination_), sizeof(destination_));
  if (sent != static_cast<int>(datagram.size())) {
    throw_winsock("sendto failed");
  }
}

MulticastReceiver::MulticastReceiver(std::string group, const uint16_t port,
                                     std::string interface_address) {
  socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_ == INVALID_SOCKET) {
    throw_winsock("receiver socket failed");
  }
  const BOOL reuse = TRUE;
  if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse)) == SOCKET_ERROR) {
    throw_winsock("SO_REUSEADDR failed");
  }
  sockaddr_in bind_address{};
  bind_address.sin_family = AF_INET;
  bind_address.sin_port = htons(port);
  bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(socket_, reinterpret_cast<const sockaddr*>(&bind_address), sizeof(bind_address)) == SOCKET_ERROR) {
    throw_winsock("bind failed");
  }
  ip_mreq request{};
  request.imr_multiaddr = parse_ipv4(group);
  request.imr_interface = parse_ipv4(interface_address);
  if (setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&request),
                 sizeof(request)) == SOCKET_ERROR) {
    throw_winsock("IP_ADD_MEMBERSHIP failed");
  }
}

MulticastReceiver::~MulticastReceiver() {
  if (socket_ != INVALID_SOCKET) {
    closesocket(socket_);
  }
}

bool MulticastReceiver::receive(std::vector<uint8_t>& datagram, const int timeout_ms) const {
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(socket_, &readable);
  timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  const int selected = select(0, &readable, nullptr, nullptr, &timeout);
  if (selected == SOCKET_ERROR) {
    throw_winsock("select failed");
  }
  if (selected == 0) {
    return false;
  }
  datagram.resize(65'507U);
  const int received = recvfrom(socket_, reinterpret_cast<char*>(datagram.data()),
                                static_cast<int>(datagram.size()), 0, nullptr, nullptr);
  if (received == SOCKET_ERROR) {
    throw_winsock("recvfrom failed");
  }
  datagram.resize(static_cast<size_t>(received));
  return true;
}

} // namespace phase0
