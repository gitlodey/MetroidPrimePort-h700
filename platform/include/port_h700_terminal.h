#ifndef METROID_PRIME_PORT_PORT_H700_TERMINAL_H
#define METROID_PRIME_PORT_PORT_H700_TERMINAL_H

#include "../../extern/aurora/lib/h700/lifecycle.hpp"

namespace PortH700 {

// Root-only transport, deliberately unrelated to std::exception/runtime_error. Never
// throw through constructors, GX/skinning guards, C/noexcept, callbacks or workers.
struct RootTerminalToken {
  const char* safePoint;
  aurora::h700::ShutdownResult checkpoint;
};

inline bool RootUnwindReady(aurora::h700::ShutdownResult result) noexcept {
  return result.status == aurora::h700::ShutdownStatus::CpuQuiescent &&
         result.stage == aurora::h700::ShutdownStage::Maps;
}

// Shared CPU policy, not a GPU simulator. Production poll must be GM09's actual
// poll_generation_cpu_quiescence (it requests terminal shutdown itself). Only the
// exact checkpoint permits the caller to throw, with owners OUTSIDE its root try.
// Unknown, Released and every other non-Pending result stop polling, NOT permission
// to unwind. Pending at the diagnostic deadline must fail-stop BEFORE any throw.
// A synchronous driver call inside poll cannot be preempted by this deadline.
// This helper neither throws nor cleans up; the caller retains owners on failure.
template <class IsMainThread, class Poll, class Idle, class DeadlineExpired>
aurora::h700::ShutdownResult PrepareRootUnwind(IsMainThread isMainThread, Poll poll,
                                              Idle idle, DeadlineExpired deadlineExpired) {
  if (!isMainThread())
    return {aurora::h700::ShutdownStatus::Unknown, aurora::h700::ShutdownStage::Unknown};
  for (;;) {
    const auto result = poll();
    if (result.status != aurora::h700::ShutdownStatus::Pending || deadlineExpired()) return result;
    idle();
  }
}

} // namespace PortH700

#endif // METROID_PRIME_PORT_PORT_H700_TERMINAL_H
