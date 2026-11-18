#pragma once

#include <cstdint>

namespace aurora::h700 {

enum class ShutdownStage : uint8_t {
  Running, Producers, Fifo, Render, Compiler, Maps, Native, Holders, Device, Exports, Window, Done, Unknown
};
enum class ShutdownStatus { Pending, Released, FailedReleased, Unknown, CpuQuiescent };
enum class ShutdownGoal { Full, CpuQuiescence };
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
    m_mapRetirementStarted = false;
    m_stage = ShutdownStage::Running;
    return true;
  }
  void fail() { m_failed = true; }
  void request_stop() { if (m_stage == ShutdownStage::Running) m_stage = ShutdownStage::Producers; }
  // Full polling explicitly resumes the Maps body. Remember entry even if retirement
  // remains Pending: a later CPU-only poll must not claim that maps are still held intact.
  bool begin_map_retirement(ShutdownGoal goal) {
    if (m_stage != ShutdownStage::Maps || goal != ShutdownGoal::Full) return false;
    m_mapRetirementStarted = true;
    return true;
  }
  bool advance(ShutdownStage completed, ShutdownGoal goal = ShutdownGoal::Full) {
    if (m_stage != completed || completed == ShutdownStage::Running || completed >= ShutdownStage::Done) return false;
    if (goal == ShutdownGoal::CpuQuiescence && completed >= ShutdownStage::Maps) return false;
    m_stage = static_cast<ShutdownStage>(static_cast<uint8_t>(m_stage) + 1);
    return true;
  }
  void unknown() { m_failed = true; m_stage = ShutdownStage::Unknown; }
  bool failed() const { return m_failed; }
  ShutdownStage stage() const { return m_stage; }
  ShutdownResult result(ShutdownGoal goal = ShutdownGoal::Full) const {
    if (m_stage == ShutdownStage::Unknown) return {ShutdownStatus::Unknown, m_stage};
    if (m_stage == ShutdownStage::Done)
      return {m_failed ? ShutdownStatus::FailedReleased : ShutdownStatus::Released, m_stage};
    // Maps is reachable only by advancing Compiler after its actual joins. The poll
    // barrier must hold before entering Maps, not inspect this enum after retirement.
    if (goal == ShutdownGoal::CpuQuiescence && m_stage == ShutdownStage::Maps && !m_mapRetirementStarted)
      return {ShutdownStatus::CpuQuiescent, m_stage};
    return {ShutdownStatus::Pending, m_stage};
  }
private:
  bool m_started = false;
  bool m_failed = false;
  bool m_mapRetirementStarted = false;
  ShutdownStage m_stage = ShutdownStage::Done;
};

#ifdef AURORA_H700
// Driver main safe-point only. poll never dispatches application notifications; take owned
// PresentationState records separately, while consumers live. No application worker/capacity
// wait is allowed in poll. Main-side raw Dawn/EGL/GL/SDL calls themselves remain synchronous:
// a firmware driver stall inside such a call cannot be preempted by the caller's deadline.
// Main-only startup query; no event dispatch or GPU calls. Used before game owners exist.
bool initialization_ready() noexcept;

// Main safe-point only, with game ticks/draw stopped and no game reentry during progress.
// Ongoing health does not depend on the startup-only initial-frame flag.
bool game_work_healthy() noexcept;
// Existing presenter progress API, also used by game skin-token/ring waits.
// Main only, nonrecursive, no locks held; never dispatches game callbacks.
bool main_progress_once();
// Caller must first finish any open Aurora frame exactly once. Includes unpublished FIFO
// bytes, render work, after-submit callback return and destruction of their CPU captures.
// True protects game-owned source buffers, NOT GPU completion/native retirement. Leaves
// root admission/backend/compiler live. False never authorizes owner destruction or frees.
bool synchronize_game_work() noexcept;

void request_generation_shutdown() noexcept;
ShutdownResult poll_generation_shutdown() noexcept;
// Terminal failure only, NOT the healthy reset barrier. Closes producers and waits for
// actual FIFO/render/compiler exit acknowledgements and joins; never throws/unwinds.
// CpuQuiescent holds BEFORE Maps (including abandoned-recorder retirement), keeping
// maps/native/holders/device/exports alive. It is NOT GPU completion or release.
// Repeated CPU polls hold that boundary; only an explicit full shutdown poll resumes
// Maps after caller-owned no-new-graphics cleanup. This path forbids a clean restart.
// Pending/Unknown never authorize owner cleanup. A late CPU poll after Maps has begun
// cannot obtain CpuQuiescent; it retains the current cursor rather than advancing it.
ShutdownResult poll_generation_cpu_quiescence() noexcept;

// Read-only main-thread query, false during polling or outside the actual held
// CpuQuiescent/Maps boundary (including Unknown or Maps retirement already begun).
// Does not poll, progress, request shutdown or manufacture a checkpoint. True permits
// no-new-graphics CPU cleanup only, NOT GPU completion or generation release.
// Game producers must remain stopped until the explicit full shutdown resumes Maps.
bool generation_cpu_quiescence_held() noexcept;

// Main-only resident portion of the game's CanDestroyWithoutGraphicsCommands guard.
// Call only after actual poll_generation_cpu_quiescence() returned CpuQuiescent/Maps,
// with game producers stopped, and before any full poll resumes Maps retirement.
// False outside that boundary (without reading registries), or if EITHER producer retains
// or processor entries remain, even with zero GPU bytes. Read-only: no polling/progress,
// reset, release, enqueue or command consumption. True is NOT GPU completion/release and
// does not authorize cleanup of other game-owner obligations. Also requires empty
// immutable admission/ticket/processor registries; surviving owning CPU views are safe.
bool resident_resources_empty() noexcept;
// Root-only CPU source/ticket release, before the existing read-only obligation
// guard. Rechecks the actual held checkpoint; no FIFO consumption or GPU release.
// Held SourceViews remain alive and budget-accounted until their last owner dies.
bool release_immutable_sources_at_cpu_quiescence() noexcept;
#endif
} // namespace aurora::h700
