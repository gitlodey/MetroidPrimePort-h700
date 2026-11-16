#pragma once

#include <atomic>
#include <cstdint>

namespace aurora::gfx::detail {

// One record per actual MapAsync request. Callbacks own only this record (or guarded
// staging ownership), never a reusable readback slot. Cancelling a waiter is NOT completion.
class MapCompletion {
public:
  bool begin_completion() {
    auto expected = State::Pending;
    return m_state.compare_exchange_strong(expected, State::Completing, std::memory_order_acq_rel);
  }
  bool finish_completion(bool success) {
    auto expected = State::Completing;
    return m_state.compare_exchange_strong(expected, success ? State::Succeeded : State::Failed,
                                            std::memory_order_acq_rel);
  }
  bool pending() const {
    const auto state = m_state.load(std::memory_order_acquire);
    return state == State::Pending || state == State::Completing;
  }
  bool succeeded() const { return m_state.load(std::memory_order_acquire) == State::Succeeded; }
  // Call only after the owning thread has consumed/unmapped this request's buffer.
  // False preserves all ownership while a callback is pending/executing, or already retired.
  bool retire() {
    auto state = m_state.load(std::memory_order_acquire);
    while (state == State::Succeeded || state == State::Failed) {
      if (m_state.compare_exchange_weak(state, State::Retired, std::memory_order_acq_rel)) return true;
    }
    return false;
  }
private:
  enum class State : uint8_t { Pending, Completing, Succeeded, Failed, Retired };
  std::atomic<State> m_state{State::Pending};
};

} // namespace aurora::gfx::detail
