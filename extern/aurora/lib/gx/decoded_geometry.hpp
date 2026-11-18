#pragma once

#include "decoded_cpu.hpp"

#include <optional>

namespace aurora::gx::decoded {

inline constexpr uint64_t GpuBudgetBytes = 64ull * 1024 * 1024;
inline constexpr uint32_t MaxCacheEntries = 8192;

constexpr bool requested(const char* value, const char* bench) noexcept {
  return value && std::string_view(value) == "1" && bench &&
         (std::string_view(bench) == "1" || std::string_view(bench) == "menu");
}
bool enabled() noexcept;

struct Admission {
  MeshRef ref;
  uint32_t vertexCount;
  uint32_t indexCount;
};

// FIFO processor only. A hit depends on source IDs, raw attribute semantics and array
// bases, never on recycled game pointers. Success includes a strong recording pin.
std::optional<Admission> acquire(const immutable::SourceView& list, uint32_t vertexOffset,
                                  const DecodeInput& input) noexcept;
void invalidate_source(immutable::SourceId id) noexcept;

// Render worker only: stale recorded references are a contract failure, not raw fallback.
bool valid(MeshRef ref) noexcept;
void bind(MeshRef ref, const wgpu::RenderPassEncoder& pass);
// Only from the existing guarded GPU-holder release, after worker/compiler/map retirement.
void shutdown() noexcept;

} // namespace aurora::gx::decoded
