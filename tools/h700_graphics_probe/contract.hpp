#pragma once

#include <webgpu/webgpu_cpp.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace h700_probe::contract {

struct Policy {
  bool texture_fetch = false;
  bool no_compute = false;
  bool lightmap = false;
};

Policy choose_policy(const wgpu::Limits&, const wgpu::CompatibilityModeLimits&);
// The caller owns and chains required_compatibility(); this result has no chain.
wgpu::Limits required_limits(const wgpu::Limits&, const Policy&);
wgpu::CompatibilityModeLimits required_compatibility(const Policy&);
std::vector<wgpu::FeatureName> required_features(std::span<const wgpu::FeatureName>);
uint32_t fragment_texture_count(bool texture_fetch, bool lightmap);
// Creates only layouts, preserving U's texture-fetch budget mismatch for validation.
wgpu::PipelineLayout gx_layout(const wgpu::Device&, bool texture_fetch, bool lightmap, bool shadow);
inline constexpr uint64_t kMixedBufferBytes = 65536;
struct MixedLayouts {
  std::array<wgpu::BindGroupLayout, 3> groups;
  wgpu::PipelineLayout pipeline;
};
// A proposed adaptation, separate from the unchanged U mirror above. No lightmap.
void check_mixed_limits(const wgpu::Limits&, const wgpu::CompatibilityModeLimits&);
wgpu::Limits mixed_required_limits(const wgpu::Limits&, const Policy&);
wgpu::CompatibilityModeLimits mixed_required_compatibility();
MixedLayouts mixed_layout(const wgpu::Device&, bool shadow);
// CPU-only assertions; throws std::runtime_error and never initializes Dawn.
void self_test();

} // namespace h700_probe::contract
