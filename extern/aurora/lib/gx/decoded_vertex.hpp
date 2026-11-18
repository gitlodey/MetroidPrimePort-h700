#pragma once

#include "gx.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace aurora::gx::decoded {

struct MeshRef {
  uint32_t slot;
  uint32_t generation;

  bool operator==(const MeshRef&) const = default;
};
static_assert(sizeof(MeshRef) == sizeof(gfx::Range));
static_assert(std::is_trivially_copyable_v<MeshRef> && std::is_aggregate_v<MeshRef>);
static_assert(std::is_trivially_default_constructible_v<MeshRef>);

inline constexpr uint32_t MaxAttributes = 13;
inline constexpr uint32_t MaxStride = 132;

struct Selector {
  GXAttr attr;
  uint32_t word;
  uint32_t shift;
};
// Raw bytes, including PNMTXIDX (divided by 3 only in WGSL). Unused lanes are zero.
inline constexpr std::array<Selector, 9> Selectors{{
    {GX_VA_PNMTXIDX, 0, 0},
    {GX_VA_TEX0MTXIDX, 0, 8},
    {GX_VA_TEX1MTXIDX, 0, 16},
    {GX_VA_TEX2MTXIDX, 0, 24},
    {GX_VA_TEX3MTXIDX, 1, 0},
    {GX_VA_TEX4MTXIDX, 1, 8},
    {GX_VA_TEX5MTXIDX, 1, 16},
    {GX_VA_TEX6MTXIDX, 1, 24},
    {GX_VA_TEX7MTXIDX, 2, 0},
}};

constexpr uint16_t selector_bit(GXAttr attr) noexcept {
  for (uint32_t i = 0; i < Selectors.size(); ++i) {
    if (Selectors[i].attr == attr) {
      return uint16_t(1u << i);
    }
  }
  return 0;
}

constexpr uint32_t format_size(wgpu::VertexFormat format) noexcept {
  switch (format) {
  case wgpu::VertexFormat::Float32x2: return 8;
  case wgpu::VertexFormat::Float32x3:
  case wgpu::VertexFormat::Uint32x3: return 12;
  case wgpu::VertexFormat::Float32x4: return 16;
  default: return 0;
  }
}

constexpr std::string_view wgsl_type(wgpu::VertexFormat format) noexcept {
  switch (format) {
  case wgpu::VertexFormat::Float32x2: return "vec2f";
  case wgpu::VertexFormat::Float32x3: return "vec3f";
  case wgpu::VertexFormat::Float32x4: return "vec4f";
  case wgpu::VertexFormat::Uint32x3: return "vec3u";
  default: return {};
  }
}

struct Field {
  // GX_VA_NULL denotes the packed selector block, not an additional GX attribute.
  GXAttr attr = GX_VA_NULL;
  uint32_t offset = 0;
  uint32_t location = 0;
  wgpu::VertexFormat format{};

  constexpr uint32_t size() const noexcept { return format_size(format); }
};

struct Layout {
  std::array<Field, MaxAttributes> fields{};
  uint32_t attributeCount = 0;
  uint32_t stride = 0;
  uint16_t selectorMask = 0;
  bool valid = false;

  constexpr const Field* find(GXAttr attr) const noexcept {
    for (uint32_t i = 0; i < attributeCount; ++i) {
      if (fields[i].attr == attr) {
        return &fields[i];
      }
    }
    return nullptr;
  }
  constexpr bool has_selector(GXAttr attr) const noexcept { return (selectorMask & selector_bit(attr)) != 0; }

  std::array<wgpu::VertexAttribute, MaxAttributes> vertex_attributes() const noexcept {
    std::array<wgpu::VertexAttribute, MaxAttributes> attributes{};
    for (uint32_t i = 0; i < attributeCount; ++i) {
      const auto& field = fields[i];
      attributes[i] = {.format = field.format, .offset = field.offset, .shaderLocation = field.location};
    }
    return attributes;
  }
  // The caller owns attributes: no self-referential pointer survives a Layout copy.
  wgpu::VertexBufferLayout buffer_layout(const std::array<wgpu::VertexAttribute, MaxAttributes>& attributes) const noexcept {
    return {.stepMode = wgpu::VertexStepMode::Vertex,
            .arrayStride = stride,
            .attributeCount = attributeCount,
            .attributes = attributes.data()};
  }
};

// Raw attrs remain untouched. Presence, not a material's current attribute use, selects the full tuple.
constexpr Layout make_layout(const ShaderConfig& config) noexcept {
  Layout layout;
  if (!decoded_native_eligible(config)) {
    return layout;
  }
  const auto append = [&layout](GXAttr attr, wgpu::VertexFormat format) {
    const auto location = layout.attributeCount++;
    layout.fields[location] = {attr, layout.stride, location, format};
    layout.stride += format_size(format);
  };
  for (uint32_t i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if (config.attrs[i].attrType == GX_NONE) {
      continue;
    }
    const auto format = i == GX_VA_POS || i == GX_VA_NRM ? wgpu::VertexFormat::Float32x3
                      : i == GX_VA_CLR0 || i == GX_VA_CLR1 ? wgpu::VertexFormat::Float32x4
                                                        : wgpu::VertexFormat::Float32x2;
    append(static_cast<GXAttr>(i), format);
  }
  for (const auto& selector : Selectors) {
    if (config.attrs[selector.attr].attrType != GX_NONE) {
      layout.selectorMask |= selector_bit(selector.attr);
    }
  }
  if (layout.selectorMask != 0) {
    append(GX_VA_NULL, wgpu::VertexFormat::Uint32x3);
  }
  layout.valid = true;
  return layout;
}

// Only already-granted limits are evidence of support; undefined values are not large budgets.
constexpr bool limits_valid(const Layout& layout, const wgpu::Limits& limits) noexcept {
  if (!layout.valid || layout.attributeCount == 0 || layout.attributeCount > MaxAttributes ||
      layout.stride == 0 || layout.stride > MaxStride || layout.stride % 4 != 0 ||
      limits.maxVertexBuffers == WGPU_LIMIT_U32_UNDEFINED || limits.maxVertexBuffers < 1 ||
      limits.maxVertexAttributes == WGPU_LIMIT_U32_UNDEFINED || limits.maxVertexAttributes < layout.attributeCount ||
      limits.maxVertexBufferArrayStride == WGPU_LIMIT_U32_UNDEFINED || limits.maxVertexBufferArrayStride < layout.stride) {
    return false;
  }
  uint32_t end = 0;
  for (uint32_t i = 0; i < layout.attributeCount; ++i) {
    const auto& field = layout.fields[i];
    if (field.location != i || field.offset != end || field.size() == 0 || field.offset % 4 != 0 ||
        uint64_t{field.offset} + field.size() > layout.stride) {
      return false;
    }
    end += field.size();
  }
  return end == layout.stride;
}

} // namespace aurora::gx::decoded
