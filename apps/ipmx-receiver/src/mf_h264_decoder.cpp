#include "ipmx/receiver/mf_h264_decoder.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace phase0 {

using Microsoft::WRL::ComPtr;

struct MfH264Decoder::State {
  ComPtr<IMFTransform> decoder;
  uint32_t width{};
  uint32_t height{};
  uint32_t fps_numerator{};
  uint32_t fps_denominator{};
  uint64_t sample_index{};
  std::deque<uint64_t> capture_times;
};

namespace {

void check(const HRESULT result, const char* operation) {
  if (FAILED(result)) {
    throw std::runtime_error(std::string(operation) + " failed, HRESULT=" +
                             std::to_string(static_cast<unsigned long>(result)));
  }
}

[[nodiscard]] ComPtr<IMFMediaType> make_video_type(const GUID& subtype, const uint32_t width,
                                                   const uint32_t height, const uint32_t fps_numerator,
                                                   const uint32_t fps_denominator) {
  ComPtr<IMFMediaType> type;
  check(MFCreateMediaType(&type), "MFCreateMediaType");
  check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set major type");
  check(type->SetGUID(MF_MT_SUBTYPE, subtype), "Set subtype");
  check(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height), "Set frame size");
  check(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps_numerator, fps_denominator),
        "Set frame rate");
  check(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1U, 1U), "Set pixel aspect ratio");
  check(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set interlace mode");
  return type;
}

template <typename StateType>
void configure_output(StateType& state) {
  for (DWORD index = 0U;; ++index) {
    ComPtr<IMFMediaType> candidate;
    const HRESULT result = state.decoder->GetOutputAvailableType(0U, index, &candidate);
    if (result == MF_E_NO_MORE_TYPES) {
      break;
    }
    check(result, "GetOutputAvailableType");
    GUID subtype{};
    if (SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) {
      check(MFSetAttributeSize(candidate.Get(), MF_MT_FRAME_SIZE, state.width, state.height),
            "Set output frame size");
      check(MFSetAttributeRatio(candidate.Get(), MF_MT_FRAME_RATE, state.fps_numerator,
                                state.fps_denominator), "Set output frame rate");
      check(state.decoder->SetOutputType(0U, candidate.Get(), 0U), "SetOutputType NV12");
      return;
    }
  }
  throw std::runtime_error("Media Foundation H.264 decoder has no NV12 output type");
}

[[nodiscard]] ComPtr<IMFSample> allocate_output_sample(const MFT_OUTPUT_STREAM_INFO& info,
                                                       const uint32_t width, const uint32_t height) {
  if ((info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0U) {
    return nullptr;
  }
  ComPtr<IMFSample> sample;
  ComPtr<IMFMediaBuffer> buffer;
  check(MFCreateSample(&sample), "MFCreateSample output");
  const DWORD minimum = width * height * 3U / 2U;
  check(MFCreateMemoryBuffer(std::max(info.cbSize, minimum), &buffer), "MFCreateMemoryBuffer output");
  check(sample->AddBuffer(buffer.Get()), "Add output buffer");
  return sample;
}

[[nodiscard]] DecodedFrame copy_nv12(IMFSample* sample, const uint32_t width, const uint32_t height,
                                     const uint64_t capture_time_ns) {
  ComPtr<IMFMediaBuffer> buffer;
  check(sample->ConvertToContiguousBuffer(&buffer), "ConvertToContiguousBuffer");
  BYTE* bytes = nullptr;
  DWORD length = 0U;
  check(buffer->Lock(&bytes, nullptr, &length), "Lock decoded buffer");
  const size_t expected = static_cast<size_t>(width) * height * 3U / 2U;
  if (length < expected) {
    buffer->Unlock();
    throw std::runtime_error("decoded NV12 buffer is smaller than the configured frame");
  }
  DecodedFrame frame;
  frame.width = width;
  frame.height = height;
  frame.y_stride = width;
  frame.uv_stride = width;
  frame.capture_time_ns = capture_time_ns;
  frame.pixels.assign(bytes, bytes + expected);
  buffer->Unlock();
  return frame;
}

template <typename StateType>
[[nodiscard]] std::vector<DecodedFrame> drain(StateType& state) {
  std::vector<DecodedFrame> frames;
  while (true) {
    MFT_OUTPUT_STREAM_INFO stream_info{};
    check(state.decoder->GetOutputStreamInfo(0U, &stream_info), "GetOutputStreamInfo");
    auto sample = allocate_output_sample(stream_info, state.width, state.height);
    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = 0U;
    output.pSample = sample.Get();
    DWORD status = 0U;
    const HRESULT result = state.decoder->ProcessOutput(0U, 1U, &output, &status);
    if (output.pEvents) {
      output.pEvents->Release();
    }
    if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      break;
    }
    if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
      configure_output(state);
      continue;
    }
    check(result, "H.264 ProcessOutput");
    IMFSample* produced = output.pSample ? output.pSample : sample.Get();
    if (produced == nullptr) {
      throw std::runtime_error("H.264 decoder returned no output sample");
    }
    const uint64_t capture_time = state.capture_times.empty() ? 0U : state.capture_times.front();
    if (!state.capture_times.empty()) {
      state.capture_times.pop_front();
    }
    frames.push_back(copy_nv12(produced, state.width, state.height, capture_time));
    if (output.pSample && output.pSample != sample.Get()) {
      output.pSample->Release();
    }
  }
  return frames;
}

} // namespace

MfH264Decoder::MfH264Decoder(const uint32_t width, const uint32_t height,
                             const uint32_t fps_numerator, const uint32_t fps_denominator)
    : state_(std::make_unique<State>()) {
  if (width == 0U || height == 0U || fps_numerator == 0U || fps_denominator == 0U) {
    throw std::invalid_argument("invalid decoder configuration");
  }
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
    check(apartment, "CoInitializeEx");
  }
  check(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup");
  state_->width = width;
  state_->height = height;
  state_->fps_numerator = fps_numerator;
  state_->fps_denominator = fps_denominator;
  check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&state_->decoder)), "Create H.264 decoder MFT");

  auto input_type = make_video_type(MFVideoFormat_H264, width, height, fps_numerator, fps_denominator);
  check(state_->decoder->SetInputType(0U, input_type.Get(), 0U), "SetInputType H264");
  configure_output(*state_);
  check(state_->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0U), "Begin streaming");
  check(state_->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0U), "Start stream");
}

MfH264Decoder::~MfH264Decoder() {
  if (state_ && state_->decoder) {
    state_->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0U);
    state_->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0U);
  }
  MFShutdown();
}

std::vector<DecodedFrame> MfH264Decoder::decode(const std::span<const uint8_t> annex_b,
                                                const uint64_t capture_time_ns) {
  if (annex_b.empty() || annex_b.size() > std::numeric_limits<DWORD>::max()) {
    return {};
  }

  ComPtr<IMFSample> sample;
  ComPtr<IMFMediaBuffer> buffer;
  check(MFCreateSample(&sample), "MFCreateSample input");
  check(MFCreateMemoryBuffer(static_cast<DWORD>(annex_b.size()), &buffer), "MFCreateMemoryBuffer input");
  BYTE* destination = nullptr;
  check(buffer->Lock(&destination, nullptr, nullptr), "Lock input buffer");
  std::memcpy(destination, annex_b.data(), annex_b.size());
  buffer->Unlock();
  check(buffer->SetCurrentLength(static_cast<DWORD>(annex_b.size())), "Set input length");
  check(sample->AddBuffer(buffer.Get()), "Add input buffer");
  const LONGLONG duration = 10'000'000LL * state_->fps_denominator / state_->fps_numerator;
  check(sample->SetSampleTime(static_cast<LONGLONG>(state_->sample_index) * duration), "Set sample time");
  check(sample->SetSampleDuration(duration), "Set sample duration");

  HRESULT input_result = state_->decoder->ProcessInput(0U, sample.Get(), 0U);
  std::vector<DecodedFrame> frames;
  if (input_result == MF_E_NOTACCEPTING) {
    frames = drain(*state_);
    input_result = state_->decoder->ProcessInput(0U, sample.Get(), 0U);
  }
  check(input_result, "H.264 ProcessInput");
  state_->capture_times.push_back(capture_time_ns);
  ++state_->sample_index;
  auto more = drain(*state_);
  frames.insert(frames.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
  return frames;
}

} // namespace phase0
