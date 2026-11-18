#include "gfx/resources.hpp"
#include "gx/pipeline.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <optional>
#include <vector>
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
    ASSERT_EQ(entries.size(), 4);
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
    EXPECT_EQ(entries[3].binding, MixedFetchSamplerBinding);
    EXPECT_EQ(entries[3].visibility, wgpu::ShaderStage::Vertex);
    EXPECT_EQ(entries[3].sampler.type, wgpu::SamplerBindingType::NonFiltering);
    EXPECT_EQ(entries[3].texture.sampleType, wgpu::TextureSampleType::BindingNotUsed);
    EXPECT_EQ(entries[3].buffer.type, wgpu::BufferBindingType::BindingNotUsed);
    for (const auto stage : {wgpu::ShaderStage::Vertex, wgpu::ShaderStage::Fragment}) {
      uint32_t samplerCount = 0;
      for (const auto& entry : entries) {
        if ((entry.visibility & stage) != wgpu::ShaderStage::None &&
            entry.sampler.type != wgpu::SamplerBindingType::BindingNotUsed) ++samplerCount;
      }
      EXPECT_EQ(samplerCount, stage == wgpu::ShaderStage::Vertex ? 1u : 0u);
    }
    EXPECT_EQ(sampled_texture_count(entries, wgpu::ShaderStage::Vertex), 2);
    EXPECT_EQ(sampled_texture_count(entries, wgpu::ShaderStage::Fragment), 0);
  }
}

TEST(H700MixedFetch, GatherSamplerIsNearestNonComparisonClampAtMipZero) {
  const auto desc = fetch_sampler_descriptor();
  EXPECT_EQ(desc.addressModeU, wgpu::AddressMode::ClampToEdge);
  EXPECT_EQ(desc.addressModeV, wgpu::AddressMode::ClampToEdge);
  EXPECT_EQ(desc.addressModeW, wgpu::AddressMode::ClampToEdge);
  EXPECT_EQ(desc.magFilter, wgpu::FilterMode::Nearest);
  EXPECT_EQ(desc.minFilter, wgpu::FilterMode::Nearest);
  EXPECT_EQ(desc.mipmapFilter, wgpu::MipmapFilterMode::Nearest);
  EXPECT_EQ(desc.lodMinClamp, 0.f);
  EXPECT_EQ(desc.lodMaxClamp, 0.f);
  EXPECT_EQ(desc.compare, wgpu::CompareFunction::Undefined);
  EXPECT_EQ(desc.maxAnisotropy, 1);
}

// CPU model of the WGSL gather footprint, not of textureLoad's integer addressing.
// .w selects floor(uv * dimensions - 0.5), with the sampler's edge clamp.
std::optional<std::array<uint32_t, 2>> gather_coordinates(uint32_t word, uint32_t height) {
  constexpr uint32_t width = 4096;
  if (word >= width * height) return std::nullopt;
  const float u = (static_cast<float>(word & 4095u) + 1.f) / static_cast<float>(width);
  const float v = (static_cast<float>(word >> 12u) + 1.f) / static_cast<float>(height);
  const auto x = static_cast<int32_t>(std::floor(u * static_cast<float>(width) - 0.5f));
  const auto y = static_cast<int32_t>(std::floor(v * static_cast<float>(height) - 0.5f));
  return std::array{static_cast<uint32_t>(std::clamp(x, 0, static_cast<int32_t>(width - 1))),
                    static_cast<uint32_t>(std::clamp(y, 0, static_cast<int32_t>(height - 1)))};
}

TEST(H700MixedFetch, GatherCoordinatesAndU32ValuesMatchPointReadsIncludingBounds) {
  constexpr uint32_t width = 4096;
  std::vector<uint32_t> patterns{0u, 1u, 0x7f800000u, 0x7f800001u, 0x7fc00000u, 0x80000000u,
                                 0xffffffffu, 0xaaaaaaaau, 0x55555555u, 0xdeadbeefu};
  for (uint32_t bit = 0; bit < 32; ++bit) {
    patterns.push_back(1u << bit);
    patterns.push_back(~(1u << bit));
  }
  const auto texel = [](uint32_t x, uint32_t y, uint32_t pattern) {
    return pattern ^ std::rotl(x * 0x9e3779b9u, 7) ^ std::rotl(y * 0x85ebca6bu, 19);
  };
  for (const uint32_t height : {1u, 2u, 3u, 7u, 255u, 256u, 257u, 511u, 512u, 513u, 4095u, 4096u}) {
    SCOPED_TRACE(height);
    const uint32_t count = width * height;
    // Inspect every row, at both edges and in the middle; odd heights exercise the FP division.
    for (uint32_t y = 0; y < height; ++y) {
      for (const uint32_t x : {0u, 1u, 2047u, 4094u, 4095u}) {
        const auto coordinates = gather_coordinates(y * width + x, height);
        ASSERT_TRUE(coordinates.has_value());
        EXPECT_EQ((*coordinates)[0], x);
        EXPECT_EQ((*coordinates)[1], y);
      }
    }
    for (const uint32_t word : {0u, 4095u, 4096u, count - 1, count, count + 1, UINT32_MAX}) {
      SCOPED_TRACE(word);
      const auto coordinates = gather_coordinates(word, height);
      EXPECT_EQ(coordinates.has_value(), word < count);
      for (const uint32_t pattern : patterns) {
        const uint32_t gathered = coordinates ? texel((*coordinates)[0], (*coordinates)[1], pattern) : 0u;
        const uint32_t point = word < count ? texel(word % width, word / width, pattern) : 0u;
        EXPECT_EQ(gathered, point) << pattern;
      }
    }
    // At (0,0) the address salt is zero, including patterns that would be NaNs if reinterpreted as f32.
    for (const uint32_t pattern : patterns) {
      const auto coordinates = gather_coordinates(0, height);
      ASSERT_TRUE(coordinates.has_value());
      EXPECT_EQ(texel((*coordinates)[0], (*coordinates)[1], pattern), pattern);
    }
  }
}

TEST(H700MixedFetch, DrawUniformUsesTwoDynamicWindowsWithoutChangingLegacyLayout) {
  const auto legacy = uniform_bind_group_layout_entries();
  const auto candidate = uniform_bind_group_layout_entries<true>(sizeof(gx::DrawImmediateData));
  ASSERT_EQ(legacy.size(), 1);
  ASSERT_EQ(candidate.size(), 2);
  EXPECT_EQ(legacy[0].binding, 0);
  EXPECT_EQ(legacy[0].buffer.minBindingSize, 0);
  for (const auto& entries : {std::span<const wgpu::BindGroupLayoutEntry>{legacy},
                             std::span<const wgpu::BindGroupLayoutEntry>{candidate}}) {
    for (size_t i = 0; i < entries.size(); ++i) {
      EXPECT_EQ(entries[i].binding, i);
      EXPECT_EQ(entries[i].visibility, wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment);
      EXPECT_EQ(entries[i].buffer.type, wgpu::BufferBindingType::Uniform);
      EXPECT_TRUE(entries[i].buffer.hasDynamicOffset);
      EXPECT_EQ(entries[i].texture.sampleType, wgpu::TextureSampleType::BindingNotUsed);
      EXPECT_EQ(entries[i].sampler.type, wgpu::SamplerBindingType::BindingNotUsed);
    }
  }
  EXPECT_EQ(candidate[0].buffer.minBindingSize, legacy[0].buffer.minBindingSize);
  EXPECT_EQ(candidate[1].buffer.minBindingSize, 64);
}

TEST(H700MixedFetch, DrawUniformRejectsMissingMisalignedOrOutOfBoundsSnapshots) {
  gx::DrawData draw{};
  EXPECT_TRUE(gx::draw_uniform_range_valid(draw, 1024, 256));
  draw.drawUniform = true;
  EXPECT_FALSE(gx::draw_uniform_range_valid(draw, 1024, 256));
  draw.drawUniformRange = {256, 64};
  EXPECT_TRUE(gx::draw_uniform_range_valid(draw, 320, 256));
  EXPECT_FALSE(gx::draw_uniform_range_valid(draw, 319, 256));
  EXPECT_FALSE(gx::draw_uniform_range_valid(draw, 1024, 0));
  draw.shadowGroup = true;
  EXPECT_FALSE(gx::draw_uniform_range_valid(draw, 1024, 256));
  draw.shadowGroup = false;
  for (const gfx::Range bad : {gfx::Range{257, 64}, gfx::Range{256, 63}, gfx::Range{256, 65},
                              gfx::Range{UINT32_MAX, 0}, gfx::Range{UINT32_MAX, 64}}) {
    draw.drawUniformRange = bad;
    EXPECT_FALSE(gx::draw_uniform_range_valid(draw, 1024, 256));
  }
  draw.drawUniformRange = {256, 64};
  draw.drawUniform = false;
  EXPECT_FALSE(gx::draw_uniform_range_valid(draw, 1024, 256));
  gx::clear_draw_uniform(draw);
  EXPECT_TRUE(gx::draw_uniform_range_valid(draw, 1024, 256));
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

TEST(H700MixedFetch, SourceGatherSamplerFollowsCurrentAliasesAndKnownRelease) {
  const auto frame = source("/gfx/frame.cpp");
  const auto binding = section(frame, "void create_static_bind_group()", "void create_uniform_bind_group()");
  contains(binding, "std::array<wgpu::BindGroupEntry, g_mixedFetch ? 4 : 2>");
  contains(binding, "entries[MixedFetchSamplerBinding].sampler = g_resources.fetchSampler;");
  EXPECT_EQ(binding.find("CreateSampler"), std::string::npos);
  const auto init = section(frame, "void initialize()", "bool shutdown()");
  contains(init, "const auto fetchSamplerDescriptor = fetch_sampler_descriptor();");
  contains(init, "g_resources.fetchSampler = g_device.CreateSampler(&fetchSamplerDescriptor);");
  EXPECT_LT(init.find("g_resources.fetchSampler ="), init.find("create_static_bind_group()"));
  EXPECT_LT(init.find("create_static_bind_group()"), init.find("initialize_pipeline_cache()"));
  EXPECT_EQ(init.find("fetch_gather_enabled"), std::string::npos);
  for (const auto& block : {section(frame, "bool grow_frame_buffer(", "bool grow_staging("),
                           section(frame, "bool reserve_held_frame_buffers()", "bool begin_frame()")}) {
    contains(block, "create_static_bind_group()");
    EXPECT_EQ(block.find("fetchSampler ="), std::string::npos);
    EXPECT_EQ(block.find("CreateSampler"), std::string::npos);
  }
  const auto retirement = section(frame, "bool release_frame_holders_after_quiescence() {", "bool wait_for_staging_buffer(");
  contains(retirement, "!gx::fifo::is_quiescent()");
  contains(retirement, "!render_worker::is_quiescent()");
  contains(retirement, "!pipeline_cache_threads_joined()");
  contains(retirement, "pending_internal_maps() != 0) return false;");
  contains(retirement, "g_resources.fetchSampler = {};");
  EXPECT_LT(retirement.find("if (!release_pipeline_cache_holders()) return false;"),
            retirement.find("g_resources.fetchSampler = {}"));
  EXPECT_LT(retirement.find("gx::shutdown()"), retirement.find("g_resources.fetchSampler = {}"));
  EXPECT_LT(retirement.find("g_resources.staticBindGroup = {}"), retirement.find("g_resources.fetchSampler = {}"));
  const auto gx = source("/gx/gx.cpp");
  const auto gate = section(gx, "bool fetch_gather_enabled() noexcept {", "#endif");
  contains(gate, "static const bool enabled = fetch_gather_requested(std::getenv(\"MP_H700_FETCH_GATHER\"), std::getenv(\"MP_H700_BENCH\"))");
  contains(gx, "config.shaderConfig.fetchGather = fetch_gather_enabled()");
  const auto processor = source("/gx/command_processor.cpp");
  const auto producer = section(processor, "static void push_gx_draw(", "static void handle_draw_unmerged(");
  contains(producer, "cache.config.shaderConfig.fetchGather == fetch_gather_enabled()");
  const auto merge = section(processor, "const bool canMerge =", "handle_draw_unmerged(prim, fmt, vtxCount, vertRange);");
  contains(merge, "sDrawCache.config.shaderConfig.fetchGather == fetch_gather_enabled()");
}

TEST(H700MixedFetch, SourceDrawUniformAliasesFollowExistingBufferLifetime) {
  const auto frame = source("/gfx/frame.cpp");
  const auto binding = section(frame, "void create_uniform_bind_group()", "struct RuntimeDrawType");
  contains(binding, ".size = gx::MaxUniformSize");
  contains(binding, "entries[0]");
  contains(binding, ".binding = 1");
  contains(binding, ".buffer = g_resources.uniformBuffer");
  contains(binding, ".size = sizeof(gx::DrawImmediateData)");
  contains(binding, ".layout = g_resources.drawUniformBindGroupLayout");
  contains(binding, "g_resources.drawUniformBindGroup = g_device.CreateBindGroup(&drawDescriptor)");
  contains(frame, "uniform_bind_group_layout_entries<true>(sizeof(gx::DrawImmediateData))");
  for (const auto& block : {section(frame, "bool grow_frame_buffer(", "bool grow_staging("),
                           section(frame, "bool reserve_held_frame_buffers()", "bool begin_frame()")}) {
    contains(block, "create_uniform_bind_group()");
  }
  const auto retirement = section(frame, "bool release_frame_holders_after_quiescence() {", "bool wait_for_staging_buffer(");
  contains(retirement, "!gx::fifo::is_quiescent()");
  contains(retirement, "pending_internal_maps() != 0");
  contains(retirement, "g_resources.drawUniformBindGroup = {}");
  contains(retirement, "g_resources.drawUniformBindGroupLayout = {}");
  const auto gx = source("/gx/gx.cpp");
  contains(gx, "gfx::detail::resources().drawUniformBindGroupLayout");
  const auto layout = section(gx, ".label = \"GX Draw Uniform Pipeline Layout\"", "sDrawUniformPipelineLayout =");
  contains(layout, ".immediateSize = 0");
  contains(gx, "sDrawUniformPipelineLayout = {}");
  contains(gx, "uses_draw_uniform(config.shaderConfig)\n                    ? sDrawUniformPipelineLayout");
  const auto gate = section(gx, "bool draw_uniform_enabled() noexcept {", "#endif");
  contains(gate, "draw_uniform_requested(std::getenv(\"MP_H700_DRAW_UBO\"), std::getenv(\"MP_H700_BENCH\"))");
}

TEST(H700MixedFetch, SourceDrawUniformSnapshotsAreFinalPerDrawNotCachedMaterialData) {
  const auto processor = source("/gx/command_processor.cpp");
  const auto producer = section(processor, "static void push_gx_draw(", "static void handle_draw_unmerged(");
  const auto snapshot = producer.find("drawUniformRange = gfx::push_uniform(immediates)");
  ASSERT_NE(snapshot, std::string::npos);
  EXPECT_LT(producer.find("const bool uniformValid ="), snapshot);
  EXPECT_LT(producer.find("immediates.fogRangeBase = cache.fogRange.offset / sizeof(u32)"), snapshot);
  EXPECT_LT(producer.find("if (cache.prepassSkip)"), snapshot);
  EXPECT_LT(snapshot, producer.find("const DrawData draw"));
  EXPECT_LT(snapshot, producer.find("gfx::push_draw_command(draw)"));
  contains(producer, "drop_overflowed_draw(drawUniformRange, \"draw uniform\")");
  contains(producer, "AURORA_ASSERT(drawUniformRange.size == sizeof(immediates)");
  contains(producer, ".drawUniformRange = drawUniformRange");
  contains(producer, "cache.config.shaderConfig.drawUniform == draw_uniform_for_state(cache.config.shaderConfig)");
  contains(producer, "config.shaderConfig.drawUniform = 0");
  contains(producer, "actorConfig.shaderConfig.drawUniform = 0");
  contains(producer, "clear_draw_uniform(caster)");
  const auto merge = section(processor, "const bool canMerge =", "handle_draw_unmerged(prim, fmt, vtxCount, vertRange);");
  contains(merge, "lastDraw->drawUniform == uses_draw_uniform(sDrawCache.config.shaderConfig)");
  contains(merge, "lastDraw->drawUniform == draw_uniform_for_state(sDrawCache.config.shaderConfig)");
  EXPECT_EQ(merge.find("push_uniform"), std::string::npos);
  EXPECT_EQ(merge.find("drawUniformRange ="), std::string::npos);
}

TEST(H700MixedFetch, SourceDrawUniformEncodingAndDerivedCastersKeepSeparateAbis) {
  const auto pipeline = source("/gx/pipeline.cpp");
  contains(pipeline, "AURORA_ASSERT(draw_uniform_range_valid(data");
  const auto candidate = section(pipeline, "if (data.drawUniform) {", "} else {");
  contains(candidate, "const std::array offsets{data.uniformRange.offset, data.drawUniformRange.offset}");
  contains(candidate, "pass.SetBindGroup(1, resources.drawUniformBindGroup");
  EXPECT_EQ(candidate.find("SetImmediates"), std::string::npos);
  EXPECT_EQ(pipeline.find("push_uniform"), std::string::npos);
  const auto legacy = section(pipeline, "} else {", "// A shadow receiver");
  contains(legacy, "pass.SetImmediates(0, &data.immediateData, sizeof(data.immediateData))");
  contains(legacy, "pass.SetBindGroup(1, resources.uniformBindGroup");
  for (const char* path : {"/gfx/shadow.cpp", "/gfx/actor_shadow.cpp"}) {
    const auto caster = source(path);
    contains(caster, "gx::clear_draw_uniform(caster)");
    contains(caster, "AURORA_ASSERT(!draw.drawUniform && draw.drawUniformRange == Range{}");
    contains(caster, "pass.SetImmediates(0, &immediates, sizeof(immediates))");
    contains(caster, "pass.SetBindGroup(1, resources.uniformBindGroup, 1, &draw.uniformRange.offset)");
    EXPECT_EQ(caster.find("push_uniform"), std::string::npos);
  }
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
