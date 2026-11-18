#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace aurora::gx::immutable {

using SourceId = uint64_t;
inline constexpr uint64_t RetainedBudgetBytes = 32ull * 1024 * 1024;
enum class SourceKind : uint8_t { Array, DisplayList };

namespace detail {
struct Source;
}

// A CPU-only owning view. Copying it keeps the snapshot AND its budget charge alive,
// even after unregister/release. No method reads original_base().
class SourceView {
public:
  SourceView() = default;
  explicit operator bool() const noexcept { return bool(m_source); }
  SourceId id() const noexcept;
  SourceKind kind() const noexcept;
  const uint8_t* data() const noexcept;
  uint32_t size() const noexcept { return m_source ? m_size : 0; }
  std::span<const uint8_t> bytes() const noexcept { return {data(), size()}; }
  const void* original_base() const noexcept;
  uint32_t original_size() const noexcept;

private:
  friend SourceView lookup_array(SourceId, uint32_t) noexcept;
  friend SourceView lookup_display_list(SourceId, uint32_t) noexcept;
  SourceView(std::shared_ptr<const detail::Source> source, uint32_t size) noexcept
  : m_source(std::move(source)), m_size(m_source ? size : 0) {}
  std::shared_ptr<const detail::Source> m_source;
  uint32_t m_size = 0;
};

// Game/main producer only. One snapshot per successful registration; 0 means use
// unchanged legacy transport. Exact bases only; overlapping active ranges are rejected.
// IDs are never reused, including rejected enqueues and terminal retirement.
SourceId register_source(const void* data, uint32_t size, SourceKind kind) noexcept;
// Revokes producer admission immediately. False retains an obligation for root cleanup
// (or a later retry); it never frees a snapshot still referenced by accepted work.
bool unregister_source(SourceId id) noexcept;
SourceId producer_admission(const void* data, uint32_t size, SourceKind kind) noexcept;
// Changes on registration/revocation, so CGX cannot suppress a rebind after pointer ABA.
uint64_t producer_epoch() noexcept;

// FIFO processor only, in stream order. REGISTER/RELEASE each carry one BE u64 ID.
// A failed consume_register is a transport contract failure, NOT raw-pointer fallback.
bool consume_register(SourceId ticket) noexcept;
bool consume_release(SourceId id) noexcept;
// Ordered active admission, separate from lifetime: an already held SourceView stays
// readable when active(id) becomes false. Cache entries must use IDs, never pointers.
bool active(SourceId id) noexcept;
SourceView lookup_array(SourceId id, uint32_t size) noexcept;
SourceView lookup_display_list(SourceId id, uint32_t size) noexcept;
SourceView lookup_array(const void* originalBase, uint32_t size) noexcept;
SourceView lookup_display_list(const void* originalBase, uint32_t size) noexcept;

// Thread-safe statistics: includes pending tickets, active entries AND all held views.
// Unregister and terminal cleanup do not reset this counter.
uint64_t retained_bytes() noexcept;
// Read-only obligation query, only with producer stopped and processor joined.
// Held CPU views do not require new commands and therefore need not be empty.
bool registries_empty() noexcept;
// Production checks the actual H700 CpuQuiescent/Maps bridge before touching state.
// CPU-only, idempotent, terminal (no same-generation restart), never touches GPU holders.
bool release_at_cpu_quiescence() noexcept;

} // namespace aurora::gx::immutable
