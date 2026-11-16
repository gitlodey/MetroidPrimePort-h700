#include "presentation_state.hpp"

#include <utility>

namespace aurora::h700 {

PresentationState::PresentationState(std::uint64_t generation) : m_generation(generation) {
  if (generation == 0) {
    fail_locked("Presentation generation must be nonzero");
  }
}

PresentationSlot* PresentationState::find_slot(const FrameDescriptor& descriptor) {
  if (descriptor.generation != m_generation || descriptor.slot >= m_slots.size()) {
    return nullptr;
  }
  auto& slot = m_slots[descriptor.slot];
  return slot.descriptor && *slot.descriptor == descriptor ? &slot : nullptr;
}

std::optional<FrameDescriptor> PresentationState::try_reserve(std::uint64_t frame) {
  std::lock_guard lock(m_mutex);
  if (m_phase != PresentationPhase::Running || (m_last_frame && frame <= *m_last_frame)) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < m_slots.size(); ++i) {
    auto& slot = m_slots[i];
    if (slot.status == SlotStatus::Free) {
      slot.descriptor.emplace(FrameDescriptor{m_generation, frame, static_cast<std::uint8_t>(i)});
      slot.status = SlotStatus::Reserved;
      m_last_frame = frame;
      return slot.descriptor;
    }
  }
  return std::nullopt;
}

void PresentationState::release_locked(PresentationSlot& slot) {
  slot.status = SlotStatus::Free;
  slot.descriptor.reset();
  slot.forward.reset();
  slot.reverse.reset();
  slot.forward_complete = false;
  slot.reverse_complete = false;
  slot.swap_succeeded = false;
}

bool PresentationState::abort_reservation(const FrameDescriptor& descriptor) {
  std::lock_guard lock(m_mutex);
  auto* slot = find_slot(descriptor);
  if (m_phase == PresentationPhase::Stopped || !slot || slot->status != SlotStatus::Reserved) {
    return false;
  }
  m_notifications.push_back({descriptor, PresentationNotification::Outcome::Aborted});
  release_locked(*slot);
  return true;
}

bool PresentationState::mark_submitted(const FrameDescriptor& descriptor) {
  std::lock_guard lock(m_mutex);
  auto* slot = find_slot(descriptor);
  if ((m_phase != PresentationPhase::Running && m_phase != PresentationPhase::Draining) ||
      !slot || slot->status != SlotStatus::Reserved) {
    return false;
  }
  slot->status = SlotStatus::Submitted;
  return true;
}

bool PresentationState::publish_submitted(const FrameDescriptor& descriptor, CompletionId forward) {
  std::lock_guard lock(m_mutex);
  auto* slot = find_slot(descriptor);
  if (m_phase == PresentationPhase::Stopped || !slot || forward == 0 || slot->forward ||
      (slot->status != SlotStatus::Submitted && slot->status != SlotStatus::Quarantined) ||
      m_size == m_mailbox.size()) {
    return false;
  }
  // Each of the two credits can publish once. A valid producer can never need mailbox space
  // released by main: the mailbox has the same capacity as the entire set of native credits.
  m_mailbox[(m_head + m_size) % m_mailbox.size()].emplace(Submission{descriptor, forward});
  ++m_size;
  slot->forward = forward;
  return true;
}

std::optional<Submission> PresentationState::front_submission() const {
  std::lock_guard lock(m_mutex);
  if (m_size == 0) {
    return std::nullopt;
  }
  return m_mailbox[m_head];
}

bool PresentationState::complete_forward(const FrameDescriptor& descriptor, CompletionId forward) {
  std::lock_guard lock(m_mutex);
  auto* slot = find_slot(descriptor);
  if (m_phase == PresentationPhase::Stopped || !slot || slot->status != SlotStatus::Submitted ||
      !slot->forward || *slot->forward != forward) {
    return false;
  }
  slot->forward_complete = true;
  slot->status = SlotStatus::Ready;
  return true;
}

std::optional<FrameDescriptor> PresentationState::try_begin_read() {
  std::lock_guard lock(m_mutex);
  if ((m_phase != PresentationPhase::Running && m_phase != PresentationPhase::Draining) ||
      m_size == 0) {
    return std::nullopt;
  }
  const auto descriptor = m_mailbox[m_head]->descriptor;
  auto* slot = find_slot(descriptor);
  if (!slot || slot->status != SlotStatus::Ready) {
    return std::nullopt;
  }
  slot->status = SlotStatus::Reading;
  m_mailbox[m_head].reset();
  m_head = (m_head + 1) % m_mailbox.size();
  --m_size;
  return descriptor;
}

bool PresentationState::begin_retiring(const FrameDescriptor& descriptor, CompletionId reverse) {
  std::lock_guard lock(m_mutex);
  auto* slot = find_slot(descriptor);
  if (m_phase == PresentationPhase::Stopped || !slot || slot->status != SlotStatus::Reading ||
      reverse == 0) {
    return false;
  }
  slot->reverse = reverse;
  slot->status = SlotStatus::Retiring;
  return true;
}

bool PresentationState::complete_reverse(const FrameDescriptor& descriptor, CompletionId reverse) {
  std::lock_guard lock(m_mutex);
  auto* slot = find_slot(descriptor);
  if (m_phase == PresentationPhase::Stopped || !slot || slot->status != SlotStatus::Retiring ||
      !slot->reverse || *slot->reverse != reverse || slot->reverse_complete) {
    return false;
  }
  slot->reverse_complete = true;
  return true;
}

bool PresentationState::complete_swap(const FrameDescriptor& descriptor) {
  std::lock_guard lock(m_mutex);
  auto* slot = find_slot(descriptor);
  if (m_phase == PresentationPhase::Stopped || !slot || slot->status != SlotStatus::Retiring ||
      slot->swap_succeeded) {
    return false;
  }
  m_notifications.push_back({descriptor, PresentationNotification::Outcome::Presented});
  slot->swap_succeeded = true;
  return true;
}

bool PresentationState::recycle(const FrameDescriptor& descriptor) {
  std::lock_guard lock(m_mutex);
  auto* slot = find_slot(descriptor);
  if (m_phase == PresentationPhase::Stopped || !slot || slot->status != SlotStatus::Retiring ||
      !slot->reverse_complete || !slot->swap_succeeded) {
    return false;
  }
  release_locked(*slot);
  return true;
}

void PresentationState::begin_draining() {
  std::lock_guard lock(m_mutex);
  if (m_phase == PresentationPhase::Running) {
    m_phase = PresentationPhase::Draining;
  }
}

void PresentationState::fail_locked(std::string error) {
  if (!m_first_error) {
    m_first_error.emplace(std::move(error));
  }
  m_phase = PresentationPhase::Failed;
}

void PresentationState::fail(std::string error) {
  std::lock_guard lock(m_mutex);
  if (m_phase != PresentationPhase::Stopped) {
    fail_locked(std::move(error));
  }
}

bool PresentationState::quarantine(const FrameDescriptor& descriptor, std::string error) {
  std::lock_guard lock(m_mutex);
  auto* slot = find_slot(descriptor);
  if (m_phase == PresentationPhase::Stopped || !slot || slot->status == SlotStatus::Reserved ||
      slot->status == SlotStatus::Quarantined) {
    return false;
  }
  // A successful swap already emitted this frame's result. A later reverse-fence failure is
  // still terminal, but must not produce a second presentation result or erase the future.
  if (!slot->swap_succeeded) {
    m_notifications.push_back({descriptor, PresentationNotification::Outcome::Quarantined});
  }
  slot->status = SlotStatus::Quarantined;
  fail_locked(std::move(error));
  return true;
}

bool PresentationState::try_stop() {
  std::lock_guard lock(m_mutex);
  if ((m_phase != PresentationPhase::Draining && m_phase != PresentationPhase::Failed) || m_size) {
    return false;
  }
  for (const auto& slot : m_slots) {
    if (slot.status != SlotStatus::Free) {
      return false;
    }
  }
  m_phase = PresentationPhase::Stopped;
  return true;
}

bool PresentationState::stop_after_teardown() {
  std::lock_guard lock(m_mutex);
  if (m_phase != PresentationPhase::Failed) {
    return false;
  }
  m_phase = PresentationPhase::Stopped;
  return true;
}

std::vector<PresentationNotification> PresentationState::take_notifications() {
  std::vector<PresentationNotification> result;
  {
    std::lock_guard lock(m_mutex);
    result.swap(m_notifications);
  }
  return result;
}

PresentationSnapshot PresentationState::snapshot() const {
  std::lock_guard lock(m_mutex);
  std::array<std::optional<Submission>, 2> mailbox;
  for (std::size_t i = 0; i < m_size; ++i) {
    mailbox[i].emplace(*m_mailbox[(m_head + i) % m_mailbox.size()]);
  }
  return {m_phase, m_slots, std::move(mailbox), m_size, m_first_error};
}

} // namespace aurora::h700
