#include "immutable.hpp"

#include <dolphin/gx/GXAurora.h>

#include <atomic>
#include <limits>
#include <iterator>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

// Keep snapshot ownership independent of internal.hpp, WebGPU and resident allocators.
namespace aurora::gx::fifo {
bool enqueue_ownership(uint16_t command, uint64_t sourceId) noexcept;
bool in_display_list();
}
#ifdef AURORA_H700
namespace aurora::h700 {
bool generation_cpu_quiescence_held() noexcept;
}
#endif

namespace aurora::gx::immutable {
namespace {
std::atomic<uint64_t> sRetainedBytes{0};

struct BudgetLease {
  uint32_t size = 0;
  BudgetLease() = default;
  BudgetLease(const BudgetLease&) = delete;
  BudgetLease& operator=(const BudgetLease&) = delete;
  BudgetLease(BudgetLease&& other) noexcept : size(std::exchange(other.size, 0)) {}
  ~BudgetLease() { if (size != 0) sRetainedBytes.fetch_sub(size, std::memory_order_acq_rel); }
  bool acquire(uint32_t bytes) noexcept {
    uint64_t used = sRetainedBytes.load(std::memory_order_acquire);
    do {
      if (bytes > RetainedBudgetBytes - used) return false;
    } while (!sRetainedBytes.compare_exchange_weak(used, used + bytes, std::memory_order_acq_rel));
    size = bytes;
    return true;
  }
};
} // namespace

namespace detail {
struct Source {
  // Declared before bytes: the payload is destroyed BEFORE its budget is released.
  BudgetLease budget;
  SourceId id;
  SourceKind kind;
  const void* originalBase;
  uint32_t size;
  std::vector<uint8_t> bytes;
  Source(BudgetLease&& charge, SourceId sourceId, SourceKind sourceKind, const void* base, uint32_t length)
  : budget(std::move(charge)), id(sourceId), kind(sourceKind), originalBase(base), size(length),
    bytes(static_cast<const uint8_t*>(base), static_cast<const uint8_t*>(base) + length) {}
};
} // namespace detail

namespace {
using SourceRef = std::shared_ptr<const detail::Source>;
struct Admission {
  const void* base;
  uint32_t size;
  SourceKind kind;
};
// Only pending handoff is shared by producer/processor. Active lookup never takes
// a transport mutex per draw. Root may access all maps only after actual joins.
std::mutex sTicketMutex;
std::unordered_map<SourceId, SourceRef> sTickets;
std::map<uintptr_t, SourceId> sProducerRanges;
std::unordered_map<SourceId, Admission> sProducerIds;
std::unordered_map<SourceId, SourceRef> sProcessorIds;
std::unordered_map<const void*, SourceId> sProcessorBases;
SourceId sNextId = 1;
uint64_t sProducerEpoch = 0;
bool sTerminal = false;

void advance_epoch() noexcept {
  if (sProducerEpoch != std::numeric_limits<uint64_t>::max()) ++sProducerEpoch;
}

bool valid_range(const void* data, uint32_t size) noexcept {
  return data != nullptr && size != 0 &&
         uintptr_t(data) <= std::numeric_limits<uintptr_t>::max() - size;
}

bool overlaps(uintptr_t base, uint32_t size) noexcept {
  const auto next = sProducerRanges.lower_bound(base);
  if (next != sProducerRanges.end() && next->first - base < size) return true;
  if (next != sProducerRanges.begin()) {
    const auto prev = std::prev(next);
    const auto found = sProducerIds.find(prev->second);
    if (found != sProducerIds.end() && base - prev->first < found->second.size) return true;
  }
  return false;
}

SourceRef find_source(SourceId id, uint32_t size, SourceKind kind) noexcept {
  const auto found = sProcessorIds.find(id);
  if (found == sProcessorIds.end() || found->second->kind != kind || size == 0 ||
      size > found->second->size || (kind == SourceKind::DisplayList && size != found->second->size)) return {};
  return found->second;
}
} // namespace

SourceId SourceView::id() const noexcept { return m_source ? m_source->id : 0; }
SourceKind SourceView::kind() const noexcept { return m_source ? m_source->kind : SourceKind::Array; }
const uint8_t* SourceView::data() const noexcept { return m_source ? m_source->bytes.data() : nullptr; }
const void* SourceView::original_base() const noexcept { return m_source ? m_source->originalBase : nullptr; }
uint32_t SourceView::original_size() const noexcept { return m_source ? m_source->size : 0; }

SourceId register_source(const void* data, uint32_t size, SourceKind kind) noexcept {
  if (sTerminal || !valid_range(data, size) || fifo::in_display_list() ||
      (kind != SourceKind::Array && kind != SourceKind::DisplayList) ||
      sNextId == std::numeric_limits<SourceId>::max() || overlaps(uintptr_t(data), size)) return 0;
  const SourceId id = sNextId++; // even a rejected enqueue burns its ID
  BudgetLease charge;
  if (!charge.acquire(size)) return 0;
  try {
    auto source = std::make_shared<const detail::Source>(std::move(charge), id, kind, data, size);
    sProducerIds.emplace(id, Admission{data, size, kind});
    sProducerRanges.emplace(uintptr_t(data), id);
    {
      std::lock_guard lock{sTicketMutex};
      sTickets.emplace(id, std::move(source));
    }
    if (fifo::enqueue_ownership(GX_AURORA_REGISTER_IMMUTABLE_SOURCE, id)) {
      advance_epoch();
      return id;
    }
  } catch (...) {
    // No command is enqueued until every owning/index allocation has succeeded.
  }
  {
    std::lock_guard lock{sTicketMutex};
    sTickets.erase(id);
  }
  sProducerRanges.erase(uintptr_t(data));
  sProducerIds.erase(id);
  return 0;
}

bool unregister_source(SourceId id) noexcept {
  const auto found = sProducerIds.find(id);
  if (found == sProducerIds.end()) return true; // includes handles retired by the root bridge
  const auto range = sProducerRanges.find(uintptr_t(found->second.base));
  if (range != sProducerRanges.end() && range->second == id) {
    sProducerRanges.erase(range);
    advance_epoch();
  }
  // On rejection keep this ID as an obligation; a new snapshot can never make a
  // stale release target its new generation at the same address.
  if (!fifo::enqueue_ownership(GX_AURORA_RELEASE_IMMUTABLE_SOURCE, id)) return false;
  sProducerIds.erase(found);
  return true;
}

SourceId producer_admission(const void* data, uint32_t size, SourceKind kind) noexcept {
  if (sTerminal || data == nullptr || size == 0) return 0;
  const auto range = sProducerRanges.find(uintptr_t(data));
  if (range == sProducerRanges.end()) return 0;
  const auto found = sProducerIds.find(range->second);
  if (found == sProducerIds.end() || found->second.kind != kind || size > found->second.size ||
      (kind == SourceKind::DisplayList && size != found->second.size)) return 0;
  return found->first;
}
uint64_t producer_epoch() noexcept { return sProducerEpoch; }

bool consume_register(SourceId ticket) noexcept {
  if (sTerminal || ticket == 0 || sProcessorIds.contains(ticket)) return false;
  std::lock_guard lock{sTicketMutex};
  const auto pending = sTickets.find(ticket);
  if (pending == sTickets.end() || sProcessorBases.contains(pending->second->originalBase)) return false;
  try {
    sProcessorIds.emplace(ticket, pending->second);
    sProcessorBases.emplace(pending->second->originalBase, ticket);
  } catch (...) {
    sProcessorIds.erase(ticket);
    return false; // ticket stays owned for actual CPU-quiescent retirement
  }
  sTickets.erase(pending);
  return true;
}

bool consume_release(SourceId id) noexcept {
  const auto found = sProcessorIds.find(id);
  if (found == sProcessorIds.end()) return true;
  const auto base = sProcessorBases.find(found->second->originalBase);
  if (base != sProcessorBases.end() && base->second == id) sProcessorBases.erase(base);
  sProcessorIds.erase(found);
  return true;
}

bool active(SourceId id) noexcept { return id != 0 && sProcessorIds.contains(id); }
SourceView lookup_array(SourceId id, uint32_t size) noexcept {
  return SourceView{find_source(id, size, SourceKind::Array), size};
}
SourceView lookup_display_list(SourceId id, uint32_t size) noexcept {
  return SourceView{find_source(id, size, SourceKind::DisplayList), size};
}
SourceView lookup_array(const void* originalBase, uint32_t size) noexcept {
  const auto found = sProcessorBases.find(originalBase);
  return found == sProcessorBases.end() ? SourceView{} : lookup_array(found->second, size);
}
SourceView lookup_display_list(const void* originalBase, uint32_t size) noexcept {
  const auto found = sProcessorBases.find(originalBase);
  return found == sProcessorBases.end() ? SourceView{} : lookup_display_list(found->second, size);
}

uint64_t retained_bytes() noexcept { return sRetainedBytes.load(std::memory_order_acquire); }
bool registries_empty() noexcept {
  std::lock_guard lock{sTicketMutex};
  return sProducerIds.empty() && sProducerRanges.empty() && sTickets.empty() &&
         sProcessorIds.empty() && sProcessorBases.empty();
}
bool release_at_cpu_quiescence() noexcept {
#ifdef AURORA_H700
  if (!h700::generation_cpu_quiescence_held()) return false;
  sTerminal = true;
  advance_epoch();
  sProducerRanges.clear();
  sProducerIds.clear();
  {
    std::lock_guard lock{sTicketMutex};
    sTickets.clear();
  }
  sProcessorBases.clear();
  sProcessorIds.clear();
  return true;
#else
  return false;
#endif
}

} // namespace aurora::gx::immutable
