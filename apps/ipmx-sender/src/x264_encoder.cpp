#include "ipmx/sender/x264_encoder.hpp"
#include "ipmx/h264.hpp"

#include <x264.h>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace ipmx::sender {

struct X264Encoder::State {
  x264_t* encoder{};
  EncoderSettings settings{};
  uint64_t missing_picture_timing_count{};
  uint64_t incomplete_recovery_point_count{};
  int64_t previous_pts{-1};
  bool warned_picture_timing{};
  bool warned_recovery_point{};

  ~State() {
    if (encoder) {
      x264_encoder_close(encoder);
    }
  }
};

namespace {

[[nodiscard]] std::vector<NalUnit> collect_nals(x264_nal_t* nals, const int count) {
  std::vector<NalUnit> result;
  for (int index = 0; index < count; ++index) {
    const auto parsed = parse_annex_b(std::span<const uint8_t>(
        nals[index].p_payload, static_cast<size_t>(nals[index].i_payload)));
    result.insert(result.end(), parsed.begin(), parsed.end());
  }
  return result;
}

} // namespace

X264Encoder::X264Encoder(const EncoderSettings& settings) : state_(std::make_unique<State>()) {
  if (settings.width == 0U || settings.height == 0U || (settings.width & 1U) != 0U ||
      (settings.height & 1U) != 0U || settings.fps_numerator == 0U ||
      settings.fps_denominator == 0U) {
    throw std::invalid_argument("invalid x264 encoder settings");
  }
  state_->settings = settings;

  x264_param_t parameters{};
  if (x264_param_default_preset(&parameters, "veryfast", "zerolatency") != 0) {
    throw std::runtime_error("x264_param_default_preset failed");
  }
  parameters.i_threads = 2;
  parameters.i_width = static_cast<int>(settings.width);
  parameters.i_height = static_cast<int>(settings.height);
  parameters.i_csp = X264_CSP_NV12;
  parameters.i_bitdepth = 8;
  parameters.i_fps_num = settings.fps_numerator;
  parameters.i_fps_den = settings.fps_denominator;
  parameters.i_timebase_num = settings.fps_denominator;
  parameters.i_timebase_den = settings.fps_numerator;
  parameters.b_vfr_input = 0;
  parameters.i_bframe = 0;
  parameters.i_bframe_adaptive = 0;
  parameters.i_bframe_pyramid = 0;
  parameters.b_open_gop = 0;
  parameters.b_intra_refresh = 0;
  parameters.i_keyint_max =
      std::max(1, static_cast<int>(settings.fps_numerator / settings.fps_denominator));
  parameters.i_keyint_min = 1;
  parameters.b_repeat_headers = 1;
  parameters.b_annexb = 1;
  parameters.rc.i_rc_method = X264_RC_ABR;
  parameters.rc.i_bitrate = static_cast<int>(settings.target_bitrate_kbps);
  parameters.rc.i_vbv_max_bitrate = static_cast<int>(settings.target_bitrate_kbps);
  parameters.rc.i_vbv_buffer_size =
      std::max(1, static_cast<int>((static_cast<uint64_t>(settings.target_bitrate_kbps) *
                                        settings.fps_denominator +
                                    settings.fps_numerator - 1U) /
                                   settings.fps_numerator));
  parameters.i_nal_hrd = X264_NAL_HRD_VBR;
  parameters.vui.i_sar_width = 1;
  parameters.vui.i_sar_height = 1;
  parameters.vui.i_vidformat = 5; // Unspecified source format; colour description is explicit.
  parameters.vui.b_fullrange = 0;
  parameters.vui.i_colorprim = 1; // BT.709.
  parameters.vui.i_transfer = 1;  // BT.709.
  parameters.vui.i_colmatrix = 1; // BT.709 non-constant luminance.
  parameters.b_pic_struct = 1;
  const char* profile = settings.profile == H264Profile::main ? "main" : "high";
  if (x264_param_apply_profile(&parameters, profile) != 0) {
    throw std::runtime_error("requested x264 Main/High 8-bit profile is unavailable");
  }
  state_->encoder = x264_encoder_open(&parameters);
  if (state_->encoder == nullptr) {
    throw std::runtime_error("x264_encoder_open failed");
  }

  x264_nal_t* headers = nullptr;
  int header_count = 0;
  if (x264_encoder_headers(state_->encoder, &headers, &header_count) < 0) {
    throw std::runtime_error("x264_encoder_headers failed");
  }
  for (auto& nal : collect_nals(headers, header_count)) {
    if (!nal.empty() && (nal.front() & 0x1FU) == 7U) {
      sps_ = nal;
    } else if (!nal.empty() && (nal.front() & 0x1FU) == 8U) {
      pps_ = nal;
    }
  }
  if (sps_.empty() || pps_.empty()) {
    throw std::runtime_error("x264 did not provide SPS/PPS headers");
  }
  const auto parsed_sps = parse_h264_sps(sps_);
  const auto parsed_pps = parse_h264_pps(pps_);
  if (!parsed_sps || !parsed_pps) {
    throw std::runtime_error("x264 produced invalid SPS/PPS headers");
  }
  const auto errors = validate_ipmx_vbr_sender_sps(
      *parsed_sps,
      {settings.width, settings.height, settings.fps_numerator, settings.fps_denominator});
  if (!errors.empty()) {
    std::ostringstream message;
    message << "x264 SPS does not conform to the IPMX H.264 profile";
    for (const auto& error : errors)
      message << "; " << error;
    throw std::runtime_error(message.str());
  }
}

X264Encoder::~X264Encoder() = default;

uint64_t X264Encoder::missing_picture_timing_count() const noexcept {
  return state_->missing_picture_timing_count;
}

uint64_t X264Encoder::incomplete_recovery_point_count() const noexcept {
  return state_->incomplete_recovery_point_count;
}

EncodedAccessUnit X264Encoder::encode(const Nv12Frame& frame, const int64_t pts) {
  if (frame.width != state_->settings.width || frame.height != state_->settings.height) {
    throw std::invalid_argument("NV12 frame dimensions changed during encoding");
  }
  x264_picture_t input{};
  x264_picture_init(&input);
  input.img.i_csp = X264_CSP_NV12;
  input.img.i_plane = 2;
  input.img.plane[0] = const_cast<uint8_t*>(frame.y_plane());
  input.img.plane[1] = const_cast<uint8_t*>(frame.uv_plane());
  input.img.i_stride[0] = static_cast<int>(frame.y_stride);
  input.img.i_stride[1] = static_cast<int>(frame.uv_stride);
  input.i_pts = pts;
  if (state_->previous_pts >= 0 && pts != state_->previous_pts + 1)
    input.i_type = X264_TYPE_IDR;
  state_->previous_pts = pts;
  input.i_pic_struct = PIC_STRUCT_PROGRESSIVE;

  x264_picture_t output{};
  x264_nal_t* nals = nullptr;
  int nal_count = 0;
  const int bytes = x264_encoder_encode(state_->encoder, &nals, &nal_count, &input, &output);
  if (bytes < 0) {
    throw std::runtime_error("x264_encoder_encode failed");
  }
  EncodedAccessUnit access_unit;
  access_unit.pts = output.i_pts;
  access_unit.dts = output.i_dts;
  access_unit.keyframe = output.b_keyframe != 0;
  access_unit.nals = collect_nals(nals, nal_count);
  bool has_sps = false;
  bool has_pps = false;
  bool has_idr = false;
  bool has_buffering_period = false;
  bool has_picture_timing = false;
  for (const auto& nal : access_unit.nals) {
    switch (h264_nal_type(nal)) {
    case kH264NalSps:
      has_sps = true;
      break;
    case kH264NalPps:
      has_pps = true;
      break;
    case kH264NalIdr:
      has_idr = true;
      break;
    case kH264NalSei:
      if (const auto messages = parse_h264_sei(nal)) {
        has_buffering_period =
            has_buffering_period || h264_sei_contains(*messages, kH264SeiBufferingPeriod);
        has_picture_timing =
            has_picture_timing || h264_sei_contains(*messages, kH264SeiPictureTiming);
      }
      break;
    default:
      break;
    }
  }
  access_unit.picture_timing_present = has_picture_timing;
  access_unit.recovery_point_complete =
      !access_unit.keyframe || (has_sps && has_pps && has_idr && has_buffering_period);
  if (!access_unit.nals.empty() && !has_picture_timing) {
    ++state_->missing_picture_timing_count;
    if (!state_->warned_picture_timing) {
      std::cerr << "warning: x264 access unit is missing Picture Timing SEI; continuing and "
                   "counting violations\n";
      state_->warned_picture_timing = true;
    }
  }
  if (!access_unit.recovery_point_complete) {
    ++state_->incomplete_recovery_point_count;
    if (!state_->warned_recovery_point) {
      std::cerr << "warning: x264 recovery point is incomplete; continuing and counting "
                   "violations\n";
      state_->warned_recovery_point = true;
    }
  }
  return access_unit;
}

} // namespace ipmx::sender
