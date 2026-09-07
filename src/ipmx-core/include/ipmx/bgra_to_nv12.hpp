#pragma once

#include "ipmx/types.hpp"

namespace ipmx {
inline namespace v0 {

// BT.709, studio-range YCbCr. Dimensions must be even.
void bgra_to_nv12(const BgraFrame& source, Nv12Frame& result);
[[nodiscard]] Nv12Frame bgra_to_nv12(const BgraFrame& source);

} // namespace v0
} // namespace ipmx
