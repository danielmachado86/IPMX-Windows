#pragma once

#include <cstdint>

namespace ipmx {
inline namespace v0 {

// ST 2110-10 Standard UDP Size Limit for a 1500-byte IPv4 Ethernet path.
inline constexpr uint16_t kMaximumStandardUdpPayloadBytes = 1'460U;

} // namespace v0
} // namespace ipmx
