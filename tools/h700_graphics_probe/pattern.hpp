#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>

namespace h700_probe {
inline constexpr uint32_t kSize = 64;
inline constexpr uint32_t kRowBytes = kSize * 4;
using Pixel = std::array<uint8_t, 4>;

// Coordinates here are WebGPU readback rows, not physical panel coordinates.
constexpr Pixel expected_pixel(uint32_t x, uint32_t y, uint32_t frame) {
  if (x < 8 && y < 8) {
    return (frame & 1) ? Pixel{255, 0, 255, 255} : Pixel{255, 255, 255, 255};
  }
  if (y < kSize / 2) {
    return x < kSize / 2 ? Pixel{255, 0, 0, 255} : Pixel{0, 255, 0, 255};
  }
  return x < kSize / 2 ? Pixel{0, 0, 255, 255} : Pixel{255, 255, 0, 255};
}

inline bool pixel_matches(const Pixel& actual, const Pixel& expected) {
  for (size_t i = 0; i < actual.size(); ++i) {
    if (std::abs(int(actual[i]) - int(expected[i])) > 1) {
      return false;
    }
  }
  return true;
}

inline constexpr char kShader[] = R"wgsl(
@group(0) @binding(0) var<uniform> settings: vec4<u32>;

@vertex fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
  let positions = array<vec2<f32>, 3>(vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
  return vec4(positions[index], 0.0, 1.0);
}

@fragment fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
  if position.x < 8.0 && position.y < 8.0 {
    return select(vec4(1.0, 1.0, 1.0, 1.0), vec4(1.0, 0.0, 1.0, 1.0), (settings.x & 1u) != 0u);
  }
  if position.y < 32.0 {
    return select(vec4(0.0, 1.0, 0.0, 1.0), vec4(1.0, 0.0, 0.0, 1.0), position.x < 32.0);
  }
  return select(vec4(1.0, 1.0, 0.0, 1.0), vec4(0.0, 0.0, 1.0, 1.0), position.x < 32.0);
}
)wgsl";
} // namespace h700_probe
