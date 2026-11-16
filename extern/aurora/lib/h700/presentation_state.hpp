#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aurora::h700 {

using CompletionId = std::uint64_t;

struct FrameDescriptor {
  const std::uint64_t generation;
  const std::uint64_t frame;
  const std::uint8_t slot;

  bool operator==(const FrameDescriptor&) const = default;
};

struct Submission {
  FrameDescriptor descriptor;
  CompletionId forward;
};

enum class SlotStatus { Free, Reserved, Submitted, Ready, Reading, Retiring, Quarantined };
enum class PresentationPhase { Running, Draining, Failed, Stopped };

struct PresentationNotification {
  enum class Outcome { Presented, Aborted, Quarantined };
  FrameDescriptor descriptor;
  Outcome outcome;
};

struct PresentationSlot {
  SlotStatus status = SlotStatus::Free;
  std::optional<FrameDescriptor> descriptor;
  std::optional<CompletionId> forward;
  std::optional<CompletionId> reverse;
  bool forward_complete = false;
  bool reverse_complete = false;
  bool swap_succeeded = false;
};

struct PresentationSnapshot {
  PresentationPhase phase;
  std::array<PresentationSlot, 2> slots;
  // Logical arrival order, including publications not yet forward-complete.
  std::array<std::optional<Submission>, 2> mailbox;
  std::size_t mailbox_size;
  std::optional<std::string> first_error;
};

// CPU ownership only. No GPU calls, callbacks, wait loops or packet references. Every operation
// takes only this object's mutex; callers must not hold renderer/FIFO locks while calling it.
// Generation is nonzero and unique per device lifetime; construct a new object for the next one.
class PresentationState {
public:
  explicit PresentationState(std::uint64_t generation);

  // Main: strictly increasing frame IDs (0 is valid). Failure never consumes a credit/frame ID.
  [[nodiscard]] std::optional<FrameDescriptor> try_reserve(std::uint64_t frame);
  [[nodiscard]] bool abort_reservation(const FrameDescriptor& descriptor);

  // Render: mark immediately BEFORE the irreversible Submit boundary. After this succeeds even
  // a failed Submit must go through failure/quarantine, never reservation rollback. Publication
  // remains open to accepted work in Draining/Failed, including a quarantined submit's late future.
  [[nodiscard]] bool mark_submitted(const FrameDescriptor& descriptor);
  [[nodiscard]] bool publish_submitted(const FrameDescriptor& descriptor, CompletionId forward);

  // Main progress: peek without discarding the future, poll outside the lock, then report success.
  // Completion IDs are opaque nonzero metadata, not proof of completion; the wrapper supplies
  // actual successful GPU events. Failed/unknown completion must use quarantine(), not these APIs.
  [[nodiscard]] std::optional<Submission> front_submission() const;
  [[nodiscard]] bool complete_forward(const FrameDescriptor& descriptor, CompletionId forward);
  [[nodiscard]] std::optional<FrameDescriptor> try_begin_read();
  [[nodiscard]] bool begin_retiring(const FrameDescriptor& descriptor, CompletionId reverse);
  [[nodiscard]] bool complete_reverse(const FrameDescriptor& descriptor, CompletionId reverse);
  [[nodiscard]] bool complete_swap(const FrameDescriptor& descriptor);
  [[nodiscard]] bool recycle(const FrameDescriptor& descriptor);

  void begin_draining();
  void fail(std::string error);
  // Preserve post-submit ownership on unknown completion. Reserved work must abort instead.
  [[nodiscard]] bool quarantine(const FrameDescriptor& descriptor, std::string error);

  // Normal stop needs all credits retired/aborted and no outstanding publication. This describes
  // presentation ownership only, not render/compiler/map quiescence or graphics destruction.
  [[nodiscard]] bool try_stop();
  // Failure-only terminal seal, called AFTER producers have joined and the whole graphics
  // generation has been destroyed. Retains every slot/future/mailbox record for diagnostics;
  // it neither proves completion nor makes quarantined storage reusable.
  [[nodiscard]] bool stop_after_teardown();

  // Main safe-point ONLY, outside recording/progress and consumer locks. Returns owned values;
  // no user code runs under our lock. Dispatch is never needed to release a native credit.
  [[nodiscard]] std::vector<PresentationNotification> take_notifications();
  [[nodiscard]] PresentationSnapshot snapshot() const;

private:
  PresentationSlot* find_slot(const FrameDescriptor& descriptor);
  void fail_locked(std::string error);
  void release_locked(PresentationSlot& slot);

  const std::uint64_t m_generation;
  mutable std::mutex m_mutex;
  PresentationPhase m_phase = PresentationPhase::Running;
  std::optional<std::uint64_t> m_last_frame;
  std::array<PresentationSlot, 2> m_slots;
  std::array<std::optional<Submission>, 2> m_mailbox;
  std::size_t m_head = 0;
  std::size_t m_size = 0;
  std::optional<std::string> m_first_error;
  std::vector<PresentationNotification> m_notifications;
};

} // namespace aurora::h700
