#include "gfx/resources.hpp"
#include "gx/pipeline.hpp"
#include "h700/cache_policy.hpp"
#include "h700/frame_dest_ring.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>
#include <iterator>
#include <string>

namespace aurora::gfx {
namespace {
using namespace detail;

TEST(H700MixedFetch, ProgramCacheRequiresExactBenchmarkOptIn) {
  using h700::program_cache_enabled;
  for (const char* value : std::array<const char*, 6>{nullptr, "", "0", "true", "01", "1 "}) {
    EXPECT_FALSE(program_cache_enabled(value, "1"));
    EXPECT_FALSE(program_cache_enabled(value, "menu"));
  }
  for (const char* bench : std::array<const char*, 6>{nullptr, "", "0", "true", "01", "menu "}) {
    EXPECT_FALSE(program_cache_enabled("1", bench));
  }
  EXPECT_TRUE(program_cache_enabled("1", "1"));
  EXPECT_TRUE(program_cache_enabled("1", "menu"));
}

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

FrameBufferSizes ring_test_sizes() {
  FrameBufferSizes sizes;
  sizes.deviceUniform = h700::FrameDestRingBufferBytes;
  sizes.deviceStorage = h700::FrameDestRingBufferBytes;
  return sizes;
}

wgpu::Limits ring_test_limits() {
  wgpu::Limits limits{};
  limits.maxBufferSize = MixedMinimumBufferSize;
  limits.maxStorageBufferBindingSize = StorageBufferSize;
  return limits;
}

std::optional<h700::FrameDescriptor> ring_native(uint64_t frame, uint8_t slot, uint64_t generation = 7) {
  return h700::FrameDescriptor{generation, frame, slot};
}

struct CpuDestResources {
  int uniformBuffer = -1;
  int storageBuffer = -1;
  int staticBindGroup = -1;
  int uniformBindGroup = -1;
  int drawUniformBindGroup = -1;
  int vertexBuffer = 100;
  int indexBuffer = 101;
  int vertexMirror = 102;
  int storageMirror = 103;
  int layout = 104;
  int sampler = 105;
};

TEST(H700FrameDestRing, RequiresExactBenchmarkOptIn) {
  for (const char* value : std::array<const char*, 7>{nullptr, "", "0", "true", "01", "1 ", "menu"}) {
    EXPECT_FALSE(h700::frame_dest_ring_requested(value, "1"));
    EXPECT_FALSE(h700::frame_dest_ring_requested(value, "menu"));
  }
  for (const char* bench : std::array<const char*, 7>{nullptr, "", "0", "true", "01", "menu ", "1 "}) {
    EXPECT_FALSE(h700::frame_dest_ring_requested("1", bench));
  }
  EXPECT_TRUE(h700::frame_dest_ring_requested("1", "1"));
  EXPECT_TRUE(h700::frame_dest_ring_requested("1", "menu"));
}

TEST(H700FrameDestRing, EligibilityUsesActualSizesAndPreservesTheLargerGrowthLimit) {
  const auto sizes = ring_test_sizes();
  const auto limits = ring_test_limits();
  const auto bytes = h700::FrameDestRingBufferBytes;
  using Reason = h700::FrameDestRingFallback;
  EXPECT_EQ(h700::frame_dest_ring_eligibility(true, 0, sizes, bytes, bytes, limits), Reason::None);
  EXPECT_EQ(sizes.maxUniform, UniformBufferSize);
  EXPECT_GT(sizes.maxUniform, sizes.deviceUniform);
  EXPECT_EQ(h700::frame_dest_ring_eligibility(false, 0, sizes, bytes, bytes, limits), Reason::UnsupportedProfile);
  for (const uint64_t actual : {uint64_t{0}, bytes - 4, bytes + 4, UniformBufferSize}) {
    EXPECT_EQ(h700::frame_dest_ring_eligibility(true, 0, sizes, actual, bytes, limits), Reason::DeviceSize);
    EXPECT_EQ(h700::frame_dest_ring_eligibility(true, 0, sizes, bytes, actual, limits), Reason::DeviceSize);
  }
  for (auto member : {&FrameBufferSizes::deviceUniform, &FrameBufferSizes::deviceStorage}) {
    auto mismatch = sizes;
    mismatch.*member = bytes / 2;
    EXPECT_EQ(h700::frame_dest_ring_eligibility(true, 0, mismatch, bytes, bytes, limits), Reason::DeviceSize);
  }
}

TEST(H700FrameDestRing, ResidentHeldAndUnsupportedLimitsTakeLegacyWithoutMutation) {
  const auto sizes = ring_test_sizes();
  const auto limits = ring_test_limits();
  const auto bytes = h700::FrameDestRingBufferBytes;
  using Reason = h700::FrameDestRingFallback;
  EXPECT_EQ(h700::frame_dest_ring_eligibility(true, 1, sizes, bytes, bytes, limits), Reason::ResidentGeometry);
  for (auto member : {&FrameBufferSizes::residentVertex, &FrameBufferSizes::residentIndex,
                      &FrameBufferSizes::residentStorage}) {
    auto resident = sizes;
    resident.*member = 256;
    EXPECT_EQ(h700::frame_dest_ring_eligibility(true, 0, resident, bytes, bytes, limits), Reason::ResidentGeometry);
    EXPECT_EQ(resident.*member, 256u);
  }
  for (auto member : {&FrameBufferSizes::heldVertex, &FrameBufferSizes::heldUniform,
                      &FrameBufferSizes::heldIndex, &FrameBufferSizes::heldStorage}) {
    auto held = sizes;
    held.*member = bytes;
    EXPECT_EQ(h700::frame_dest_ring_eligibility(true, 0, held, bytes, bytes, limits), Reason::HeldLimits);
    EXPECT_EQ(held.*member, bytes);
    EXPECT_EQ(held.maxUniform, UniformBufferSize);
  }
  for (auto member : {&wgpu::Limits::maxBufferSize, &wgpu::Limits::maxStorageBufferBindingSize}) {
    for (const uint64_t value : {bytes - 1, uint64_t{WGPU_LIMIT_U64_UNDEFINED}}) {
      auto unsupported = limits;
      unsupported.*member = value;
      EXPECT_EQ(h700::frame_dest_ring_eligibility(true, 0, sizes, bytes, bytes, unsupported), Reason::BufferLimits);
    }
  }
}

TEST(H700FrameDestRing, ActualAllocationDescriptorsKeepSizesUsagesAndUnmappedZeroInitialization) {
  const auto descriptors = h700::frame_dest_ring_buffer_descriptors();
  ASSERT_EQ(descriptors.size(), 2u);
  for (const auto& descriptor : descriptors) {
    EXPECT_EQ(descriptor.size, h700::FrameDestRingBufferBytes);
    EXPECT_FALSE(descriptor.mappedAtCreation);
    EXPECT_EQ(descriptor.nextInChain, nullptr);
  }
  EXPECT_EQ(descriptors[0].usage,
            wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst);
  EXPECT_EQ(descriptors[1].usage,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst);
  EXPECT_EQ(descriptors[0].size + descriptors[1].size, 16u << 20);
  // Descriptor semantics only: no GPU allocation, execution or zero-content readback is claimed.
}

TEST(H700FrameDestRing, ActualStaticEntriesShareMirrorsButBindTheSelectedWholeStorage) {
  std::array<int, 5> opaque{}; // raw identity tokens: never passed to WebGPU or wrapped in owning handles
  const auto vertex = reinterpret_cast<WGPUTextureView>(&opaque[0]);
  const auto mirror = reinterpret_cast<WGPUTextureView>(&opaque[1]);
  const auto sampler = reinterpret_cast<WGPUSampler>(&opaque[2]);
  const std::array storage{reinterpret_cast<WGPUBuffer>(&opaque[3]), reinterpret_cast<WGPUBuffer>(&opaque[4])};
  for (const auto buffer : storage) {
    const auto entries = h700::frame_dest_ring_static_entries(vertex, mirror, buffer, sampler);
    ASSERT_EQ(entries.size(), 4u);
    for (uint32_t i = 0; i < entries.size(); ++i) {
      EXPECT_EQ(entries[i].binding, i);
      EXPECT_EQ(entries[i].nextInChain, nullptr);
      EXPECT_EQ(entries[i].offset, 0u);
    }
    EXPECT_EQ(entries[0].textureView, vertex);
    EXPECT_EQ(entries[1].textureView, mirror);
    EXPECT_EQ(entries[2].buffer, buffer);
    EXPECT_EQ(entries[2].size, WGPU_WHOLE_SIZE);
    EXPECT_EQ(entries[3].sampler, sampler);
    EXPECT_EQ(entries[0].buffer, nullptr);
    EXPECT_EQ(entries[1].buffer, nullptr);
    EXPECT_EQ(entries[2].textureView, nullptr);
    EXPECT_EQ(entries[3].buffer, nullptr);
  }
}

TEST(H700FrameDestRing, ActualUniformEntriesUseTheSameSelectedBufferForBothUnchangedWindows) {
  std::array<int, 2> opaque{};
  for (int& identity : opaque) {
    const auto buffer = reinterpret_cast<WGPUBuffer>(&identity);
    const auto legacy = h700::frame_dest_ring_uniform_entries(buffer, gx::MaxUniformSize);
    const auto draw = h700::frame_dest_ring_uniform_entries<true>(buffer, gx::MaxUniformSize,
                                                                 sizeof(gx::DrawImmediateData));
    ASSERT_EQ(legacy.size(), 1u);
    ASSERT_EQ(draw.size(), 2u);
    EXPECT_EQ(legacy[0].buffer, buffer);
    EXPECT_EQ(legacy[0].binding, 0u);
    EXPECT_EQ(legacy[0].size, gx::MaxUniformSize);
    EXPECT_EQ(draw[0].buffer, legacy[0].buffer);
    EXPECT_EQ(draw[0].size, legacy[0].size);
    EXPECT_EQ(draw[1].buffer, buffer);
    EXPECT_EQ(draw[1].binding, 1u);
    EXPECT_EQ(draw[1].size, sizeof(gx::DrawImmediateData));
    for (const auto& entry : draw) {
      EXPECT_EQ(entry.offset, 0u);
      EXPECT_EQ(entry.nextInChain, nullptr);
      EXPECT_EQ(entry.textureView, nullptr);
      EXPECT_EQ(entry.sampler, nullptr);
    }
  }
}

TEST(H700FrameDestRing, NativeSelectionAndAliasInstallationDoNotUseCpuSlotOrRotateMirrors) {
  using Bindings = h700::FrameDestRingBindings<int, int>;
  const std::array<Bindings, 2> owners{{{10, 11, 12, 13, 14}, {20, 21, 22, 23, 24}}};
  CpuDestResources resources;
  h700::FrameDestRingState state{true};
  const std::array<uint8_t, 4> nativeSlots{1, 0, 1, 1};
  const size_t cpuSlot = 0; // deliberately different from the first native slot
  for (size_t frame = 0; frame < nativeSlots.size(); ++frame) {
    const auto decision = state.select(ring_native(frame, nativeSlots[frame]));
    ASSERT_EQ(decision.route, h700::FrameDestRingRoute::Ring);
    ASSERT_EQ(decision.slot, nativeSlots[frame]);
    if (frame == 0) EXPECT_NE(decision.slot, cpuSlot);
    h700::apply_frame_dest_ring_bindings(resources, owners[decision.slot]);
    EXPECT_EQ(resources.uniformBuffer, owners[nativeSlots[frame]].uniformBuffer);
    EXPECT_EQ(resources.storageBuffer, owners[nativeSlots[frame]].storageBuffer);
    EXPECT_EQ(resources.staticBindGroup, owners[nativeSlots[frame]].staticBindGroup);
    EXPECT_EQ(resources.uniformBindGroup, owners[nativeSlots[frame]].uniformBindGroup);
    EXPECT_EQ(resources.drawUniformBindGroup, owners[nativeSlots[frame]].drawUniformBindGroup);
    EXPECT_EQ(resources.vertexBuffer, 100);
    EXPECT_EQ(resources.indexBuffer, 101);
    EXPECT_EQ(resources.vertexMirror, 102);
    EXPECT_EQ(resources.storageMirror, 103);
    EXPECT_EQ(resources.layout, 104);
    EXPECT_EQ(resources.sampler, 105);
  }
  // Consecutive native slot reuse is allowed only by the real presentation owner, not proved here.
}

TEST(H700FrameDestRing, DefaultOffAndMissingInitialNativeSelectionStayLegacyForTheGeneration) {
  using Route = h700::FrameDestRingRoute;
  h700::FrameDestRingState off;
  EXPECT_EQ(off.select(std::nullopt).route, Route::Legacy);
  EXPECT_EQ(off.select(ring_native(0, 1)).route, Route::Legacy);
  EXPECT_FALSE(off.fallback(h700::FrameDestRingFallback::VertexGrowth));
  h700::FrameDestRingState state{true};
  EXPECT_EQ(state.select(std::nullopt).route, Route::Fallback);
  EXPECT_EQ(state.fallback_reason(), h700::FrameDestRingFallback::MissingNativeSelection);
  EXPECT_FALSE(state.enabled());
  EXPECT_EQ(state.select(ring_native(0, 1)).route, Route::Legacy);
  EXPECT_EQ(state.select(std::nullopt).route, Route::Legacy);
  EXPECT_FALSE(state.fallback(h700::FrameDestRingFallback::DeferredReplacement));
  EXPECT_EQ(state.fallback_reason(), h700::FrameDestRingFallback::MissingNativeSelection);
}

TEST(H700FrameDestRing, MissingOrStaleNativeSelectionAfterActivationIsAnExactContractFailure) {
  using Route = h700::FrameDestRingRoute;
  for (const auto invalid : {ring_native(0, 2), ring_native(0, 255), ring_native(0, 0, 0)}) {
    h700::FrameDestRingState state{true};
    const auto decision = state.select(invalid);
    EXPECT_EQ(decision.route, Route::ContractError);
    EXPECT_NE(decision.error, nullptr);
  }
  for (const auto invalid : {std::optional<h700::FrameDescriptor>{}, ring_native(11, 0, 8),
                             ring_native(10, 1), ring_native(9, 0)}) {
    h700::FrameDestRingState state{true};
    ASSERT_EQ(state.select(ring_native(10, 1)).route, Route::Ring);
    const auto decision = state.select(invalid);
    EXPECT_EQ(decision.route, Route::ContractError);
    EXPECT_NE(decision.error, nullptr);
    // The production caller fails the frame/worker, rather than selecting slot 0 or doing live recovery.
  }
}

TEST(H700FrameDestRing, EveryDirectGrowthAndDeferredReplacementLatchCurrentAliasesNotInitialBuffers) {
  using Reason = h700::FrameDestRingFallback;
  using Route = h700::FrameDestRingRoute;
  using Bindings = h700::FrameDestRingBindings<int, int>;
  const std::array<Bindings, 2> owners{{{10, 11, 12, 13, 14}, {20, 21, 22, 23, 24}}};
  const std::array reasons{
      h700::frame_dest_ring_growth_reason(StagingKind::Vertex),
      h700::frame_dest_ring_growth_reason(StagingKind::Uniform),
      h700::frame_dest_ring_growth_reason(StagingKind::Index),
      h700::frame_dest_ring_growth_reason(StagingKind::Storage), Reason::DeferredReplacement};
  EXPECT_EQ(reasons, (std::array{Reason::VertexGrowth, Reason::UniformGrowth, Reason::IndexGrowth,
                                Reason::StorageGrowth, Reason::DeferredReplacement}));
  for (const auto reason : reasons) {
    SCOPED_TRACE(h700::frame_dest_ring_reason(reason));
    h700::FrameDestRingState state{true};
    CpuDestResources current;
    const auto active = state.select(ring_native(0, 1));
    ASSERT_EQ(active.route, Route::Ring);
    h700::apply_frame_dest_ring_bindings(current, owners[active.slot]);
    const auto queuedNative = ring_native(1, 0); // accepted before the growth job runs
    EXPECT_TRUE(state.fallback(reason));
    EXPECT_EQ(current.uniformBuffer, 20);
    EXPECT_EQ(current.storageBuffer, 21);
    // Stand-ins for the legacy grow's new buffer/groups; fallback must never restore owners[0] or [1].
    current.uniformBuffer = 30;
    current.uniformBindGroup = 33;
    current.drawUniformBindGroup = 34;
    EXPECT_EQ(state.select(queuedNative).route, Route::Legacy);
    // A latched fallback is not permission to lose the native contract after active ring data.
    EXPECT_EQ(state.select(std::nullopt).route, Route::ContractError);
    EXPECT_EQ(current.uniformBuffer, 30);
    EXPECT_EQ(current.storageBuffer, 21);
    EXPECT_EQ(current.uniformBindGroup, 33);
    EXPECT_EQ(current.drawUniformBindGroup, 34);
    EXPECT_FALSE(state.fallback(Reason::MissingNativeSelection));
    EXPECT_EQ(state.fallback_reason(), reason);
    EXPECT_EQ(owners[1].uniformBuffer, 20); // owners are retained, not rewritten by the fallback latch
  }
}

TEST(H700FrameDestRing, ProofIsBoundedButIncludesBothNativeSlotsWhenOneAppearsLate) {
  h700::FrameDestRingState state{true};
  size_t logged = 0;
  for (uint64_t frame = 0; frame < 10000; ++frame) {
    const auto decision = state.select(ring_native(frame, frame < 100 ? 0 : 1));
    ASSERT_EQ(decision.route, h700::FrameDestRingRoute::Ring);
    logged += decision.logSelection;
    if (frame == 100) EXPECT_TRUE(decision.logSelection);
  }
  EXPECT_EQ(logged, 5u);
}

TEST(H700FrameDestRing, FallbackAndPacketResetRetainDestinationOwnersUntilExplicitHolderRelease) {
  using Handle = std::shared_ptr<int>;
  using Bindings = h700::FrameDestRingBindings<Handle, Handle>;
  std::array<Bindings, 2> owners{};
  owners[1] = {std::make_shared<int>(20), std::make_shared<int>(21), std::make_shared<int>(22),
               std::make_shared<int>(23), std::make_shared<int>(24)};
  std::weak_ptr<int> uniform = owners[1].uniformBuffer;
  std::weak_ptr<int> storage = owners[1].storageBuffer;
  std::weak_ptr<int> group = owners[1].staticBindGroup;
  h700::FrameDestRingState state{true};
  auto packetNative = ring_native(0, 1);
  const auto decision = state.select(packetNative);
  ASSERT_EQ(decision.route, h700::FrameDestRingRoute::Ring);
  Bindings current{};
  h700::apply_frame_dest_ring_bindings(current, owners[decision.slot]);
  packetNative.reset();
  EXPECT_TRUE(state.fallback(h700::FrameDestRingFallback::StorageGrowth));
  current = {};
  EXPECT_FALSE(uniform.expired());
  EXPECT_FALSE(storage.expired());
  EXPECT_FALSE(group.expired());
  owners.fill({}); // production does this only at its existing guarded holder-release point
  EXPECT_TRUE(uniform.expired());
  EXPECT_TRUE(storage.expired());
  EXPECT_TRUE(group.expired());
  state = h700::FrameDestRingState{true}; // a new, released generation can select again
  EXPECT_EQ(state.select(ring_native(0, 0, 8)).route, h700::FrameDestRingRoute::Ring);
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

// Supplemental wiring checks only; the descriptor/state/identity tests above execute helpers,
// and neither kind of test claims to execute Dawn commands or prove physical GPU completion.
TEST(H700FrameDestRingSource, NativeValueIsCapturedAndInstalledOnlyInsideOrderedBeginFrame) {
  const auto aurora = source("/aurora.cpp");
  const auto admission = section(aurora, "bool begin_frame() noexcept", "void end_frame(bool fifoDrained");
  contains(admission, "if (!gfx::begin_frame(nativeFrame))");
  EXPECT_LT(admission.find("h700::acquire_frame()"), admission.find("gfx::begin_frame(nativeFrame)"));
  const auto frame = source("/gfx/frame.cpp");
  const auto begin = section(frame, "bool begin_frame(std::optional<h700::FrameDescriptor>", "bool end_frame(");
  contains(begin, ", nativeFrame");
  EXPECT_LT(begin.find("enqueue_begin_frame"), begin.find("select_frame_dest_ring(nativeFrame, frameSlot)"));
  EXPECT_LT(begin.find("select_frame_dest_ring(nativeFrame, frameSlot)"), begin.find("g_device.CreateCommandEncoder"));
  const auto selection = section(frame, "bool select_frame_dest_ring(", "struct RuntimeDrawType");
  contains(selection, "render_worker::is_worker_thread()");
  contains(selection, "g_frameDestRingState.select(nativeFrame)");
  contains(selection, "!h700::native_slot(*nativeFrame)");
  contains(selection, "apply_frame_dest_ring_bindings(g_resources, g_frameDestRing[decision.slot])");
  EXPECT_EQ(selection.find("g_frameDestRing[frameSlot]"), std::string::npos);
  EXPECT_LT(selection.find("fail_frame_work(frameSlot)"), selection.find("apply_frame_dest_ring_bindings"));
}

TEST(H700FrameDestRingSource, RealGrowthFallsBackBeforeReplacementAndRetirementKeepsItsGuards) {
  const auto frame = source("/gfx/frame.cpp");
  const auto grow = section(frame, "bool grow_frame_buffer(", "bool grow_staging(");
  EXPECT_LT(grow.find("if (need <= device)"), grow.find("fallback_frame_dest_ring("));
  contains(grow, "fallback_frame_dest_ring(h700::frame_dest_ring_growth_reason(kind))");
  EXPECT_LT(grow.find("fallback_frame_dest_ring("), grow.find("encoder.CopyBufferToBuffer(current"));
  EXPECT_LT(grow.find("fallback_frame_dest_ring("), grow.find("current = std::move(buffer)"));
  const auto deferred = section(frame, "bool reserve_held_frame_buffers()", "bool begin_frame(");
  contains(deferred, "fallback_frame_dest_ring(h700::FrameDestRingFallback::DeferredReplacement)");
  EXPECT_LT(deferred.find("fallback_frame_dest_ring("), deferred.find("g_resources.*buffer.member ="));
  const auto fallback = section(frame, "void fallback_frame_dest_ring(", "void create_frame_dest_ring_bind_groups(");
  EXPECT_EQ(fallback.find("g_resources."), std::string::npos); // canonical=current, no restore of initial buffers
  EXPECT_EQ(fallback.find("g_frameDestRing.fill"), std::string::npos);
  const auto retirement = section(frame, "bool release_frame_holders_after_quiescence() {", "bool wait_for_staging_buffer(");
  contains(retirement, "!gx::fifo::is_quiescent()");
  contains(retirement, "!render_worker::is_quiescent()");
  contains(retirement, "!pipeline_cache_threads_joined()");
  contains(retirement, "pending_internal_maps() != 0) return false;");
  contains(retirement, "g_frameDestRing.fill({})");
  EXPECT_LT(retirement.find("pending_internal_maps()"), retirement.find("g_frameDestRing.fill({})"));
  EXPECT_LT(retirement.find("shutdown_resource_cache()"), retirement.find("g_frameDestRing.fill({})"));
  const auto end = section(frame, "bool end_frame(", "uint32_t current_frame()");
  EXPECT_EQ(end.find("g_frameDestRing.fill"), std::string::npos);
}

TEST(H700MixedFetch, ProgramCacheOptInPreservesAdapterAndDescriptorGuards) {
  const auto gpu = source("/webgpu/gpu.cpp");
  const auto gate = gpu.find("H700 requires a hardware OpenGLES Compatibility adapter");
  const auto policy = gpu.find("const bool glWithoutCache = !h700::program_cache_enabled");
  ASSERT_NE(gate, std::string::npos);
  ASSERT_NE(policy, std::string::npos);
  EXPECT_LT(gate, policy);
  const auto cache = section(gpu, "#if defined(AURORA_H700)", "wgpu::DeviceDescriptor deviceDescriptor");
  contains(cache, "std::getenv(\"MP_H700_PROGRAM_CACHE\")");
  contains(cache, "std::getenv(\"MP_H700_BENCH\")");
  contains(cache, "#elif _WIN32");
  contains(cache, "g_backendType == wgpu::BackendType::OpenGLES || g_backendType == wgpu::BackendType::OpenGL");
  contains(cache, ".nextInChain = glWithoutCache ? nullptr : &cacheDescriptor");
  contains(gpu, "cacheDescriptor.SetDawnLoadCacheDataCallback");
  contains(gpu, "cacheDescriptor.SetDawnStoreCacheDataCallback");
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
