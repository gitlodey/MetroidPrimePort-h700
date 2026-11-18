// Standalone CPU contract tests: link immutable.cpp, no renderer/device or CMake setup.
#include "../lib/gx/immutable.hpp"
#include <dolphin/gx/GXAurora.h>

#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {
bool acceptEnqueue = true;
bool inDisplayList = false;
bool checkpointHeld = false;
uint64_t lastAttempt = 0;
std::vector<std::pair<uint16_t, uint64_t>> records;
unsigned checks = 0;
void check(bool condition, const char* why) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "immutable contract failed: %s\n", why);
    std::abort();
  }
}
} // namespace

namespace aurora::gx::fifo {
bool enqueue_ownership(uint16_t command, uint64_t id) noexcept {
  lastAttempt = id;
  if (!acceptEnqueue || inDisplayList) return false;
  records.emplace_back(command, id);
  return true;
}
bool in_display_list() { return inDisplayList; }
} // namespace aurora::gx::fifo
namespace aurora::h700 {
bool generation_cpu_quiescence_held() noexcept { return checkpointHeld; }
} // namespace aurora::h700

using namespace aurora::gx::immutable;

static void snapshot_and_ordered_release() {
  std::array<uint8_t, 4> original{1, 2, 3, 4};
  const auto epoch = producer_epoch();
  const auto id = register_source(original.data(), original.size(), SourceKind::Array);
  check(id != 0 && producer_epoch() != epoch, "registration publishes a new epoch");
  check(!active(id) && !lookup_array(original.data(), 4), "producer admission is not processor admission");
  check(producer_admission(original.data(), 4, SourceKind::Array) == id, "producer exact-base lookup");
  check(records.back() == std::pair<uint16_t, uint64_t>{GX_AURORA_REGISTER_IMMUTABLE_SOURCE, id}, "register record ID");
  check(consume_register(id), "consume checked ticket");
  check(!consume_register(id), "a ticket cannot be consumed twice");
  auto held = lookup_array(id, 3);
  check(held && held.id() == id && held.size() == 3 && held.original_size() == 4 &&
        held.original_base() == original.data(), "owning prefix view preserves original range");
  original.fill(99);
  check(held.bytes()[0] == 1 && held.bytes()[2] == 3, "snapshot never rereads original storage");
  check(!lookup_array(id, 5) && lookup_array(id, 5).size() == 0, "invalid view is empty");
  check(!lookup_display_list(id, 4), "array cannot be admitted as a display list");
  check(unregister_source(id), "queue release");
  check(producer_admission(original.data(), 4, SourceKind::Array) == 0, "unregister revokes future producer calls");
  check(active(id) && retained_bytes() == 4, "queued uses retain ordered processor admission");
  check(consume_release(id) && !active(id) && !lookup_array(id, 4), "release revokes processor lookup");
  check(registries_empty() && retained_bytes() == 4 && held.bytes()[0] == 1, "held view outlives both registries");
  held = {};
  check(retained_bytes() == 0, "last view releases its budget");
}

static void freed_original_and_moved_view() {
  auto original = std::make_unique<std::array<uint8_t, 4>>(std::array<uint8_t, 4>{11, 12, 13, 14});
  const auto id = register_source(original->data(), 4, SourceKind::Array);
  check(consume_register(id), "heap source registration");
  auto view = lookup_array(id, 4);
  check(unregister_source(id) && consume_release(id), "heap source unregistered");
  original.reset();
  auto held = std::move(view);
  check(!view && view.data() == nullptr && view.bytes().empty(), "moved-from view is empty");
  check(held.bytes()[3] == 14, "legacy decoder view survives actual original free under ASan");
  held = {};
  check(retained_bytes() == 0, "freed original view charge retired");
}

static void concurrent_ticket_handoff() {
  constexpr unsigned count = 64;
  std::array<std::array<uint8_t, 4>, count> originals{};
  std::array<SourceId, count> ids{};
  std::atomic<unsigned> published{0};
  std::atomic<bool> okay{true};
  std::thread processor([&] {
    for (unsigned i = 0; i < count; ++i) {
      while (published.load(std::memory_order_acquire) <= i) std::this_thread::yield();
      if (!consume_register(ids[i])) { okay.store(false); continue; }
      auto held = lookup_array(ids[i], 4);
      if (!consume_release(ids[i]) || !held || held.data()[0] != i || active(ids[i])) okay.store(false);
    }
  });
  for (unsigned i = 0; i < count; ++i) {
    originals[i].fill(uint8_t(i));
    ids[i] = register_source(originals[i].data(), 4, SourceKind::Array);
    if (ids[i] == 0 || !unregister_source(ids[i])) okay.store(false);
    originals[i].fill(255);
    published.store(i + 1, std::memory_order_release);
  }
  processor.join();
  check(okay.load() && registries_empty() && retained_bytes() == 0, "concurrent producer/ticket/processor ownership handoff");
}

static void pointer_aba_and_display_lists() {
  std::array<uint8_t, 8> storage{1, 2, 3, 4, 5, 6, 7, 8};
  const auto oldId = register_source(storage.data(), 8, SourceKind::DisplayList);
  check(consume_register(oldId), "old DL registration");
  auto oldView = lookup_display_list(oldId, 8);
  check(!lookup_display_list(oldId, 7), "DL admission requires exact length");
  check(register_source(storage.data() + 1, 4, SourceKind::Array) == 0, "overlapping active registration rejected");
  check(unregister_source(oldId), "old source unregister");
  storage.fill(42);
  const auto newId = register_source(storage.data(), 8, SourceKind::Array);
  check(newId > oldId && !active(newId), "reused address has new non-reused ID");
  check(consume_release(oldId) && consume_register(newId), "release/register consumed in FIFO order");
  check(consume_release(oldId) && active(newId), "stale release cannot target replacement at same address");
  check(oldView.data()[0] == 1 && lookup_array(newId, 8).data()[0] == 42, "old and new snapshots coexist");
  check(lookup_array(storage.data(), 8).id() == newId, "pointer lookup resolves current generation only");
  check(unregister_source(newId) && consume_release(newId), "replacement retired");
  oldView = {};
  check(registries_empty() && retained_bytes() == 0, "ABA references retired");
}

static void rejected_and_unconsumed_tickets() {
  std::array<uint8_t, 4> original{8, 7, 6, 5};
  const auto before = records.size();
  acceptEnqueue = false;
  check(register_source(original.data(), 4, SourceKind::Array) == 0, "rejected registration returns legacy admission");
  const auto rejectedId = lastAttempt;
  check(records.size() == before && registries_empty() && retained_bytes() == 0, "enqueue rejection rolls back all ownership");
  acceptEnqueue = true;
  inDisplayList = true;
  check(register_source(original.data(), 4, SourceKind::Array) == 0, "ownership cannot be captured into user DL");
  inDisplayList = false;
  const auto id = register_source(original.data(), 4, SourceKind::Array);
  check(id > rejectedId, "rejected IDs are never recycled");
  check(unregister_source(id), "unregister before ticket consume");
  original.fill(0);
  check(!registries_empty() && retained_bytes() == 4, "accepted unpublished ticket stays owned");
  check(consume_register(id) && lookup_array(id, 4).data()[0] == 8, "ticket survives producer unregister and original rewrite");
  acceptEnqueue = false;
  const auto retryId = register_source(original.data(), 4, SourceKind::Array);
  check(retryId == 0, "another rejected registration is inert");
  acceptEnqueue = true;
  check(consume_release(id) && registries_empty() && retained_bytes() == 0, "queued release completes handoff lifetime");

  const auto live = register_source(original.data(), 4, SourceKind::Array);
  check(consume_register(live), "live source before rejected release");
  acceptEnqueue = false;
  check(!unregister_source(live), "rejected release reports failure");
  check(!registries_empty() && active(live) && retained_bytes() == 4 &&
        producer_admission(original.data(), 4, SourceKind::Array) == 0, "rejected release preserves obligation but closes admission");
  acceptEnqueue = true;
  check(unregister_source(live) && consume_release(live), "release can be retried without pointer ABA");
  check(registries_empty() && retained_bytes() == 0, "retry retires snapshot");
}

static void hard_budget_tracks_last_view() {
  std::vector<uint8_t> storage(RetainedBudgetBytes, 17);
  uint8_t extra = 9;
  const auto id = register_source(storage.data(), uint32_t(storage.size()), SourceKind::Array);
  check(id != 0 && consume_register(id), "entire hard budget can be retained");
  auto held = lookup_array(id, uint32_t(storage.size()));
  check(unregister_source(id) && consume_release(id) && registries_empty(), "budget source unregistered");
  check(retained_bytes() == RetainedBudgetBytes, "unregister does not reset surviving snapshot bytes");
  check(register_source(&extra, 1, SourceKind::Array) == 0, "one byte over 32 MiB is rejected while view lives");
  check(held.data()[storage.size() - 1] == 17, "full held snapshot stays readable");
  held = {};
  check(retained_bytes() == 0, "payload freed before charge is released");
  const auto next = register_source(&extra, 1, SourceKind::Array);
  check(next != 0 && consume_register(next) && unregister_source(next) && consume_release(next), "capacity recovered after last view");
}

static void guarded_terminal_release() {
  std::array<uint8_t, 4> a{3, 4, 5, 6}, b{7, 8, 9, 10};
  const auto live = register_source(a.data(), 4, SourceKind::Array);
  check(consume_register(live), "active terminal fixture");
  auto held = lookup_array(live, 4);
  const auto pending = register_source(b.data(), 4, SourceKind::DisplayList);
  check(pending != 0, "unconsumed terminal fixture");
  const auto recordCount = records.size();
  const auto epoch = producer_epoch();
  check(!release_at_cpu_quiescence() && active(live) && retained_bytes() == 8,
        "unproven checkpoint cannot mutate ownership");
  checkpointHeld = true;
  check(release_at_cpu_quiescence() && registries_empty() && !active(live), "actual held checkpoint clears CPU registries/tickets");
  check(records.size() == recordCount && producer_epoch() != epoch, "terminal release enqueues nothing");
  check(retained_bytes() == 4 && held.data()[0] == 3, "terminal cleanup keeps held views and budget alive");
  check(unregister_source(live) && unregister_source(pending) && records.size() == recordCount, "owner destructor handles are inert after root release");
  check(release_at_cpu_quiescence() && register_source(a.data(), 4, SourceKind::Array) == 0, "terminal release is idempotent and forbids restart");
  held = {};
  check(retained_bytes() == 0, "last terminal view releases final CPU bytes");
}

int main() {
  check(registries_empty() && retained_bytes() == 0, "initial state");
  uint8_t byte = 1;
  check(register_source(nullptr, 4, SourceKind::Array) == 0 &&
        register_source(&byte, 0, SourceKind::Array) == 0 &&
        register_source(&byte, 1, static_cast<SourceKind>(9)) == 0 &&
        register_source(reinterpret_cast<const void*>(std::numeric_limits<uintptr_t>::max() - 1),
                        4, SourceKind::Array) == 0, "invalid source ranges/kinds rejected without copying");
  snapshot_and_ordered_release();
  freed_original_and_moved_view();
  concurrent_ticket_handoff();
  pointer_aba_and_display_lists();
  rejected_and_unconsumed_tickets();
  hard_budget_tracks_last_view();
  guarded_terminal_release();
  std::printf("immutable source contracts: %u checks PASS\n", checks);
}
