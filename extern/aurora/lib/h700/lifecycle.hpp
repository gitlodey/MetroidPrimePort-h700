#pragma once

#include <cstdint>

namespace aurora::h700 {

enum class ShutdownStage : uint8_t {
  Running, Producers, Fifo, Render, Compiler, Maps, Native, Holders, Device, Exports, Window, Done, Unknown
};
enum class ShutdownStatus { Pending, Released, FailedReleased, Unknown };
struct ShutdownResult {
  ShutdownStatus status;
  ShutdownStage stage;
};

// Main-owned monotonic cursor, not a callback executor. Each production stage must obtain
// actual ownership evidence before advancing. A failed/unknown generation cannot be restarted.
class GenerationLifecycle {
public:
  bool begin() {
    if (m_started && (m_stage != ShutdownStage::Done || m_failed)) return false;
    m_started = true;
    m_failed = false;
    m_stage = ShutdownStage::Running;
    return true;
  }
  void fail() { m_failed = true; }
  void request_stop() { if (m_stage == ShutdownStage::Running) m_stage = ShutdownStage::Producers; }
  bool advance(ShutdownStage completed) {
    if (m_stage != completed || completed == ShutdownStage::Running || completed >= ShutdownStage::Done) return false;
    m_stage = static_cast<ShutdownStage>(static_cast<uint8_t>(m_stage) + 1);
    return true;
  }
  void unknown() { m_failed = true; m_stage = ShutdownStage::Unknown; }
  bool failed() const { return m_failed; }
  ShutdownStage stage() const { return m_stage; }
  ShutdownResult result() const {
    if (m_stage == ShutdownStage::Unknown) return {ShutdownStatus::Unknown, m_stage};
    if (m_stage == ShutdownStage::Done)
      return {m_failed ? ShutdownStatus::FailedReleased : ShutdownStatus::Released, m_stage};
    return {ShutdownStatus::Pending, m_stage};
  }
private:
  bool m_started = false;
  bool m_failed = false;
  ShutdownStage m_stage = ShutdownStage::Done;
};

#ifdef AURORA_H700
// Driver main safe-point only. poll never dispatches application notifications; take owned
// PresentationState records separately, while consumers live. No application worker/capacity
// wait is allowed in poll. Main-side raw Dawn/EGL/GL/SDL calls themselves remain synchronous:
// a firmware driver stall inside such a call cannot be preempted by the caller's deadline.
// Graphics CLI remains IG12-gated.
void request_generation_shutdown() noexcept;
ShutdownResult poll_generation_shutdown() noexcept;
#endif
} // namespace aurora::h700
