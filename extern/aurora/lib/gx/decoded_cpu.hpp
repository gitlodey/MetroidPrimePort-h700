#pragma once

#include "decoded_vertex.hpp"
#include "immutable.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace aurora::gx::decoded {

// Per-call workspace AND worst-case output allocations; excludes the borrowed input
// snapshots and any previous caller-owned mesh. No growing hash table or output buffer.
inline constexpr uint64_t ScratchBudgetBytes = 16ull * 1024 * 1024;
inline constexpr uint32_t MaxLocalVertices = 1u << 16;

struct DecodeInput {
  // Keep the original FIFO attrs/vtxStride, including when decodedNative is set.
  const ShaderConfig& config;
  GXPrimitive primitive;
  uint32_t vertexCount;
  std::span<const uint8_t> vertices;
  const std::array<immutable::SourceView, MaxVtxAttr>& arrays;
  // Element indices, not byte offsets: array address = (baseIndex + index) * attr.stride.
  const std::array<uint32_t, MaxVtxAttr>& baseIndices;
};

struct CpuMesh {
  // Native Float32/Uint32 fields from make_layout(config), with local Uint16
  // triangle-list indices (0xffff is an ordinary vertex, not primitive restart).
  std::vector<uint8_t> vertices;
  std::vector<uint16_t> indices;
  uint32_t vertexCount = 0;
};

enum class DecodeFailure : uint8_t {
  None,
  UnsupportedPrimitive,
  UnsupportedConfig,
  InvalidVertexLayout,
  VertexDataOutOfBounds,
  MissingImmutableSource,
  ArrayDataOutOfBounds,
  NonFiniteValue,
  VertexLimitExceeded,
  ScratchBudgetExceeded,
  AllocationFailed,
};

// CPU only: never reads original_base(), GX globals or GPU state. Borrowed inputs
// must stay stable through this call. On failure out is unchanged, failure explains
// why the WHOLE draw must use the legacy path. On success failure is None.
[[nodiscard]] bool decode(const DecodeInput& input, CpuMesh& out, DecodeFailure& failure) noexcept;

} // namespace aurora::gx::decoded
