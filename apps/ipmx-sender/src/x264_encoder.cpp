#include "ipmx/sender/x264_encoder.hpp"

#include <x264.h>

#include <stdexcept>

namespace phase0 {

struct X264Encoder::State {
  x264_t* encoder{};
  EncoderSettings settings{};
};

namespace {

[[nodiscard]] std::vector<NalUnit> collect_nals(x264_nal_t* nals, const int count) {
  std::vector<NalUnit> result;
  for (int index = 0; index < count; ++index) {
    const auto parsed = parse_annex_b(std::span<const uint8_t>(nals[index].p_payload,
                                                               static_cast<size_t>(nals[index].i_payload)));
    result.insert(result.end(), parsed.begin(), parsed.end());
  }
  return result;
}

} // namespace

X264Encoder::X264Encoder(const EncoderSettings& settings) : state_(std::make_unique<State>()) {
  if (settings.width == 0U || settings.height == 0U || (settings.width & 1U) != 0U ||
      (settings.height & 1U) != 0U || settings.fps_numerator == 0U || settings.fps_denominator == 0U) {
    throw std::invalid_argument("invalid x264 encoder settings");
  }
  state_->settings = settings;

  x264_param_t parameters{};
  if (x264_param_default_preset(&parameters, "veryfast", "zerolatency") != 0) {
    throw std::runtime_error("x264_param_default_preset failed");
  }
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
  parameters.i_keyint_max = static_cast<int>(settings.fps_numerator * 2U / settings.fps_denominator);
  parameters.i_keyint_min = parameters.i_keyint_max;
  parameters.b_repeat_headers = 1;
  parameters.b_annexb = 1;
  parameters.rc.i_rc_method = X264_RC_ABR;
  parameters.rc.i_bitrate = static_cast<int>(settings.target_bitrate_kbps);
  parameters.rc.i_vbv_max_bitrate = static_cast<int>(settings.target_bitrate_kbps);
  parameters.rc.i_vbv_buffer_size = static_cast<int>(settings.target_bitrate_kbps);
  parameters.vui.i_sar_width = 1;
  parameters.vui.i_sar_height = 1;
  if (x264_param_apply_profile(&parameters, "high") != 0) {
    throw std::runtime_error("x264 High Profile is unavailable (an 8-bit build is required)");
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
}

X264Encoder::~X264Encoder() {
  if (state_ && state_->encoder) {
    x264_encoder_close(state_->encoder);
  }
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

  x264_picture_t output{};
  x264_nal_t* nals = nullptr;
  int nal_count = 0;
  const int bytes = x264_encoder_encode(state_->encoder, &nals, &nal_count, &input, &output);
  if (bytes < 0) {
    throw std::runtime_error("x264_encoder_encode failed");
  }
  EncodedAccessUnit access_unit;
  access_unit.pts = output.i_pts;
  access_unit.keyframe = output.b_keyframe != 0;
  access_unit.nals = collect_nals(nals, nal_count);
  return access_unit;
}

} // namespace phase0
