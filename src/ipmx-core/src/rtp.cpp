#include "ipmx/phase0/rtp.hpp"

#include <algorithm>
#include <limits>
#include <random>
#include <stdexcept>

namespace phase0 {
namespace {

void write_u16(std::vector<uint8_t>& bytes, const size_t offset, const uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value >> 8U);
  bytes[offset + 1U] = static_cast<uint8_t>(value);
}

void write_u32(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value) {
  bytes[offset] = static_cast<uint8_t>(value >> 24U);
  bytes[offset + 1U] = static_cast<uint8_t>(value >> 16U);
  bytes[offset + 2U] = static_cast<uint8_t>(value >> 8U);
  bytes[offset + 3U] = static_cast<uint8_t>(value);
}

void write_u64(std::vector<uint8_t>& bytes, const size_t offset, const uint64_t value) {
  for (size_t i = 0; i < 8U; ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (56U - static_cast<unsigned>(i * 8U)));
  }
}

[[nodiscard]] uint16_t read_u16(const std::span<const uint8_t> bytes, const size_t offset) {
  return static_cast<uint16_t>((static_cast<uint16_t>(bytes[offset]) << 8U) | bytes[offset + 1U]);
}

[[nodiscard]] uint32_t read_u32(const std::span<const uint8_t> bytes, const size_t offset) {
  return (static_cast<uint32_t>(bytes[offset]) << 24U) |
         (static_cast<uint32_t>(bytes[offset + 1U]) << 16U) |
         (static_cast<uint32_t>(bytes[offset + 2U]) << 8U) | bytes[offset + 3U];
}

[[nodiscard]] uint64_t read_u64(const std::span<const uint8_t> bytes, const size_t offset) {
  uint64_t value = 0U;
  for (size_t i = 0; i < 8U; ++i) {
    value = (value << 8U) | bytes[offset + i];
  }
  return value;
}

} // namespace

RtpPacketizer::RtpPacketizer(const size_t maximum_datagram_bytes, const uint8_t payload_type,
                             const std::optional<uint32_t> deterministic_seed)
    : maximum_datagram_bytes_(maximum_datagram_bytes), payload_type_(payload_type) {
  if (maximum_datagram_bytes_ < 64U || maximum_datagram_bytes_ > 65'507U || payload_type_ > 127U) {
    throw std::invalid_argument("invalid RTP packetizer configuration");
  }
  std::random_device random_device;
  std::mt19937 generator(deterministic_seed.value_or(random_device()));
  std::uniform_int_distribution<uint32_t> u32(1U, std::numeric_limits<uint32_t>::max());
  std::uniform_int_distribution<uint32_t> u16(0U, std::numeric_limits<uint16_t>::max());
  ssrc_ = u32(generator);
  sequence_ = static_cast<uint16_t>(u16(generator));
}

std::vector<uint8_t> RtpPacketizer::make_packet(const std::span<const uint8_t> payload,
                                                const uint32_t timestamp,
                                                const uint64_t capture_time_ns,
                                                const bool marker) {
  constexpr size_t base_header_size = 12U;
  constexpr size_t extension_header_size = 4U;
  constexpr size_t extension_data_size = 12U;
  constexpr size_t full_header_size = base_header_size + extension_header_size + extension_data_size;
  std::vector<uint8_t> bytes(full_header_size + payload.size(), 0U);
  bytes[0] = 0x90U; // RTP v2, extension present, no CSRC.
  bytes[1] = static_cast<uint8_t>((marker ? 0x80U : 0U) | payload_type_);
  write_u16(bytes, 2U, sequence_++);
  write_u32(bytes, 4U, timestamp);
  write_u32(bytes, 8U, ssrc_);
  write_u16(bytes, 12U, kCaptureTimeExtensionProfile);
  write_u16(bytes, 14U, 3U); // Three 32-bit words follow.
  bytes[16U] = static_cast<uint8_t>((kCaptureTimeExtensionId << 4U) | 7U); // Eight bytes.
  write_u64(bytes, 17U, capture_time_ns);
  std::copy(payload.begin(), payload.end(), bytes.begin() + static_cast<std::ptrdiff_t>(full_header_size));
  return bytes;
}

std::vector<std::vector<uint8_t>> RtpPacketizer::packetize(const std::vector<NalUnit>& nals,
                                                            const uint32_t timestamp,
                                                            const uint64_t capture_time_ns) {
  constexpr size_t full_header_size = 28U;
  const size_t maximum_payload = maximum_datagram_bytes_ - full_header_size;
  if (maximum_payload <= 2U) {
    throw std::logic_error("RTP MTU leaves no H.264 payload space");
  }

  std::vector<std::vector<uint8_t>> packets;
  for (size_t nal_index = 0; nal_index < nals.size(); ++nal_index) {
    const auto& nal = nals[nal_index];
    if (nal.empty()) {
      continue;
    }
    const bool final_nal = nal_index + 1U == nals.size();
    if (nal.size() <= maximum_payload) {
      packets.push_back(make_packet(nal, timestamp, capture_time_ns, final_nal));
      continue;
    }

    const uint8_t original_header = nal.front();
    const uint8_t fu_indicator = static_cast<uint8_t>((original_header & 0xE0U) | 28U);
    const uint8_t nal_type = static_cast<uint8_t>(original_header & 0x1FU);
    const size_t maximum_fragment_data = maximum_payload - 2U;
    size_t offset = 1U;
    bool first = true;
    while (offset < nal.size()) {
      const size_t length = std::min(maximum_fragment_data, nal.size() - offset);
      const bool last = offset + length == nal.size();
      std::vector<uint8_t> payload(2U + length);
      payload[0] = fu_indicator;
      payload[1] = static_cast<uint8_t>(nal_type | (first ? 0x80U : 0U) | (last ? 0x40U : 0U));
      std::copy_n(nal.data() + offset, length, payload.data() + 2U);
      packets.push_back(make_packet(payload, timestamp, capture_time_ns, final_nal && last));
      offset += length;
      first = false;
    }
  }
  return packets;
}

std::optional<ParsedRtpPacket> parse_rtp_packet(const std::span<const uint8_t> datagram) {
  if (datagram.size() < 12U || (datagram[0] >> 6U) != 2U) {
    return std::nullopt;
  }
  const bool padding = (datagram[0] & 0x20U) != 0U;
  const bool extension = (datagram[0] & 0x10U) != 0U;
  const size_t csrc_count = datagram[0] & 0x0FU;
  size_t payload_offset = 12U + csrc_count * 4U;
  if (payload_offset > datagram.size()) {
    return std::nullopt;
  }

  uint64_t capture_time_ns = 0U;
  if (extension) {
    if (payload_offset + 4U > datagram.size()) {
      return std::nullopt;
    }
    const uint16_t profile = read_u16(datagram, payload_offset);
    const size_t extension_size = static_cast<size_t>(read_u16(datagram, payload_offset + 2U)) * 4U;
    const size_t extension_begin = payload_offset + 4U;
    if (extension_begin + extension_size > datagram.size()) {
      return std::nullopt;
    }
    if (profile == kCaptureTimeExtensionProfile) {
      size_t cursor = extension_begin;
      const size_t end = extension_begin + extension_size;
      while (cursor < end) {
        const uint8_t header = datagram[cursor++];
        if (header == 0U) {
          continue;
        }
        const uint8_t id = header >> 4U;
        if (id == 15U) {
          break;
        }
        const size_t length = (header & 0x0FU) + 1U;
        if (cursor + length > end) {
          return std::nullopt;
        }
        if (id == kCaptureTimeExtensionId && length == 8U) {
          capture_time_ns = read_u64(datagram, cursor);
        }
        cursor += length;
      }
    }
    payload_offset = extension_begin + extension_size;
  }

  size_t payload_end = datagram.size();
  if (padding) {
    const size_t padding_size = datagram.back();
    if (padding_size == 0U || padding_size > payload_end - payload_offset) {
      return std::nullopt;
    }
    payload_end -= padding_size;
  }
  if (payload_offset >= payload_end) {
    return std::nullopt;
  }

  ParsedRtpPacket result;
  result.sequence = read_u16(datagram, 2U);
  result.timestamp = read_u32(datagram, 4U);
  result.ssrc = read_u32(datagram, 8U);
  result.payload_type = datagram[1] & 0x7FU;
  result.marker = (datagram[1] & 0x80U) != 0U;
  result.capture_time_ns = capture_time_ns;
  result.payload = datagram.subspan(payload_offset, payload_end - payload_offset);
  return result;
}

void H264Depacketizer::start_timestamp(const uint32_t timestamp, const uint64_t capture_time_ns) {
  active_ = true;
  fragmented_ = false;
  damaged_ = false;
  timestamp_ = timestamp;
  capture_time_ns_ = capture_time_ns;
  nals_.clear();
  fragment_.clear();
}

void H264Depacketizer::reset() noexcept {
  active_ = false;
  fragmented_ = false;
  damaged_ = false;
  nals_.clear();
  fragment_.clear();
}

std::optional<CompletedAccessUnit> H264Depacketizer::push(const ParsedRtpPacket& packet) {
  if (packet.payload_type != kH264PayloadType || packet.payload.empty()) {
    return std::nullopt;
  }
  if (!active_ || packet.timestamp != timestamp_) {
    start_timestamp(packet.timestamp, packet.capture_time_ns);
  }
  if (capture_time_ns_ == 0U) {
    capture_time_ns_ = packet.capture_time_ns;
  }

  if ((fragmented_ || !nals_.empty()) && static_cast<uint16_t>(last_sequence_ + 1U) != packet.sequence) {
    damaged_ = true;
  }
  last_sequence_ = packet.sequence;

  const uint8_t nal_type = packet.payload[0] & 0x1FU;
  if (nal_type >= 1U && nal_type <= 23U) {
    if (fragmented_) {
      damaged_ = true;
      fragment_.clear();
      fragmented_ = false;
    }
    nals_.emplace_back(packet.payload.begin(), packet.payload.end());
  } else if (nal_type == 28U && packet.payload.size() >= 2U) {
    const bool start = (packet.payload[1] & 0x80U) != 0U;
    const bool end = (packet.payload[1] & 0x40U) != 0U;
    if (start) {
      fragment_.clear();
      fragment_.push_back(static_cast<uint8_t>((packet.payload[0] & 0xE0U) | (packet.payload[1] & 0x1FU)));
      fragment_.insert(fragment_.end(), packet.payload.begin() + 2, packet.payload.end());
      fragmented_ = true;
    } else if (fragmented_) {
      fragment_.insert(fragment_.end(), packet.payload.begin() + 2, packet.payload.end());
    } else {
      damaged_ = true;
    }
    if (end && fragmented_) {
      nals_.push_back(std::move(fragment_));
      fragment_.clear();
      fragmented_ = false;
    }
  } else {
    damaged_ = true; // Phase 0 supports only Single NAL and FU-A.
  }

  if (!packet.marker) {
    return std::nullopt;
  }
  if (fragmented_) {
    damaged_ = true;
  }
  if (damaged_ || nals_.empty()) {
    reset();
    return std::nullopt;
  }

  CompletedAccessUnit result{timestamp_, capture_time_ns_, build_annex_b(nals_)};
  reset();
  return result;
}

void SequenceTracker::observe(const uint16_t sequence) noexcept {
  ++stats_.received;
  if (!initialized_) {
    initialized_ = true;
    expected_ = static_cast<uint16_t>(sequence + 1U);
    return;
  }
  const int16_t distance = static_cast<int16_t>(sequence - expected_);
  if (distance == 0) {
    expected_ = static_cast<uint16_t>(expected_ + 1U);
  } else if (distance > 0) {
    stats_.lost += static_cast<uint16_t>(distance);
    expected_ = static_cast<uint16_t>(sequence + 1U);
  } else {
    ++stats_.reordered;
  }
}

uint32_t rtp_timestamp_for_frame(const uint32_t initial_timestamp, const uint64_t frame_index,
                                 const uint32_t fps_numerator, const uint32_t fps_denominator) {
  if (fps_numerator == 0U || fps_denominator == 0U) {
    throw std::invalid_argument("frame rate must be non-zero");
  }
  const uint64_t ticks = (frame_index * kRtpClockRate * fps_denominator + fps_numerator / 2U) /
                         fps_numerator;
  return initial_timestamp + static_cast<uint32_t>(ticks);
}

} // namespace phase0
