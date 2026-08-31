#pragma once

#include "ipmx/annexb.hpp"
#include "ipmx/types.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace ipmx::sender {

struct EncoderSettings {
  uint32_t width{};
  uint32_t height{};
  uint32_t fps_numerator{60U};
  uint32_t fps_denominator{1U};
  uint32_t target_bitrate_kbps{4'000U};
};

struct EncodedAccessUnit {
  int64_t pts{};
  bool keyframe{};
  std::vector<NalUnit> nals;
};

class X264Encoder {
public:
  explicit X264Encoder(const EncoderSettings& settings);
  ~X264Encoder();
  X264Encoder(const X264Encoder&) = delete;
  X264Encoder& operator=(const X264Encoder&) = delete;

  [[nodiscard]] EncodedAccessUnit encode(const Nv12Frame& frame, int64_t pts);
  [[nodiscard]] const NalUnit& sps() const noexcept { return sps_; }
  [[nodiscard]] const NalUnit& pps() const noexcept { return pps_; }

private:
  struct State;
  std::unique_ptr<State> state_;
  NalUnit sps_;
  NalUnit pps_;
};

} // namespace ipmx::sender
