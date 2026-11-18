#include "port_h700_quit_chord.h"
#include "port_test.h"

#include <array>

namespace {
using Chord = PortH700::QuitChord;
using Button = Chord::Button;

void CheckBothOrdersAndOneShot() {
  for (const Button first : {Button::Select, Button::Start}) {
    Chord chord;
    const Button second = first == Button::Select ? Button::Start : Button::Select;
    CHECK(!chord.Requested());
    CHECK(!chord.Update(7, first, true));
    CHECK(!chord.Update(7, first, true)); // duplicate down is not a chord
    CHECK(!chord.Requested());
    CHECK(chord.Update(7, second, true));
    CHECK(chord.Requested());
    for (int repeat = 0; repeat < 10; ++repeat) {
      CHECK(!chord.Update(7, first, true));
      CHECK(!chord.Update(7, second, true));
    }
    CHECK(!chord.Update(7, first, false));
    CHECK(!chord.Update(7, second, false));
    CHECK(!chord.Update(7, first, true));
    CHECK(!chord.Update(7, second, true)); // quit cannot be cancelled or re-enqueued
    chord.Remove(7);
    CHECK(chord.Requested());
    CHECK(!chord.Update(8, first, true));
    CHECK(!chord.Update(8, second, true));
  }
}

void CheckNonOverlappingPresses() {
  for (const Button first : {Button::Select, Button::Start}) {
    Chord chord;
    const Button second = first == Button::Select ? Button::Start : Button::Select;
    CHECK(!chord.Update(1, first, false)); // stray release is harmless
    CHECK(!chord.Update(1, first, true));
    CHECK(!chord.Update(1, first, false));
    CHECK(!chord.Update(1, second, true));
    CHECK(!chord.Requested());
    CHECK(chord.Update(1, first, true));
  }
}

void CheckPadsNeverMix() {
  Chord chord;
  CHECK(!chord.Update(1, Button::Select, true));
  CHECK(!chord.Update(2, Button::Start, true));
  CHECK(!chord.Update(0xffffffffu, Button::Start, true));
  CHECK(!chord.Requested());
  CHECK(!chord.Update(2, Button::Start, false));
  CHECK(!chord.Update(2, Button::Select, true));
  CHECK(!chord.Update(1, Button::Select, false));
  CHECK(!chord.Update(1, Button::Start, true));
  CHECK(!chord.Requested());
  CHECK(chord.Update(0xffffffffu, Button::Select, true));
}

void CheckDisconnectDropsHeldState() {
  for (const Button first : {Button::Select, Button::Start}) {
    Chord chord;
    const Button second = first == Button::Select ? Button::Start : Button::Select;
    CHECK(!chord.Update(3, first, true));
    CHECK(!chord.Update(4, first, true));
    chord.Remove(3);
    chord.Remove(99);
    CHECK(!chord.Update(3, second, true)); // even a reused ID starts fresh
    CHECK(!chord.Requested());
    CHECK(chord.Update(4, second, true)); // another pad's held state is intact
  }
}

void CheckShortChordRetainedUntilQuitPump() {
  Chord chord;
  int quitRequests = 0;
  struct Event { Button button; bool down; };
  // All four events can arrive in one slow frame, before any UI/input poll.
  const std::array<Event, 4> events{{
      {Button::Select, true}, {Button::Start, true},
      {Button::Start, false}, {Button::Select, false},
  }};
  for (const auto& event : events) {
    if (chord.Update(5, event.button, event.down)) ++quitRequests;
  }
  CHECK_EQ(quitRequests, 1);
  CHECK(chord.Requested()); // suppress port actions even though both buttons are now up
}

void CheckInvalidInstanceIgnored() {
  Chord chord;
  CHECK(!chord.Update(0, Button::Select, true));
  CHECK(!chord.Update(0, Button::Start, true));
  CHECK(!chord.Update(0, Button::Start, false));
  CHECK(!chord.Requested());
  CHECK(!chord.Update(1, Button::Start, true));
  CHECK(!chord.Requested());
  CHECK(chord.Update(1, Button::Select, true));
}
} // namespace

int main() {
  CheckBothOrdersAndOneShot();
  CheckNonOverlappingPresses();
  CheckPadsNeverMix();
  CheckDisconnectDropsHeldState();
  CheckShortChordRetainedUntilQuitPump();
  CheckInvalidInstanceIgnored();
  return PORT_TEST_RESULT();
}
