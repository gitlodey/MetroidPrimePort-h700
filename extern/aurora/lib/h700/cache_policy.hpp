#pragma once

#include <string_view>

namespace aurora::h700 {

// Firmware Mali is not the Mesa driver covered by Aurora's Linux GL cache blacklist.
// Keep this opt-in until cold/warm launches have exercised program-binary loading on-device.
constexpr bool program_cache_enabled(const char* value, const char* bench) noexcept {
  return value && std::string_view(value) == "1" && bench &&
         (std::string_view(bench) == "1" || std::string_view(bench) == "menu");
}

} // namespace aurora::h700
