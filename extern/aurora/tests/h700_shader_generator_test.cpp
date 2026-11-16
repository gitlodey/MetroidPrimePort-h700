#include <gtest/gtest.h>

#include "gfx/hash.hpp"
#include "gfx/pipeline_cache.hpp"
#include "gfx/resources.hpp"
#include "gx/gx.hpp"
#include "webgpu/gpu.hpp"

#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace aurora::gx {
namespace {

ShaderConfig normal_config() {
  ShaderConfig config{};
  config.vtxStride = 3;
  config.tevStageCount = 1;
  config.attrs[GX_VA_POS] = {.attrType = GX_INDEX16, .cnt = 3, .compType = GX_S16,
                           .offset = 1, .stride = 6, .frac = 8, .le = false};
  config.attrs[GX_VA_PNMTXIDX] = {.attrType = GX_DIRECT, .cnt = 1, .compType = GX_U8};
  // Read TEV registers and a vertex colour through the ordinary raster channel.
  auto& stage = config.tevStages[0];
  stage.colorPass = {.a = GX_CC_C0, .b = GX_CC_RASC, .c = GX_CC_KONST, .d = GX_CC_C1};
  stage.alphaPass.d = GX_CA_A2;
  stage.kcSel = GX_TEV_KCSEL_K0;
  stage.channelId = GX_COLOR0A0;
  config.colorChannels[GX_COLOR0].matSrc = GX_SRC_VTX;
  config.attrs[GX_VA_CLR0] = {.attrType = GX_INDEX8, .cnt = 4, .compType = GX_RGBA8,
                            .offset = 0, .stride = 4};
  return config;
}

ShaderConfig fog_config() {
  auto config = normal_config();
  config.fogType = GX_FOG_PERSP_LIN;
  config.fogRangeEnabled = true;
  return config;
}

ShaderConfig shadow_config() {
  auto config = fog_config();
  config.shadow = true;
  config.pbr = 1;
  config.pbrCut = true;
  config.pbrBindPos = true;
  config.attrs[GX_VA_NRM] = {.attrType = GX_INDEX16, .cnt = 9, .compType = GX_F32,
                           .offset = 1, .stride = 36};
  config.attrs[GX_VA_TEX0] = {.attrType = GX_DIRECT, .cnt = 2, .compType = GX_F32, .stride = 8};
  config.tcgs[0].src = GX_TG_TEX0;
  auto& stage = config.tevStages[0];
  stage.texMapId = GX_TEXMAP0;
  stage.texCoordId = GX_TEXCOORD0;
  stage.colorPass = {.b = GX_CC_TEXC, .c = GX_CC_RASC};
  stage.alphaPass = {.b = GX_CA_TEXA, .c = GX_CA_RASA};
  config.colorChannels[GX_COLOR0].lightingEnabled = true;
  return config;
}

class H700ShaderGenerator : public ::testing::Test {
protected:
  void SetUp() override {
    oldTextureFetch = gfx::g_vertexFetchTexture;
    oldLightmap = webgpu::g_lightmapBinding;
    gfx::g_vertexFetchTexture = gfx::g_mixedFetch;
    webgpu::g_lightmapBinding = false;
  }
  void TearDown() override {
    gfx::g_vertexFetchTexture = oldTextureFetch;
    webgpu::g_lightmapBinding = oldLightmap;
  }
  bool oldTextureFetch;
  bool oldLightmap;
};

TEST_F(H700ShaderGenerator, ProductionCacheNamespace) {
  EXPECT_EQ(gfx::detail::pipeline_cache_path("/cache"),
            gfx::g_mixedFetch ? "/cache/pipeline_cache.h700-mixed-v1.db" : "/cache/pipeline_cache.db");
  EXPECT_EQ(gfx::detail::pipeline_cache_path("/another/frontend/"),
            gfx::g_mixedFetch ? "/another/frontend/pipeline_cache.h700-mixed-v1.db"
                              : "/another/frontend/pipeline_cache.db");
#ifdef AURORA_H700
  // Also pin the production open/cleanup and legacy-seed hooks to that namespace.
  std::ifstream file(std::string{H700_SHADER_SOURCE_DIR} + "/gfx/pipeline_cache.cpp");
  ASSERT_TRUE(file.good());
  const std::string source{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  EXPECT_NE(source.find("return detail::pipeline_cache_path(g_config.cachePath);"), std::string::npos);
  EXPECT_NE(source.find("sqlite::open_cache_db(path, &g_pipelineCacheDb, Log)"), std::string::npos);
  EXPECT_NE(source.find("sqlite::delete_db_files(pipeline_cache_path(), Log)"), std::string::npos);
  EXPECT_NE(source.find("if (g_mixedFetch || g_config.pipelineCacheNoSeed"), std::string::npos);
#endif
}

#ifdef AURORA_H700
void expect_mixed_module(const std::string& source) {
  EXPECT_NE(source.find("@group(0) @binding(0)\nvar vbuf: texture_2d<u32>;"), std::string::npos);
  EXPECT_NE(source.find("@group(0) @binding(1)\nvar abuf: texture_2d<u32>;"), std::string::npos);
  EXPECT_NE(source.find("@group(0) @binding(2)\nvar<storage, read> fs_abuf: array<u32>;"), std::string::npos);
  EXPECT_EQ(source.find("&vbuf"), std::string::npos);
  EXPECT_EQ(source.find("&abuf"), std::string::npos);
  EXPECT_EQ(source.find("abuf["), std::string::npos);
  EXPECT_NE(source.find("fn load_word(p: texture_2d<u32>, word_idx: u32)"), std::string::npos);
  EXPECT_NE(source.find("fn raw_fetch_u16_1(p: texture_2d<u32>"), std::string::npos);
  EXPECT_NE(source.find("fn fetch_s16_1(p: texture_2d<u32>"), std::string::npos);
  EXPECT_NE(source.find("fn fs_raw_fetch_u16_1(p: ptr<storage, array<u32>>"), std::string::npos);
  EXPECT_NE(source.find("fn fs_fetch_rgba8(p: ptr<storage, array<u32>>"), std::string::npos);
  EXPECT_NE(source.find("let dims = textureDimensions(p);\n  if (word_idx < dims.x * dims.y)"), std::string::npos);
  EXPECT_NE(source.find("textureLoad(p, vec2u(word_idx & 4095u, word_idx >> 12u), 0).x"), std::string::npos);

  const auto fsHelpers = source.find("fn fs_load_word(");
  const auto sharedTail = source.find("fn tev_overflow_f32(");
  ASSERT_NE(fsHelpers, std::string::npos);
  ASSERT_NE(sharedTail, std::string::npos);
  const auto fragmentHelpers = source.substr(fsHelpers, sharedTail - fsHelpers);
  EXPECT_NE(fragmentHelpers.find("if (word_idx < arrayLength(p)) {\n    return p[word_idx];\n  }\n  return 0u;"),
            std::string::npos);
  EXPECT_EQ(fragmentHelpers.find("texture"), std::string::npos);
  EXPECT_NE(fragmentHelpers.find("let hi = fs_load_word(p, word_idx + 1u);"), std::string::npos);
  EXPECT_NE(fragmentHelpers.find("return fs_load_f32(p, byte_off, le);"), std::string::npos);
  EXPECT_EQ(fragmentHelpers.find("return load_"), std::string::npos);

  const auto vs = source.find("fn vs_main(");
  const auto fs = source.find("@fragment\nfn fs_main(");
  ASSERT_NE(vs, std::string::npos);
  ASSERT_NE(fs, std::string::npos);
  const auto vertex = source.substr(vs, fs - vs);
  EXPECT_NE(vertex.find("raw_fetch_u16_1(vbuf,"), std::string::npos);
  EXPECT_NE(vertex.find("fetch_s16_1(abuf,"), std::string::npos);
  EXPECT_NE(vertex.find("fetch_rgba8(abuf,"), std::string::npos);
  EXPECT_EQ(vertex.find("fs_abuf"), std::string::npos);
  EXPECT_EQ(vertex.find("fs_load_"), std::string::npos);
  // All current user varyings are float scalars/vectors. Do not flatten or change
  // their sampling to solve Compatibility's rule for integer/flat varyings.
  const auto outStart = source.find("struct VertexOutput {");
  const auto outEnd = source.find("};", outStart);
  ASSERT_NE(outStart, std::string::npos);
  ASSERT_NE(outEnd, std::string::npos);
  const auto varyings = source.substr(outStart, outEnd - outStart);
  EXPECT_EQ(varyings.find("interpolate"), std::string::npos);
  EXPECT_EQ(varyings.find(": u32"), std::string::npos);
  EXPECT_EQ(varyings.find(": vec4u"), std::string::npos);
}

TEST_F(H700ShaderGenerator, NormalUsesBothVertexMirrorsAndKeepsTevUniforms) {
  const auto source = build_shader_source(normal_config());
  expect_mixed_module(source);
  EXPECT_NE(source.find("var tevreg0 = ubuf.tevreg0;"), std::string::npos);
  EXPECT_NE(source.find("var tevreg1 = ubuf.tevreg1;"), std::string::npos);
  EXPECT_NE(source.find("var tevreg2 = ubuf.tevreg2;"), std::string::npos);
  const auto fs = source.substr(source.find("fn fs_main("));
  EXPECT_EQ(fs.find("load_word("), std::string::npos);
  EXPECT_EQ(fs.find("vbuf"), std::string::npos);
  EXPECT_EQ(fs.find("fetch_"), std::string::npos);
}

TEST_F(H700ShaderGenerator, FogRangeUsesOriginalFragmentStorageWithZeroBounds) {
  const auto source = build_shader_source(fog_config());
  expect_mixed_module(source);
  EXPECT_NE(source.find("fogBase *= bitcast<f32>(fs_load_word(&fs_abuf, imm.fog_range_base + u32(in.pos.x)));"),
            std::string::npos);
  EXPECT_EQ(source.find("p[min(word_idx"), std::string::npos);
}

TEST_F(H700ShaderGenerator, ShadowAndCutCasterKeepStageSpecificAccess) {
  const auto source = build_shader_source(shadow_config());
  expect_mixed_module(source);
  const auto shadowVs = source.find("fn vs_shadow(");
  const auto cutVs = source.find("fn vs_shadow_cut(");
  const auto shadowFs = source.find("fn fs_shadow(");
  ASSERT_NE(shadowVs, std::string::npos);
  ASSERT_NE(cutVs, std::string::npos);
  ASSERT_NE(shadowFs, std::string::npos);
  const auto shadowVertex = source.substr(shadowVs, cutVs - shadowVs);
  EXPECT_NE(shadowVertex.find("fetch_s16_1(abuf,"), std::string::npos);
  EXPECT_NE(shadowVertex.find("raw_fetch_u16_1(vbuf,"), std::string::npos);
  EXPECT_EQ(shadowVertex.find("fs_abuf"), std::string::npos);
  const auto cutVertex = source.substr(cutVs, shadowFs - cutVs);
  EXPECT_NE(cutVertex.find("raw_fetch_f32_1(abuf,"), std::string::npos); // bind-pose array
  EXPECT_EQ(cutVertex.find("fs_abuf"), std::string::npos);
  EXPECT_NE(source.substr(shadowFs).find("discard;"), std::string::npos);
  EXPECT_EQ(source.substr(shadowFs).find("vbuf"), std::string::npos);
  EXPECT_EQ(source.substr(shadowFs).find("textureDimensions"), std::string::npos);

  // Every room-light component is loaded from the actual fragment SSBO; none
  // may silently go back to the vertex-only array mirror.
  for (u32 row = 0; row < 4; ++row) {
    std::string expected = "let r" + std::to_string(row) + " = bitcast<vec4f>(vec4u(";
    for (u32 col = 0; col < 4; ++col) {
      const u32 offset = row * 4 + col;
      if (col != 0) {
        expected += ", ";
      }
      expected += "fs_load_word(&fs_abuf, rb";
      if (offset != 0) {
        expected += " + " + std::to_string(offset) + "u";
      }
      expected += ")";
    }
    expected += "));";
    EXPECT_NE(source.find(expected), std::string::npos) << expected;
  }
}

TEST_F(H700ShaderGenerator, MixedContractDoesNotDependOnRuntimeTextureFlag) {
  gfx::g_vertexFetchTexture = false;
  expect_mixed_module(build_shader_source(fog_config()));
}
#else
TEST_F(H700ShaderGenerator, NonH700GeneratedContract) {
  // Baselines captured from e600cf150's actual generator with the same configs.
  const std::array configs{normal_config(), fog_config(), shadow_config()};
  const std::array<uint64_t, 6> expected{
        11427147837321265010ull, 3574029196174419617ull, 7504903709510261083ull,
        4054525701515668604ull, 10552335145920386854ull, 13770224563110124249ull};
  size_t i = 0;
  for (bool textureFetch : {false, true}) {
    gfx::g_vertexFetchTexture = textureFetch;
    for (const auto& config : configs) {
      const auto source = build_shader_source(config);
      EXPECT_EQ(xxh3_hash_s(source.data(), source.size()), expected[i]) << "baseline " << i;
      ++i;
    }
  }
}
#endif

} // namespace
} // namespace aurora::gx
