#include <gtest/gtest.h>

#include "gfx/hash.hpp"
#include "gfx/resources.hpp"
#include "gx/native_vertex.hpp"
#include "gx/pipeline.hpp"

#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace aurora::gx {
namespace {
ShaderConfig menu_config(bool fixedUv = false) {
  ShaderConfig sc{};
  sc.vtxStride = fixedUv ? 16 : 20;
  sc.attrs[GX_VA_POS] = {.attrType = GX_DIRECT, .cnt = 3, .compType = GX_F32, .le = false};
  sc.attrs[GX_VA_TEX0] = {.attrType = GX_DIRECT, .cnt = 2, .compType = u8(fixedUv ? GX_U16 : GX_F32),
                         .offset = 12, .le = false};
  sc.tevStageCount = 1;
  sc.tevStages[0].texMapId = GX_TEXMAP0;
  sc.tevStages[0].texCoordId = GX_TEXCOORD0;
  sc.tevStages[0].colorPass.b = GX_CC_TEXC;
  sc.tevStages[0].colorPass.c = GX_CC_ONE;
  sc.tevStages[0].alphaPass.d = GX_CA_TEXA;
  sc.tcgs[0].src = GX_TG_TEX0;
  return sc;
}

wgpu::Limits vertex_limits() {
  wgpu::Limits limits{};
  limits.maxVertexBuffers = 1;
  limits.maxVertexAttributes = 2;
  limits.maxVertexBufferArrayStride = 20;
  return limits;
}

std::string source_file(const char* path) {
  std::ifstream file(std::string{H700_SHADER_SOURCE_DIR} + path);
  EXPECT_TRUE(file.good()) << path;
  return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::string section(const std::string& source, std::string_view start, std::string_view end) {
  const auto first = source.find(start);
  EXPECT_NE(first, std::string::npos) << start;
  if (first == std::string::npos) return {};
  const auto last = source.find(end, first + start.size());
  EXPECT_NE(last, std::string::npos) << end;
  return last == std::string::npos ? std::string{} : source.substr(first, last - first);
}

class H700NativeVertex : public ::testing::Test {
protected:
  void SetUp() override {
    oldTextureFetch = gfx::g_vertexFetchTexture;
    gfx::g_vertexFetchTexture = gfx::g_mixedFetch;
  }
  void TearDown() override { gfx::g_vertexFetchTexture = oldTextureFetch; }
  bool oldTextureFetch = false;
};

TEST_F(H700NativeVertex, ExactOptInRequiresBenchAndDefaultsOff) {
  for (const char* value : std::array<const char*, 9>{nullptr, "", "0", "true", "01", "1 ", " 1", "menu", "ON"}) {
    EXPECT_FALSE(native_vertex_requested(value, "1"));
    EXPECT_FALSE(native_vertex_requested(value, "menu"));
  }
  for (const char* bench : std::array<const char*, 8>{nullptr, "", "0", "true", "01", "MENU", "menu ", " 1"}) {
    EXPECT_FALSE(native_vertex_requested("1", bench));
  }
  EXPECT_TRUE(native_vertex_requested("1", "1"));
  EXPECT_TRUE(native_vertex_requested("1", "menu"));
#ifndef AURORA_H700
  EXPECT_FALSE(native_vertex_enabled());
#endif
}

TEST_F(H700NativeVertex, OnlyTwoExactDirectLayoutsAreEligible) {
  for (const bool fixedUv : {false, true}) {
    auto sc = menu_config(fixedUv);
    EXPECT_TRUE(native_vertex_eligible(sc));
    EXPECT_FALSE(uses_native_vertex(sc));
    sc.nativeVertex = 1;
    EXPECT_EQ(uses_native_vertex(sc), gfx::g_mixedFetch);
    EXPECT_EQ(native_vertex_stride(sc), gfx::g_mixedFetch ? sc.vtxStride : 0);
    for (const u8 frac : {u8{0}, u8{5}, u8{31}}) {
      sc.attrs[GX_VA_TEX0].frac = frac;
      EXPECT_TRUE(native_vertex_eligible(sc));
    }
    for (size_t attr = 0; attr < sc.attrs.size(); ++attr) {
      if (attr == GX_VA_POS || attr == GX_VA_TEX0) continue;
      auto bad = sc;
      bad.attrs[attr].attrType = GX_DIRECT;
      EXPECT_FALSE(native_vertex_eligible(bad)) << attr;
    }
    for (auto change : {+[](ShaderConfig& c) { c.attrs[GX_VA_POS].attrType = GX_NONE; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_POS].attrType = GX_INDEX8; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_TEX0].attrType = GX_INDEX16; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_POS].compType = GX_S16; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_POS].cnt = 2; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_POS].offset = 4; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_POS].stride = 12; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_POS].le = true; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_POS].nbt3 = true; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_TEX0].attrType = GX_NONE; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_TEX0].compType = GX_S16; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_TEX0].cnt = 3; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_TEX0].offset = 10; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_TEX0].stride = 4; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_TEX0].le = true; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_TEX0].nbt3 = true; },
                        +[](ShaderConfig& c) { c.attrs[GX_VA_TEX0].frac = 32; },
                        +[](ShaderConfig& c) { c.vtxStride += 4; },
                        +[](ShaderConfig& c) { c.vtxStride -= 1; }}) {
      auto bad = sc;
      change(bad);
      EXPECT_FALSE(native_vertex_eligible(bad));
      EXPECT_FALSE(uses_native_vertex(bad));
    }
  }
}

TEST_F(H700NativeVertex, SpecialsLightingAndOtherTexgenInputsKeepFallback) {
  for (auto change : {+[](ShaderConfig& c) { c.lineMode = 1; },
                      +[](ShaderConfig& c) { c.shadow = true; },
                      +[](ShaderConfig& c) { c.sceneHdr = true; },
                      +[](ShaderConfig& c) { c.pbr = 1; },
                      +[](ShaderConfig& c) { c.depthOnly = 1; },
                      +[](ShaderConfig& c) { c.drawId = true; },
                      +[](ShaderConfig& c) { c.sdf = 1; },
                      +[](ShaderConfig& c) { c.hudSample = 1; },
                      +[](ShaderConfig& c) { c.volFog = VolFogOpaque; },
                      +[](ShaderConfig& c) { c.perPixelLight = true; },
                      +[](ShaderConfig& c) { c.clipPlane = true; },
                      +[](ShaderConfig& c) { c.shell = 1; },
                      +[](ShaderConfig& c) { c.actorDecal = true; },
                      +[](ShaderConfig& c) { c.colorMatrix = true; },
                      +[](ShaderConfig& c) { c.pbrBindPos = true; },
                      +[](ShaderConfig& c) { c.pbrBindLe = true; },
                      +[](ShaderConfig& c) { c.pbrLightmapAttr = GX_VA_TEX0; },
                      +[](ShaderConfig& c) { c.pbrKind = 1; },
                      +[](ShaderConfig& c) { c.pbrCut = true; },
                      +[](ShaderConfig& c) { c.pbrThermal = true; },
                      +[](ShaderConfig& c) { c.pbrPlain = true; },
                      +[](ShaderConfig& c) { c.pbrNoDebug = true; },
                      +[](ShaderConfig& c) { c.numIndStages = 1; },
                      +[](ShaderConfig& c) { c.colorChannels[GX_ALPHA0].lightingEnabled = true; },
                      +[](ShaderConfig& c) { c.tcgs[0].src = GX_TG_NRM; }}) {
    auto sc = menu_config();
    change(sc);
    const auto legacy = build_shader_source(sc);
    sc.nativeVertex = 1; // stale cache bits must not change a special shader's ABI
    EXPECT_FALSE(native_vertex_eligible(sc));
    EXPECT_FALSE(uses_native_vertex(sc));
    EXPECT_EQ(build_shader_source(sc), legacy);
  }
}

TEST_F(H700NativeVertex, EmbossRejectsNativeButRetainsValidNBTFallback) {
  auto sc = menu_config();
  sc.tcgs[1].src = GX_TG_TEX0;
  sc.tcgs[1].type = GX_TG_BUMP0;
  sc.tcgs[1].embossSrc = 0;
  sc.tevStages[0].texCoordId = GX_TEXCOORD1;
  EXPECT_FALSE(native_vertex_eligible(sc));
  // A real emboss layout needs NBT normals; do not ask the fallback generator
  // to compile a deliberately malformed normal-less emboss shader.
  sc.attrs[GX_VA_NRM] = {.attrType = GX_INDEX16, .cnt = 9, .compType = GX_F32,
                        .offset = 12, .stride = 36};
  sc.attrs[GX_VA_TEX0].offset = 14;
  sc.vtxStride = 22;
  const auto legacy = build_shader_source(sc);
  sc.nativeVertex = 1;
  EXPECT_FALSE(uses_native_vertex(sc));
  EXPECT_EQ(build_shader_source(sc), legacy);
}

TEST_F(H700NativeVertex, IndependentSpareBitPreservesLegacyCacheBytesAndKeys) {
  static_assert(offsetof(ShaderConfig, attrs) == 12);
  static_assert(std::has_unique_object_representations_v<ShaderConfig>);
  for (const bool drawUniform : {false, true}) {
    for (const bool gather : {false, true}) {
      auto legacy = menu_config();
      legacy.drawUniform = drawUniform;
      legacy.fetchGather = gather;
      auto candidate = legacy;
      candidate.nativeVertex = 1;
      const auto before = std::bit_cast<std::array<u8, sizeof(ShaderConfig)>>(legacy);
      const auto after = std::bit_cast<std::array<u8, sizeof(ShaderConfig)>>(candidate);
      for (size_t i = 0; i < before.size(); ++i) EXPECT_EQ(before[i] ^ after[i], i == 11 ? 16 : 0) << i;
      EXPECT_NE(xxh3_hash(candidate), xxh3_hash(legacy));
      PipelineConfig pipeline{};
      pipeline.shaderConfig = legacy;
      auto native = pipeline;
      native.shaderConfig = candidate;
      EXPECT_EQ(native.version, pipeline.version);
      EXPECT_NE(xxh3_hash(native), xxh3_hash(pipeline));
      candidate.nativeVertex = 0;
      native.shaderConfig = candidate;
      EXPECT_EQ(candidate, legacy);
      EXPECT_EQ(xxh3_hash(candidate), xxh3_hash(legacy));
      EXPECT_EQ(xxh3_hash(native), xxh3_hash(pipeline));
    }
  }
}

TEST_F(H700NativeVertex, LayoutsUseAlignedRawWordsAndGrantedLimits) {
  auto limits = vertex_limits();
  for (const u8 stride : {u8{16}, u8{20}}) {
    const auto attrs = native_vertex_attributes(stride);
    const auto layout = native_vertex_layout(stride, attrs);
    EXPECT_EQ(layout.stepMode, wgpu::VertexStepMode::Vertex);
    EXPECT_EQ(layout.arrayStride, stride);
    ASSERT_EQ(layout.attributeCount, 2u);
    EXPECT_EQ(layout.attributes, attrs.data());
    EXPECT_EQ(attrs[0].format, wgpu::VertexFormat::Uint32x3);
    EXPECT_EQ(attrs[0].offset, 0u);
    EXPECT_EQ(attrs[0].shaderLocation, 0u);
    EXPECT_EQ(attrs[1].format, stride == 16 ? wgpu::VertexFormat::Uint32 : wgpu::VertexFormat::Uint32x2);
    EXPECT_EQ(attrs[1].offset, 12u);
    EXPECT_EQ(attrs[1].shaderLocation, 1u);
    EXPECT_TRUE(native_vertex_limits_valid(stride, limits));
    auto bad = limits;
    bad.maxVertexBuffers = 0;
    EXPECT_FALSE(native_vertex_limits_valid(stride, bad));
    bad = limits;
    bad.maxVertexAttributes = 1;
    EXPECT_FALSE(native_vertex_limits_valid(stride, bad));
    bad = limits;
    bad.maxVertexBufferArrayStride = stride - 1;
    EXPECT_FALSE(native_vertex_limits_valid(stride, bad));
  }
  for (const u8 stride : {u8{0}, u8{12}, u8{17}, u8{24}, u8{255}}) {
    EXPECT_FALSE(native_vertex_limits_valid(stride, limits));
  }
}

TEST_F(H700NativeVertex, RealBindingRequiresExactWholeVerticesAndActualBufferBounds) {
  const auto limits = vertex_limits();
  for (const u8 stride : {u8{16}, u8{20}}) {
    DrawData draw{};
    draw.nativeVertexStride = stride;
    draw.vtxCount = 4;
    draw.instanceCount = 1;
    draw.vertRange = {256, u32{stride} * 4};
    const uint64_t end = uint64_t{draw.vertRange.offset} + draw.vertRange.size;
    EXPECT_TRUE(native_vertex_draw_range_valid(draw, end, limits));
    EXPECT_FALSE(native_vertex_draw_range_valid(draw, end - 1, limits));
    for (auto change : {+[](DrawData& d) { d.vertRange.offset = UINT32_MAX; },
                        +[](DrawData& d) { ++d.vertRange.offset; },
                        +[](DrawData& d) { --d.vertRange.size; },
                        +[](DrawData& d) { ++d.vertRange.size; },
                        +[](DrawData& d) { d.vertRange.size = 0; },
                        +[](DrawData& d) { d.vtxCount = 0; },
                        +[](DrawData& d) { d.vtxCount = UINT32_MAX; },
                        +[](DrawData& d) { d.instanceCount = 0; },
                        +[](DrawData& d) { d.instanceCount = 2; },
                        +[](DrawData& d) { d.nativeVertexStride = 24; }}) {
      auto bad = draw;
      change(bad);
      EXPECT_FALSE(native_vertex_draw_range_valid(bad, end, limits));
    }
    draw.vertRange.offset = 0; // no sentinel at buffer origin; resident slices use it too
    EXPECT_TRUE(native_vertex_draw_range_valid(draw, draw.vertRange.size, limits));
    draw.vertRange.offset = UINT32_MAX - 3;
    EXPECT_FALSE(native_vertex_draw_range_valid(draw, UINT32_MAX, limits));
    draw.nativeVertexStride = 0;
    EXPECT_TRUE(native_vertex_draw_range_valid(draw, 0, limits)); // legacy bind path is unchanged
  }
}

TEST_F(H700NativeVertex, RecordedModeSurvivesSnapshotAndOnlyCompatibleDrawsMerge) {
  for (const bool fixedUv : {false, true}) {
    auto sc = menu_config(fixedUv);
    DrawData draw{};
    draw.vtxCount = 4;
    draw.instanceCount = 1;
    draw.vertRange = {128, u32{sc.vtxStride} * 4};
    EXPECT_TRUE(native_vertex_merge_compatible(draw, sc, false));
    EXPECT_FALSE(native_vertex_merge_compatible(draw, sc, true));
    sc.nativeVertex = 1;
    draw.nativeVertexStride = native_vertex_stride(sc);
    EXPECT_TRUE(native_vertex_merge_compatible(draw, sc, true));
    EXPECT_FALSE(native_vertex_merge_compatible(draw, sc, false));
    auto bytes = std::bit_cast<std::array<std::byte, sizeof(DrawData)>>(draw);
    DrawData snapshot{};
    std::memcpy(&snapshot, bytes.data(), bytes.size());
    sc.nativeVertex = 0; // later config/env changes cannot select the recorded render ABI
    EXPECT_EQ(snapshot.nativeVertexStride, draw.nativeVertexStride);
    EXPECT_EQ(snapshot.vertRange, draw.vertRange);
    EXPECT_EQ(snapshot.vtxCount, draw.vtxCount);
#ifdef AURORA_H700
    EXPECT_FALSE(native_vertex_merge_compatible(snapshot, sc, false));
    sc.nativeVertex = 1;
    snapshot.nativeVertexStride = fixedUv ? 20 : 16;
    EXPECT_FALSE(native_vertex_merge_compatible(snapshot, sc, true));
    snapshot = draw;
    snapshot.vertRange.size *= 2;
    snapshot.vtxCount *= 2; // merged indices stay relative to the original bound slice
    EXPECT_TRUE(native_vertex_draw_range_valid(snapshot, 1024, vertex_limits()));
#endif
  }
}

TEST_F(H700NativeVertex, NativeVertexBodyDecodesBEWithoutVertexFetchAndKeepsFragmentUnchanged) {
  for (const bool fixedUv : {false, true}) {
    for (const bool uniform : {false, true}) {
      for (const bool gather : {false, true}) {
        auto sc = menu_config(fixedUv);
        sc.drawUniform = uniform;
        sc.fetchGather = gather;
        sc.attrs[GX_VA_TEX0].frac = 5;
        sc.fogType = GX_FOG_PERSP_LIN;
        sc.fogRangeEnabled = true; // the fragment SSBO and its bounds guard must stay
        const auto legacy = build_shader_source(sc);
        sc.nativeVertex = 1;
        const auto native = build_shader_source(sc);
#ifdef AURORA_H700
        const auto vs = section(native, "fn vs_main(", "@fragment");
        EXPECT_NE(vs.find("@location(0) native_pos: vec3u"), std::string::npos);
        EXPECT_NE(vs.find(fixedUv ? "@location(1) native_uv: u32" : "@location(1) native_uv: vec2u"), std::string::npos);
        EXPECT_NE(vs.find("bitcast<vec3f>(vec3u(bswap32(native_pos.x, false), bswap32(native_pos.y, false), bswap32(native_pos.z, false)))"), std::string::npos);
        if (fixedUv) {
          EXPECT_NE(vs.find("vec2f(f32(bswap16(native_uv & 0xFFFFu, false)), f32(bswap16(native_uv >> 16u, false))) / f32(1u << 5u)"), std::string::npos);
        } else {
          EXPECT_NE(vs.find("bitcast<vec2f>(vec2u(bswap32(native_uv.x, false), bswap32(native_uv.y, false)))"), std::string::npos);
          EXPECT_EQ(vs.find("1u <<"), std::string::npos); // F32 ignores VAT frac, as the original fetch does
        }
        for (const char* forbidden : {"vbuf", "abuf", "fetch_", "textureGather", "textureLoad", "imm.vtx_start"}) {
          EXPECT_EQ(vs.find(forbidden), std::string::npos) << forbidden;
        }
        EXPECT_EQ(native.substr(native.find("@fragment")), legacy.substr(legacy.find("@fragment")));
        EXPECT_NE(native.find("fs_load_word(&fs_abuf, imm.fog_range_base"), std::string::npos);
        EXPECT_NE(native.find("if (word_idx < arrayLength(p))"), std::string::npos);
        EXPECT_NE(native.find(uniform ? "@group(1) @binding(1)\nvar<uniform> imm: Immediate;"
                                     : "var<immediate> imm: Immediate;"), std::string::npos);
#else
        EXPECT_EQ(native, legacy);
#endif
        sc.nativeVertex = 0;
        EXPECT_EQ(build_shader_source(sc), legacy);
      }
    }
  }
}

TEST_F(H700NativeVertex, U16FractionEndpointsAreEmittedWithoutChangingBEWordOrder) {
  for (const u8 frac : {u8{0}, u8{31}}) {
    auto sc = menu_config(true);
    sc.nativeVertex = 1;
    sc.attrs[GX_VA_TEX0].frac = frac;
    const auto source = build_shader_source(sc);
#ifdef AURORA_H700
    EXPECT_NE(source.find("/ f32(1u << " + std::to_string(frac) + "u)"), std::string::npos);
    EXPECT_NE(source.find("bswap16(native_uv & 0xFFFFu, false)"), std::string::npos);
    EXPECT_NE(source.find("bswap16(native_uv >> 16u, false)"), std::string::npos);
#else
    EXPECT_EQ(source.find("native_uv"), std::string::npos);
#endif
  }
}

TEST_F(H700NativeVertex, SourceHooksKeepSelectionRecordingMergeAndRealBindConnected) {
  const auto gx = source_file("/gx/gx.cpp");
  const auto gate = section(gx, "bool native_vertex_enabled() noexcept {", "#endif");
  EXPECT_NE(gate.find("native_vertex_requested(std::getenv(\"MP_H700_NATIVE_VERTEX\"), std::getenv(\"MP_H700_BENCH\"))"), std::string::npos);
  EXPECT_NE(gate.find("draw_uniform_state_eligible(g_gxState) && native_vertex_eligible(sc)"), std::string::npos);
  EXPECT_NE(gate.find("native_vertex_limits_valid(sc.vtxStride, gfx::detail::resources().limits)"), std::string::npos);
  EXPECT_NE(gx.find("config.shaderConfig.nativeVertex = native_vertex_for_state(config.shaderConfig)"), std::string::npos);
  const auto processor = source_file("/gx/command_processor.cpp");
  EXPECT_NE(processor.find("cache.config.shaderConfig.decodedNative == (decodedMesh != nullptr)"), std::string::npos);
  EXPECT_NE(processor.find("(decodedMesh ? false : native_vertex_for_state(cache.config.shaderConfig))"), std::string::npos);
  EXPECT_NE(processor.find(".nativeVertexStride = native_vertex_stride(cache.config.shaderConfig)"), std::string::npos);
  const auto merge = section(processor, "const bool canMerge =", "handle_draw_unmerged(prim, fmt, vtxCount, vertRange);");
  EXPECT_NE(merge.find("native_vertex_merge_compatible(*lastDraw, sDrawCache.config.shaderConfig,"), std::string::npos);
  EXPECT_NE(merge.find("native_vertex_for_state(sDrawCache.config.shaderConfig)"), std::string::npos);
  const auto pipeline = source_file("/gx/pipeline.cpp");
  const auto create = section(pipeline, "wgpu::RenderPipeline create_pipeline(", "void render(");
  EXPECT_NE(create.find("uses_native_vertex(config.shaderConfig)"), std::string::npos);
  EXPECT_NE(create.find("build_pipeline(config, layouts, shader"), std::string::npos);
  EXPECT_EQ(create.find("native_vertex_enabled"), std::string::npos); // cached native rows compile on an OFF run
  const auto bind = section(pipeline, "if (data.nativeVertexStride != 0) {", "pass.SetIndexBuffer");
  EXPECT_NE(bind.find("resources.vertexBuffer.GetUsage() & wgpu::BufferUsage::Vertex"), std::string::npos);
  EXPECT_NE(bind.find("native_vertex_draw_range_valid(data, resources.vertexBuffer.GetSize(), resources.limits)"), std::string::npos);
  EXPECT_NE(bind.find("pass.SetVertexBuffer(0, resources.vertexBuffer, data.vertRange.offset, data.vertRange.size)"), std::string::npos);
  EXPECT_EQ(bind.find("getenv"), std::string::npos);
  EXPECT_EQ(pipeline.find("DrawIndexed(data.indexCount, data.instanceCount,"), std::string::npos);
}
} // namespace
} // namespace aurora::gx
