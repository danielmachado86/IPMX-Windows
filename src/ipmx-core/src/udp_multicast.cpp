// clang-format off: winsock2 must precede Windows networking headers.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
// clang-format on

#include "ipmx/udp_multicast.hpp"

#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace ipmx {
inline namespace v0 {
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

class WinsockRuntime {
public:
  WinsockRuntime() {
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
      throw std::system_error(result, std::system_category(), "WSAStartup failed");
    }
  }
  ~WinsockRuntime() { WSACleanup(); }
  WinsockRuntime(const WinsockRuntime&) = delete;
  WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};

class SocketHandle {
public:
  SocketHandle() {
    value_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (value_ == INVALID_SOCKET) {
      throw_winsock("UDP socket creation failed");
    }
  }
  ~SocketHandle() {
    if (value_ != INVALID_SOCKET) {
      closesocket(value_);
    }
  }
  SocketHandle(const SocketHandle&) = delete;
  SocketHandle& operator=(const SocketHandle&) = delete;
  [[nodiscard]] SOCKET get() const noexcept { return value_; }

private:
  SOCKET value_{INVALID_SOCKET};
};

} // namespace

std::string resolve_ipv4_source_address(const std::string& destination_address,
                                        const uint16_t destination_port,
                                        const std::string interface_address) {
  const in_addr requested = parse_ipv4(interface_address);
  if (requested.s_addr != htonl(INADDR_ANY))
    return interface_address;

  WinsockRuntime winsock;
  SocketHandle socket;
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(destination_port);
  destination.sin_addr = parse_ipv4(destination_address);
  if (connect(socket.get(), reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) ==
      SOCKET_ERROR) {
    throw_winsock("cannot resolve multicast source interface");
  }
  sockaddr_in source{};
  int source_size = sizeof(source);
  if (getsockname(socket.get(), reinterpret_cast<sockaddr*>(&source), &source_size) ==
      SOCKET_ERROR) {
    throw_winsock("cannot read multicast source interface");
  }
  char text[INET_ADDRSTRLEN]{};
  if (!InetNtopA(AF_INET, &source.sin_addr, text, static_cast<DWORD>(sizeof(text))))
    throw_winsock("cannot format multicast source interface");
  return text;
}

std::string local_mac_reference(const std::string interface_address) {
  const in_addr requested = parse_ipv4(interface_address);
  ULONG size = 0U;
  constexpr ULONG flags =
      GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW)
    throw std::runtime_error("cannot enumerate network interfaces");
  std::vector<uint8_t> storage(size);
  auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) != NO_ERROR)
    throw std::runtime_error("cannot enumerate network interfaces");

  const bool any = requested.s_addr == htonl(INADDR_ANY);
  const IP_ADAPTER_ADDRESSES* selected = nullptr;
  for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp || adapter->PhysicalAddressLength != 6U)
      continue;
    bool matches = any;
    for (auto* unicast = adapter->FirstUnicastAddress; !matches && unicast;
         unicast = unicast->Next) {
      if (unicast->Address.lpSockaddr && unicast->Address.lpSockaddr->sa_family == AF_INET) {
        const auto* address = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
        matches = address->sin_addr.s_addr == requested.s_addr;
      }
    }
    if (matches) {
      selected = adapter;
      break;
    }
  }
  if (!selected)
    throw std::runtime_error("no active EUI-48 interface matches --interface");
  std::ostringstream result;
  result << "localmac=" << std::uppercase << std::hex << std::setfill('0');
  for (ULONG index = 0U; index < selected->PhysicalAddressLength; ++index) {
    if (index != 0U)
      result << '-';
    result << std::setw(2) << static_cast<unsigned>(selected->PhysicalAddress[index]);
  }
  return result.str();
}

struct MulticastSender::State {
  WinsockRuntime winsock;
  SocketHandle socket;
  sockaddr_in destination{};
};

struct MulticastReceiver::State {
  WinsockRuntime winsock;
  SocketHandle socket;
};

MulticastSender::MulticastSender(std::string group, const uint16_t port,
                                 std::string interface_address, const uint8_t dscp)
    : state_(std::make_unique<State>()) {
  if (dscp > 63U)
    throw std::invalid_argument("DSCP must be in the range 0..63");
  const BOOL loopback = TRUE;
  const int ttl = 1;
  const int traffic_class = static_cast<int>(dscp) << 2;
  if (setsockopt(state_->socket.get(), IPPROTO_IP, IP_MULTICAST_LOOP,
                 reinterpret_cast<const char*>(&loopback), sizeof(loopback)) == SOCKET_ERROR ||
      setsockopt(state_->socket.get(), IPPROTO_IP, IP_MULTICAST_TTL,
                 reinterpret_cast<const char*>(&ttl), sizeof(ttl)) == SOCKET_ERROR ||
      setsockopt(state_->socket.get(), IPPROTO_IP, IP_TOS,
                 reinterpret_cast<const char*>(&traffic_class),
                 sizeof(traffic_class)) == SOCKET_ERROR) {
    throw_winsock("multicast sender option failed");
  }

  const in_addr interface_ip = parse_ipv4(interface_address);
  if (setsockopt(state_->socket.get(), IPPROTO_IP, IP_MULTICAST_IF,
                 reinterpret_cast<const char*>(&interface_ip),
                 sizeof(interface_ip)) == SOCKET_ERROR) {
    throw_winsock("IP_MULTICAST_IF failed");
  }

  state_->destination.sin_family = AF_INET;
  state_->destination.sin_port = htons(port);
  state_->destination.sin_addr = parse_ipv4(group);
}

MulticastSender::~MulticastSender() = default;

void MulticastSender::send(const std::span<const uint8_t> datagram) const {
  if (datagram.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("datagram is too large");
  }
  const int sent =
      sendto(state_->socket.get(), reinterpret_cast<const char*>(datagram.data()),
             static_cast<int>(datagram.size()), 0,
             reinterpret_cast<const sockaddr*>(&state_->destination), sizeof(state_->destination));
  if (sent != static_cast<int>(datagram.size())) {
    throw_winsock("sendto failed");
  }
}

MulticastReceiver::MulticastReceiver(std::string group, const uint16_t port,
                                     std::string interface_address)
    : state_(std::make_unique<State>()) {
  const BOOL reuse = TRUE;
  if (setsockopt(state_->socket.get(), SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse)) == SOCKET_ERROR) {
    throw_winsock("SO_REUSEADDR failed");
  }

  constexpr int receive_buffer_bytes = 4 * 1024 * 1024;
  if (setsockopt(state_->socket.get(), SOL_SOCKET, SO_RCVBUF,
                 reinterpret_cast<const char*>(&receive_buffer_bytes),
                 sizeof(receive_buffer_bytes)) == SOCKET_ERROR) {
    throw_winsock("SO_RCVBUF failed");
  }

  sockaddr_in bind_address{};
  bind_address.sin_family = AF_INET;
  bind_address.sin_port = htons(port);
  bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(state_->socket.get(), reinterpret_cast<const sockaddr*>(&bind_address),
           sizeof(bind_address)) == SOCKET_ERROR) {
    throw_winsock("bind failed");
  }

  ip_mreq request{};
  request.imr_multiaddr = parse_ipv4(group);
  request.imr_interface = parse_ipv4(interface_address);
  if (setsockopt(state_->socket.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                 reinterpret_cast<const char*>(&request), sizeof(request)) == SOCKET_ERROR) {
    throw_winsock("IP_ADD_MEMBERSHIP failed");
  }
}

MulticastReceiver::~MulticastReceiver() = default;

std::optional<size_t> MulticastReceiver::receive(const std::span<uint8_t> storage,
                                                 const int timeout_ms) const {
  if (storage.empty() || storage.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      timeout_ms < 0) {
    throw std::invalid_argument("invalid multicast receive buffer or timeout");
  }

  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(state_->socket.get(), &readable);
  timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  const int selected = select(0, &readable, nullptr, nullptr, &timeout);
  if (selected == SOCKET_ERROR) {
    throw_winsock("select failed");
  }
  if (selected == 0) {
    return std::nullopt;
  }

  const int received = recvfrom(state_->socket.get(), reinterpret_cast<char*>(storage.data()),
                                static_cast<int>(storage.size()), 0, nullptr, nullptr);
  if (received == SOCKET_ERROR) {
    throw_winsock("recvfrom failed");
  }
  return static_cast<size_t>(received);
}

} // namespace v0
} // namespace ipmx
