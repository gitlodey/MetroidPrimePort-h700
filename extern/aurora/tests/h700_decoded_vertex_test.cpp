#include <gtest/gtest.h>

#include "gfx/hash.hpp"
#include "gfx/resources.hpp"
#include "gx/decoded_vertex.hpp"
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

ShaderConfig retail_config() {
  ShaderConfig sc{};
  sc.vtxStride = 8;
  sc.attrs[GX_VA_POS] = {.attrType = GX_INDEX16, .cnt = 3, .compType = GX_F32, .stride = 12, .le = false};
  sc.attrs[GX_VA_NRM] = {.attrType = GX_INDEX16, .cnt = 3, .compType = GX_S16,
                       .offset = 2, .stride = 6, .frac = 14, .le = false};
  sc.attrs[GX_VA_CLR0] = {.attrType = GX_INDEX16, .cnt = 1, .compType = GX_RGBA8,
                        .offset = 4, .stride = 4, .le = false};
  sc.attrs[GX_VA_TEX0] = {.attrType = GX_INDEX16, .cnt = 2, .compType = GX_U16,
                        .offset = 6, .stride = 4, .frac = 15, .le = false};
  sc.tevStageCount = 1;
  auto& stage = sc.tevStages[0];
  stage.texMapId = GX_TEXMAP0;
  stage.texCoordId = GX_TEXCOORD0;
  stage.channelId = GX_COLOR0A0;
  stage.colorPass = {.b = GX_CC_TEXC, .c = GX_CC_RASC};
  stage.alphaPass = {.b = GX_CA_TEXA, .c = GX_CA_RASA};
  sc.tcgs[0].src = GX_TG_TEX0;
  sc.colorChannels[GX_COLOR0].lightingEnabled = true;
  sc.colorChannels[GX_COLOR0].matSrc = GX_SRC_VTX;
  sc.colorChannels[GX_ALPHA0].matSrc = GX_SRC_VTX;
  return sc;
}

ShaderConfig full_config(bool selectors = true) {
  auto sc = retail_config();
  sc.attrs[GX_VA_CLR1] = sc.attrs[GX_VA_CLR0];
  for (u32 i = 0; i < MaxTexCoord; ++i) {
    sc.attrs[GX_VA_TEX0 + i] = sc.attrs[GX_VA_TEX0];
    sc.tcgs[i].src = static_cast<GXTexGenSrc>(GX_TG_TEX0 + i);
    auto& stage = sc.tevStages[i];
    stage = sc.tevStages[0];
    stage.texCoordId = static_cast<GXTexCoordID>(i);
  }
  sc.tevStageCount = MaxTexCoord;
  if (selectors) {
    for (const auto& selector : decoded::Selectors) {
      sc.attrs[selector.attr] = {.attrType = GX_DIRECT, .cnt = 1, .compType = GX_U8};
    }
  }
  return sc;
}

wgpu::Limits granted_limits() {
  wgpu::Limits limits{};
  limits.maxVertexBuffers = 1;
  limits.maxVertexAttributes = decoded::MaxAttributes;
  limits.maxVertexBufferArrayStride = decoded::MaxStride;
  return limits;
}

std::string function_source(const std::string& source, std::string_view name) {
  const auto start = source.find(name);
  EXPECT_NE(start, std::string::npos) << name;
  if (start == std::string::npos) return {};
  const auto brace = source.find('{', start);
  EXPECT_NE(brace, std::string::npos);
  if (brace == std::string::npos) return {};
  u32 depth = 1;
  for (size_t i = brace + 1; i < source.size(); ++i) {
    depth += source[i] == '{' ? 1 : source[i] == '}' ? -1 : 0;
    if (depth == 0) return source.substr(start, i - start + 1);
  }
  ADD_FAILURE() << "Unclosed function " << name;
  return {};
}

void expect_no_vertex_fetch(const std::string& vertex) {
  for (const char* forbidden : {"vbuf", "abuf", "fetch_", "textureLoad", "textureGather", "imm.vtx_start"}) {
    EXPECT_EQ(vertex.find(forbidden), std::string::npos) << forbidden;
  }
}

class H700DecodedVertex : public ::testing::Test {
protected:
  void SetUp() override {
    oldTextureFetch = gfx::g_vertexFetchTexture;
    gfx::g_vertexFetchTexture = gfx::g_mixedFetch;
  }
  void TearDown() override { gfx::g_vertexFetchTexture = oldTextureFetch; }
  bool oldTextureFetch = false;
};

TEST_F(H700DecodedVertex, CompactFullTupleLayoutAndRawConfigStayIndependent) {
  static_assert(decoded::MaxAttributes <= 16);
  const auto sc = full_config();
  const auto before = std::bit_cast<std::array<u8, sizeof(ShaderConfig)>>(sc);
  const auto layout = decoded::make_layout(sc);
  ASSERT_TRUE(layout.valid);
  EXPECT_EQ(layout.attributeCount, 13u);
  EXPECT_EQ(layout.stride, 132u);
  EXPECT_EQ(layout.selectorMask, 0x1FFu);
  const std::array offsets{0u, 12u, 24u, 40u, 56u, 64u, 72u, 80u, 88u, 96u, 104u, 112u, 120u};
  const auto attributes = layout.vertex_attributes();
  const auto buffer = layout.buffer_layout(attributes);
  EXPECT_EQ(buffer.stepMode, wgpu::VertexStepMode::Vertex);
  EXPECT_EQ(buffer.arrayStride, layout.stride);
  EXPECT_EQ(buffer.attributeCount, layout.attributeCount);
  EXPECT_EQ(buffer.attributes, attributes.data());
  for (u32 i = 0; i < layout.attributeCount; ++i) {
    const auto& field = layout.fields[i];
    EXPECT_EQ(field.location, i);
    EXPECT_EQ(field.offset, offsets[i]);
    EXPECT_EQ(attributes[i].shaderLocation, field.location);
    EXPECT_EQ(attributes[i].offset, field.offset);
    EXPECT_EQ(attributes[i].format, field.format);
    EXPECT_LE(field.offset + field.size(), layout.stride);
    EXPECT_FALSE(decoded::wgsl_type(field.format).empty());
  }
  EXPECT_EQ(layout.find(GX_VA_POS)->format, wgpu::VertexFormat::Float32x3);
  EXPECT_EQ(layout.find(GX_VA_NRM)->format, wgpu::VertexFormat::Float32x3);
  EXPECT_EQ(layout.find(GX_VA_CLR0)->format, wgpu::VertexFormat::Float32x4);
  EXPECT_EQ(layout.find(GX_VA_CLR1)->format, wgpu::VertexFormat::Float32x4);
  EXPECT_EQ(layout.find(GX_VA_TEX7)->format, wgpu::VertexFormat::Float32x2);
  EXPECT_EQ(layout.find(GX_VA_NULL)->format, wgpu::VertexFormat::Uint32x3);
  EXPECT_EQ((std::bit_cast<std::array<u8, sizeof(ShaderConfig)>>(sc)), before);
}

TEST_F(H700DecodedVertex, OptionalFieldsUseDenseLocationsWithoutMaterialPruning) {
  auto sc = retail_config();
  auto layout = decoded::make_layout(sc);
  ASSERT_TRUE(layout.valid);
  EXPECT_EQ(layout.attributeCount, 4u);
  EXPECT_EQ(layout.stride, 48u);
  EXPECT_EQ(layout.find(GX_VA_NULL), nullptr);
  sc.attrs[GX_VA_NRM] = {};
  sc.attrs[GX_VA_CLR0] = {};
  sc.attrs[GX_VA_CLR1] = {.attrType = GX_DIRECT, .cnt = 1, .compType = GX_RGB565};
  sc.attrs[GX_VA_TEX7] = {.attrType = GX_INDEX8, .cnt = 1, .compType = GX_S8, .stride = 1, .frac = 6};
  layout = decoded::make_layout(sc);
  ASSERT_TRUE(layout.valid);
  EXPECT_EQ(layout.attributeCount, 4u);
  EXPECT_EQ(layout.stride, 44u);
  EXPECT_EQ(layout.find(GX_VA_CLR1)->location, 1u);
  EXPECT_EQ(layout.find(GX_VA_TEX0)->location, 2u);
  EXPECT_EQ(layout.find(GX_VA_TEX7)->location, 3u); // retained even though this material never reads it
  EXPECT_EQ(layout.find(GX_VA_TEX7)->size(), 8u); // S expands to ST before upload
  EXPECT_EQ(layout.find(GX_VA_NRM), nullptr);
}

TEST_F(H700DecodedVertex, SelectorPackingHasOneAuthoritativeRawByteMapping) {
  auto sc = retail_config();
  for (u32 i = 0; i < decoded::Selectors.size(); ++i) {
    const auto& selector = decoded::Selectors[i];
    EXPECT_EQ(selector.word, i / 4);
    EXPECT_EQ(selector.shift, (i % 4) * 8);
    EXPECT_EQ(decoded::selector_bit(selector.attr), 1u << i);
    sc.attrs[selector.attr] = {.attrType = GX_DIRECT, .cnt = 1};
    const auto layout = decoded::make_layout(sc);
    ASSERT_TRUE(layout.valid);
    EXPECT_TRUE(layout.has_selector(selector.attr));
    EXPECT_EQ(layout.attributeCount, 5u);
    EXPECT_EQ(layout.find(GX_VA_NULL)->offset, 48u);
    EXPECT_EQ(layout.stride, 60u);
    sc.attrs[selector.attr] = {};
  }
  EXPECT_EQ(decoded::selector_bit(GX_VA_POS), 0);
}

TEST_F(H700DecodedVertex, LimitsAreGrantedDenseAttributeLimitsNotUndefinedBudgets) {
  const auto layout = decoded::make_layout(full_config());
  const auto limits = granted_limits();
  EXPECT_TRUE(decoded::limits_valid(layout, limits));
  for (const u32 count : {0u, 12u, WGPU_LIMIT_U32_UNDEFINED}) {
    auto bad = limits;
    bad.maxVertexAttributes = count;
    EXPECT_FALSE(decoded::limits_valid(layout, bad));
  }
  for (const u32 count : {0u, WGPU_LIMIT_U32_UNDEFINED}) {
    auto bad = limits;
    bad.maxVertexBuffers = count;
    EXPECT_FALSE(decoded::limits_valid(layout, bad));
  }
  for (const u32 stride : {0u, 131u, WGPU_LIMIT_U32_UNDEFINED}) {
    auto bad = limits;
    bad.maxVertexBufferArrayStride = stride;
    EXPECT_FALSE(decoded::limits_valid(layout, bad));
  }
  EXPECT_FALSE(decoded::limits_valid({}, limits));
  auto bad = layout;
  bad.fields[1].offset += 1;
  EXPECT_FALSE(decoded::limits_valid(bad, limits));
  bad = layout;
  bad.fields[1].location = 13;
  EXPECT_FALSE(decoded::limits_valid(bad, limits));
}

TEST_F(H700DecodedVertex, UnsupportedGeometryKeepsFallbackAndDoesNotGrowLayout) {
  for (auto change : {+[](ShaderConfig& c) { c.lineMode = 1; },
                      +[](ShaderConfig& c) { c.lineMode = 3; },
                      +[](ShaderConfig& c) { c.pbrBindPos = 1; },
                      +[](ShaderConfig& c) { c.nativeVertex = 1; },
                      +[](ShaderConfig& c) { c.attrs[GX_VA_POS] = {}; },
                      +[](ShaderConfig& c) { c.attrs[GX_VA_NRM].cnt = 9; },
                      +[](ShaderConfig& c) { c.attrs[GX_VA_NRM].cnt = 15; },
                      +[](ShaderConfig& c) { c.attrs[GX_VA_NRM].nbt3 = true; },
                      +[](ShaderConfig& c) { c.attrs[GX_VA_NBT].attrType = GX_DIRECT; },
                      +[](ShaderConfig& c) { c.attrs[GX_VA_POS].compType = 0xFF; },
                      +[](ShaderConfig& c) { c.attrs[GX_VA_CLR0].compType = 0xFF; },
                      +[](ShaderConfig& c) { c.attrs[GX_VA_TEX0].cnt = 3; },
                      +[](ShaderConfig& c) { c.attrs[GX_VA_TEX0].frac = 32; },
                      +[](ShaderConfig& c) { c.attrs[GX_VA_TEX7MTXIDX] = {.attrType = GX_INDEX8, .cnt = 1}; },
                      +[](ShaderConfig& c) { c.tcgs[0].src = GX_TG_BINRM; },
                      +[](ShaderConfig& c) { c.tcgs[0].src = GX_TG_TANGENT; },
                      +[](ShaderConfig& c) { c.tcgs[0].type = GX_TG_BUMP0; }}) {
    auto sc = retail_config();
    change(sc);
    EXPECT_FALSE(decoded_native_eligible(sc));
    sc.decodedNative = 1;
    EXPECT_FALSE(uses_decoded_native(sc));
    const auto layout = decoded::make_layout(sc);
    EXPECT_FALSE(layout.valid);
    EXPECT_EQ(layout.attributeCount, 0u);
    EXPECT_EQ(layout.stride, 0u);
    PipelineConfig pipeline{};
    pipeline.shaderConfig = sc;
    EXPECT_FALSE(pipeline_config_compatible(pipeline, true));
  }
}

TEST_F(H700DecodedVertex, SpareBitPreservesRawMetadataAndLegacyPipelineKeys) {
  static_assert(offsetof(ShaderConfig, attrs) == 12);
  static_assert(std::has_unique_object_representations_v<ShaderConfig>);
  for (const bool rawNative : {false, true}) {
    for (const bool uniform : {false, true}) {
      for (const bool gather : {false, true}) {
        auto legacy = retail_config();
        legacy.nativeVertex = rawNative;
        legacy.drawUniform = uniform;
        legacy.fetchGather = gather;
        auto candidate = legacy;
        candidate.decodedNative = 1;
        const auto before = std::bit_cast<std::array<u8, sizeof(ShaderConfig)>>(legacy);
        const auto after = std::bit_cast<std::array<u8, sizeof(ShaderConfig)>>(candidate);
        for (size_t i = 0; i < before.size(); ++i) EXPECT_EQ(before[i] ^ after[i], i == 11 ? 0x20 : 0);
        EXPECT_NE(xxh3_hash(candidate), xxh3_hash(legacy));
        PipelineConfig oldPipeline{};
        oldPipeline.shaderConfig = legacy;
        auto newPipeline = oldPipeline;
        newPipeline.shaderConfig = candidate;
        EXPECT_EQ(oldPipeline.version, newPipeline.version);
        EXPECT_NE(xxh3_hash(oldPipeline), xxh3_hash(newPipeline));
        candidate.decodedNative = 0;
        EXPECT_EQ(candidate, legacy);
        EXPECT_EQ(xxh3_hash(candidate), xxh3_hash(legacy));
      }
    }
  }
}

TEST_F(H700DecodedVertex, TaggedMeshSnapshotAndCasterCopyKeepThe128BytePayload) {
  static_assert(sizeof(void*) != 8 || sizeof(DrawData) == 128);
  DrawData draw{};
  draw.decodedGeometry = true;
  draw.decodedMesh = {7, 42};
  draw.vtxCount = 4;
  draw.indexCount = 6;
  draw.instanceCount = 1;
  draw.drawUniform = true;
  draw.drawUniformRange = {256, sizeof(DrawImmediateData)};
  auto bytes = std::bit_cast<std::array<std::byte, sizeof(DrawData)>>(draw);
  DrawData snapshot{};
  std::memcpy(&snapshot, bytes.data(), bytes.size());
  clear_draw_uniform(snapshot);
  EXPECT_TRUE(snapshot.decodedGeometry);
  EXPECT_EQ(snapshot.decodedMesh, draw.decodedMesh);
  EXPECT_EQ(snapshot.vertRange, gfx::Range{});
  EXPECT_EQ(snapshot.vtxCount, draw.vtxCount);
  EXPECT_EQ(snapshot.indexCount, draw.indexCount);
  EXPECT_FALSE(snapshot.drawUniform);
  auto sc = retail_config();
  EXPECT_FALSE(native_vertex_merge_compatible(snapshot, sc, false));
  sc.decodedNative = 1;
  EXPECT_FALSE(native_vertex_merge_compatible(DrawData{}, sc, false));
  DrawData legacy{.idxRange = {4, 12}};
  EXPECT_EQ(legacy.idxRange.offset, 4u);
  EXPECT_EQ(legacy.idxRange.size, 12u);
  EXPECT_FALSE(legacy.decodedGeometry);
}

TEST_F(H700DecodedVertex, NativeInputsKeepLightingFragmentGuardsAndRawFetchSemanticsOutsideVertex) {
  for (const bool uniform : {false, true}) {
    for (const bool gather : {false, true}) {
      auto sc = retail_config();
      sc.drawUniform = uniform;
      sc.fetchGather = gather;
      sc.fogType = GX_FOG_PERSP_LIN;
      sc.fogRangeEnabled = true;
      const auto legacy = build_shader_source(sc);
      sc.decodedNative = 1;
      const auto source = build_shader_source(sc);
#ifdef AURORA_H700
      const auto vertex = function_source(source, "fn vs_main(");
      expect_no_vertex_fetch(vertex);
      EXPECT_NE(vertex.find("@location(0) decoded_pos: vec3f"), std::string::npos);
      EXPECT_NE(vertex.find("@location(1) decoded_nrm: vec3f"), std::string::npos);
      EXPECT_NE(vertex.find("@location(2) decoded_clr0: vec4f"), std::string::npos);
      EXPECT_NE(vertex.find("@location(3) decoded_tex0_uv: vec2f"), std::string::npos);
      EXPECT_NE(vertex.find("let in_pnmtxidx = imm.current_pnmtx;"), std::string::npos);
      EXPECT_NE(vertex.find("let in_nrm = decoded_nrm;"), std::string::npos);
      EXPECT_NE(vertex.find("let nrm_tmp = vec4f(in_nrm, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];"), std::string::npos);
      EXPECT_NE(vertex.find("out.cc0"), std::string::npos);
      EXPECT_EQ(function_source(source, "fn fs_main("), function_source(legacy, "fn fs_main("));
      EXPECT_NE(source.find("fs_load_word(&fs_abuf, imm.fog_range_base"), std::string::npos);
      EXPECT_NE(source.find("if (word_idx < arrayLength(p))"), std::string::npos);
      EXPECT_NE(source.find(uniform ? "var<uniform> imm: Immediate;" : "var<immediate> imm: Immediate;"), std::string::npos);
#else
      EXPECT_EQ(source, legacy);
#endif
      sc.decodedNative = 0;
      EXPECT_EQ(build_shader_source(sc), legacy);
    }
  }
}

TEST_F(H700DecodedVertex, EveryUvAndMatrixSelectorUseTheSameLayoutInMainAndShadow) {
  auto sc = full_config();
  sc.shadow = true;
  sc.decodedNative = 1;
  const auto source = build_shader_source(sc);
#ifdef AURORA_H700
  const auto layout = decoded::make_layout(sc);
  const auto main = function_source(source, "fn vs_main(");
  const auto shadow = function_source(source, "fn vs_shadow(");
  for (const auto& vertex : {main, shadow}) {
    expect_no_vertex_fetch(vertex);
    EXPECT_NE(vertex.find("@location(12) decoded_mtx: vec3u"), std::string::npos);
    EXPECT_NE(vertex.find("let in_pnmtxidx = ((decoded_mtx.x >> 0u) & 255u) / 3u;"), std::string::npos);
    EXPECT_EQ(vertex.find("imm.current_pnmtx"), std::string::npos);
    for (u32 i = 0; i < MaxTexCoord; ++i) {
      const auto* field = layout.find(static_cast<GXAttr>(GX_VA_TEX0 + i));
      ASSERT_NE(field, nullptr);
      EXPECT_NE(vertex.find("@location(" + std::to_string(field->location) + ") decoded_tex" + std::to_string(i) +
                            "_uv: vec2f"), std::string::npos);
      EXPECT_NE(vertex.find("let in_tex" + std::to_string(i) + "_uv = decoded_tex" + std::to_string(i) + "_uv;"),
                std::string::npos);
    }
    for (u32 i = 0; i < MaxTexCoord; ++i) {
      const auto& selector = decoded::Selectors[i + 1];
      const auto value = std::string{"decoded_mtx."} + "xyz"[selector.word] + " >> " + std::to_string(selector.shift) + "u";
      EXPECT_NE(vertex.find("let in_texmtxidx" + std::to_string(i) + " = ((" + value + ") & 255u);"), std::string::npos);
    }
  }
  EXPECT_NE(main.find("ubuf.postex_mtx[in_texmtxidx7 / 3u]"), std::string::npos);
  EXPECT_NE(shadow.find("ubuf.shadow_caster[imm.serial]"), std::string::npos);
  EXPECT_NE(source.find("@builtin(position) @invariant pos: vec4f"), std::string::npos);
#else
  EXPECT_EQ(source.find("decoded_pos"), std::string::npos);
#endif
}

TEST_F(H700DecodedVertex, ColourDepthIndirectAndEffectsKeepTheSameShadingBody) {
  for (auto change : {+[](ShaderConfig&) {},
                      +[](ShaderConfig& c) { c.perPixelLight = 1; },
                      +[](ShaderConfig& c) { c.depthOnly = 1; },
                      +[](ShaderConfig& c) { c.depthOnly = 2; },
                      +[](ShaderConfig& c) { c.sceneHdr = 1; },
                      +[](ShaderConfig& c) { c.clipPlane = 1; },
                      +[](ShaderConfig& c) { c.colorMatrix = 1; },
                      +[](ShaderConfig& c) { c.volFog = VolFogOpaque; },
                      +[](ShaderConfig& c) { c.shell = 2; },
                      +[](ShaderConfig& c) { c.shadow = 1; c.actorDecal = 1; },
                      +[](ShaderConfig& c) { c.colorChannels[GX_COLOR0].matSrc = GX_SRC_REG; },
                      +[](ShaderConfig& c) {
                        c.numIndStages = 1;
                        c.indStages[0].texCoordId = GX_TEXCOORD0;
                        c.indStages[0].texMapId = GX_TEXMAP0;
                        c.tevStages[0].indTexMtxId = GX_ITM_0;
                      },
                      +[](ShaderConfig& c) { c.pbr = 1; c.shadow = 1; c.pbrCut = 1; }}) {
    auto sc = retail_config();
    change(sc);
    ASSERT_TRUE(decoded_native_eligible(sc));
    const auto legacy = build_shader_source(sc);
    sc.decodedNative = 1;
    const auto source = build_shader_source(sc);
#ifdef AURORA_H700
    expect_no_vertex_fetch(function_source(source, "fn vs_main("));
    EXPECT_EQ(function_source(source, "fn fs_main("), function_source(legacy, "fn fs_main("));
    if (sc.pbr) {
      ASSERT_NE(source.find("fn vs_shadow_cut("), std::string::npos);
      expect_no_vertex_fetch(function_source(source, "fn vs_shadow_cut("));
      EXPECT_EQ(function_source(source, "fn fs_shadow("), function_source(legacy, "fn fs_shadow("));
    }
#else
    EXPECT_EQ(source, legacy);
#endif
  }
}

TEST_F(H700DecodedVertex, UnsupportedStaleBitGeneratesUnchangedLegacyShaders) {
  for (auto change : {+[](ShaderConfig& c) { c.lineMode = 1; },
                      +[](ShaderConfig& c) { c.pbr = 1; c.pbrBindPos = 1; },
                      +[](ShaderConfig& c) { c.attrs[GX_VA_NRM].cnt = 9; }}) {
    auto sc = retail_config();
    change(sc);
    const auto legacy = build_shader_source(sc);
    sc.decodedNative = 1;
    EXPECT_FALSE(uses_decoded_native(sc));
    EXPECT_EQ(build_shader_source(sc), legacy);
  }
}

TEST_F(H700DecodedVertex, AllEncodersUseTheTaggedBinderWithoutChangingFrameGroups) {
  const auto read = [](const char* name) {
    std::ifstream file(std::string{H700_SHADER_SOURCE_DIR} + name);
    EXPECT_TRUE(file.good()) << name;
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  };
  const auto pipeline = read("/gx/pipeline.cpp");
  const auto binder = function_source(pipeline, "void bind_draw_geometry(");
  EXPECT_LT(binder.find("if (data.decodedGeometry)"), binder.find("const auto& resources"));
  EXPECT_NE(binder.find("decoded::valid(data.decodedMesh)"), std::string::npos);
  EXPECT_NE(binder.find("decoded::bind(data.decodedMesh, pass);\n    return;"), std::string::npos);
  EXPECT_NE(binder.find("pass.SetVertexBuffer(0, resources.vertexBuffer"), std::string::npos);
  EXPECT_NE(binder.find("pass.SetIndexBuffer(resources.indexBuffer"), std::string::npos);
  const auto render = function_source(pipeline, "void render(");
  EXPECT_NE(render.find("bind_draw_geometry(data, pass)"), std::string::npos);
  for (const char* name : {"/gfx/shadow.cpp", "/gfx/actor_shadow.cpp"}) {
    const auto source = read(name);
    EXPECT_NE(source.find("gx::bind_draw_geometry(draw, pass)"), std::string::npos);
    EXPECT_EQ(source.find("pass.SetIndexBuffer(resources.indexBuffer"), std::string::npos);
    EXPECT_NE(source.find("pass.SetBindGroup(0, resources.staticBindGroup)"), std::string::npos);
    EXPECT_NE(source.find("pass.SetBindGroup(1, resources.uniformBindGroup"), std::string::npos);
  }
}

} // namespace
} // namespace aurora::gx
