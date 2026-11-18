#pragma once

#include <cstdint>
#include <map>

namespace PortH700 {

// Main-thread SDL button events, not sampled pad-port state: short chords survive
// a slow frame, and changing the active pad cannot combine two devices' buttons.
class QuitChord {
public:
  enum class Button : uint8_t { Select = 1, Start = 2 };

  bool Update(uint32_t instance, Button button, bool down) {
    if (instance == 0 || mRequested) return false;
    const auto bit = static_cast<uint8_t>(button);
    if (!down) {
      const auto it = mHeld.find(instance);
      if (it != mHeld.end()) {
        it->second &= static_cast<uint8_t>(~bit);
        if (it->second == 0) mHeld.erase(it);
      }
      return false;
    }
    uint8_t& held = mHeld[instance];
    held |= bit;
    if (held != 3) return false;
    mRequested = true; // Quit has no confirmation/cancel: never enqueue it twice.
    return true;
  }

  void Remove(uint32_t instance) { mHeld.erase(instance); }
  bool Requested() const { return mRequested; }

private:
  std::map<uint32_t, uint8_t> mHeld;
  bool mRequested = false;
};

} // namespace PortH700
