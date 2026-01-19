#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>

namespace remy::utils {

inline std::string pretty_size(uint64_t size) {
  static const std::array SIZE_NAMES = {"", "K", "M", "G", "T"};
  size_t div = 0;
  size_t rem = 0;

  while (size >= 1024 && div < SIZE_NAMES.size()) {
    rem = (size % 1024);
    div++;
    size /= 1024;
  }

  return std::format("{:.{}f}{}", size + rem / 1024.f, rem == 0 ? 0 : 1, SIZE_NAMES[div]);
}

inline void trim_end(std::string &str) {
  str.erase(std::find_if(str.rbegin(), str.rend(), [](auto ch) {
    return !std::isspace(ch);
  }).base(), str.end());
}

}  // namespace remy::utils
