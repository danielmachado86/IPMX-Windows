#include "ipmx/annexb.hpp"

namespace ipmx {
inline namespace v0 {
namespace {

struct StartCode {
  size_t offset{};
  size_t length{};
};

[[nodiscard]] bool find_start_code(const std::span<const uint8_t> bytes, const size_t from,
                                   StartCode& result) noexcept {
  for (size_t i = from; i + 2U < bytes.size(); ++i) {
    if (bytes[i] != 0U || bytes[i + 1U] != 0U) {
      continue;
    }
    if (bytes[i + 2U] == 1U) {
      result = {i, 3U};
      return true;
    }
    if (i + 3U < bytes.size() && bytes[i + 2U] == 0U && bytes[i + 3U] == 1U) {
      result = {i, 4U};
      return true;
    }
  }
  return false;
}

} // namespace

std::vector<NalUnit> parse_annex_b(const std::span<const uint8_t> bytes) {
  std::vector<NalUnit> result;
  StartCode current{};
  if (!find_start_code(bytes, 0U, current)) {
    return result;
  }

  while (true) {
    const size_t nal_begin = current.offset + current.length;
    StartCode next{};
    const bool has_next = find_start_code(bytes, nal_begin, next);
    size_t nal_end = has_next ? next.offset : bytes.size();
    while (nal_end > nal_begin && bytes[nal_end - 1U] == 0U) {
      --nal_end;
    }
    if (nal_end > nal_begin) {
      result.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(nal_begin),
                          bytes.begin() + static_cast<std::ptrdiff_t>(nal_end));
    }
    if (!has_next) {
      break;
    }
    current = next;
  }
  return result;
}

std::vector<uint8_t> build_annex_b(const std::vector<NalUnit>& nals) {
  size_t size = 0U;
  for (const auto& nal : nals) {
    size += 4U + nal.size();
  }
  std::vector<uint8_t> result;
  result.reserve(size);
  for (const auto& nal : nals) {
    result.insert(result.end(), {0U, 0U, 0U, 1U});
    result.insert(result.end(), nal.begin(), nal.end());
  }
  return result;
}

} // namespace v0
} // namespace ipmx
