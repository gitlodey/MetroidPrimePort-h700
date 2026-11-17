#pragma once

#include "../gfx/types.hpp"
#include "gx.hpp"

namespace aurora::gx {
struct DrawData {
  gfx::PipelineRef pipeline;
  GXBindGroups bindGroups;
  gfx::Range vertRange;
  gfx::Range idxRange;
  gfx::Range uniformRange;
  gfx::Range drawUniformRange{};
  DrawImmediateData immediateData;
  uint32_t vtxCount;
  uint32_t indexCount;
  uint32_t dstAlpha;
  // Instanced lines/points come from a u16 GX vertex count; merged triangles keep one instance.
  uint16_t instanceCount;
  bool shadowGroup; // bindGroups.textureBindGroup has the shadow receiver layout
  bool drawUniform = false;
};
static_assert(sizeof(DrawData) <= gfx::InlineDrawPayloadSize);
static_assert(std::is_trivially_copyable_v<DrawData> && std::is_aggregate_v<DrawData>);

inline void clear_draw_uniform(DrawData& draw) noexcept {
  draw.drawUniform = false;
  draw.drawUniformRange = {};
}
inline bool draw_uniform_range_valid(const DrawData& draw, uint64_t bufferSize, uint32_t alignment) noexcept {
  if (!draw.drawUniform) {
    return draw.drawUniformRange == gfx::Range{};
  }
  return !draw.shadowGroup && draw.drawUniformRange.offset != UINT32_MAX && alignment != 0 &&
         draw.drawUniformRange.offset % alignment == 0 &&
         draw.drawUniformRange.size == sizeof(DrawImmediateData) &&
         uint64_t{draw.drawUniformRange.offset} + draw.drawUniformRange.size <= bufferSize;
}

constexpr uint32_t GXPipelineConfigVersion = 16;
struct PipelineConfig {
  uint32_t version = GXPipelineConfigVersion;
  uint32_t msaaSamples = 1;
  ShaderConfig shaderConfig;
  GXCompare depthFunc;
  GXCullMode cullMode;
  GXBlendMode blendMode;
  GXBlendFactor blendFacSrc, blendFacDst;
  GXLogicOp blendOp;
  uint32_t dstAlpha;
  uint32_t polygonOffsetBits;
  uint32_t polygonOffsetScaleBits;
  uint32_t polygonOffsetClampBits;
  // 1: the sun's shadow-map pipeline of a ShaderConfig::shadow draw (vs_shadow, depth only).
  // 2: the same with the draw's cut test (vs_shadow_cut, fs_shadow; the colour pass's group 2 and layout).
  uint32_t shadowPass = 0;
  bool depthCompare, depthUpdate, alphaUpdate, colorUpdate;
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

constexpr bool pipeline_config_compatible(const PipelineConfig& config, bool lightmapBinding) noexcept {
  const auto& shader = config.shaderConfig;
  // A cached row from another build at the same version can name a shadow pass this one doesn't
  // know, or one whose shader has no vs_shadow: its pipeline would abort the game in Dawn.
  const bool shadowPassOk =
      config.shadowPass == 0 || (config.shadowPass <= 2 && shader.shadow && shader.lineMode == 0);
  return shadowPassOk && (shader.pbr == 0 || shader.pbrLightmapAttr == GX_VA_NULL || lightmapBinding);
}

wgpu::RenderPipeline create_pipeline([[maybe_unused]] const PipelineConfig& config);
void render(const DrawData& data, const wgpu::RenderPassEncoder& pass);

void queue_surface(const u8* dlStart, uint32_t dlSize, bool bigEndian) noexcept;
} // namespace aurora::gx
