#include "contract.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace h700_probe::contract {
namespace {

// U c872da704: extern/aurora/lib/gx/gx.hpp:52-66,95-103 and gfx/probe.hpp:48.
constexpr uint32_t kImmediateSize = 64;
constexpr uint32_t kMaxTextures = 8;
constexpr uint32_t kVolumeTextures = 5;
constexpr uint32_t kProbeBinding = kMaxTextures * 2;
constexpr uint32_t kBrdfBinding = kProbeBinding + 7;
constexpr uint32_t kFroxelBinding = kProbeBinding + 8;
constexpr uint32_t kFogSamplerBinding = kProbeBinding + 9;
constexpr uint32_t kLightmapBinding = kProbeBinding + 10;
constexpr uint32_t kTextureBindings = kProbeBinding + 11;
constexpr auto kVertexFragment = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;

// U: extern/aurora/lib/webgpu/gpu.cpp:1306-1347, native Dawn, without env opt-outs.
constexpr std::array kFeatures{
    wgpu::FeatureName::CoreFeaturesAndLimits,
    wgpu::FeatureName::TextureCompressionBC,
    wgpu::FeatureName::TextureCompressionASTC,
    wgpu::FeatureName::TextureComponentSwizzle,
    wgpu::FeatureName::DualSourceBlending,
    wgpu::FeatureName::TimestampQuery,
    wgpu::FeatureName::ImplicitDeviceSynchronization,
};

// U: extern/aurora/lib/gfx/frame.cpp:822-852.
std::array<wgpu::BindGroupLayoutEntry, 2> static_entries(bool texture_fetch) {
  std::array<wgpu::BindGroupLayoutEntry, 2> entries{};
  for (uint32_t i = 0; i < entries.size(); ++i) {
    auto& entry = entries[i];
    entry.binding = i;
    entry.visibility = i == 0 ? wgpu::ShaderStage::Vertex : kVertexFragment;
    if (texture_fetch) {
      entry.texture = {
          .sampleType = wgpu::TextureSampleType::Uint,
          .viewDimension = wgpu::TextureViewDimension::e2D,
      };
    } else {
      entry.buffer = {.type = wgpu::BufferBindingType::ReadOnlyStorage};
    }
  }
  return entries;
}

std::array<wgpu::BindGroupLayoutEntry, 3> mixed_static_entries() {
  const auto original = static_entries(true);
  std::array<wgpu::BindGroupLayoutEntry, 3> entries{original[0], original[1], {}};
  entries[1].visibility = wgpu::ShaderStage::Vertex;
  entries[2] = {.binding = 2, .visibility = wgpu::ShaderStage::Fragment,
                .buffer = {.type = wgpu::BufferBindingType::ReadOnlyStorage}};
  return entries;
}

// U: extern/aurora/lib/gfx/frame.cpp:856-874. minBindingSize remains zero.
wgpu::BindGroupLayoutEntry uniform_entry() {
  return {
      .binding = 0,
      .visibility = kVertexFragment,
      .buffer = {.type = wgpu::BufferBindingType::Uniform, .hasDynamicOffset = true},
  };
}

// U: extern/aurora/lib/gx/gx.cpp:866-973. Lightmap is the optional final entry.
std::array<wgpu::BindGroupLayoutEntry, kTextureBindings> texture_entries(bool shadow) {
  std::array<wgpu::BindGroupLayoutEntry, kTextureBindings> entries{};
  const auto texture = [&](uint32_t binding, wgpu::TextureViewDimension dimension,
                           wgpu::ShaderStage visibility = wgpu::ShaderStage::Fragment,
                           wgpu::TextureSampleType type = wgpu::TextureSampleType::Float) {
    entries[binding] = {
        .binding = binding,
        .visibility = visibility,
        .texture = {.sampleType = type, .viewDimension = dimension},
    };
  };
  const auto sampler = [&](uint32_t binding, wgpu::SamplerBindingType type,
                           wgpu::ShaderStage visibility = wgpu::ShaderStage::Fragment) {
    entries[binding] = {
        .binding = binding,
        .visibility = visibility,
        .sampler = {.type = type},
    };
  };
  for (uint32_t i = 0; i < kMaxTextures; ++i) {
    texture(i * 2, wgpu::TextureViewDimension::e2D);
    sampler(i * 2 + 1, wgpu::SamplerBindingType::Filtering);
  }
  texture(kProbeBinding, wgpu::TextureViewDimension::Cube);
  sampler(kProbeBinding + 1, wgpu::SamplerBindingType::Filtering);
  for (uint32_t i = 0; i < kVolumeTextures; ++i) {
    texture(kProbeBinding + 2 + i, wgpu::TextureViewDimension::e3D);
  }
  texture(kBrdfBinding, wgpu::TextureViewDimension::e2D);
  texture(kFroxelBinding, wgpu::TextureViewDimension::e3D, kVertexFragment);
  sampler(kFogSamplerBinding, wgpu::SamplerBindingType::Filtering, kVertexFragment);
  texture(kLightmapBinding, wgpu::TextureViewDimension::e2DArray);
  if (shadow) {
    texture(kFroxelBinding, wgpu::TextureViewDimension::e2DArray,
            wgpu::ShaderStage::Fragment, wgpu::TextureSampleType::Depth);
    sampler(kFogSamplerBinding, wgpu::SamplerBindingType::Comparison);
  }
  return entries;
}

void check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

} // namespace

// U: extern/aurora/lib/webgpu/gpu.cpp:1145-1282; no forced environment policies.
Policy choose_policy(const wgpu::Limits& limits, const wgpu::CompatibilityModeLimits& compatibility) {
  return {
      .texture_fetch = compatibility.maxStorageBuffersInVertexStage != WGPU_LIMIT_U32_UNDEFINED &&
                       compatibility.maxStorageBuffersInVertexStage < 2,
      .no_compute = limits.maxComputeWorkgroupsPerDimension == 0 ||
                    limits.maxComputeInvocationsPerWorkgroup == 0,
      .lightmap = limits.maxSampledTexturesPerShaderStage >= 17,
  };
}

// U: extern/aurora/lib/webgpu/gpu.cpp:1204-1234. Leave unspecified fields undefined;
// in particular do not replace them with guessed Compatibility device defaults.
wgpu::Limits required_limits(const wgpu::Limits& supported, const Policy& policy) {
  const bool needStorage = !policy.texture_fetch || !policy.no_compute;
  return {
      .nextInChain = nullptr,
      .maxTextureDimension1D = supported.maxTextureDimension1D == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                : supported.maxTextureDimension1D,
      .maxTextureDimension2D = supported.maxTextureDimension2D == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                : supported.maxTextureDimension2D,
      .maxTextureDimension3D = supported.maxTextureDimension3D == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                : supported.maxTextureDimension3D,
      .maxTextureArrayLayers = supported.maxTextureArrayLayers == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                : supported.maxTextureArrayLayers,
      .maxSampledTexturesPerShaderStage = supported.maxSampledTexturesPerShaderStage >= 17
                                             ? 17u : WGPU_LIMIT_U32_UNDEFINED,
      .maxStorageBuffersPerShaderStage = needStorage ? 2u : WGPU_LIMIT_U32_UNDEFINED,
      .maxStorageBufferBindingSize = !needStorage || supported.maxStorageBufferBindingSize == 0
                                        ? WGPU_LIMIT_U64_UNDEFINED : supported.maxStorageBufferBindingSize,
      .minUniformBufferOffsetAlignment = std::max(supported.minUniformBufferOffsetAlignment, 64u),
      .minStorageBufferOffsetAlignment = std::max(supported.minStorageBufferOffsetAlignment, 16u),
      .maxBufferSize = supported.maxBufferSize == 0 ? WGPU_LIMIT_U64_UNDEFINED : supported.maxBufferSize,
      .maxImmediateSize = kImmediateSize,
  };
}

wgpu::CompatibilityModeLimits required_compatibility(const Policy& policy) {
  return wgpu::CompatibilityModeLimits{wgpu::CompatibilityModeLimits::Init{
      .maxStorageBuffersInVertexStage = policy.texture_fetch ? 0u : 2u,
      .maxStorageBuffersInFragmentStage = policy.texture_fetch ? 0u : 2u,
  }};
}

std::vector<wgpu::FeatureName> required_features(std::span<const wgpu::FeatureName> supported) {
  std::vector<wgpu::FeatureName> required;
  for (auto feature : supported) {
    if (std::find(kFeatures.begin(), kFeatures.end(), feature) != kFeatures.end()) {
      required.push_back(feature);
    }
  }
  return required;
}

uint32_t fragment_texture_count(bool texture_fetch, bool lightmap) {
  return kMaxTextures + 1 + kVolumeTextures + 1 + 1 + uint32_t(texture_fetch) + uint32_t(lightmap);
}

wgpu::PipelineLayout gx_layout(const wgpu::Device& device, bool texture_fetch, bool lightmap, bool shadow) {
  const auto staticEntries = static_entries(texture_fetch);
  const wgpu::BindGroupLayoutDescriptor staticDescriptor{
      .label = "Static bind group layout",
      .entryCount = staticEntries.size(),
      .entries = staticEntries.data(),
  };
  const auto uniformEntry = uniform_entry();
  const wgpu::BindGroupLayoutDescriptor uniformDescriptor{
      .label = "Uniform bind group layout",
      .entryCount = 1,
      .entries = &uniformEntry,
  };
  const auto textureEntries = texture_entries(shadow);
  const wgpu::BindGroupLayoutDescriptor textureDescriptor{
      .label = shadow ? "GX Shadow Receiver Texture Bind Group Layout" : "GX Texture Bind Group Layout",
      .entryCount = kTextureBindings - (lightmap ? 0u : 1u),
      .entries = textureEntries.data(),
  };
  const std::array layouts{
      device.CreateBindGroupLayout(&staticDescriptor),
      device.CreateBindGroupLayout(&uniformDescriptor),
      device.CreateBindGroupLayout(&textureDescriptor),
  };
  // U: extern/aurora/lib/gx/gx.cpp:1040-1065. Group 0's fragment Uint texture is
  // deliberately NOT removed: texture fetch needs 17/18 textures, not 16/17.
  const wgpu::PipelineLayoutDescriptor descriptor{
      .label = shadow ? "GX Shadow Receiver Pipeline Layout" : "GX Pipeline Layout",
      .bindGroupLayoutCount = layouts.size(),
      .bindGroupLayouts = layouts.data(),
      .immediateSize = kImmediateSize,
  };
  return device.CreatePipelineLayout(&descriptor);
}

void check_mixed_limits(const wgpu::Limits& limits, const wgpu::CompatibilityModeLimits& compatibility) {
  const auto at_least = [](uint32_t value, uint32_t minimum) {
    return value != WGPU_LIMIT_U32_UNDEFINED && value >= minimum;
  };
  check(at_least(limits.maxSampledTexturesPerShaderStage, 16), "Mixed fetch requires 16 sampled textures");
  check(at_least(limits.maxTextureDimension2D, 4096), "Mixed fetch requires 4096-wide textures");
  check(at_least(limits.maxStorageBuffersPerShaderStage, 1) &&
            at_least(compatibility.maxStorageBuffersInFragmentStage, 1),
        "Mixed fetch requires a fragment storage buffer, not a vertex storage buffer");
  check(limits.maxStorageBufferBindingSize != WGPU_LIMIT_U64_UNDEFINED &&
            limits.maxStorageBufferBindingSize >= kMixedBufferBytes &&
            limits.maxBufferSize != WGPU_LIMIT_U64_UNDEFINED && limits.maxBufferSize >= kMixedBufferBytes,
        "Mixed fetch requires a 65536-byte storage binding and buffer");
  check(at_least(limits.maxImmediateSize, 64), "Mixed fetch requires 64-byte immediates");
}

wgpu::Limits mixed_required_limits(const wgpu::Limits& supported, const Policy& policy) {
  // Storage is needed even if both original U fallbacks would otherwise avoid it.
  auto result = required_limits(supported, policy);
  result.maxSampledTexturesPerShaderStage = 16;
  result.maxStorageBuffersPerShaderStage = 2;
  result.maxStorageBufferBindingSize = supported.maxStorageBufferBindingSize;
  return result;
}

wgpu::CompatibilityModeLimits mixed_required_compatibility() {
  return wgpu::CompatibilityModeLimits{wgpu::CompatibilityModeLimits::Init{
      .maxStorageBuffersInVertexStage = 0, .maxStorageBuffersInFragmentStage = 1}};
}

MixedLayouts mixed_layout(const wgpu::Device& device, bool shadow) {
  const auto statics = mixed_static_entries();
  const auto uniform = uniform_entry();
  const auto textures = texture_entries(shadow);
  const wgpu::BindGroupLayoutDescriptor staticDesc{
      .label = "Mixed vertex textures / fragment storage", .entryCount = statics.size(), .entries = statics.data()};
  const wgpu::BindGroupLayoutDescriptor uniformDesc{
      .label = "Uniform bind group layout", .entryCount = 1, .entries = &uniform};
  const wgpu::BindGroupLayoutDescriptor textureDesc{
      .label = shadow ? "Mixed GX shadow textures" : "Mixed GX normal textures",
      .entryCount = kTextureBindings - 1, .entries = textures.data()};
  MixedLayouts result{{device.CreateBindGroupLayout(&staticDesc), device.CreateBindGroupLayout(&uniformDesc),
                        device.CreateBindGroupLayout(&textureDesc)}, {}};
  const wgpu::PipelineLayoutDescriptor descriptor{
      .label = shadow ? "Mixed GX Shadow Pipeline Layout" : "Mixed GX Pipeline Layout",
      .bindGroupLayoutCount = result.groups.size(), .bindGroupLayouts = result.groups.data(),
      .immediateSize = kImmediateSize};
  result.pipeline = device.CreatePipelineLayout(&descriptor);
  return result;
}

void self_test() {
  const Policy defaults;
  check(!defaults.texture_fetch && !defaults.no_compute && !defaults.lightmap, "Policy defaults");
  wgpu::Limits supported{};
  wgpu::CompatibilityModeLimits compatibility{wgpu::CompatibilityModeLimits::Init{}};
  auto policy = choose_policy(supported, compatibility);
  // Undefined is a sentinel, not zero: U's literal >= 17 also selects lightmap here.
  check(!policy.texture_fetch && !policy.no_compute && policy.lightmap, "Undefined policy counts");
  for (uint32_t storage : {0u, 1u, 2u, 3u, WGPU_LIMIT_U32_UNDEFINED}) {
    compatibility.maxStorageBuffersInVertexStage = storage;
    compatibility.maxStorageBuffersInFragmentStage = 0;
    policy = choose_policy(supported, compatibility);
    check(policy.texture_fetch == (storage < 2), "Vertex storage threshold; fragment count is not selection");
  }
  for (uint32_t sampled : {0u, 16u, 17u, 18u, WGPU_LIMIT_U32_UNDEFINED}) {
    supported.maxSampledTexturesPerShaderStage = sampled;
    check(choose_policy(supported, compatibility).lightmap == (sampled >= 17), "Lightmap threshold");
    check(required_limits(supported, defaults).maxSampledTexturesPerShaderStage ==
              (sampled >= 17 ? 17u : WGPU_LIMIT_U32_UNDEFINED), "Sampled texture request");
  }
  for (uint32_t groups : {0u, 1u, WGPU_LIMIT_U32_UNDEFINED}) {
    for (uint32_t invocations : {0u, 1u, WGPU_LIMIT_U32_UNDEFINED}) {
      supported.maxComputeWorkgroupsPerDimension = groups;
      supported.maxComputeInvocationsPerWorkgroup = invocations;
      check(choose_policy(supported, compatibility).no_compute == (groups == 0 || invocations == 0),
            "Either zero compute count selects fallback");
    }
  }

  supported = {};
  auto requested = required_limits(supported, defaults);
  check(requested.nextInChain == nullptr && requested.maxImmediateSize == 64, "Owned chain and 64-byte request");
  check(requested.maxTextureDimension2D == WGPU_LIMIT_U32_UNDEFINED &&
            requested.maxBufferSize == WGPU_LIMIT_U64_UNDEFINED &&
            requested.minUniformBufferOffsetAlignment == WGPU_LIMIT_U32_UNDEFINED &&
            requested.minStorageBufferOffsetAlignment == WGPU_LIMIT_U32_UNDEFINED &&
            requested.maxBindGroups == WGPU_LIMIT_U32_UNDEFINED &&
            requested.maxSamplersPerShaderStage == WGPU_LIMIT_U32_UNDEFINED &&
            requested.maxComputeInvocationsPerWorkgroup == WGPU_LIMIT_U32_UNDEFINED,
        "No fabricated limits or alignments");
  for (uint32_t dimension : {0u, 8192u, WGPU_LIMIT_U32_UNDEFINED}) {
    supported.maxTextureDimension1D = dimension;
    supported.maxTextureDimension2D = dimension;
    supported.maxTextureDimension3D = dimension;
    supported.maxTextureArrayLayers = dimension;
    requested = required_limits(supported, defaults);
    const auto expected = dimension == 0 ? WGPU_LIMIT_U32_UNDEFINED : dimension;
    check(requested.maxTextureDimension1D == expected && requested.maxTextureDimension2D == expected &&
              requested.maxTextureDimension3D == expected && requested.maxTextureArrayLayers == expected,
          "Full reported dimensions or undefined");
  }
  for (uint32_t alignment : {0u, 8u, 16u, 64u, 256u}) {
    supported.minUniformBufferOffsetAlignment = alignment;
    supported.minStorageBufferOffsetAlignment = alignment;
    requested = required_limits(supported, defaults);
    check(requested.minUniformBufferOffsetAlignment == std::max(alignment, 64u) &&
              requested.minStorageBufferOffsetAlignment == std::max(alignment, 16u), "Alignment floors");
  }
  for (uint64_t bytes : {uint64_t{0}, uint64_t{1} << 30, uint64_t{WGPU_LIMIT_U64_UNDEFINED}}) {
    supported.maxBufferSize = bytes;
    supported.maxStorageBufferBindingSize = bytes;
    for (bool fetch : {false, true}) {
      for (bool noCompute : {false, true}) {
        policy = {.texture_fetch = fetch, .no_compute = noCompute};
        requested = required_limits(supported, policy);
        const auto expected = bytes == 0 ? WGPU_LIMIT_U64_UNDEFINED : bytes;
        const bool needStorage = !fetch || !noCompute;
        check(requested.maxBufferSize == expected &&
                  requested.maxStorageBuffersPerShaderStage == (needStorage ? 2u : WGPU_LIMIT_U32_UNDEFINED) &&
                  requested.maxStorageBufferBindingSize == (needStorage ? expected : WGPU_LIMIT_U64_UNDEFINED),
              "Storage and buffer request policy");
        const auto compat = required_compatibility(policy);
        check(compat.sType == wgpu::SType::CompatibilityModeLimits && compat.nextInChain == nullptr &&
                  compat.maxStorageBuffersInVertexStage == (fetch ? 0u : 2u) &&
                  compat.maxStorageBuffersInFragmentStage == (fetch ? 0u : 2u) &&
                  compat.maxStorageTexturesInVertexStage == WGPU_LIMIT_U32_UNDEFINED &&
                  compat.maxStorageTexturesInFragmentStage == WGPU_LIMIT_U32_UNDEFINED,
              "Compatibility chain type and counts");
      }
    }
  }

  check(required_features({}).empty(), "No supported features");
  const std::array unrelated{wgpu::FeatureName::Float32Filterable};
  check(required_features(unrelated).empty(), "Do not request unrelated features");
  auto reversed = kFeatures;
  std::reverse(reversed.begin(), reversed.end());
  check(required_features(reversed) == std::vector<wgpu::FeatureName>(reversed.begin(), reversed.end()),
        "All seven native features in adapter order");
  for (auto feature : kFeatures) {
    const std::array single{feature};
    check(required_features(single) == std::vector<wgpu::FeatureName>{feature}, "Each present feature");
    std::vector<wgpu::FeatureName> absent(kFeatures.begin(), kFeatures.end());
    absent.erase(std::find(absent.begin(), absent.end(), feature));
    check(required_features(absent) == absent, "Never request an absent feature");
  }

  const auto mixed = mixed_static_entries();
  check(mixed[0].visibility == wgpu::ShaderStage::Vertex &&
            mixed[1].visibility == wgpu::ShaderStage::Vertex && mixed[2].binding == 2 &&
            mixed[2].visibility == wgpu::ShaderStage::Fragment &&
            mixed[2].buffer.type == wgpu::BufferBindingType::ReadOnlyStorage,
        "Mixed stage separation");
  const auto mixedCompat = mixed_required_compatibility();
  check(mixedCompat.maxStorageBuffersInVertexStage == 0 && mixedCompat.maxStorageBuffersInFragmentStage == 1,
        "Mixed explicit compatibility request");
  supported.maxSampledTexturesPerShaderStage = 16;
  supported.maxTextureDimension2D = 8192;
  supported.maxStorageBuffersPerShaderStage = 8;
  supported.maxStorageBufferBindingSize = kMixedBufferBytes;
  supported.maxBufferSize = kMixedBufferBytes;
  supported.maxImmediateSize = 64;
  compatibility.maxStorageBuffersInVertexStage = 0;
  compatibility.maxStorageBuffersInFragmentStage = 4;
  check_mixed_limits(supported, compatibility);
  const auto mixedRequest = mixed_required_limits(supported, {.texture_fetch = true, .no_compute = true});
  check(mixedRequest.maxSampledTexturesPerShaderStage == 16 && mixedRequest.maxStorageBuffersPerShaderStage >= 1 &&
            mixedRequest.maxStorageBufferBindingSize == kMixedBufferBytes, "Mixed fallback still requests storage");
  const auto rejects = [&](const wgpu::Limits& limits, const wgpu::CompatibilityModeLimits& compat) {
    bool rejected = false;
    try { check_mixed_limits(limits, compat); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "Mixed capacity guard must reject unsupported/undefined values");
  };
  for (uint32_t value : {0u, WGPU_LIMIT_U32_UNDEFINED}) {
    auto bad = compatibility;
    bad.maxStorageBuffersInFragmentStage = value;
    rejects(supported, bad);
  }
  for (uint32_t value : {15u, WGPU_LIMIT_U32_UNDEFINED}) {
    auto bad = supported;
    bad.maxSampledTexturesPerShaderStage = value;
    rejects(bad, compatibility);
  }
  for (uint64_t value : {kMixedBufferBytes - 1, WGPU_LIMIT_U64_UNDEFINED}) {
    auto bad = supported;
    bad.maxStorageBufferBindingSize = value;
    rejects(bad, compatibility);
  }
  for (bool shadow : {false, true}) {
    const auto textures = texture_entries(shadow);
    uint32_t fragmentTextures = 0, fragmentStorage = 0, vertexStorage = 0;
    const auto countEntry = [&](const wgpu::BindGroupLayoutEntry& entry) {
      const bool fragment = (entry.visibility & wgpu::ShaderStage::Fragment) != wgpu::ShaderStage::None;
      const bool vertex = (entry.visibility & wgpu::ShaderStage::Vertex) != wgpu::ShaderStage::None;
      fragmentTextures += fragment && entry.texture.sampleType != wgpu::TextureSampleType::BindingNotUsed;
      fragmentStorage += fragment && entry.buffer.type == wgpu::BufferBindingType::ReadOnlyStorage;
      vertexStorage += vertex && entry.buffer.type == wgpu::BufferBindingType::ReadOnlyStorage;
    };
    for (const auto& entry : mixed) countEntry(entry);
    for (size_t i = 0; i + 1 < textures.size(); ++i) countEntry(textures[i]);
    check(fragmentTextures == 16 && fragmentStorage == 1 && vertexStorage == 0, "Mixed actual resource budget");
  }

  const auto uniform = uniform_entry();
  check(uniform.binding == 0 && uniform.visibility == kVertexFragment &&
            uniform.buffer.type == wgpu::BufferBindingType::Uniform && uniform.buffer.hasDynamicOffset &&
            uniform.buffer.minBindingSize == 0, "Dynamic uniform layout");
  check(fragment_texture_count(false, false) == 16 && fragment_texture_count(false, true) == 17 &&
            fragment_texture_count(true, false) == 17 && fragment_texture_count(true, true) == 18,
        "Nominal 16/17/18 fragment texture counts");
  for (bool fetch : {false, true}) {
    const auto statics = static_entries(fetch);
    check(statics[0].binding == 0 && statics[0].visibility == wgpu::ShaderStage::Vertex &&
              statics[1].binding == 1 && statics[1].visibility == kVertexFragment, "Static layout visibility");
    for (const auto& entry : statics) {
      check(fetch ? entry.texture.sampleType == wgpu::TextureSampleType::Uint &&
                        entry.texture.viewDimension == wgpu::TextureViewDimension::e2D
                  : entry.buffer.type == wgpu::BufferBindingType::ReadOnlyStorage &&
                        !entry.buffer.hasDynamicOffset && entry.buffer.minBindingSize == 0,
            "Static layout resource type");
    }
    for (bool lightmap : {false, true}) {
      for (bool shadow : {false, true}) {
        const auto textures = texture_entries(shadow);
        uint32_t count = 0;
        const auto countTexture = [&](const wgpu::BindGroupLayoutEntry& entry) {
          if ((entry.visibility & wgpu::ShaderStage::Fragment) != wgpu::ShaderStage::None &&
              entry.texture.sampleType != wgpu::TextureSampleType::BindingNotUsed) {
            ++count;
          }
        };
        for (const auto& entry : statics) {
          countTexture(entry);
        }
        for (uint32_t i = 0; i < kTextureBindings - (lightmap ? 0u : 1u); ++i) {
          check(textures[i].binding == i, "Contiguous GX bindings");
          countTexture(textures[i]);
        }
        check(count == fragment_texture_count(fetch, lightmap), "Actual cross-group fragment budget");
        check(textures[kFroxelBinding].texture.sampleType ==
                  (shadow ? wgpu::TextureSampleType::Depth : wgpu::TextureSampleType::Float) &&
                  textures[kFroxelBinding].texture.viewDimension ==
                  (shadow ? wgpu::TextureViewDimension::e2DArray : wgpu::TextureViewDimension::e3D) &&
                  textures[kFroxelBinding].visibility == (shadow ? wgpu::ShaderStage::Fragment : kVertexFragment) &&
                  textures[kFogSamplerBinding].sampler.type ==
                  (shadow ? wgpu::SamplerBindingType::Comparison : wgpu::SamplerBindingType::Filtering),
              "Normal froxel versus shadow receiver slots");
      }
    }
  }
}

} // namespace h700_probe::contract
