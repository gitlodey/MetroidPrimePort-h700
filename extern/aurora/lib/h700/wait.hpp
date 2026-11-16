#pragma once

#include <atomic>
#include <functional>
#include <utility>

namespace aurora::h700 {
// A staging waiter may stop before MapAsync does. Its slot remains owned until the
// internal completion, independently of the cancelled waiter's stack/lifetime.
class MapSlotOwnership {
public:
  MapSlotOwnership(bool releaseOnCompletion, std::function<void()> release)
  : m_releaseRequested(releaseOnCompletion), m_release(std::move(release)) {}
  void complete() {
    m_complete.store(true);
    release_if_ready();
  }
  void cancel_wait() {
    m_releaseRequested.store(true);
    release_if_ready();
  }
private:
  void release_if_ready() {
    // Sequential consistency closes the completion-vs-cancellation lost-wakeup race.
    if (m_complete.load() && m_releaseRequested.load() && !m_released.exchange(true)) m_release();
  }
  std::atomic_bool m_complete = false;
  std::atomic_bool m_releaseRequested;
  std::atomic_bool m_released = false;
  std::function<void()> m_release;
};
} // namespace aurora::h700
