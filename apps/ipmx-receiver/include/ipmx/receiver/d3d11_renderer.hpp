#pragma once

#include "ipmx/phase0/types.hpp"

#include <cstdint>
#include <memory>

namespace phase0 {

class D3d11Renderer {
public:
  D3d11Renderer(uint32_t width, uint32_t height);
  ~D3d11Renderer();
  D3d11Renderer(const D3d11Renderer&) = delete;
  D3d11Renderer& operator=(const D3d11Renderer&) = delete;

  [[nodiscard]] bool pump_messages();
  [[nodiscard]] uint64_t present(const DecodedFrame& frame);

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace phase0
