// clang-format off: d3d11.h requires Windows base types.
#include <windows.h>
#include <d3d11.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <wrl/client.h>

namespace {

struct Options {
  uint32_t duration_seconds{90U};
  uint32_t cpu_workers{std::max(1U, std::thread::hardware_concurrency() / 2U)};
  uint32_t cpu_duty_percent{80U};
  bool gpu{};
};

template <typename T>
[[nodiscard]] T parse_number(const std::string_view text, const char* option) {
  unsigned long long value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      value > static_cast<unsigned long long>(std::numeric_limits<T>::max())) {
    throw std::invalid_argument(std::string("invalid value for ") + option);
  }
  return static_cast<T>(value);
}

[[nodiscard]] Options parse_options(const int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view key(argv[index]);
    if (key == "--help") {
      std::cout << "ipmx-load [--duration-seconds N] [--cpu-workers N] "
                   "[--cpu-duty-percent 1..100] [--gpu]\n";
      std::exit(0);
    }
    if (key == "--gpu") {
      options.gpu = true;
      continue;
    }
    if (index + 1 >= argc)
      throw std::invalid_argument("missing value after " + std::string(key));
    const std::string_view value(argv[++index]);
    if (key == "--duration-seconds")
      options.duration_seconds = parse_number<uint32_t>(value, "--duration-seconds");
    else if (key == "--cpu-workers")
      options.cpu_workers = parse_number<uint32_t>(value, "--cpu-workers");
    else if (key == "--cpu-duty-percent")
      options.cpu_duty_percent = parse_number<uint32_t>(value, "--cpu-duty-percent");
    else
      throw std::invalid_argument("unknown option: " + std::string(key));
  }
  if (options.duration_seconds == 0U || options.cpu_workers > 1'024U ||
      options.cpu_duty_percent == 0U || options.cpu_duty_percent > 100U) {
    throw std::invalid_argument("invalid artificial load settings");
  }
  return options;
}

void run_cpu_load(const uint32_t duty_percent, const std::chrono::steady_clock::time_point deadline,
                  std::atomic_uint64_t& checksum) {
  constexpr auto period = std::chrono::milliseconds(10);
  const auto busy_time = period * duty_percent / 100U;
  uint64_t value = 0x9E3779B97F4A7C15ULL ^ GetCurrentThreadId();
  while (std::chrono::steady_clock::now() < deadline) {
    const auto cycle = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - cycle < busy_time) {
      value ^= value << 13U;
      value ^= value >> 7U;
      value ^= value << 17U;
    }
    checksum.fetch_xor(value, std::memory_order_relaxed);
    const auto remaining = period - (std::chrono::steady_clock::now() - cycle);
    if (remaining > std::chrono::steady_clock::duration::zero())
      std::this_thread::sleep_for(remaining);
  }
}

void run_gpu_load(const std::chrono::steady_clock::time_point deadline,
                  std::atomic_uint64_t& checksum) {
  using Microsoft::WRL::ComPtr;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL feature_level{};
  HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0U, nullptr, 0U,
                                     D3D11_SDK_VERSION, &device, &feature_level, &context);
  if (FAILED(result))
    throw std::runtime_error("D3D11 hardware device is unavailable for GPU load");

  D3D11_TEXTURE2D_DESC description{};
  description.Width = 4096U;
  description.Height = 4096U;
  description.MipLevels = 1U;
  description.ArraySize = 1U;
  description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  description.SampleDesc.Count = 1U;
  description.Usage = D3D11_USAGE_DEFAULT;
  description.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> first;
  ComPtr<ID3D11Texture2D> second;
  ComPtr<ID3D11RenderTargetView> target;
  if (FAILED(device->CreateTexture2D(&description, nullptr, &first)) ||
      FAILED(device->CreateTexture2D(&description, nullptr, &second)) ||
      FAILED(device->CreateRenderTargetView(first.Get(), nullptr, &target))) {
    throw std::runtime_error("cannot allocate GPU load resources");
  }
  D3D11_QUERY_DESC query_description{D3D11_QUERY_EVENT, 0U};
  ComPtr<ID3D11Query> completion;
  if (FAILED(device->CreateQuery(&query_description, &completion)))
    throw std::runtime_error("cannot create GPU completion query");

  uint64_t iterations = 0U;
  while (std::chrono::steady_clock::now() < deadline) {
    const float color[4]{static_cast<float>(iterations & 255U) / 255.0F, 0.25F, 0.75F, 1.0F};
    for (unsigned index = 0U; index < 16U; ++index) {
      context->ClearRenderTargetView(target.Get(), color);
      context->CopyResource(second.Get(), first.Get());
      context->CopyResource(first.Get(), second.Get());
    }
    context->End(completion.Get());
    context->Flush();
    while (context->GetData(completion.Get(), nullptr, 0U, 0U) == S_FALSE &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    ++iterations;
  }
  checksum.fetch_xor(iterations, std::memory_order_relaxed);
}

} // namespace

int main(const int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(options.duration_seconds);
    std::atomic_uint64_t checksum{};
    std::vector<std::thread> workers;
    std::exception_ptr gpu_failure;
    workers.reserve(options.cpu_workers + (options.gpu ? 1U : 0U));
    for (uint32_t index = 0U; index < options.cpu_workers; ++index) {
      workers.emplace_back(run_cpu_load, options.cpu_duty_percent, deadline, std::ref(checksum));
    }
    if (options.gpu) {
      workers.emplace_back([&] {
        try {
          run_gpu_load(deadline, checksum);
        } catch (...) {
          gpu_failure = std::current_exception();
        }
      });
    }
    for (auto& worker : workers)
      worker.join();
    if (gpu_failure)
      std::rethrow_exception(gpu_failure);
    std::cout << "Artificial load completed: cpu_workers=" << options.cpu_workers
              << " duty=" << options.cpu_duty_percent << "% gpu=" << (options.gpu ? "on" : "off")
              << " checksum=" << checksum.load() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ipmx-load: " << error.what() << '\n';
    return 1;
  }
}
