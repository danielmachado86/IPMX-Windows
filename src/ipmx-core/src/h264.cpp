#include "ipmx/h264.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ipmx {
inline namespace v0 {
namespace {

[[nodiscard]] std::optional<std::vector<uint8_t>> rbsp_from_nal(const NalUnit& nal,
                                                                const uint8_t expected_type) {
  if (nal.empty() || h264_nal_type(nal) != expected_type || (nal.front() & 0x80U) != 0U) {
    return std::nullopt;
  }
  std::vector<uint8_t> rbsp;
  rbsp.reserve(nal.size() - 1U);
  unsigned zero_count = 0U;
  for (size_t index = 1U; index < nal.size(); ++index) {
    const uint8_t byte = nal[index];
    if (zero_count >= 2U && byte == 0x03U) {
      zero_count = 0U;
      continue;
    }
    rbsp.push_back(byte);
    zero_count = byte == 0U ? zero_count + 1U : 0U;
  }
  return rbsp;
}

class BitReader {
public:
  explicit BitReader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

  [[nodiscard]] bool read_bit(bool& value) {
    uint32_t bit = 0U;
    if (!read_bits(1U, bit))
      return false;
    value = bit != 0U;
    return true;
  }

  [[nodiscard]] bool read_bits(const unsigned count, uint32_t& value) {
    if (count > 32U || bit_offset_ + count > bytes_.size() * 8U)
      return false;
    value = 0U;
    for (unsigned index = 0U; index < count; ++index) {
      value = static_cast<uint32_t>((value << 1U) |
                                    ((bytes_[bit_offset_ / 8U] >> (7U - bit_offset_ % 8U)) & 1U));
      ++bit_offset_;
    }
    return true;
  }

  [[nodiscard]] bool read_ue(uint32_t& value) {
    unsigned leading_zero_bits = 0U;
    bool bit = false;
    while (true) {
      if (!read_bit(bit))
        return false;
      if (bit)
        break;
      if (++leading_zero_bits > 31U)
        return false;
    }
    uint32_t suffix = 0U;
    if (!read_bits(leading_zero_bits, suffix))
      return false;
    value = ((1U << leading_zero_bits) - 1U) + suffix;
    return true;
  }

  [[nodiscard]] bool read_se(int32_t& value) {
    uint32_t code = 0U;
    if (!read_ue(code) || code > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) * 2U) {
      return false;
    }
    value = (code & 1U) != 0U ? static_cast<int32_t>((code + 1U) / 2U)
                              : -static_cast<int32_t>(code / 2U);
    return true;
  }

private:
  const std::vector<uint8_t>& bytes_;
  size_t bit_offset_{};
};

[[nodiscard]] bool skip_scaling_list(BitReader& bits, const unsigned count) {
  int32_t last_scale = 8;
  int32_t next_scale = 8;
  for (unsigned index = 0U; index < count; ++index) {
    if (next_scale != 0) {
      int32_t delta_scale = 0;
      if (!bits.read_se(delta_scale) || delta_scale < -128 || delta_scale > 127)
        return false;
      next_scale = static_cast<int32_t>((static_cast<int64_t>(last_scale) + delta_scale + 256) %
                                        256);
    }
    last_scale = next_scale == 0 ? last_scale : next_scale;
  }
  return true;
}

[[nodiscard]] bool parse_hrd(BitReader& bits, H264HrdInfo& hrd) {
  if (!bits.read_ue(hrd.cpb_cnt_minus1) || hrd.cpb_cnt_minus1 > 31U)
    return false;
  uint32_t ignored = 0U;
  if (!bits.read_bits(4U, ignored) || !bits.read_bits(4U, ignored))
    return false;
  for (uint32_t index = 0U; index <= hrd.cpb_cnt_minus1; ++index) {
    bool cbr = false;
    if (!bits.read_ue(ignored) || !bits.read_ue(ignored) || !bits.read_bit(cbr))
      return false;
    if (index == 0U)
      hrd.cbr_flag = cbr;
  }
  return bits.read_bits(5U, ignored) && bits.read_bits(5U, ignored) &&
         bits.read_bits(5U, ignored) && bits.read_bits(5U, ignored);
}

[[nodiscard]] bool parse_vui(BitReader& bits, H264SpsInfo& sps) {
  bool present = false;
  uint32_t value = 0U;
  if (!bits.read_bit(present))
    return false;
  if (present) {
    if (!bits.read_bits(8U, value))
      return false;
    if (value == 255U && (!bits.read_bits(16U, value) || !bits.read_bits(16U, value)))
      return false;
  }
  if (!bits.read_bit(present))
    return false;
  if (present && !bits.read_bit(present))
    return false;
  if (!bits.read_bit(sps.video_signal_type_present_flag))
    return false;
  if (sps.video_signal_type_present_flag) {
    if (!bits.read_bits(3U, value) || !bits.read_bit(sps.video_full_range_flag) ||
        !bits.read_bit(sps.colour_description_present_flag))
      return false;
    if (sps.colour_description_present_flag) {
      if (!bits.read_bits(8U, value))
        return false;
      sps.colour_primaries = static_cast<uint8_t>(value);
      if (!bits.read_bits(8U, value))
        return false;
      sps.transfer_characteristics = static_cast<uint8_t>(value);
      if (!bits.read_bits(8U, value))
        return false;
      sps.matrix_coefficients = static_cast<uint8_t>(value);
    }
  }
  if (!bits.read_bit(present))
    return false;
  if (present && (!bits.read_ue(value) || !bits.read_ue(value)))
    return false;
  if (!bits.read_bit(sps.timing_info_present_flag))
    return false;
  if (sps.timing_info_present_flag) {
    if (!bits.read_bits(32U, sps.num_units_in_tick) || !bits.read_bits(32U, sps.time_scale) ||
        !bits.read_bit(sps.fixed_frame_rate_flag))
      return false;
  }
  if (!bits.read_bit(sps.nal_hrd_parameters_present_flag))
    return false;
  if (sps.nal_hrd_parameters_present_flag) {
    H264HrdInfo hrd;
    if (!parse_hrd(bits, hrd))
      return false;
    sps.nal_hrd = hrd;
  }
  bool vcl_hrd_present = false;
  if (!bits.read_bit(vcl_hrd_present))
    return false;
  if (vcl_hrd_present) {
    H264HrdInfo ignored_hrd;
    if (!parse_hrd(bits, ignored_hrd))
      return false;
  }
  if ((sps.nal_hrd_parameters_present_flag || vcl_hrd_present) && !bits.read_bit(present))
    return false;
  if (!bits.read_bit(sps.pic_struct_present_flag) ||
      !bits.read_bit(sps.bitstream_restriction_flag)) {
    return false;
  }
  if (sps.bitstream_restriction_flag) {
    if (!bits.read_bit(present) || !bits.read_ue(value) || !bits.read_ue(value) ||
        !bits.read_ue(value) || !bits.read_ue(value))
      return false;
    uint32_t reorder = 0U;
    if (!bits.read_ue(reorder) || !bits.read_ue(value))
      return false;
    sps.max_num_reorder_frames = reorder;
  }
  return true;
}

[[nodiscard]] bool is_high_profile(const uint8_t profile) noexcept {
  constexpr uint8_t profiles[] = {100U, 110U, 122U, 244U, 44U,  83U, 86U,
                                  118U, 128U, 138U, 139U, 134U, 135U};
  return std::find(std::begin(profiles), std::end(profiles), profile) != std::end(profiles);
}

} // namespace

uint8_t h264_nal_type(const NalUnit& nal) noexcept {
  return nal.empty() ? 0U : static_cast<uint8_t>(nal.front() & 0x1FU);
}

std::optional<H264SpsInfo> parse_h264_sps(const NalUnit& nal) {
  const auto rbsp = rbsp_from_nal(nal, kH264NalSps);
  if (!rbsp)
    return std::nullopt;
  BitReader bits(*rbsp);
  H264SpsInfo sps;
  uint32_t value = 0U;
  if (!bits.read_bits(8U, value))
    return std::nullopt;
  sps.profile_idc = static_cast<uint8_t>(value);
  if (!bits.read_bits(8U, value) || !bits.read_bits(8U, value))
    return std::nullopt;
  sps.level_idc = static_cast<uint8_t>(value);
  if (!bits.read_ue(sps.sps_id))
    return std::nullopt;
  if (is_high_profile(sps.profile_idc)) {
    if (!bits.read_ue(sps.chroma_format_idc) || sps.chroma_format_idc > 3U)
      return std::nullopt;
    bool flag = false;
    if (sps.chroma_format_idc == 3U && !bits.read_bit(flag))
      return std::nullopt;
    uint32_t depth = 0U;
    if (!bits.read_ue(depth) || depth > 6U)
      return std::nullopt;
    sps.bit_depth_luma = depth + 8U;
    if (!bits.read_ue(depth) || depth > 6U)
      return std::nullopt;
    sps.bit_depth_chroma = depth + 8U;
    if (!bits.read_bit(flag) || !bits.read_bit(flag))
      return std::nullopt;
    if (flag) {
      const unsigned count = sps.chroma_format_idc == 3U ? 12U : 8U;
      for (unsigned index = 0U; index < count; ++index) {
        bool present = false;
        if (!bits.read_bit(present))
          return std::nullopt;
        if (present && !skip_scaling_list(bits, index < 6U ? 16U : 64U))
          return std::nullopt;
      }
    }
  }
  if (!bits.read_ue(value))
    return std::nullopt; // log2_max_frame_num_minus4
  uint32_t pic_order_cnt_type = 0U;
  if (!bits.read_ue(pic_order_cnt_type))
    return std::nullopt;
  if (pic_order_cnt_type == 0U) {
    if (!bits.read_ue(value))
      return std::nullopt;
  } else if (pic_order_cnt_type == 1U) {
    bool flag = false;
    int32_t signed_value = 0;
    uint32_t count = 0U;
    if (!bits.read_bit(flag) || !bits.read_se(signed_value) || !bits.read_se(signed_value) ||
        !bits.read_ue(count) || count > 255U)
      return std::nullopt;
    for (uint32_t index = 0U; index < count; ++index) {
      if (!bits.read_se(signed_value))
        return std::nullopt;
    }
  } else if (pic_order_cnt_type > 2U) {
    return std::nullopt;
  }
  bool flag = false;
  uint32_t width_mbs_minus1 = 0U;
  uint32_t height_map_units_minus1 = 0U;
  if (!bits.read_ue(value) || !bits.read_bit(flag) || !bits.read_ue(width_mbs_minus1) ||
      !bits.read_ue(height_map_units_minus1) || !bits.read_bit(sps.frame_mbs_only_flag)) {
    return std::nullopt;
  }
  if (!sps.frame_mbs_only_flag && !bits.read_bit(flag))
    return std::nullopt;
  if (!bits.read_bit(flag))
    return std::nullopt;
  bool crop = false;
  if (!bits.read_bit(crop))
    return std::nullopt;
  uint32_t crop_left = 0U, crop_right = 0U, crop_top = 0U, crop_bottom = 0U;
  if (crop && (!bits.read_ue(crop_left) || !bits.read_ue(crop_right) || !bits.read_ue(crop_top) ||
               !bits.read_ue(crop_bottom)))
    return std::nullopt;
  const uint32_t width = (width_mbs_minus1 + 1U) * 16U;
  const uint32_t height =
      (height_map_units_minus1 + 1U) * 16U * (sps.frame_mbs_only_flag ? 1U : 2U);
  const uint32_t crop_unit_x = sps.chroma_format_idc == 0U || sps.chroma_format_idc == 3U ? 1U : 2U;
  const uint32_t crop_unit_y =
      (sps.chroma_format_idc == 1U ? 2U : 1U) * (sps.frame_mbs_only_flag ? 1U : 2U);
  if ((crop_left + crop_right) * crop_unit_x > width ||
      (crop_top + crop_bottom) * crop_unit_y > height)
    return std::nullopt;
  sps.width = width - (crop_left + crop_right) * crop_unit_x;
  sps.height = height - (crop_top + crop_bottom) * crop_unit_y;
  if (!bits.read_bit(sps.vui_parameters_present_flag))
    return std::nullopt;
  if (sps.vui_parameters_present_flag && !parse_vui(bits, sps))
    return std::nullopt;
  return sps;
}

std::optional<H264PpsInfo> parse_h264_pps(const NalUnit& nal) {
  const auto rbsp = rbsp_from_nal(nal, kH264NalPps);
  if (!rbsp)
    return std::nullopt;
  BitReader bits(*rbsp);
  H264PpsInfo pps;
  if (!bits.read_ue(pps.pps_id) || !bits.read_ue(pps.sps_id))
    return std::nullopt;
  return pps;
}

std::optional<std::vector<H264SeiMessage>> parse_h264_sei(const NalUnit& nal) {
  const auto rbsp = rbsp_from_nal(nal, kH264NalSei);
  if (!rbsp)
    return std::nullopt;
  std::vector<H264SeiMessage> messages;
  size_t offset = 0U;
  while (offset < rbsp->size()) {
    if ((*rbsp)[offset] == 0x80U && offset + 1U == rbsp->size())
      break;
    uint64_t payload_type = 0U;
    while (offset < rbsp->size() && (*rbsp)[offset] == 0xFFU) {
      payload_type += 255U;
      ++offset;
    }
    if (offset >= rbsp->size())
      return std::nullopt;
    payload_type += (*rbsp)[offset++];
    uint64_t payload_size = 0U;
    while (offset < rbsp->size() && (*rbsp)[offset] == 0xFFU) {
      payload_size += 255U;
      ++offset;
    }
    if (offset >= rbsp->size())
      return std::nullopt;
    payload_size += (*rbsp)[offset++];
    if (payload_type > std::numeric_limits<uint32_t>::max() || payload_size > rbsp->size() - offset)
      return std::nullopt;
    H264SeiMessage message;
    message.payload_type = static_cast<uint32_t>(payload_type);
    message.payload.assign(rbsp->begin() + static_cast<std::ptrdiff_t>(offset),
                           rbsp->begin() + static_cast<std::ptrdiff_t>(offset + payload_size));
    messages.push_back(std::move(message));
    offset += static_cast<size_t>(payload_size);
  }
  return messages;
}

bool h264_sei_contains(const std::vector<H264SeiMessage>& messages,
                       const uint32_t payload_type) noexcept {
  return std::any_of(messages.begin(), messages.end(), [payload_type](const auto& message) {
    return message.payload_type == payload_type;
  });
}

std::vector<std::string> validate_ipmx_sps(const H264SpsInfo& sps,
                                           const H264StreamFormat& expected) {
  std::vector<std::string> errors;
  if (sps.profile_idc != 77U && sps.profile_idc != 100U)
    errors.emplace_back("profile is not Main or High");
  if (sps.chroma_format_idc != 1U)
    errors.emplace_back("chroma format is not 4:2:0");
  if (sps.bit_depth_luma != 8U || sps.bit_depth_chroma != 8U)
    errors.emplace_back("sample depth is not 8-bit");
  if (sps.width != expected.width || sps.height != expected.height)
    errors.emplace_back("coded dimensions do not match");
  if (!sps.frame_mbs_only_flag)
    errors.emplace_back("stream is not progressive frame-coded");
  if (!sps.vui_parameters_present_flag)
    errors.emplace_back("VUI is absent");
  if (!sps.video_signal_type_present_flag)
    errors.emplace_back("video_signal_type is absent");
  if (sps.video_full_range_flag)
    errors.emplace_back("video range is not narrow");
  if (!sps.colour_description_present_flag)
    errors.emplace_back("colour description is absent");
  if (sps.colour_primaries != 1U || sps.transfer_characteristics != 1U ||
      sps.matrix_coefficients != 1U)
    errors.emplace_back("colourimetry is not BT.709");
  if (!sps.timing_info_present_flag)
    errors.emplace_back("timing info is absent");
  if (sps.num_units_in_tick != expected.fps_denominator ||
      static_cast<uint64_t>(sps.time_scale) != static_cast<uint64_t>(expected.fps_numerator) * 2U)
    errors.emplace_back("VUI timing does not match frame rate");
  if (!sps.nal_hrd_parameters_present_flag || !sps.nal_hrd) {
    errors.emplace_back("NAL HRD is absent");
  } else {
    if (sps.nal_hrd->cpb_cnt_minus1 != 0U)
      errors.emplace_back("HRD has more than one CPB");
  }
  if (sps.max_num_reorder_frames && *sps.max_num_reorder_frames != 0U) {
    errors.emplace_back("pictures may be reordered");
  }
  return errors;
}

std::vector<std::string> validate_ipmx_vbr_sender_sps(const H264SpsInfo& sps,
                                                      const H264StreamFormat& expected) {
  auto errors = validate_ipmx_sps(sps, expected);
  if (sps.nal_hrd && sps.nal_hrd->cbr_flag)
    errors.emplace_back("HRD signals CBR instead of VBR");
  return errors;
}

} // namespace v0
} // namespace ipmx
