#pragma once

#include "types.hpp"

#include <array>
#include <span>

namespace aurora::gfx {
inline constexpr bool UseTextureBuffer = true;
inline constexpr uint64_t UniformBufferSize = 25165824; // 24 MiB
inline constexpr uint64_t VertexBufferSize = 5242880;   // 5 MiB
inline constexpr uint64_t IndexBufferSize = 2097152;    // 2 MiB
inline constexpr uint64_t StorageBufferSize = 8388608;  // 8 MiB
inline constexpr uint64_t TextureUploadSize = 25165824; // 24 MiB
// What a staging buffer holds per kind at the start (unpinned scale): about twice the
// per-frame peaks measured over a tour of the game's rooms. A frame that needs more spills
// into memory it allocates for that frame (see SpillBuffer), up to the sizes above.
inline constexpr uint64_t StartVertexSize = 4u << 20;
inline constexpr uint64_t StartUniformSize = 8u << 20;
inline constexpr uint64_t StartIndexSize = 2u << 20;
inline constexpr uint64_t StartStorageSize = 8u << 20;
inline constexpr uint64_t StartTextureUploadSize = 8u << 20;

enum class StagingKind : uint8_t { Vertex, Uniform, Index, Storage };

// Texture vertex fetch: the GX shaders read the vertex and array data from R32Uint textures
// instead of the whole-buffer read-only storage bindings, which a fragment stage can do on any
// backend but a vertex stage cannot on one without vertex-stage storage buffers (OpenGL 3.3).
// One texture row is 4096 texels (16 KiB) and holds words 4096 * row .. 4096 * row + 4095 of the
// buffer, so a shader finds word n at (n & 4095, n >> 12). Off by default; fixed once at device
// creation (webgpu::initialize) from AURORA_VERTEX_FETCH_TEXTURE=1 or from the adapter's
// compatibility-mode limits, and never changes after.
extern bool g_vertexFetchTexture;
// H700's permanent stage-specific contract. IG-05 uses this same immutable flag for
// shader generation/cache identity; it is not a runtime fallback or an environment override.
#ifdef AURORA_H700
inline constexpr bool g_mixedFetch = true;
#else
inline constexpr bool g_mixedFetch = false;
#endif
inline constexpr uint32_t MixedSampledTextureBudget = 16;
inline constexpr uint32_t MixedFragmentStorageBinding = 2;
// A scale-1 frame's full staging allocation, including texture uploads (not a probe buffer).
inline constexpr uint64_t MixedMinimumBufferSize =
    VertexBufferSize + UniformBufferSize + IndexBufferSize + StorageBufferSize +
    (UseTextureBuffer ? TextureUploadSize : 0);
// Compute shaders: OpenGL 3.3 has none, so the two compute passes (the volumetric fog's froxels and
// the depth peek readback) run as render passes over the same per-pixel math instead. Off by
// default; fixed once at device creation (webgpu::initialize) from AURORA_NO_COMPUTE=1 or from the
// adapter reporting no compute (0 workgroups per dimension or 0 invocations per workgroup), and
// never changes after. Nothing may create a compute pipeline while it is set. In the H700
// mixed profile the fragment storage binding is still required, independently of compute.
extern bool g_noCompute;


// What a frame can hold of each kind of data. The constants above are the sizes at
// AuroraConfig::frameBufferScale 1; initialize() fixes the sizes in use for the run.
struct FrameBufferSizes {
  uint64_t vertex = VertexBufferSize;
  uint64_t uniform = UniformBufferSize;
  uint64_t index = IndexBufferSize;
  uint64_t storage = StorageBufferSize;
  uint64_t textureUpload = TextureUploadSize;
  // What a frame can hold of each kind, staging and spill together: the most the device
  // buffers' per-frame part grows to (storage: storageReserve). The four above are what the
  // staging buffers hold, the same at a pinned scale and smaller otherwise.
  uint64_t maxVertex = VertexBufferSize;
  uint64_t maxUniform = UniformBufferSize;
  uint64_t maxIndex = IndexBufferSize;
  uint64_t maxStorage = StorageBufferSize;
  // The device buffers' per-frame part now: the max above at a pinned scale, else it starts at
  // the staging size and grows (grow_frame_buffer) when a frame needs more.
  uint64_t deviceVertex = VertexBufferSize;
  uint64_t deviceUniform = UniformBufferSize;
  uint64_t deviceIndex = IndexBufferSize;
  uint64_t deviceStorage = StorageBufferSize;
  // The arrays' scaled size (frameStorageScale in use), before staging starts smaller.
  uint64_t scaledStorage = StorageBufferSize;
  // The vertices' scaled size (frameBufferScale in use); maxVertex can be more in resident mode.
  uint64_t scaledVertex = VertexBufferSize;
  // The most a frame's arrays take in the device storage buffer: the most `storage` can grow
  // to, which is also maxStorage.
  uint64_t storageReserve = StorageBufferSize;
  // The most `storage` grows to after an overflow (0: it is fixed).
  uint64_t storageCap = 0;
  // The scaled limits AuroraConfig::frameBuffersDeferred holds back while the max* above are
  // the unscaled ones (all 0: none held).
  uint64_t heldVertex = 0;
  uint64_t heldUniform = 0;
  uint64_t heldIndex = 0;
  uint64_t heldStorage = 0;

  // Kept across frames (AuroraConfig::residentGeometryMiB): the device buffers hold these
  // first, from offset 0, so they stay put when the per-frame part after them grows; no
  // staging buffer holds them.
  uint64_t residentVertex = 0;
  uint64_t residentIndex = 0;
  uint64_t residentStorage = 0;

  uint64_t staging() const noexcept {
    return vertex + uniform + index + storage + (UseTextureBuffer ? textureUpload : 0);
  }
  // Where a frame's data of this kind starts in its device buffer: after the resident region.
  uint64_t base(StagingKind kind) const noexcept {
    switch (kind) {
    case StagingKind::Vertex:
      return residentVertex;
    case StagingKind::Index:
      return residentIndex;
    case StagingKind::Storage:
      return residentStorage;
    default:
      return 0;
    }
  }
  uint64_t limit(StagingKind kind) const noexcept {
    switch (kind) {
    case StagingKind::Vertex:
      return maxVertex;
    case StagingKind::Uniform:
      return maxUniform;
    case StagingKind::Index:
      return maxIndex;
    default:
      return maxStorage;
    }
  }
  uint64_t device(StagingKind kind) const noexcept {
    switch (kind) {
    case StagingKind::Vertex:
      return deviceVertex;
    case StagingKind::Uniform:
      return deviceUniform;
    case StagingKind::Index:
      return deviceIndex;
    default:
      return deviceStorage;
    }
  }
};
const FrameBufferSizes& frame_buffer_sizes() noexcept;
// Grows a device buffer's per-frame part to hold `need` bytes of a frame (at most its limit)
// in the middle of recording one: waits for the render worker, copies the old buffer whole into
// a bigger one in the frame slot's encoder (after its BeginFrame job), and rebuilds the bind
// groups. The slot remains owned until every queued/running continuation is quiescent.
bool grow_frame_buffer(StagingKind kind, uint64_t need, size_t frameSlot);
// A frame spilled (see SpillBuffer) after using `peak` bytes of this kind. At the next frame
// start the staging buffers grow to the next power of two >= peak, capped at what a frame can
// hold, so a scene that outgrows the starting size spills once and not every frame.
void request_staging_growth(StagingKind kind, uint64_t peak) noexcept;

enum class ResidentBuffer : uint8_t { Vertex, Index, Storage };
// The part of a shared buffer kept across frames, as offsets into that buffer; size 0
// when there is none.
Range resident_region(ResidentBuffer kind) noexcept;
// Copies `size` bytes into a resident region at `offset` (an offset into the buffer, as
// resident_region gives), before the pass being recorded draws. False outside a frame's
// recording, where nothing is copied.
bool queue_resident_upload(ResidentBuffer kind, uint32_t offset, const uint8_t* data, size_t size);

namespace detail {
// Group 1's legacy material window stays unchanged. The H700 candidate adds a second
// dynamic window into the same buffer, sized to the draw's complete immediate snapshot.
template <bool DrawUniform = false>
inline auto uniform_bind_group_layout_entries(uint64_t drawSize = 0) {
  std::array<wgpu::BindGroupLayoutEntry, DrawUniform ? 2 : 1> entries{};
  entries[0] = {
      .binding = 0,
      .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
      .buffer = {.type = wgpu::BufferBindingType::Uniform, .hasDynamicOffset = true},
  };
  if constexpr (DrawUniform) {
    entries[1] = {
        .binding = 1,
        .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
        .buffer = {.type = wgpu::BufferBindingType::Uniform, .hasDynamicOffset = true, .minBindingSize = drawSize},
    };
  }
  return entries;
}

// Shared by the real layout creation and CPU contract tests. Both mirrors remain vertex-visible;
// the fragment stage reads the original array buffer, never a third mirrored allocation.
inline auto static_bind_group_layout_entries(bool textureFetch) {
  std::array<wgpu::BindGroupLayoutEntry, g_mixedFetch ? 3 : 2> entries{};
  for (uint32_t i = 0; i < 2; ++i) {
    auto& entry = entries[i];
    entry.binding = i;
    entry.visibility = i == 0 || g_mixedFetch ? wgpu::ShaderStage::Vertex
                                            : wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    if (textureFetch || g_mixedFetch) {
      entry.texture = {.sampleType = wgpu::TextureSampleType::Uint,
                       .viewDimension = wgpu::TextureViewDimension::e2D};
    } else {
      entry.buffer = {.type = wgpu::BufferBindingType::ReadOnlyStorage};
    }
  }
  if constexpr (g_mixedFetch) {
    entries[MixedFragmentStorageBinding] = {
        .binding = MixedFragmentStorageBinding,
        .visibility = wgpu::ShaderStage::Fragment,
        .buffer = {.type = wgpu::BufferBindingType::ReadOnlyStorage},
    };
  }
  return entries;
}

inline uint32_t sampled_texture_count(std::span<const wgpu::BindGroupLayoutEntry> entries,
                                      wgpu::ShaderStage stage) noexcept {
  uint32_t count = 0;
  for (const auto& entry : entries) {
    if ((entry.visibility & stage) != wgpu::ShaderStage::None &&
        entry.texture.sampleType != wgpu::TextureSampleType::BindingNotUsed) {
      ++count;
    }
  }
  return count;
}

// Checked on the adapter AND on the device's granted limits. Undefined is not evidence
// of support. The baseline must fit before gfx's scale/resident sizing runs.
inline bool mixed_fetch_limits_supported(const wgpu::Limits& limits,
                                         const wgpu::CompatibilityModeLimits& compatibility,
                                         bool noCompute) noexcept {
  return limits.maxSampledTexturesPerShaderStage != WGPU_LIMIT_U32_UNDEFINED &&
         limits.maxSampledTexturesPerShaderStage >= MixedSampledTextureBudget &&
         limits.maxStorageBuffersPerShaderStage != WGPU_LIMIT_U32_UNDEFINED &&
         limits.maxStorageBuffersPerShaderStage >= (noCompute ? 1u : 2u) &&
         compatibility.maxStorageBuffersInVertexStage != WGPU_LIMIT_U32_UNDEFINED &&
         compatibility.maxStorageBuffersInFragmentStage != WGPU_LIMIT_U32_UNDEFINED &&
         compatibility.maxStorageBuffersInFragmentStage >= 1 &&
         limits.maxStorageBufferBindingSize != WGPU_LIMIT_U64_UNDEFINED &&
         limits.maxStorageBufferBindingSize >= StorageBufferSize &&
         limits.maxBufferSize != WGPU_LIMIT_U64_UNDEFINED && limits.maxBufferSize >= MixedMinimumBufferSize &&
         limits.maxTextureDimension2D != WGPU_LIMIT_U32_UNDEFINED && limits.maxTextureDimension2D >= 4096;
}

inline void request_mixed_fetch_limits(wgpu::Limits& limits, wgpu::CompatibilityModeLimits& compatibility,
                                       bool noCompute) noexcept {
  compatibility.maxStorageBuffersInVertexStage = 0;
  compatibility.maxStorageBuffersInFragmentStage = 1;
  limits.maxSampledTexturesPerShaderStage = MixedSampledTextureBudget;
  limits.maxStorageBuffersPerShaderStage = noCompute ? 1u : 2u;
  // Keep the supported binding-size/buffer limits requested by U: scaled, resident and
  // growable buffers are sized against these, not against a synthetic 64 KiB window.
}

struct Resources {
  wgpu::Buffer vertexBuffer;
  wgpu::Buffer uniformBuffer;
  wgpu::Buffer indexBuffer;
  wgpu::Buffer storageBuffer;
  // Texture vertex fetch: the vertex and storage buffers as textures (g_vertexFetchTexture).
  // Each is sized to the buffer it mirrors, which is rounded up to whole rows for it.
  wgpu::Texture vertexFetchTexture;
  wgpu::TextureView vertexFetchTextureView;
  wgpu::Texture storageFetchTexture;
  wgpu::TextureView storageFetchTextureView;
  wgpu::BindGroupLayout staticBindGroupLayout;
  wgpu::BindGroup staticBindGroup;
  wgpu::BindGroupLayout uniformBindGroupLayout;
  wgpu::BindGroup uniformBindGroup;
  wgpu::BindGroupLayout drawUniformBindGroupLayout;
  wgpu::BindGroup drawUniformBindGroup;
  wgpu::Limits limits;
  AuroraStats stats{};
};

Resources& resources() noexcept;
// Texture vertex fetch: the bytes in one row of a fetch texture.
inline constexpr uint64_t FetchTextureRowBytes = 16384;
// Texture vertex fetch: copies the rows of `kind`'s device buffer that hold bytes [lo, hi) into
// that buffer's fetch texture. Nothing when the mode is off, the kind has no fetch texture or the
// range is empty; call it after the copies that wrote the bytes.
void upload_fetch_texture_rows(const wgpu::CommandEncoder& encoder, StagingKind kind, uint64_t lo, uint64_t hi);
} // namespace detail
} // namespace aurora::gfx
