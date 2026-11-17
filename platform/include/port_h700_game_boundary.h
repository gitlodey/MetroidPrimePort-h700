#ifndef METROID_PRIME_PORT_PORT_H700_GAME_BOUNDARY_H
#define METROID_PRIME_PORT_PORT_H700_GAME_BOUNDARY_H

namespace PortH700 {

// Main safe-point policy shared with the CPU-only regression test. finishOpenFrame must
// use the existing frame latch (and clear it); it must not run the game's full EndScene.
// drainAndSync supplies actual FIFO/render completion, never queue-empty or GPU evidence.
// On any failure the caller must retain owners; this helper performs no cleanup/shutdown.
template <class Healthy, class FinishOpenFrame, class DrainAndSync>
bool GameOwnerBoundary(bool frameOpen, Healthy healthy, FinishOpenFrame finishOpenFrame,
                       DrainAndSync drainAndSync) {
  if (!healthy()) return false;
  if (frameOpen) finishOpenFrame();
  return healthy() && drainAndSync() && healthy();
}

} // namespace PortH700

#endif // METROID_PRIME_PORT_PORT_H700_GAME_BOUNDARY_H
