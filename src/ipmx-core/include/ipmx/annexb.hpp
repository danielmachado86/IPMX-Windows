#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ipmx {
inline namespace v0 {

using NalUnit = std::vector<uint8_t>;

[[nodiscard]] std::vector<NalUnit> parse_annex_b(std::span<const uint8_t> bytes);
[[nodiscard]] std::vector<uint8_t> build_annex_b(const std::vector<NalUnit>& nals);

} // namespace v0
} // namespace ipmx
