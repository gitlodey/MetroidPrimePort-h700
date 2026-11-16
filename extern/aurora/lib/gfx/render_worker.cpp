#include "render_worker.hpp"

#include "../thread.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>

#include <tracy/Tracy.hpp>

namespace aurora::gfx::render_worker {
namespace {
constexpr size_t QueueCapacity = 256;
constexpr auto IdlePumpInterval = std::chrono::milliseconds{1};
BoundedQueue g_queue{QueueCapacity};
thread::Thread g_thread;
std::atomic_bool g_running = false;
std::atomic_bool g_started = false;
std::atomic_bool g_joined = true;
std::atomic_bool g_exited = true;
std::atomic_bool g_rootsOpen = true;
std::atomic_bool g_failed = false;
std::atomic_size_t g_pendingItems = 0;
std::atomic<std::shared_ptr<const WaitHooks>> g_hooks;
thread_local bool g_onWorker = false;

void complete_sync(const std::shared_ptr<SyncState>& sync, Outcome outcome) {
  if (!sync) return;
  {
    std::lock_guard lock{sync->mutex};
    sync->outcome = outcome;
    sync->complete = true;
  }
  sync->cv.notify_all();
}
void cancel_item(QueueItem& item, Outcome outcome) {
  if (item.cancel) item.cancel();
  complete_sync(item.sync, outcome);
}
void worker_main(std::stop_token token) {
  g_onWorker = true;
  while (true) {
    if (token.stop_requested()) { begin_draining(); close_admission(); }
    bool closed = false;
    auto item = g_queue.pop_for(IdlePumpInterval, closed);
    if (!item) {
      if (closed) break;
      continue;
    }
    const auto outcome = wait_healthy() ? Outcome::Completed : Outcome::Failed;
    if (outcome == Outcome::Completed) {
      if (item->work) item->work();
    } else if (item->cancel) {
      item->cancel();
    }
    // A cancelled waiter never destroys an executing job's captures. Release them here,
    // after the job returns, before acknowledging this marker/worker quiescence.
    auto sync = item->sync;
    item.reset();
    g_pendingItems.fetch_sub(1, std::memory_order_acq_rel);
    complete_sync(sync, outcome);
  }
  g_onWorker = false;
  g_running.store(false, std::memory_order_release);
  g_exited.store(true, std::memory_order_release);
}
Outcome enqueue(QueueItem item) {
  ZoneScoped;
  if (!wait_healthy()) {
    cancel_item(item, Outcome::Failed);
    return Outcome::Failed;
  }
  if (is_worker_thread() || !g_started.load(std::memory_order_acquire)) {
    // Explicit cold bootstrap only. Stopped/closed workers NEVER run rejected work inline.
    if (item.work) item.work();
    complete_sync(item.sync, Outcome::Completed);
    return Outcome::Completed;
  }
  if (!g_running.load(std::memory_order_acquire)) {
    cancel_item(item, Outcome::Rejected);
    return Outcome::Rejected;
  }
  g_pendingItems.fetch_add(1, std::memory_order_acq_rel);
  // Keep the CPU cancellation/sync obligations if the queue refuses admission.
  auto cancel = item.cancel;
  auto sync = item.sync;
  const auto result = g_queue.push_result(std::move(item));
  if (result != Outcome::Completed) {
    if (cancel) cancel();
    complete_sync(sync, result);
    cancel = {};
    sync = {};
    g_pendingItems.fetch_sub(1, std::memory_order_acq_rel);
  }
  return result;
}
} // namespace

void set_wait_hooks(WaitHooks hooks) {
  g_hooks.store(std::make_shared<const WaitHooks>(std::move(hooks)), std::memory_order_release);
}
void notify_failure() {
  g_failed.store(true, std::memory_order_release);
  g_queue.notify_waiters();
}
bool wait_healthy() {
  if (g_failed.load(std::memory_order_acquire)) return false;
  const auto hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->healthy && !hooks->healthy()) {
    notify_failure();
    return false;
  }
  return true;
}
bool progress_wait() {
  if (!wait_healthy()) return false;
  const auto hooks = g_hooks.load(std::memory_order_acquire);
  if (hooks && hooks->main == std::this_thread::get_id() && hooks->progress) hooks->progress();
  return wait_healthy();
}
bool root_admission_open() {
  if (!wait_healthy() || !g_rootsOpen.load(std::memory_order_acquire)) return false;
  const auto hooks = g_hooks.load(std::memory_order_acquire);
  return !hooks || !hooks->admitRoot || hooks->admitRoot();
}
void begin_draining() { g_rootsOpen.store(false, std::memory_order_release); }
void close_admission() { begin_draining(); g_queue.close(); }
bool is_quiescent() noexcept {
  return g_joined.load(std::memory_order_acquire) && g_pendingItems.load(std::memory_order_acquire) == 0;
}
bool prepare_bootstrap() {
  if (!is_quiescent()) return false;
  g_hooks.store({}, std::memory_order_release);
  g_failed.store(false, std::memory_order_release);
  g_rootsOpen.store(true, std::memory_order_release);
  g_started.store(false, std::memory_order_release);
  return true;
}

BoundedQueue::BoundedQueue(size_t capacity) : m_capacity(capacity) {}
bool BoundedQueue::push(QueueItem item) { return push_result(std::move(item)) == Outcome::Completed; }
Outcome BoundedQueue::push_result(QueueItem item) {
  ZoneScoped;
  while (true) {
    if (!wait_healthy()) return Outcome::Failed;
    std::unique_lock lock{m_mutex};
    if (m_closed) return Outcome::Rejected;
    if (m_items.size() < m_capacity) {
      m_items.emplace_back(std::move(item));
      lock.unlock();
      m_notEmpty.notify_one();
      return Outcome::Completed;
    }
    lock.unlock();
    if (!progress_wait()) return Outcome::Failed;
    lock.lock();
    m_notFull.wait_for(lock, IdlePumpInterval, [&] { return m_closed || m_items.size() < m_capacity; });
  }
}
std::optional<QueueItem> BoundedQueue::pop_for(std::chrono::milliseconds timeout, bool& closed) {
  std::unique_lock lock{m_mutex};
  m_notEmpty.wait_for(lock, timeout, [&] { return m_closed || !m_items.empty(); });
  closed = m_closed && m_items.empty();
  if (m_items.empty()) return std::nullopt;
  auto item = std::move(m_items.front());
  m_items.pop_front();
  lock.unlock();
  m_notFull.notify_one();
  return item;
}
void BoundedQueue::notify_waiters() {
  m_notEmpty.notify_all();
  m_notFull.notify_all();
}
void BoundedQueue::close() {
  {
    std::lock_guard lock{m_mutex};
    m_closed = true;
  }
  notify_waiters();
}
void BoundedQueue::reset() {
  {
    std::lock_guard lock{m_mutex};
    m_items.clear();
    m_closed = false;
  }
  notify_waiters();
}
size_t BoundedQueue::size() const {
  std::lock_guard lock{m_mutex};
  return m_items.size();
}

FrameSlotPool::FrameSlotPool(size_t slotCount) : m_freeSlots(slotCount, true) {}
size_t FrameSlotPool::acquire() {
  std::unique_lock lock{m_mutex};
  m_cv.wait(lock, [&] { return std::ranges::find(m_freeSlots, true) != m_freeSlots.end(); });
  for (size_t i = 0; i < m_freeSlots.size(); ++i) {
    if (m_freeSlots[i]) {
      m_freeSlots[i] = false;
      return i;
    }
  }
  return SIZE_MAX;
}
std::optional<size_t> FrameSlotPool::acquire_wait() {
  while (wait_healthy()) {
    if (auto slot = try_acquire()) return slot;
    if (!progress_wait()) break;
    std::unique_lock lock{m_mutex};
    m_cv.wait_for(lock, IdlePumpInterval);
  }
  return std::nullopt;
}
std::optional<size_t> FrameSlotPool::try_acquire() {
  std::lock_guard lock{m_mutex};
  for (size_t i = 0; i < m_freeSlots.size(); ++i) {
    if (m_freeSlots[i]) {
      m_freeSlots[i] = false;
      return i;
    }
  }
  return std::nullopt;
}
void FrameSlotPool::release(size_t slot) {
  {
    std::lock_guard lock{m_mutex};
    if (slot < m_freeSlots.size()) m_freeSlots[slot] = true;
  }
  m_cv.notify_all();
}
void FrameSlotPool::reset() {
  {
    std::lock_guard lock{m_mutex};
    std::fill(m_freeSlots.begin(), m_freeSlots.end(), true);
  }
  m_cv.notify_all();
}
size_t FrameSlotPool::free_count() const {
  std::lock_guard lock{m_mutex};
  return static_cast<size_t>(std::ranges::count(m_freeSlots, true));
}

void initialize() {
  if (g_running.exchange(true, std::memory_order_acq_rel)) return;
  g_started.store(true, std::memory_order_release);
  g_joined.store(false, std::memory_order_release);
  g_exited.store(false, std::memory_order_release);
  g_rootsOpen.store(true, std::memory_order_release);
  g_failed.store(false, std::memory_order_release);
  g_queue.reset();
  g_pendingItems.store(0, std::memory_order_release);
  g_thread = thread::Thread{{.name = "Aurora render worker", .affinity = thread::Affinity::SharedCache}, worker_main};
}
void shutdown() {
  if (!g_started.load(std::memory_order_acquire) || g_joined.load(std::memory_order_acquire)) return;
  begin_draining();
  close_admission(); // queue close drains accepted items; it does not discard them
  while (!g_exited.load(std::memory_order_acquire)) {
    const auto hooks = g_hooks.load(std::memory_order_acquire);
    if (hooks && hooks->main == std::this_thread::get_id() && hooks->progress) hooks->progress();
    std::this_thread::sleep_for(IdlePumpInterval);
  }
  if (g_thread.joinable()) g_thread.join();
  g_joined.store(true, std::memory_order_release);
}

Outcome enqueue_begin_frame(uint64_t frameId, WorkCallback work, WorkCallback cancel) {
  return enqueue({.type = ItemType::BeginFrame, .frameId = frameId, .work = std::move(work), .cancel = std::move(cancel)});
}
Outcome enqueue_encode_pass(uint64_t frameId, uint32_t passIndex, WorkCallback work, WorkCallback cancel) {
  return enqueue({.type = ItemType::EncodePass, .frameId = frameId, .passIndex = passIndex,
                  .work = std::move(work), .cancel = std::move(cancel)});
}
Outcome enqueue_end_frame(uint64_t frameId, WorkCallback work, WorkCallback cancel) {
  return enqueue({.type = ItemType::EndFrame, .frameId = frameId, .work = std::move(work), .cancel = std::move(cancel)});
}
Outcome enqueue_work(WorkCallback work, WorkCallback cancel) {
  return enqueue({.work = std::move(work), .cancel = std::move(cancel)});
}
Outcome synchronize() {
  if (!wait_healthy()) return Outcome::Failed;
  if (is_worker_thread()) return Outcome::Completed;
  auto sync = std::make_shared<SyncState>();
  const auto accepted = enqueue({.sync = sync});
  if (accepted != Outcome::Completed) return accepted;
  while (true) {
    std::unique_lock lock{sync->mutex};
    if (sync->complete) {
      const auto outcome = sync->outcome;
      lock.unlock();
      return outcome == Outcome::Completed && !wait_healthy() ? Outcome::Failed : outcome;
    }
    lock.unlock();
    if (!progress_wait()) return Outcome::Failed;
    lock.lock();
    sync->cv.wait_for(lock, IdlePumpInterval, [&] { return sync->complete; });
  }
}
bool is_worker_thread() noexcept { return g_onWorker; }
bool is_idle() noexcept { return g_pendingItems.load(std::memory_order_acquire) == 0; }
} // namespace aurora::gfx::render_worker
