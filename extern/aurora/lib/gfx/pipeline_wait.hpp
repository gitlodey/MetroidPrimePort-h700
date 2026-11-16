#pragma once

#include "render_worker.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace aurora::gfx::detail {

enum class PipelineWaitOutcome { Pending, Ready, Failed, Cancelled };

// Cache queues retain their existing priority/ownership. This only separates new background
// roots from compiler continuations of accepted FIFO/render work and tracks actual driver calls.
class PipelineWorkControl {
public:
  void reset() {
    m_draining.store(false, std::memory_order_release);
    m_closed.store(false, std::memory_order_release);
  }
  void begin_draining() { m_draining.store(true, std::memory_order_release); }
  void close_continuations() { begin_draining(); m_closed.store(true, std::memory_order_release); }
  bool background_open() const {
    return !m_draining.load(std::memory_order_acquire) && render_worker::root_admission_open();
  }
  bool continuation_open() const {
    return !m_closed.load(std::memory_order_acquire) && render_worker::wait_healthy();
  }
  bool closed() const { return m_closed.load(std::memory_order_acquire); }
  void started() { m_active.fetch_add(1, std::memory_order_acq_rel); }
  void returned() { m_active.fetch_sub(1, std::memory_order_acq_rel); }
  size_t active() const { return m_active.load(std::memory_order_acquire); }
private:
  std::atomic_bool m_draining{false};
  std::atomic_bool m_closed{false};
  std::atomic_size_t m_active{0};
};

// Inspect runs under the cache lock; health/progress NEVER do. A cancelled wait says
// nothing about driver-call completion: the compiler still owns its callback and captures.
template <class Inspect>
PipelineWaitOutcome wait_pipeline(std::unique_lock<std::mutex>& lock, std::condition_variable& cv, Inspect inspect) {
  while (true) {
    lock.unlock();
    const bool healthy = render_worker::wait_healthy();
    lock.lock();
    if (!healthy) return PipelineWaitOutcome::Failed;
    const auto result = inspect();
    if (result != PipelineWaitOutcome::Pending) return result;
    lock.unlock();
    const bool progressed = render_worker::progress_wait();
    lock.lock();
    if (!progressed) return PipelineWaitOutcome::Failed;
    cv.wait_for(lock, std::chrono::milliseconds{1});
  }
}

} // namespace aurora::gfx::detail
