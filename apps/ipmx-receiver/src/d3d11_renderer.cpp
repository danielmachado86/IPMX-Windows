#include "ipmx/receiver/d3d11_renderer.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>

namespace ipmx::receiver {

using Microsoft::WRL::ComPtr;

struct D3d11Renderer::State {
  HWND window{};
  uint32_t width{};
  uint32_t height{};
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGISwapChain1> swap_chain;
  ComPtr<ID3D11VideoDevice> video_device;
  ComPtr<ID3D11VideoContext> video_context;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
  ComPtr<ID3D11VideoProcessor> processor;
  ComPtr<ID3D11Texture2D> input_texture;
  ComPtr<ID3D11VideoProcessorInputView> input_view;
  ComPtr<ID3D11VideoProcessorOutputView> output_view;
};

namespace {

void check(const HRESULT result, const char* operation) {
  if (FAILED(result)) {
    throw std::runtime_error(std::string(operation) + " failed, HRESULT=" +
                             std::to_string(static_cast<unsigned long>(result)));
  }
}

LRESULT CALLBACK window_proc(const HWND window, const UINT message, const WPARAM wparam,
                             const LPARAM lparam) {
  if (message == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

D3d11Renderer::D3d11Renderer(const uint32_t width, const uint32_t height)
    : state_(std::make_unique<State>()) {
  state_->width = width;
  state_->height = height;
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW window_class{sizeof(WNDCLASSEXW)};
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  window_class.lpszClassName = L"IPMXWindowsReceiver";
  RegisterClassExW(&window_class);
  RECT rectangle{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
  state_->window = CreateWindowExW(0U, window_class.lpszClassName, L"IPMX Phase 0 Receiver",
                                   WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                   rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
                                   nullptr, nullptr, instance, nullptr);
  if (!state_->window) {
    throw std::runtime_error("CreateWindowExW failed");
  }

  D3D_FEATURE_LEVEL feature_level{};
  HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0U,
                                     D3D11_SDK_VERSION, &state_->device, &feature_level,
                                     &state_->context);
  if (FAILED(result)) {
    result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0U,
                               D3D11_SDK_VERSION, &state_->device, &feature_level,
                               &state_->context);
  }
  check(result, "D3D11CreateDevice");
  check(state_->device.As(&state_->video_device), "ID3D11VideoDevice");
  check(state_->context.As(&state_->video_context), "ID3D11VideoContext");

  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  check(state_->device.As(&dxgi_device), "IDXGIDevice");
  check(dxgi_device->GetAdapter(&adapter), "GetAdapter");
  check(adapter->GetParent(IID_PPV_ARGS(&factory)), "Get DXGI factory");
  DXGI_SWAP_CHAIN_DESC1 swap_description{};
  swap_description.Width = width;
  swap_description.Height = height;
  swap_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  swap_description.SampleDesc.Count = 1U;
  swap_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_description.BufferCount = 2U;
  swap_description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  check(factory->CreateSwapChainForHwnd(state_->device.Get(), state_->window, &swap_description,
                                        nullptr, nullptr, &state_->swap_chain), "CreateSwapChainForHwnd");

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
  content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content.InputWidth = width;
  content.InputHeight = height;
  content.OutputWidth = width;
  content.OutputHeight = height;
  content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  content.InputFrameRate = {60U, 1U};
  content.OutputFrameRate = {60U, 1U};
  check(state_->video_device->CreateVideoProcessorEnumerator(&content, &state_->enumerator),
        "CreateVideoProcessorEnumerator");
  check(state_->video_device->CreateVideoProcessor(state_->enumerator.Get(), 0U,
                                                    &state_->processor), "CreateVideoProcessor");

  D3D11_TEXTURE2D_DESC texture_description{};
  texture_description.Width = width;
  texture_description.Height = height;
  texture_description.MipLevels = 1U;
  texture_description.ArraySize = 1U;
  texture_description.Format = DXGI_FORMAT_NV12;
  texture_description.SampleDesc.Count = 1U;
  texture_description.Usage = D3D11_USAGE_DEFAULT;
  check(state_->device->CreateTexture2D(&texture_description, nullptr, &state_->input_texture),
        "Create NV12 texture");
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_description{};
  input_description.FourCC = 0U;
  input_description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  input_description.Texture2D.MipSlice = 0U;
  input_description.Texture2D.ArraySlice = 0U;
  check(state_->video_device->CreateVideoProcessorInputView(state_->input_texture.Get(),
                                                            state_->enumerator.Get(),
                                                            &input_description, &state_->input_view),
        "CreateVideoProcessorInputView");
  ComPtr<ID3D11Texture2D> back_buffer;
  check(state_->swap_chain->GetBuffer(0U, IID_PPV_ARGS(&back_buffer)), "Get back buffer");
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_description{};
  output_description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  output_description.Texture2D.MipSlice = 0U;
  check(state_->video_device->CreateVideoProcessorOutputView(back_buffer.Get(),
                                                              state_->enumerator.Get(),
                                                              &output_description,
                                                              &state_->output_view),
        "CreateVideoProcessorOutputView");
}

D3d11Renderer::~D3d11Renderer() {
  if (state_ && state_->window) {
    DestroyWindow(state_->window);
  }
}

bool D3d11Renderer::pump_messages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE)) {
    if (message.message == WM_QUIT) {
      return false;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return true;
}

uint64_t D3d11Renderer::present(const DecodedFrame& frame) {
  if (frame.width != state_->width || frame.height != state_->height ||
      frame.pixels.size() < static_cast<size_t>(frame.width) * frame.height * 3U / 2U) {
    throw std::invalid_argument("renderer received an invalid NV12 frame");
  }
  state_->context->UpdateSubresource(state_->input_texture.Get(), 0U, nullptr, frame.pixels.data(),
                                     frame.y_stride, 0U);
  RECT source{0, 0, static_cast<LONG>(frame.width), static_cast<LONG>(frame.height)};
  state_->video_context->VideoProcessorSetStreamSourceRect(state_->processor.Get(), 0U, TRUE, &source);
  state_->video_context->VideoProcessorSetStreamDestRect(state_->processor.Get(), 0U, TRUE, &source);
  state_->video_context->VideoProcessorSetOutputTargetRect(state_->processor.Get(), TRUE, &source);
  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.pInputSurface = state_->input_view.Get();
  check(state_->video_context->VideoProcessorBlt(state_->processor.Get(), state_->output_view.Get(),
                                                  0U, 1U, &stream), "VideoProcessorBlt");
  check(state_->swap_chain->Present(0U, 0U), "Present");
  return steady_now_ns();
}

} // namespace ipmx::receiver
