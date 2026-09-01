#pragma once

#include "ipmx/annexb.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ipmx {
inline namespace v0 {

inline constexpr uint8_t kH264NalSlice = 1U;
inline constexpr uint8_t kH264NalIdr = 5U;
inline constexpr uint8_t kH264NalSei = 6U;
inline constexpr uint8_t kH264NalSps = 7U;
inline constexpr uint8_t kH264NalPps = 8U;
inline constexpr uint32_t kH264SeiBufferingPeriod = 0U;
inline constexpr uint32_t kH264SeiPictureTiming = 1U;

struct H264HrdInfo {
  uint32_t cpb_cnt_minus1{};
  bool cbr_flag{};
};

struct H264SpsInfo {
  uint8_t profile_idc{};
  uint8_t level_idc{};
  uint32_t sps_id{};
  uint32_t chroma_format_idc{1U};
  uint32_t bit_depth_luma{8U};
  uint32_t bit_depth_chroma{8U};
  uint32_t width{};
  uint32_t height{};
  bool frame_mbs_only_flag{};
  bool vui_parameters_present_flag{};
  bool video_signal_type_present_flag{};
  bool video_full_range_flag{};
  bool colour_description_present_flag{};
  uint8_t colour_primaries{2U};
  uint8_t transfer_characteristics{2U};
  uint8_t matrix_coefficients{2U};
  bool timing_info_present_flag{};
  uint32_t num_units_in_tick{};
  uint32_t time_scale{};
  bool fixed_frame_rate_flag{};
  bool nal_hrd_parameters_present_flag{};
  std::optional<H264HrdInfo> nal_hrd;
  bool pic_struct_present_flag{};
  bool bitstream_restriction_flag{};
  std::optional<uint32_t> max_num_reorder_frames;
};

struct H264PpsInfo {
  uint32_t pps_id{};
  uint32_t sps_id{};
};

struct H264SeiMessage {
  uint32_t payload_type{};
  std::vector<uint8_t> payload;
};

struct H264StreamFormat {
  uint32_t width{};
  uint32_t height{};
  uint32_t fps_numerator{};
  uint32_t fps_denominator{};
};

[[nodiscard]] uint8_t h264_nal_type(const NalUnit& nal) noexcept;
[[nodiscard]] std::optional<H264SpsInfo> parse_h264_sps(const NalUnit& nal);
[[nodiscard]] std::optional<H264PpsInfo> parse_h264_pps(const NalUnit& nal);
[[nodiscard]] std::optional<std::vector<H264SeiMessage>> parse_h264_sei(const NalUnit& nal);
[[nodiscard]] bool h264_sei_contains(const std::vector<H264SeiMessage>& messages,
                                     uint32_t payload_type) noexcept;
[[nodiscard]] std::vector<std::string>
validate_ipmx_sps(const H264SpsInfo& sps, const H264StreamFormat& expected);

} // namespace v0
} // namespace ipmx
