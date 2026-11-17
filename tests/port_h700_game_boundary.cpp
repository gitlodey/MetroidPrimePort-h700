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
enum class ExitEvent {
  Boundary, DrainOldWork, ClearArchAlias, DestroyArch, DrainArchTail, FlushArch,
  ShutdownSubsystems, ClearGlobalsAlias, DestroyGlobals, DestroyLoader,
  DrainGlobalTail, FlushFinal, ShutdownARAM
};

struct ExitScript {
  struct Arch {
    ExitScript& script;
    explicit Arch(ExitScript& script) : script(script) {}
    ~Arch() {
      if (!script.retirementInProgress) return;
      CHECK(script.archAlias == nullptr);
      CHECK(script.globals != nullptr);
      CHECK_EQ(script.boundaries, 1);
      CHECK_EQ(script.queuedWork, 0);
      script.events.push_back(ExitEvent::DestroyArch);
      ++script.archDestroyed;
      ++script.queuedWork;
      ++script.delayedFrees;
    }
  };

  struct Loader {
    ExitScript& script;
    explicit Loader(ExitScript& script) : script(script) {}
    ~Loader() {
      if (!script.retirementInProgress) return;
      // CResLoader must still be able to finish outstanding pak IO in its destructor.
      CHECK(script.discOpen);
      CHECK(script.loaderServicesLive);
      CHECK(script.runtimeLive);
      CHECK(script.aramLive);
      CHECK(script.globalsAlias == nullptr);
      script.events.push_back(ExitEvent::DestroyLoader);
      ++script.loaderDestroyed;
      ++script.queuedWork;
      ++script.delayedFrees;
    }
  };

  struct Globals {
    ExitScript& script;
    Loader loader;
    explicit Globals(ExitScript& script) : script(script), loader(script) {}
    ~Globals() {
      if (!script.retirementInProgress) return;
      CHECK(script.globalsAlias == nullptr);
      CHECK(script.arch == nullptr);
      CHECK_EQ(script.subsystemsDestroyed, 1);
      CHECK_EQ(script.boundaries, 3);
      script.events.push_back(ExitEvent::DestroyGlobals);
      ++script.globalsDestroyed;
      // Renderer/graphics/pool/resource-factory cleanup also leaves commands and frees.
      ++script.queuedWork;
      ++script.delayedFrees;
    }
  };

  bool retirementInProgress = false;
  bool discOpen = true;
  bool loaderServicesLive = true;
  bool runtimeLive = true;
  bool aramLive = true;
  int failBeforeWorkAt = 0;
  int failAfterWorkAt = 0;
  int boundaries = 0;
  int queuedWork = 1;
  int delayedFrees = 1;
  int freed = 0;
  int flushes = 0;
  int archDestroyed = 0;
  int subsystemsDestroyed = 0;
  int globalsDestroyed = 0;
  int loaderDestroyed = 0;
  std::vector<ExitEvent> events;
  Arch* archAlias = nullptr;
  Globals* globalsAlias = nullptr;
  std::unique_ptr<Globals> globals;
  std::unique_ptr<Arch> arch;

  ExitScript() : globals(std::make_unique<Globals>(*this)), arch(std::make_unique<Arch>(*this)) {
    archAlias = arch.get();
    globalsAlias = globals.get();
  }

  bool Boundary() {
    events.push_back(ExitEvent::Boundary);
    ++boundaries;
    CHECK(runtimeLive); // neither retirement requests generation shutdown/joins
    CHECK(discOpen);
    CHECK(loaderServicesLive);
    CHECK(aramLive);
    if (boundaries == failBeforeWorkAt) return false;
    if (boundaries == 1) {
      events.push_back(ExitEvent::DrainOldWork);
    } else if (boundaries == 2) {
      events.push_back(ExitEvent::DrainArchTail);
      ++delayedFrees; // a cleanup callback can release another allocation
    } else if (boundaries == 4) {
      events.push_back(ExitEvent::DrainGlobalTail);
      ++delayedFrees;
    }
    CHECK_EQ(queuedWork, boundaries == 3 ? 0 : (boundaries == 4 ? 3 : 1));
    queuedWork = 0;
    return boundaries != failAfterWorkAt;
  }

  void Flush() {
    CHECK_EQ(queuedWork, 0);
    CHECK(aramLive);
    CHECK(runtimeLive);
    events.push_back(flushes == 0 ? ExitEvent::FlushArch : ExitEvent::FlushFinal);
    ++flushes;
    freed += delayedFrees;
    delayedFrees = 0;
  }

  bool Run() {
    retirementInProgress = true;
    const bool archRetired = PortH700::RetireGameOwner(
        [&] { return Boundary(); },
        [&] {
          events.push_back(ExitEvent::ClearArchAlias);
          archAlias = nullptr;
          arch = nullptr;
        },
        [&] { Flush(); });
    // A false result models the production fail-stop, not a return from RsMain/unwind.
    if (!archRetired) {
      retirementInProgress = false;
      return false;
    }
    const bool globalsRetired = PortH700::RetireGameOwner(
        [&] { return Boundary(); },
        [&] {
          events.push_back(ExitEvent::ShutdownSubsystems);
          ++subsystemsDestroyed;
          ++queuedWork;
          ++delayedFrees; // no premature CFDk::ShutDown/flush here
          events.push_back(ExitEvent::ClearGlobalsAlias);
          globalsAlias = nullptr;
          globals = nullptr; // includes the actual simulated Loader destructor
        },
        [&] { Flush(); });
    retirementInProgress = false; // test-scope teardown is not production unwind
    if (!globalsRetired) return false;
    CHECK_EQ(flushes, 2);
    CHECK_EQ(delayedFrees, 0);
    CHECK_EQ(loaderDestroyed, 1);
    events.push_back(ExitEvent::ShutdownARAM);
    aramLive = false;
    return true;
  }
};

void CheckNormalExitOrdering() {
  ExitScript script;
  CHECK(script.Run());
  CHECK_EQ(script.boundaries, 4);
  CHECK_EQ(script.archDestroyed, 1);
  CHECK_EQ(script.subsystemsDestroyed, 1);
  CHECK_EQ(script.globalsDestroyed, 1);
  CHECK_EQ(script.loaderDestroyed, 1);
  CHECK_EQ(script.queuedWork, 0);
  CHECK_EQ(script.delayedFrees, 0);
  CHECK_EQ(script.freed, 7);
  CHECK(!script.aramLive);
  CHECK(script.discOpen);
  CHECK(script.loaderServicesLive);
  CHECK(script.runtimeLive); // full IG11 belongs to platform, after the game's cleanup
  CHECK((script.events == std::vector<ExitEvent>{
      ExitEvent::Boundary, ExitEvent::DrainOldWork, ExitEvent::ClearArchAlias,
      ExitEvent::DestroyArch, ExitEvent::Boundary, ExitEvent::DrainArchTail, ExitEvent::FlushArch,
      ExitEvent::Boundary, ExitEvent::ShutdownSubsystems, ExitEvent::ClearGlobalsAlias,
      ExitEvent::DestroyGlobals, ExitEvent::DestroyLoader, ExitEvent::Boundary,
      ExitEvent::DrainGlobalTail, ExitEvent::FlushFinal, ExitEvent::ShutdownARAM}));
}

void CheckNormalExitFailuresRetainRemainingOwners() {
  for (const int failAt : {1, 2, 3, 4}) {
    for (const bool afterWork : {false, true}) {
      ExitScript script;
      auto* oldArch = script.arch.get();
      auto* oldGlobals = script.globals.get();
      if (afterWork) script.failAfterWorkAt = failAt;
      else script.failBeforeWorkAt = failAt;
      CHECK(!script.Run());
      CHECK_EQ(script.boundaries, failAt);
      CHECK_EQ(script.archDestroyed, failAt > 1 ? 1 : 0);
      CHECK_EQ(script.subsystemsDestroyed, failAt == 4 ? 1 : 0);
      CHECK_EQ(script.globalsDestroyed, failAt == 4 ? 1 : 0);
      CHECK_EQ(script.loaderDestroyed, failAt == 4 ? 1 : 0);
      CHECK(script.arch.get() == (failAt == 1 ? oldArch : nullptr));
      CHECK(script.archAlias == script.arch.get());
      CHECK(script.globals.get() == (failAt < 4 ? oldGlobals : nullptr));
      CHECK(script.globalsAlias == script.globals.get());
      CHECK_EQ(script.flushes, failAt > 2 ? 1 : 0);
      CHECK_EQ(script.freed, failAt > 2 ? 3 : 0); // only an earlier proven tail may flush
      CHECK_EQ(script.delayedFrees, failAt == 1 ? 1 : (failAt == 2 ? (afterWork ? 3 : 2)
                                                     : (failAt == 3 ? 0 : (afterWork ? 4 : 3))));
      CHECK_EQ(script.queuedWork, afterWork || failAt == 3 ? 0 : (failAt == 4 ? 3 : 1));
      CHECK(script.aramLive);
      CHECK(script.runtimeLive);
      CHECK(script.discOpen);
      CHECK(script.loaderServicesLive);
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
  CheckNormalExitOrdering();
  CheckNormalExitFailuresRetainRemainingOwners();
  return PORT_TEST_RESULT();
}
