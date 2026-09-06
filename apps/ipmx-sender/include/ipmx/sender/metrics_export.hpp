#pragma once

#include "ipmx/sender/frame_transmitter.hpp"

#include <filesystem>

namespace ipmx::sender {

void write_transmitter_metrics_csv(const std::filesystem::path& path,
                                   const FrameTransmitterStats& stats,
                                   const IpmxSessionTiming& timing);
void write_transmitter_metrics_json(const std::filesystem::path& path,
                                    const FrameTransmitterStats& stats,
                                    const IpmxSessionTiming& timing);

} // namespace ipmx::sender
