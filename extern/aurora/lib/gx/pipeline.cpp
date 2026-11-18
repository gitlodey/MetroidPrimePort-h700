#include "pipeline.hpp"

#include "../gfx/encoding.hpp"
#include "../gfx/overdraw.hpp"
#include "../gfx/resources.hpp"
#include "../gfx/pipeline_cache.hpp"
#include "../gfx/resource_cache.hpp"

#include "gx_fmt.hpp"
#include "shader_info.hpp"
#include "../logging.hpp"
#include "../webgpu/gpu.hpp"

#include <tracy/Tracy.hpp>

static aurora::Module Log("aurora::gx::pipeline");

namespace aurora::gx {

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) {
  if (!pipeline_config_compatible(config, webgpu::g_lightmapBinding)) {
    Log.error("Refusing GX pipeline: shadow pass {} (shader shadow {}), PBR lightmap attribute {} (lightmap "
              "binding {})",
              config.shadowPass, static_cast<int>(config.shaderConfig.shadow), config.shaderConfig.pbrLightmapAttr,
              webgpu::g_lightmapBinding);
    return {};
  }

  ZoneScoped;
  auto dstAlphaMode = dst_alpha_mode(config);
  const auto shader = build_shader(config.shaderConfig, dstAlphaMode);
  const auto label =
      fmt::format("GX Pipeline {:x} shader {:x}", xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX)),
                  xxh3_hash(config.shaderConfig));
  if (uses_native_vertex(config.shaderConfig)) {
    const auto stride = config.shaderConfig.vtxStride;
    if (!native_vertex_limits_valid(stride, gfx::detail::resources().limits)) {
      Log.error("Refusing native GX vertex layout: stride {} exceeds granted vertex limits", stride);
      return {};
    }
    const auto attributes = native_vertex_attributes(stride);
    const std::array layouts{native_vertex_layout(stride, attributes)};
    return build_pipeline(config, layouts, shader, label.c_str(), dstAlphaMode);
  }
  return build_pipeline(config, {}, shader, label.c_str(), dstAlphaMode);
}

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass) {
  if (!gfx::bind_pipeline(data.pipeline, pass)) {
    return;
  }

  const auto& resources = gfx::detail::resources();
  AURORA_ASSERT(draw_uniform_range_valid(data, gfx::frame_buffer_sizes().deviceUniform,
                                        resources.limits.minUniformBufferOffsetAlignment),
                "GX draw uniform mode/range mismatch");
  if (data.drawUniform) {
    AURORA_ASSERT(gfx::g_mixedFetch && resources.drawUniformBindGroup, "GX draw uniform layout unavailable");
    const std::array offsets{data.uniformRange.offset, data.drawUniformRange.offset};
    pass.SetBindGroup(1, resources.drawUniformBindGroup, offsets.size(), offsets.data());
  } else {
    pass.SetImmediates(0, &data.immediateData, sizeof(data.immediateData));
    const std::array offsets{data.uniformRange.offset};
    pass.SetBindGroup(1, resources.uniformBindGroup, offsets.size(), offsets.data());
  }
  // A shadow receiver's group 2 has its own layout, so a draw without a group of its own can't
  // inherit it. A stale flag (a pass starts with the empty group bound) only costs a rebind.
  static bool sShadowGroupBound = false;
  if (data.bindGroups.textureBindGroup) {
    gfx::bind_texture_group(data.bindGroups.textureBindGroup, pass);
    sShadowGroupBound = data.shadowGroup;
  } else if (sShadowGroupBound) {
    pass.SetBindGroup(2, g_emptyTextureBindGroup);
    gfx::forget_texture_group();
    sShadowGroupBound = false;
  }
  if (data.nativeVertexStride != 0) {
    AURORA_ASSERT(gfx::g_mixedFetch && resources.vertexBuffer &&
                      (resources.vertexBuffer.GetUsage() & wgpu::BufferUsage::Vertex) != wgpu::BufferUsage::None,
                  "GX native vertex buffer unavailable");
    AURORA_ASSERT(native_vertex_draw_range_valid(data, resources.vertexBuffer.GetSize(), resources.limits),
                  "GX native vertex mode/range mismatch");
    // Indices remain local to this slice. Neither baseVertex nor imm.vtx_start is
    // added in the native shader; the slice offset is applied exactly once here.
    pass.SetVertexBuffer(0, resources.vertexBuffer, data.vertRange.offset, data.vertRange.size);
  }
  pass.SetIndexBuffer(resources.indexBuffer, wgpu::IndexFormat::Uint16, data.idxRange.offset, data.idxRange.size);
  if (data.dstAlpha != UINT32_MAX) {
    const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  const uint32_t query = gfx::overdraw::draw_begin(data.pipeline, pass);
  if (data.indexCount == 0) {
    pass.Draw(data.vtxCount, data.instanceCount);
  } else {
    pass.DrawIndexed(data.indexCount, data.instanceCount);
  }
  gfx::overdraw::draw_end(query, pass);
}

} // namespace aurora::gx
