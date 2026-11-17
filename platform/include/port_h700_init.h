#ifndef METROID_PRIME_PORT_PORT_H700_INIT_H
#define METROID_PRIME_PORT_PORT_H700_INIT_H

#include <aurora/aurora.h>
#include "../../extern/aurora/lib/h700/lifecycle.hpp"

namespace PortH700 {

inline bool InitAccepted(const AuroraInfo& info, bool ready) noexcept {
  // Check actual native output, not the requested size or the scaled GX framebuffer.
  return info.backend == BACKEND_OPENGLES && info.window != nullptr && ready &&
         info.windowSize.native_fb_width == 640 && info.windowSize.native_fb_height == 480;
}

// Generation shutdown for rejected init or after all normal game-owner cleanup.
// Reuse IG11's milestone-based full/partial cleanup; this helper owns no game cleanup.
// Pending at the diagnostic deadline and Unknown are NOT release evidence: the caller must
// retain its main stack/runtime in fail-stop, not unwind or attempt another cleanup/init.
// request/poll can enter synchronous driver calls; a hang there cannot be preempted by
// this deadline. No deadline check manufactures completion or authorizes releasing owners.
template <class Request, class Poll, class Idle, class DeadlineExpired>
aurora::h700::ShutdownResult PollGenerationShutdown(Request request, Poll poll, Idle idle,
                                                  DeadlineExpired deadlineExpired) {
  request();
  for (;;) {
    const auto result = poll();
    if (result.status != aurora::h700::ShutdownStatus::Pending || deadlineExpired()) return result;
    idle();
  }
}

} // namespace PortH700

#endif // METROID_PRIME_PORT_PORT_H700_INIT_H
