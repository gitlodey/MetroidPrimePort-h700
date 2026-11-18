#pragma once

#include "presentation_state.hpp"
#include "../gfx/resources.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <tuple>

namespace aurora::h700 {

inline constexpr size_t FrameDestRingSlots = std::tuple_size_v<decltype(PresentationSnapshot::slots)>;
static_assert(FrameDestRingSlots == 2);
inline constexpr uint64_t FrameDestRingBufferBytes = 8u << 20;

constexpr bool frame_dest_ring_requested(const char* value, const char* bench) noexcept {
  return value && std::string_view(value) == "1" && bench &&
         (std::string_view(bench) == "1" || std::string_view(bench) == "menu");
}

enum class FrameDestRingFallback {
  None,
  UnsupportedProfile,
  ResidentGeometry,
  HeldLimits,
  DeviceSize,
  BufferLimits,
  MissingNativeSelection,
  VertexGrowth,
  UniformGrowth,
  IndexGrowth,
  StorageGrowth,
  DeferredReplacement,
};

constexpr std::string_view frame_dest_ring_reason(FrameDestRingFallback reason) noexcept {
  switch (reason) {
  case FrameDestRingFallback::None: return "none";
  case FrameDestRingFallback::UnsupportedProfile: return "unsupported mixed/native profile";
  case FrameDestRingFallback::ResidentGeometry: return "resident geometry";
  case FrameDestRingFallback::HeldLimits: return "held frame-buffer limits";
  case FrameDestRingFallback::DeviceSize: return "actual Uniform/Storage sizes are not both 8 MiB";
  case FrameDestRingFallback::BufferLimits: return "unsupported buffer limits";
  case FrameDestRingFallback::MissingNativeSelection: return "missing native selection before first activation";
  case FrameDestRingFallback::VertexGrowth: return "vertex device growth";
  case FrameDestRingFallback::UniformGrowth: return "uniform device growth";
  case FrameDestRingFallback::IndexGrowth: return "index device growth";
  case FrameDestRingFallback::StorageGrowth: return "storage device growth";
  case FrameDestRingFallback::DeferredReplacement: return "deferred device-buffer replacement";
  }
  return "unknown";
}

inline FrameDestRingFallback frame_dest_ring_eligibility(bool supportedProfile, uint32_t residentGeometryMiB,
                                                        const gfx::FrameBufferSizes& sizes,
                                                        uint64_t actualUniform, uint64_t actualStorage,
                                                        const wgpu::Limits& limits) noexcept {
  if (!supportedProfile) return FrameDestRingFallback::UnsupportedProfile;
  if (residentGeometryMiB != 0 || sizes.residentVertex != 0 || sizes.residentIndex != 0 ||
      sizes.residentStorage != 0) return FrameDestRingFallback::ResidentGeometry;
  if (sizes.heldVertex != 0 || sizes.heldUniform != 0 || sizes.heldIndex != 0 || sizes.heldStorage != 0)
    return FrameDestRingFallback::HeldLimits;
  // maxUniform may still be 24 MiB: an actual grow falls back, never clamps that limit.
  if (actualUniform != FrameDestRingBufferBytes || actualStorage != FrameDestRingBufferBytes ||
      sizes.deviceUniform != actualUniform || sizes.deviceStorage != actualStorage)
    return FrameDestRingFallback::DeviceSize;
  if (limits.maxBufferSize == WGPU_LIMIT_U64_UNDEFINED || limits.maxBufferSize < FrameDestRingBufferBytes ||
      limits.maxStorageBufferBindingSize == WGPU_LIMIT_U64_UNDEFINED ||
      limits.maxStorageBufferBindingSize < FrameDestRingBufferBytes)
    return FrameDestRingFallback::BufferLimits;
  return FrameDestRingFallback::None;
}

constexpr FrameDestRingFallback frame_dest_ring_growth_reason(gfx::StagingKind kind) noexcept {
  switch (kind) {
  case gfx::StagingKind::Vertex: return FrameDestRingFallback::VertexGrowth;
  case gfx::StagingKind::Uniform: return FrameDestRingFallback::UniformGrowth;
  case gfx::StagingKind::Index: return FrameDestRingFallback::IndexGrowth;
  case gfx::StagingKind::Storage: return FrameDestRingFallback::StorageGrowth;
  }
  return FrameDestRingFallback::UnsupportedProfile;
}

enum class FrameDestRingRoute { Legacy, Ring, Fallback, ContractError };

struct FrameDestRingDecision {
  FrameDestRingRoute route = FrameDestRingRoute::Legacy;
  size_t slot = 0;
  bool logSelection = false;
  const char* error = nullptr;
};

// Worker-only selection/latch, not a completion tracker. The native reservation owns reuse;
// CPU packet slots, packet reset and staging-map completion cannot release these destinations.
class FrameDestRingState {
public:
  explicit FrameDestRingState(bool enabled = false) : m_enabled(enabled) {}

  bool enabled() const noexcept { return m_enabled; }
  FrameDestRingFallback fallback_reason() const noexcept { return m_fallback; }

  bool fallback(FrameDestRingFallback reason) noexcept {
    if (!m_enabled || reason == FrameDestRingFallback::None) return false;
    m_enabled = false;
    m_fallback = reason;
    return true;
  }

  FrameDestRingDecision select(const std::optional<FrameDescriptor>& native) noexcept {
    if (!native && m_lastFrame) return {.route = FrameDestRingRoute::ContractError,
                                      .error = "missing native selection after ring activation"};
    if (!m_enabled) return {};
    if (!native) {
      (void)fallback(FrameDestRingFallback::MissingNativeSelection);
      return {.route = FrameDestRingRoute::Fallback};
    }
    if (native->generation == 0 || native->slot >= FrameDestRingSlots)
      return {.route = FrameDestRingRoute::ContractError, .error = "invalid native generation/slot"};
    if (m_lastFrame && (native->generation != m_generation || native->frame <= *m_lastFrame))
      return {.route = FrameDestRingRoute::ContractError, .error = "stale native generation/frame"};
    m_generation = native->generation;
    m_lastFrame = native->frame;
    const uint8_t bit = uint8_t(1u << native->slot);
    const bool log = m_firstSelections < 4 || (m_seenSlots & bit) == 0;
    if (m_firstSelections < 4) ++m_firstSelections;
    m_seenSlots |= bit; // at most four initial selections plus the first sighting of the other slot
    return {.route = FrameDestRingRoute::Ring, .slot = native->slot, .logSelection = log};
  }

private:
  bool m_enabled = false;
  FrameDestRingFallback m_fallback = FrameDestRingFallback::None;
  uint64_t m_generation = 0;
  std::optional<uint64_t> m_lastFrame;
  uint8_t m_firstSelections = 0;
  uint8_t m_seenSlots = 0;
};

// These are the actual allocation/entry builders used by the opt-in, testable without a device.
inline auto frame_dest_ring_buffer_descriptors() {
  return std::array{
      wgpu::BufferDescriptor{
          .label = "Native slot 1 uniform destination",
          .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc,
          .size = FrameDestRingBufferBytes,
      },
      wgpu::BufferDescriptor{
          .label = "Native slot 1 storage destination",
          .usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc,
          .size = FrameDestRingBufferBytes,
      },
  };
}

inline auto frame_dest_ring_static_entries(WGPUTextureView vertexMirror, WGPUTextureView storageMirror,
                                            WGPUBuffer storage, WGPUSampler sampler) {
  std::array<WGPUBindGroupEntry, 4> entries{};
  for (uint32_t i = 0; i < entries.size(); ++i) entries[i].binding = i;
  entries[0].textureView = vertexMirror;
  entries[1].textureView = storageMirror;
  entries[gfx::MixedFragmentStorageBinding].buffer = storage;
  entries[gfx::MixedFragmentStorageBinding].size = WGPU_WHOLE_SIZE;
  entries[gfx::MixedFetchSamplerBinding].sampler = sampler;
  return entries;
}

template <bool DrawUniform = false>
inline auto frame_dest_ring_uniform_entries(WGPUBuffer uniform, uint64_t materialSize, uint64_t drawSize = 0) {
  std::array<WGPUBindGroupEntry, DrawUniform ? 2 : 1> entries{};
  entries[0].binding = 0;
  entries[0].buffer = uniform;
  entries[0].size = materialSize;
  if constexpr (DrawUniform) {
    entries[1].binding = 1;
    entries[1].buffer = uniform;
    entries[1].size = drawSize;
  }
  return entries;
}

template <typename Buffer, typename BindGroup>
struct FrameDestRingBindings {
  Buffer uniformBuffer;
  Buffer storageBuffer;
  BindGroup staticBindGroup;
  BindGroup uniformBindGroup;
  BindGroup drawUniformBindGroup;
};

template <typename Resources, typename Buffer, typename BindGroup>
inline void apply_frame_dest_ring_bindings(Resources& resources, const FrameDestRingBindings<Buffer, BindGroup>& slot) {
  resources.uniformBuffer = slot.uniformBuffer;
  resources.storageBuffer = slot.storageBuffer;
  resources.staticBindGroup = slot.staticBindGroup;
  resources.uniformBindGroup = slot.uniformBindGroup;
  resources.drawUniformBindGroup = slot.drawUniformBindGroup;
}

} // namespace aurora::h700
