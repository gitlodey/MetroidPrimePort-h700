#include "port_h700_game_boundary.h"
#include "port_test.h"

#include <memory>
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
enum class ResetEvent {
  Boundary, DrainOldWork, ClearAlias, DestroyOwner, DrainDestructorCleanup,
  Flush, ConstructOwner, PublishAlias, Preload
};

struct ResetScript {
  struct Owner {
    ResetScript& script;
    explicit Owner(ResetScript& script) : script(script) {}
    ~Owner() {
      // Test-scope teardown is not a reset (the production failure path must retain owners).
      if (!script.resetInProgress) return;
      CHECK(script.alias == nullptr);
      CHECK_EQ(script.boundaries, 1);
      CHECK_EQ(script.queuedWork, 0);
      script.events.push_back(ResetEvent::DestroyOwner);
      ++script.destroyed;
      // Simulate RemoveAllIOWins/resource dtors: queued cleanup plus delayed frees.
      ++script.queuedWork;
      ++script.delayedFrees;
    }
  };

  bool resetInProgress = false;
  bool newOwner = false;
  int failBeforeWorkAt = 0;
  int failAfterWorkAt = 0;
  int boundaries = 0;
  int queuedWork = 1;
  int delayedFrees = 1;
  int freed = 0;
  int destroyed = 0;
  int flushes = 0;
  int preloads = 0;
  std::vector<ResetEvent> events;
  Owner* alias = nullptr;
  std::unique_ptr<Owner> owner;

  ResetScript() : owner(std::make_unique<Owner>(*this)) { alias = owner.get(); }

  bool Run() {
    resetInProgress = true;
    const bool reset = PortH700::ResetGameOwner(
        [&] {
          events.push_back(ResetEvent::Boundary);
          ++boundaries;
          if (boundaries == 1) {
            CHECK(owner != nullptr);
            CHECK(alias == owner.get());
            CHECK_EQ(destroyed, 0);
          } else {
            CHECK(owner == nullptr);
            CHECK(alias == nullptr);
            CHECK_EQ(destroyed, 1);
          }
          if (boundaries == failBeforeWorkAt) return false;
          CHECK_EQ(queuedWork, 1);
          events.push_back(boundaries == 1 ? ResetEvent::DrainOldWork
                                           : ResetEvent::DrainDestructorCleanup);
          queuedWork = 0;
          if (boundaries == 2) ++delayedFrees; // cleanup can itself release another resource
          return boundaries != failAfterWorkAt;
        },
        [&] {
          events.push_back(ResetEvent::ClearAlias);
          alias = nullptr;
          owner = nullptr; // execute the simulated owner's actual destructor
        },
        [&] {
          CHECK_EQ(boundaries, 2);
          CHECK_EQ(destroyed, 1);
          CHECK_EQ(queuedWork, 0);
          CHECK(owner == nullptr);
          CHECK(alias == nullptr);
          events.push_back(ResetEvent::Flush);
          ++flushes;
          freed += delayedFrees;
          delayedFrees = 0;
        },
        [&] {
          CHECK_EQ(flushes, 1);
          CHECK_EQ(delayedFrees, 0);
          CHECK_EQ(freed, 3);
          CHECK(owner == nullptr);
          CHECK(alias == nullptr); // even construction must not see the old raw alias
          events.push_back(ResetEvent::ConstructOwner);
          owner = std::make_unique<Owner>(*this);
          newOwner = true;
          alias = owner.get();
          events.push_back(ResetEvent::PublishAlias);
          CHECK(alias != nullptr);
          ++preloads;
          events.push_back(ResetEvent::Preload);
        });
    resetInProgress = false;
    return reset;
  }
};

void CheckResetOrdering() {
  ResetScript script;
  CHECK(script.Run());
  CHECK_EQ(script.boundaries, 2);
  CHECK_EQ(script.destroyed, 1);
  CHECK_EQ(script.queuedWork, 0);
  CHECK_EQ(script.delayedFrees, 0);
  CHECK_EQ(script.freed, 3);
  CHECK_EQ(script.flushes, 1);
  CHECK(script.newOwner);
  CHECK(script.alias == script.owner.get());
  CHECK_EQ(script.preloads, 1);
  CHECK((script.events == std::vector<ResetEvent>{
      ResetEvent::Boundary, ResetEvent::DrainOldWork, ResetEvent::ClearAlias,
      ResetEvent::DestroyOwner, ResetEvent::Boundary, ResetEvent::DrainDestructorCleanup,
      ResetEvent::Flush, ResetEvent::ConstructOwner, ResetEvent::PublishAlias, ResetEvent::Preload}));
}

void CheckResetFailuresRetainOwnersAndFrees() {
  // A failed boundary is not permission to free, even if its queued work finished.
  for (const int failAt : {1, 2}) {
    for (const bool afterWork : {false, true}) {
      ResetScript script;
      ResetScript::Owner* oldOwner = script.owner.get();
      if (afterWork) script.failAfterWorkAt = failAt;
      else script.failBeforeWorkAt = failAt;
      CHECK(!script.Run());
      CHECK_EQ(script.boundaries, failAt);
      CHECK_EQ(script.destroyed, failAt - 1);
      CHECK_EQ(script.flushes, 0);
      CHECK_EQ(script.freed, 0);
      CHECK(!script.newOwner);
      CHECK_EQ(script.preloads, 0);
      CHECK_EQ(script.queuedWork, afterWork ? 0 : 1);
      CHECK_EQ(script.delayedFrees, failAt == 1 ? 1 : (afterWork ? 3 : 2));
      if (failAt == 1) {
        CHECK(script.owner.get() == oldOwner);
        CHECK(script.alias == oldOwner);
      } else {
        CHECK(script.owner == nullptr);
        CHECK(script.alias == nullptr);
      }
    }
  }
}
} // namespace

int main() {
  CheckOpenAndClosedFrames();
  CheckReusableBoundary();
  CheckFailuresDoNotAuthorizeFrees();
  CheckResetOrdering();
  CheckResetFailuresRetainOwnersAndFrees();
  return PORT_TEST_RESULT();
}
