#include "port_h700_game_boundary.h"
#include "port_test.h"

#include <vector>

namespace {
enum class Event { Health, FinishFrame, DrainAndSync };

struct BoundaryScript {
  bool frameOpen = true;
  bool drainCompleted = true;
  int failHealthAt = 0;
  int healthChecks = 0;
  int frameEnds = 0;
  int tails = 0;
  std::vector<Event> events;

  bool Run() {
    return PortH700::GameOwnerBoundary(
        frameOpen,
        [&] {
          events.push_back(Event::Health);
          return ++healthChecks != failHealthAt;
        },
        [&] {
          CHECK(frameOpen);
          events.push_back(Event::FinishFrame);
          ++frameEnds;
          frameOpen = false; // the existing AuroraFrameEnd latch, not a second frame owner
        },
        [&] {
          CHECK(!frameOpen);
          events.push_back(Event::DrainAndSync);
          ++tails;
          return drainCompleted;
        });
  }
};

void CheckOpenAndClosedFrames() {
  BoundaryScript opened;
  CHECK(opened.Run());
  CHECK(!opened.frameOpen);
  CHECK_EQ(opened.frameEnds, 1);
  CHECK_EQ(opened.tails, 1);
  CHECK((opened.events == std::vector<Event>{Event::Health, Event::FinishFrame, Event::Health,
                                            Event::DrainAndSync, Event::Health}));

  BoundaryScript closed;
  closed.frameOpen = false;
  CHECK(closed.Run());
  CHECK_EQ(closed.frameEnds, 0);
  CHECK_EQ(closed.tails, 1); // a closed frame still has a possible destructor/unpublished tail
  CHECK((closed.events == std::vector<Event>{Event::Health, Event::Health,
                                            Event::DrainAndSync, Event::Health}));
}

void CheckReusableBoundary() {
  BoundaryScript script;
  CHECK(script.Run());
  CHECK(script.Run()); // no double end; a fresh tail is drained on every invocation
  CHECK_EQ(script.frameEnds, 1);
  CHECK_EQ(script.tails, 2);
  CHECK_EQ(script.healthChecks, 6);
  script.frameOpen = true; // the next accepted frame in the same live generation
  CHECK(script.Run());
  CHECK_EQ(script.frameEnds, 2);
  CHECK_EQ(script.tails, 3);
}

void CheckFailuresDoNotAuthorizeFrees() {
  for (const bool frameOpen : {false, true}) {
    BoundaryScript terminal;
    terminal.frameOpen = frameOpen;
    terminal.failHealthAt = 1;
    CHECK(!terminal.Run());
    CHECK_EQ(terminal.frameOpen, frameOpen);
    CHECK_EQ(terminal.frameEnds, 0);
    CHECK_EQ(terminal.tails, 0);
    CHECK_EQ(terminal.healthChecks, 1);
  }

  BoundaryScript frameFailed;
  frameFailed.failHealthAt = 2;
  CHECK(!frameFailed.Run());
  CHECK(!frameFailed.frameOpen); // attempted frame end is never repeated on failure
  CHECK_EQ(frameFailed.frameEnds, 1);
  CHECK_EQ(frameFailed.tails, 0); // failure before the tail must short-circuit

  BoundaryScript tailFailed;
  tailFailed.drainCompleted = false;
  CHECK(!tailFailed.Run());
  CHECK_EQ(tailFailed.frameEnds, 1);
  CHECK_EQ(tailFailed.tails, 1);
  CHECK_EQ(tailFailed.healthChecks, 2);

  BoundaryScript lateFailure;
  lateFailure.failHealthAt = 3;
  CHECK(!lateFailure.Run()); // completion followed by terminal health is not success
  CHECK_EQ(lateFailure.frameEnds, 1);
  CHECK_EQ(lateFailure.tails, 1);
}
} // namespace

int main() {
  CheckOpenAndClosedFrames();
  CheckReusableBoundary();
  CheckFailuresDoNotAuthorizeFrees();
  return PORT_TEST_RESULT();
}
