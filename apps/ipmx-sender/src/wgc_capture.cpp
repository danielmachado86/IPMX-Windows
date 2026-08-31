#include "ipmx/sender/frame_source.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace ipmx::sender {
namespace {

using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

class PrimaryMonitorSource final : public FrameSource {
public:
  PrimaryMonitorSource(const uint32_t fps_numerator, const uint32_t fps_denominator) {
    if (fps_numerator == 0U || fps_denominator == 0U) {
      throw std::invalid_argument("invalid WGC frame rate");
    }
    frame_period_ = std::chrono::nanoseconds(
        std::max<uint64_t>(1U, 1'000'000'000ULL * fps_denominator / fps_numerator));
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (!GraphicsCaptureSession::IsSupported()) {
      throw std::runtime_error("Windows Graphics Capture is not supported on this system");
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL level{};
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0U,
                                       D3D11_SDK_VERSION, device_.put(), &level, context_.put());
    if (FAILED(result)) {
      result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0U, D3D11_SDK_VERSION,
                                 device_.put(), &level, context_.put());
    }
    winrt::check_hresult(result);

    const auto dxgi_device = device_.as<IDXGIDevice>();
    winrt::com_ptr<IInspectable> inspectable_device;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(),
                                                              inspectable_device.put()));
    direct3d_device_ = inspectable_device.as<IDirect3DDevice>();

    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<GraphicsCaptureItem>(),
                                                   winrt::put_abi(item_)));
    const auto size = item_.Size();
    width_ = static_cast<uint32_t>(size.Width) & ~1U;
    height_ = static_cast<uint32_t>(size.Height) & ~1U;
    if (width_ == 0U || height_ == 0U) {
      throw std::runtime_error("primary monitor has invalid capture dimensions");
    }

    frame_pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
        direct3d_device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    session_ = frame_pool_.CreateCaptureSession(item_);
    token_ = frame_pool_.FrameArrived([this](auto&& pool, auto&&) { on_frame(pool); });
    session_.IsCursorCaptureEnabled(false);
    session_.StartCapture();
  }

  ~PrimaryMonitorSource() override {
    try {
      frame_pool_.FrameArrived(token_);
      session_.Close();
      frame_pool_.Close();
    } catch (...) {
    }
  }

  [[nodiscard]] uint32_t width() const noexcept override { return width_; }
  [[nodiscard]] uint32_t height() const noexcept override { return height_; }

  bool next(BgraFrame& frame) override {
    std::unique_lock lock(mutex_);
    if (!have_frame_ &&
        !ready_.wait_for(lock, std::chrono::seconds(2), [this] { return latest_.has_value(); })) {
      return false;
    }

    if (!schedule_initialized_) {
      next_deadline_ = std::chrono::steady_clock::now();
      schedule_initialized_ = true;
    }
    while (std::chrono::steady_clock::now() < next_deadline_) {
      ready_.wait_until(lock, next_deadline_);
    }

    if (latest_) {
      frame = std::move(*latest_);
      latest_.reset();
      have_frame_ = true;
    } else {
      frame = BgraFrame{};
      frame.width = width_;
      frame.height = height_;
      frame.stride = width_ * 4U;
      frame.capture_time_ns = steady_now_ns();
      frame.repeated = true;
    }

    const auto now = std::chrono::steady_clock::now();
    do {
      next_deadline_ += frame_period_;
    } while (next_deadline_ <= now);
    return true;
  }

private:
  void on_frame(const Direct3D11CaptureFramePool& pool) noexcept {
    bool staging_mapped = false;
    try {
      const auto frame = pool.TryGetNextFrame();
      if (!frame) {
        return;
      }
      const auto texture_access = frame.Surface().as<
          ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
      winrt::com_ptr<ID3D11Texture2D> texture;
      winrt::check_hresult(texture_access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
      D3D11_TEXTURE2D_DESC description{};
      texture->GetDesc(&description);

      if (!staging_ || staging_width_ != description.Width || staging_height_ != description.Height) {
        description.BindFlags = 0U;
        description.MiscFlags = 0U;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> replacement;
        winrt::check_hresult(device_->CreateTexture2D(&description, nullptr, replacement.put()));
        staging_ = std::move(replacement);
        staging_width_ = description.Width;
        staging_height_ = description.Height;
      }
      context_->CopyResource(staging_.get(), texture.get());
      D3D11_MAPPED_SUBRESOURCE mapped{};
      winrt::check_hresult(context_->Map(staging_.get(), 0U, D3D11_MAP_READ, 0U, &mapped));
      staging_mapped = true;

      BgraFrame output;
      output.width = std::min(width_, description.Width & ~1U);
      output.height = std::min(height_, description.Height & ~1U);
      output.stride = output.width * 4U;
      const auto relative_time = frame.SystemRelativeTime();
      output.capture_time_ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(relative_time).count());
      output.pixels.resize(static_cast<size_t>(output.stride) * output.height);
      for (uint32_t row = 0; row < output.height; ++row) {
        std::memcpy(output.pixels.data() + static_cast<size_t>(row) * output.stride,
                    static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch,
                    output.stride);
      }
      context_->Unmap(staging_.get(), 0U);
      staging_mapped = false;

      {
        std::lock_guard lock(mutex_);
        latest_ = std::move(output); // Drop stale frames rather than queueing latency.
      }
      ready_.notify_one();
    } catch (...) {
      if (staging_mapped) {
        context_->Unmap(staging_.get(), 0U);
      }
    }
  }

  uint32_t width_{};
  uint32_t height_{};
  uint32_t staging_width_{};
  uint32_t staging_height_{};
  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11DeviceContext> context_;
  winrt::com_ptr<ID3D11Texture2D> staging_;
  IDirect3DDevice direct3d_device_{nullptr};
  GraphicsCaptureItem item_{nullptr};
  Direct3D11CaptureFramePool frame_pool_{nullptr};
  GraphicsCaptureSession session_{nullptr};
  winrt::event_token token_{};
  std::mutex mutex_;
  std::condition_variable ready_;
  std::optional<BgraFrame> latest_;
  std::chrono::nanoseconds frame_period_{};
  std::chrono::steady_clock::time_point next_deadline_{};
  bool schedule_initialized_{};
  bool have_frame_{};
};

} // namespace

std::unique_ptr<FrameSource> make_primary_monitor_source(const uint32_t fps_numerator,
                                                         const uint32_t fps_denominator) {
  return std::make_unique<PrimaryMonitorSource>(fps_numerator, fps_denominator);
}

} // namespace ipmx::sender
