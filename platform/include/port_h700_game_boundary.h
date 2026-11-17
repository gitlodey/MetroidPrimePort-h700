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

// Same-generation reset at the main safe-point, with ticks/draw stopped. destroy must
// clear the raw alias before destroying the old owner; construct publishes the new one.
// A failed boundary never authorizes delayed frees or construction. The caller must
// fail-stop with the main stack/remaining owners retained until terminal transport exists.
// flush must not enqueue commands beyond the proven destructor-tail boundary.
template <class Boundary, class Destroy, class Flush, class Construct>
bool ResetGameOwner(Boundary boundary, Destroy destroy, Flush flush, Construct construct) {
  if (!boundary()) return false;
  destroy();
  if (!boundary()) return false;
  flush();
  construct();
  return true;
}

} // namespace PortH700

#endif // METROID_PRIME_PORT_PORT_H700_GAME_BOUNDARY_H
