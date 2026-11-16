#include "gfx/resources.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>

namespace aurora::gfx {
namespace {
using namespace detail;

TEST(H700MixedFetch, ActualGroupZeroDescriptors) {
  static_assert(g_mixedFetch);
  // H700 cannot accidentally revert to SSBO vertex fetch, even if a caller passes false.
  for (const bool textureFetch : {false, true}) {
    const auto entries = static_bind_group_layout_entries(textureFetch);
    ASSERT_EQ(entries.size(), 3);
    for (uint32_t i = 0; i < 2; ++i) {
      EXPECT_EQ(entries[i].binding, i);
      EXPECT_EQ(entries[i].visibility, wgpu::ShaderStage::Vertex);
      EXPECT_EQ(entries[i].texture.sampleType, wgpu::TextureSampleType::Uint);
      EXPECT_EQ(entries[i].texture.viewDimension, wgpu::TextureViewDimension::e2D);
      EXPECT_EQ(entries[i].buffer.type, wgpu::BufferBindingType::BindingNotUsed);
    }
    EXPECT_EQ(entries[2].binding, MixedFragmentStorageBinding);
    EXPECT_EQ(entries[2].visibility, wgpu::ShaderStage::Fragment);
    EXPECT_EQ(entries[2].buffer.type, wgpu::BufferBindingType::ReadOnlyStorage);
    EXPECT_FALSE(entries[2].buffer.hasDynamicOffset);
    EXPECT_EQ(entries[2].texture.sampleType, wgpu::TextureSampleType::BindingNotUsed);
    EXPECT_EQ(sampled_texture_count(entries, wgpu::ShaderStage::Vertex), 2);
    EXPECT_EQ(sampled_texture_count(entries, wgpu::ShaderStage::Fragment), 0);
  }
}

TEST(H700MixedFetch, TextureBudgetCountsDescriptorsNotSamplersOrStorage) {
  auto entries = static_bind_group_layout_entries(true);
  entries[0].visibility |= wgpu::ShaderStage::Fragment;
  EXPECT_EQ(sampled_texture_count(entries, wgpu::ShaderStage::Fragment), 1);
  entries[0].texture = wgpu::BindGroupLayoutEntry{}.texture;
  entries[0].sampler = {.type = wgpu::SamplerBindingType::Filtering};
  EXPECT_EQ(sampled_texture_count(entries, wgpu::ShaderStage::Fragment), 0);
}

TEST(H700MixedFetch, RequestRequiresFragmentStorageEvenWithoutCompute) {
  for (const bool noCompute : {false, true}) {
    wgpu::Limits limits{};
    limits.maxStorageBufferBindingSize = StorageBufferSize * 2;
    limits.maxBufferSize = MixedMinimumBufferSize;
    wgpu::CompatibilityModeLimits compatibility{wgpu::CompatibilityModeLimits::Init{}};
    request_mixed_fetch_limits(limits, compatibility, noCompute);
    EXPECT_EQ(limits.maxSampledTexturesPerShaderStage, 16);
    EXPECT_EQ(limits.maxStorageBuffersPerShaderStage, noCompute ? 1u : 2u);
    EXPECT_EQ(compatibility.maxStorageBuffersInVertexStage, 0);
    EXPECT_EQ(compatibility.maxStorageBuffersInFragmentStage, 1);
    EXPECT_EQ(limits.maxStorageBufferBindingSize, StorageBufferSize * 2);
    EXPECT_EQ(limits.maxBufferSize, MixedMinimumBufferSize);
    limits.maxTextureDimension2D = 4096;
    EXPECT_TRUE(mixed_fetch_limits_supported(limits, compatibility, noCompute));
  }
}

TEST(H700MixedFetch, RejectsInsufficientOrUndefinedLimits) {
  static_assert(StorageBufferSize == 8u * 1024 * 1024);
  static_assert(MixedMinimumBufferSize == 63u * 1024 * 1024);
  wgpu::Limits limits{};
  limits.maxStorageBufferBindingSize = StorageBufferSize;
  limits.maxBufferSize = MixedMinimumBufferSize;
  limits.maxTextureDimension2D = 4096;
  wgpu::CompatibilityModeLimits compatibility{wgpu::CompatibilityModeLimits::Init{}};
  request_mixed_fetch_limits(limits, compatibility, true);
  ASSERT_TRUE(mixed_fetch_limits_supported(limits, compatibility, true));
  const auto rejects = [&](auto member, auto value) {
    auto bad = limits;
    bad.*member = value;
    EXPECT_FALSE(mixed_fetch_limits_supported(bad, compatibility, true));
  };
  rejects(&wgpu::Limits::maxSampledTexturesPerShaderStage, 15u);
  rejects(&wgpu::Limits::maxSampledTexturesPerShaderStage, WGPU_LIMIT_U32_UNDEFINED);
  rejects(&wgpu::Limits::maxStorageBuffersPerShaderStage, 0u);
  rejects(&wgpu::Limits::maxStorageBuffersPerShaderStage, WGPU_LIMIT_U32_UNDEFINED);
  rejects(&wgpu::Limits::maxStorageBufferBindingSize, uint64_t{65536});
  rejects(&wgpu::Limits::maxStorageBufferBindingSize, WGPU_LIMIT_U64_UNDEFINED);
  rejects(&wgpu::Limits::maxBufferSize, MixedMinimumBufferSize - 1);
  rejects(&wgpu::Limits::maxBufferSize, WGPU_LIMIT_U64_UNDEFINED);
  rejects(&wgpu::Limits::maxTextureDimension2D, 4095u);
  rejects(&wgpu::Limits::maxTextureDimension2D, WGPU_LIMIT_U32_UNDEFINED);
  for (const uint32_t count : {0u, WGPU_LIMIT_U32_UNDEFINED}) {
    compatibility.maxStorageBuffersInFragmentStage = count;
    EXPECT_FALSE(mixed_fetch_limits_supported(limits, compatibility, true));
  }
  compatibility.maxStorageBuffersInFragmentStage = 1;
  compatibility.maxStorageBuffersInVertexStage = WGPU_LIMIT_U32_UNDEFINED;
  EXPECT_FALSE(mixed_fetch_limits_supported(limits, compatibility, true));
}

std::string source(const char* path) {
  std::ifstream file{std::string{H700_MIXED_SOURCE_DIR} + path};
  EXPECT_TRUE(file.good()) << path;
  return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::string section(const std::string& text, const char* begin, const char* end) {
  const auto first = text.find(begin);
  EXPECT_NE(first, std::string::npos) << begin;
  if (first == std::string::npos) return {};
  const auto last = text.find(end, first);
  EXPECT_NE(last, std::string::npos) << end;
  return text.substr(first, last - first);
}

void contains(const std::string& text, const char* expected) {
  EXPECT_NE(text.find(expected), std::string::npos) << expected;
}

TEST(H700MixedFetch, SourceCurrentBufferAndMirrorLifecycle) {
  const auto frame = source("/gfx/frame.cpp");
  const auto binding = section(frame, "void create_static_bind_group()", "void create_uniform_bind_group()");
  contains(binding, "entries[0].textureView = g_resources.vertexFetchTextureView;");
  contains(binding, "entries[1].textureView = g_resources.storageFetchTextureView;");
  contains(binding, "entries[MixedFragmentStorageBinding].buffer = g_resources.storageBuffer;");
  contains(binding, ".entryCount = entries.size()");
  contains(frame, ".format = wgpu::TextureFormat::R32Uint");
  contains(frame, "return AURORA_ALIGN(bytes, FetchTextureRowBytes)");
  const auto grow = section(frame, "bool grow_frame_buffer(", "bool grow_staging(");
  contains(grow, "encoder.CopyBufferToBuffer(current, 0, buffer, 0, base + device)");
  EXPECT_LT(grow.find("current = std::move(buffer)"), grow.find("create_static_bind_group()"));
  contains(grow, "create_fetch_texture(kind, base + grown)");
  contains(grow, "upload_fetch_texture_rows(encoder, kind, 0, base + grown)");
  contains(grow, "expire_unused_bind_groups_soon()");
  for (const auto& block : {section(frame, "void initialize()", "bool shutdown()"),
                           section(frame, "bool reserve_held_frame_buffers()", "bool begin_frame()")}) {
    contains(block, "upload_fetch_texture_rows(encoder, StagingKind::Vertex, 0, vertexBytes)");
    contains(block, "upload_fetch_texture_rows(encoder, StagingKind::Storage, 0, storageBytes)");
    contains(block, "create_static_bind_group()");
    contains(block, "validate_mixed_buffer_size(");
  }
  const auto encoding = source("/gfx/encoding.cpp");
  contains(encoding, "frame.spillStorage, res.storageBuffer");
  contains(encoding, "cmd.CopyBufferToBuffer(item.src, 0, dst, item.dstOffset, item.size)");
  contains(encoding, "storageWritten.add(item.dstOffset, item.size)");
  contains(encoding, "upload_fetch_texture_rows(cmd, StagingKind::Storage, storageWritten.lo, storageWritten.hi)");
  contains(encoding, "upload_fetch_texture_rows(cmd, StagingKind::Vertex, vertexWritten.lo, vertexWritten.hi)");
}

TEST(H700MixedFetch, SourceActualLayoutsAndFinalDeviceGate) {
  const auto gx = source("/gx/gx.cpp");
  contains(gx, "kTextureBindings - (webgpu::g_lightmapBinding ? 0 : 1)");
  contains(gx, "validateMixedTextureBudget({textureEntries.data(), texture_binding_count()}, \"normal\")");
  contains(gx, "validateMixedTextureBudget({textureEntries.data(), texture_binding_count()}, \"shadow receiver\")");
  contains(gx, "fragmentTextures != gfx::MixedSampledTextureBudget");
  const auto gpu = source("/webgpu/gpu.cpp");
  contains(gpu, "gfx::g_mixedFetch || forced || adapterHasNone");
  contains(gpu, "gfx::g_mixedFetch || !gfx::g_vertexFetchTexture || !gfx::g_noCompute");
  contains(gpu, "g_lightmapBinding = requiredLimits.maxSampledTexturesPerShaderStage == 17");
  const auto finalGate = section(gpu, "wgpu::Limits grantedLimits", "g_device.SetLoggingCallback");
  contains(finalGate, "g_device.GetLimits(&grantedLimits) != wgpu::Status::Success");
  contains(finalGate, "mixed_fetch_limits_supported(grantedLimits, grantedCompatibility, gfx::g_noCompute)");
  contains(finalGate, "grantedCompatibility.maxStorageBuffersInVertexStage != 0");
  contains(finalGate, "grantedLimits.maxStorageBufferBindingSize < requiredLimits.maxStorageBufferBindingSize");
  contains(finalGate, "grantedLimits.maxBufferSize < requiredLimits.maxBufferSize");
  contains(finalGate, "return false;");
}
} // namespace
} // namespace aurora::gfx
