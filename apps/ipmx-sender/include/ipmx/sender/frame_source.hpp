#pragma once

#include "ipmx/types.hpp"

#include <cstdint>
#include <memory>

namespace ipmx::sender {

class FrameSource {
public:
  virtual ~FrameSource() = default;
  [[nodiscard]] virtual uint32_t width() const noexcept = 0;
  [[nodiscard]] virtual uint32_t height() const noexcept = 0;
  virtual bool next(BgraFrame& frame) = 0;
};

[[nodiscard]] std::unique_ptr<FrameSource> make_test_pattern_source(uint32_t width,
                                                                   uint32_t height,
                                                                   uint32_t fps_numerator,
                                                                   uint32_t fps_denominator,
                                                                   bool stress = false);
[[nodiscard]] std::unique_ptr<FrameSource> make_primary_monitor_source(uint32_t fps_numerator,
                                                                       uint32_t fps_denominator);

} // namespace ipmx::sender
