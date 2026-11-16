#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace aurora::gfx::render_worker {

enum class ItemType : uint8_t {
  BeginFrame,
  EncodePass,
  EndFrame,
  Sync,
  Shutdown,
};

using WorkCallback = std::function<void()>;
enum class Outcome { Completed, Rejected, Failed, Pending };

// Installed before starting producers, cleared only after their quiescence. All hooks run
// without queue/sync/slot locks. Only the designated main thread may run progress.
struct WaitHooks {
  std::thread::id main;
  std::function<bool()> healthy;
  WorkCallback progress;
  std::function<bool()> admitRoot;
};
void set_wait_hooks(WaitHooks hooks);
bool wait_healthy();
bool progress_wait();
bool root_admission_open();
void begin_draining(); // closes roots only; accepted continuations stay admissible
void close_admission(); // IG-11: only AFTER accepted upstream producers have finished
void notify_failure();
bool is_quiescent() noexcept; // this worker only, not FIFO/compiler/maps
bool prepare_bootstrap(); // IG-11: explicit fresh pre-worker window, never an inline stop fallback

struct SyncState {
  std::mutex mutex;
  std::condition_variable cv;
  bool complete = false;
  Outcome outcome = Outcome::Rejected;
};

struct QueueItem {
  ItemType type = ItemType::Sync;
  uint64_t frameId = 0;
  uint32_t passIndex = 0;
  WorkCallback work;
  std::shared_ptr<SyncState> sync;
  // CPU-only cancellation; never the graphics job or a success-only afterSubmit callback.
  WorkCallback cancel;
};

class BoundedQueue {
public:
  explicit BoundedQueue(size_t capacity);

  bool push(QueueItem item);
  Outcome push_result(QueueItem item);
  Outcome try_push(QueueItem& item); // Pending preserves item/captures; no progress or capacity wait
  bool try_close();
  void notify_waiters();
  std::optional<QueueItem> pop_for(std::chrono::milliseconds timeout, bool& closed);
  void close();
  void reset();
  [[nodiscard]] size_t size() const;

private:
  size_t m_capacity = 0;
  mutable std::mutex m_mutex;
  std::condition_variable m_notEmpty;
  std::condition_variable m_notFull;
  std::deque<QueueItem> m_items;
  bool m_closed = false;
};

// A single already accepted main frame's final recording continuations. Capture performs
// CPU recording only; the retained batch is admitted atomically without a capacity wait.
// It is not a second worker queue or permission to run rejected graphics inline.
class FrameTail {
public:
  bool capture(WorkCallback record);
  Outcome poll(); // Completed means queue admission, NOT execution/GPU completion; Pending retains the tail
private:
  std::shared_ptr<std::vector<QueueItem>> m_items;
  QueueItem m_submission;
};
bool capturing_frame_tail() noexcept;

class FrameSlotPool {
public:
  explicit FrameSlotPool(size_t slotCount);

  size_t acquire();
  std::optional<size_t> try_acquire();
  std::optional<size_t> acquire_wait();
  void release(size_t slot);
  void reset();
  [[nodiscard]] size_t free_count() const;

private:
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::vector<bool> m_freeSlots;
};

// CPU acquisition rollback; disarm only once ownership was transferred to accepted work.
class SlotLease {
public:
  SlotLease(FrameSlotPool& pool, size_t slot) : m_pool(&pool), m_slot(slot) {}
  SlotLease(const SlotLease&) = delete;
  SlotLease& operator=(const SlotLease&) = delete;
  ~SlotLease() { if (m_pool) m_pool->release(m_slot); }
  void disarm() { m_pool = nullptr; }
private:
  FrameSlotPool* m_pool;
  size_t m_slot;
};

void initialize();
bool shutdown(); // blocking compatibility wrapper
void request_shutdown(); // close once; no join/wait
Outcome poll_shutdown(); // Pending while active; join only after owner-thread exit/unbind ack

Outcome enqueue_begin_frame(uint64_t frameId, WorkCallback work, WorkCallback cancel = {});
Outcome enqueue_encode_pass(uint64_t frameId, uint32_t passIndex, WorkCallback work, WorkCallback cancel = {});
Outcome enqueue_end_frame(uint64_t frameId, WorkCallback work, WorkCallback cancel = {});
Outcome enqueue_work(WorkCallback work, WorkCallback cancel = {});
Outcome synchronize();

bool is_worker_thread() noexcept;
bool is_idle() noexcept;

} // namespace aurora::gfx::render_worker
