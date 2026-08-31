#pragma once

#include "ipmx/phase0/types.hpp"

namespace phase0 {

// BT.709, studio-range YCbCr. Dimensions must be even.
[[nodiscard]] Nv12Frame bgra_to_nv12(const BgraFrame& source);

} // namespace phase0
