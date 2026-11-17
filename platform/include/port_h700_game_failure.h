#ifndef METROID_PRIME_PORT_PORT_H700_GAME_FAILURE_H
#define METROID_PRIME_PORT_PORT_H700_GAME_FAILURE_H

namespace PortH700 {

// Main-thread owned, including producers. Not a worker notification, shutdown
// request or proof that owners may unwind. Sites must have static storage.
enum class GameFailure { None, Backend };

namespace detail {
inline GameFailure gameFailure = GameFailure::None;
inline const char* gameFailureSite = nullptr;
} // namespace detail

inline bool GameFailurePending() noexcept { return detail::gameFailure != GameFailure::None; }
inline GameFailure GetGameFailure() noexcept { return detail::gameFailure; }
inline const char* GameFailureSite() noexcept { return detail::gameFailureSite; }

inline void NoteGameFailure(GameFailure failure, const char* staticSite) noexcept {
  if (failure == GameFailure::None || GameFailurePending()) return;
  detail::gameFailure = failure;
  detail::gameFailureSite = staticSite != nullptr ? staticSite : "unspecified game failure";
}

// Shared by boot, frame tails and root. A recovered backend cannot clear a game
// failure; only root may request the actual GM11b CPU checkpoint before unwind.
inline bool GameWorkMayContinue(bool backendHealthy, const char* staticSite) noexcept {
  if (!backendHealthy) NoteGameFailure(GameFailure::Backend, staticSite);
  return !GameFailurePending();
}

} // namespace PortH700

#endif // METROID_PRIME_PORT_PORT_H700_GAME_FAILURE_H
