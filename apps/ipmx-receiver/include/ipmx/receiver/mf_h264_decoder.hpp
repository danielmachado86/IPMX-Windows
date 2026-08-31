#pragma once

#include "ipmx/phase0/types.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace phase0 {

class MfH264Decoder {
public:
  MfH264Decoder(uint32_t width, uint32_t height, uint32_t fps_numerator, uint32_t fps_denominator);
  ~MfH264Decoder();
  MfH264Decoder(const MfH264Decoder&) = delete;
  MfH264Decoder& operator=(const MfH264Decoder&) = delete;

  [[nodiscard]] std::vector<DecodedFrame> decode(std::span<const uint8_t> annex_b,
                                                 uint64_t capture_time_ns);

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace phase0
