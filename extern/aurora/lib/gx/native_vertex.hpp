#pragma once

#include "gx.hpp"

namespace aurora::gx {
// Raw integer inputs preserve the FIFO's BE bits, including F32 payloads; WGSL decodes
// them exactly as the texture path does. Both layouts fill their last stride completely.
inline std::array<wgpu::VertexAttribute, 2> native_vertex_attributes(u8 stride) noexcept {
  return {{{.format = wgpu::VertexFormat::Uint32x3, .offset = 0, .shaderLocation = 0},
           {.format = stride == 16 ? wgpu::VertexFormat::Uint32 : wgpu::VertexFormat::Uint32x2,
            .offset = 12, .shaderLocation = 1}}};
}

inline wgpu::VertexBufferLayout native_vertex_layout(u8 stride,
                                                     const std::array<wgpu::VertexAttribute, 2>& attributes) noexcept {
  return {.stepMode = wgpu::VertexStepMode::Vertex,
          .arrayStride = stride,
          .attributeCount = attributes.size(),
          .attributes = attributes.data()};
}

inline bool native_vertex_limits_valid(u8 stride, const wgpu::Limits& limits) noexcept {
  // Uint32[xN] requires 4-byte alignment. The only offsets are 0 and 12; the
  // attribute ends are 12 and stride (16 or 20), within a whole 4-aligned stride.
  return (stride == 16 || stride == 20) && limits.maxVertexBuffers >= 1 && limits.maxVertexAttributes >= 2 &&
         limits.maxVertexBufferArrayStride >= stride;
}

inline bool native_vertex_binding_valid(gfx::Range range, u32 vertexCount, u32 instanceCount, u8 stride,
                                         uint64_t bufferSize, const wgpu::Limits& limits) noexcept {
  return native_vertex_limits_valid(stride, limits) && instanceCount == 1 && vertexCount != 0 &&
         range.offset != UINT32_MAX && range.offset % 4 == 0 &&
         uint64_t{range.size} == uint64_t{vertexCount} * stride &&
         uint64_t{range.offset} + range.size <= bufferSize;
}
} // namespace aurora::gx
