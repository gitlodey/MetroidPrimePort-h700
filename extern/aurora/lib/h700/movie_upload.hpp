#pragma once

#include <cstdint>
#include <string_view>

namespace aurora::h700 {

inline constexpr uint8_t MoviePlaneFlag = 0x40;

constexpr bool movie_client_upload_enabled(const char* value, const char* bench) noexcept {
  return value && std::string_view(value) == "1" && bench &&
         (std::string_view(bench) == "1" || std::string_view(bench) == "menu");
}

constexpr uint8_t movie_texture_metadata(uint8_t flags) noexcept {
  return flags & (MoviePlaneFlag | 1u);
}

constexpr uint8_t apply_movie_texture_metadata(uint8_t previous, uint8_t metadata) noexcept {
  return (previous & ~(0x80u | MoviePlaneFlag | 1u)) | movie_texture_metadata(metadata);
}

constexpr bool movie_client_upload_eligible(bool enabled, uint8_t flags, bool intensity8,
                                             uint32_t mips) noexcept {
  return enabled && (flags & MoviePlaneFlag) != 0 && intensity8 && mips == 1;
}

} // namespace aurora::h700
