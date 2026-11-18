#pragma once

namespace aurora::h700 {

enum class ForwardWaitStatus { TimedOut, Success, Error };
enum class ForwardWaitDecision { Pending, Unresolved, WorkFailed, Complete };

constexpr ForwardWaitDecision forward_wait_decision(ForwardWaitStatus wait, bool completed,
                                                   bool callback_called, bool work_succeeded) {
  if (wait == ForwardWaitStatus::TimedOut) return ForwardWaitDecision::Pending;
  if (wait != ForwardWaitStatus::Success) return ForwardWaitDecision::Unresolved;
  // Batch success only means that at least one entry completed.
  if (!completed) return ForwardWaitDecision::Pending;
  if (!callback_called) return ForwardWaitDecision::Unresolved;
  return work_succeeded ? ForwardWaitDecision::Complete : ForwardWaitDecision::WorkFailed;
}

} // namespace aurora::h700
