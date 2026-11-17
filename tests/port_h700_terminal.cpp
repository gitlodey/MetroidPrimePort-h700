#include "port_h700_terminal.h"
#include "port_test.h"

#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {
using Result = aurora::h700::ShutdownResult;
using Stage = aurora::h700::ShutdownStage;
using Status = aurora::h700::ShutdownStatus;
using Token = PortH700::RootTerminalToken;

static_assert(!std::is_base_of_v<std::exception, Token>);
static_assert(!std::is_base_of_v<std::runtime_error, Token>);
static_assert(std::is_trivially_copyable_v<Token>);

// Scripted CPU poll outcomes only. This does not simulate the GPU, actual joins,
// constructor internals or the safety of deep GX/skinning destructor unwinding.
struct PollScript {
  std::vector<Result> results;
  bool mainThread = true;
  int expireAfterIdles = 3;
  int threadChecks = 0;
  int polls = 0;
  int idles = 0;
  int deadlineChecks = 0;
  bool checkpointSeen = false;

  Result Prepare() {
    return PortH700::PrepareRootUnwind(
        [&] {
          ++threadChecks;
          CHECK_EQ(polls, 0);
          return mainThread;
        },
        [&] {
          CHECK(mainThread);
          CHECK_EQ(threadChecks, 1);
          REQUIRE(static_cast<size_t>(polls) < results.size());
          const auto result = results[polls++];
          if (PortH700::RootUnwindReady(result)) checkpointSeen = true;
          return result;
        },
        [&] { ++idles; },
        [&] {
          ++deadlineChecks;
          return idles >= expireAfterIdles;
        });
  }
};

struct Owner {
  int& live;
  int& destroyed;
  Owner(int& live, int& destroyed) : live(live), destroyed(destroyed) { ++live; }
  ~Owner() { --live; ++destroyed; }
};

// Mirrors the production root's prepare -> exact gate -> throw sequence. A
// rejected preparation is inspected without throwing a fake fail-stop exception:
// production holds its stack forever there, it does not unwind for test convenience.
bool Transport(PollScript& script, const char* safePoint) {
  const auto checkpoint = script.Prepare();
  if (!PortH700::RootUnwindReady(checkpoint)) return false;
  throw Token{safePoint, checkpoint};
}

struct RootLocal {
  const bool& checkpointSeen;
  int& destroyed;
  ~RootLocal() {
    CHECK(checkpointSeen); // even the audited root locals unwind only AFTER checkpoint
    ++destroyed;
  }
};

void CheckRootTransport(bool inLoop, bool architectureConstructed) {
  PollScript script{{{Status::Pending, Stage::Fifo},
                     {Status::Pending, Stage::Render},
                     {Status::Pending, Stage::Compiler},
                     {Status::CpuQuiescent, Stage::Maps}}};
  const char* safePoint = inLoop ? "loop root" : "boot root";
  int live = 0;
  int destroyed = 0;
  int rootLocalsDestroyed = 0;
  int genericAborts = 0;
  int innerRethrows = 0;
  bool caught = false;
  bool ordinaryCleanup = false;
  // Just like RsMain: BOTH empty owners are outside the boot+loop typed try.
  std::unique_ptr<Owner> globals;
  std::unique_ptr<Owner> architecture;
  try {
    globals = std::make_unique<Owner>(live, destroyed);
    if (architectureConstructed) architecture = std::make_unique<Owner>(live, destroyed);
    RootLocal local{script.checkpointSeen, rootLocalsDestroyed};
    if (inLoop) {
      try {
        CHECK(Transport(script, safePoint));
      } catch (const Token&) {
        ++innerRethrows;
        CHECK(script.checkpointSeen);
        CHECK_EQ(destroyed, 0);
        throw;
      } catch (...) {
        ++genericAborts; // existing AbortOnException must never consume the token
      }
    } else {
      // Failure observed only after a boot constructor/init/preload has RETURNED.
      CHECK(Transport(script, safePoint));
    }
    ordinaryCleanup = true;
  } catch (const std::runtime_error&) {
    CHECK_MSG(false, "root token was swallowed as runtime_error");
  } catch (const Token& token) {
    caught = true;
    CHECK(token.safePoint == safePoint);
    CHECK(PortH700::RootUnwindReady(token.checkpoint));
    CHECK(script.checkpointSeen);
    CHECK_EQ(script.polls, 4);
    CHECK_EQ(script.idles, 3);
    CHECK_EQ(rootLocalsDestroyed, 1);
    CHECK_EQ(live, architectureConstructed ? 2 : 1);
    CHECK_EQ(destroyed, 0);
    CHECK(globals != nullptr);
    CHECK_EQ(architecture != nullptr, architectureConstructed);
    CHECK(!ordinaryCleanup);
    // GM10 transport-only coverage: deliberately stop before GM11b cleanup here.
    // No owner dtors, flush or full lifecycle poll; this harness later ends scope.
  }
  CHECK(caught);
  CHECK_EQ(genericAborts, 0);
  CHECK_EQ(innerRethrows, inLoop ? 1 : 0);
  CHECK_EQ(destroyed, 0);
  CHECK(!ordinaryCleanup);
}

void CheckUnprovenPreparationDoesNotUnwind() {
  const std::vector<std::vector<Result>> cases{
      {{Status::Unknown, Stage::Unknown}, {Status::CpuQuiescent, Stage::Maps}},
      {{Status::Pending, Stage::Fifo}, {Status::Unknown, Stage::Render},
       {Status::CpuQuiescent, Stage::Maps}},
      {{Status::Pending, Stage::Fifo}, {Status::Pending, Stage::Render},
       {Status::Pending, Stage::Compiler}, {Status::CpuQuiescent, Stage::Maps}},
      {{Status::Pending, Stage::Maps}, {Status::Pending, Stage::Maps},
       {Status::Pending, Stage::Maps}, {Status::CpuQuiescent, Stage::Maps}},
      {{Status::Released, Stage::Done}, {Status::CpuQuiescent, Stage::Maps}},
      {{Status::FailedReleased, Stage::Done}, {Status::CpuQuiescent, Stage::Maps}},
      {{Status::CpuQuiescent, Stage::Compiler}, {Status::CpuQuiescent, Stage::Maps}},
  };
  for (const auto& results : cases) {
    PollScript script{results};
    script.expireAfterIdles = 2;
    int live = 0;
    int destroyed = 0;
    auto globals = std::make_unique<Owner>(live, destroyed);
    auto architecture = std::make_unique<Owner>(live, destroyed);
    bool unwound = false;
    try {
      CHECK(!Transport(script, "unproven checkpoint"));
    } catch (...) {
      unwound = true;
    }
    CHECK(!unwound);
    CHECK(!script.checkpointSeen);
    CHECK_EQ(live, 2);
    CHECK_EQ(destroyed, 0);
    CHECK(script.polls < static_cast<int>(results.size())); // never poll onward to invented permission
    CHECK_EQ(script.polls, script.idles + 1);
  }
}

void CheckExactCheckpointOnly() {
  for (const auto stage : {Stage::Running, Stage::Producers, Stage::Fifo, Stage::Render,
                           Stage::Compiler, Stage::Maps, Stage::Native, Stage::Holders,
                           Stage::Device, Stage::Exports, Stage::Window, Stage::Done, Stage::Unknown}) {
    for (const auto status : {Status::Pending, Status::Released, Status::FailedReleased,
                              Status::Unknown, Status::CpuQuiescent}) {
      PollScript script{{{status, stage}}};
      script.expireAfterIdles = 0;
      const auto result = script.Prepare();
      CHECK_EQ(result.status, status);
      CHECK_EQ(result.stage, stage);
      const bool exactCheckpoint = status == Status::CpuQuiescent && stage == Stage::Maps;
      CHECK_EQ(PortH700::RootUnwindReady(result), exactCheckpoint);
      for (const bool obligationsEmpty : {false, true}) {
        int queries = 0;
        CHECK_EQ(PortH700::RootCleanupAllowed(result, [&] {
                   ++queries;
                   CHECK(exactCheckpoint);
                   return obligationsEmpty;
                 }),
                 exactCheckpoint && obligationsEmpty);
        CHECK_EQ(queries, exactCheckpoint ? 1 : 0);
      }
      CHECK_EQ(script.polls, 1);
      CHECK_EQ(script.idles, 0);
    }
  }
}

void CheckMainThreadRequiredBeforePoll() {
  PollScript script{{{Status::CpuQuiescent, Stage::Maps}}};
  script.mainThread = false;
  CHECK(!Transport(script, "not SDL main"));
  CHECK_EQ(script.threadChecks, 1);
  CHECK_EQ(script.polls, 0);
  CHECK_EQ(script.idles, 0);
  CHECK_EQ(script.deadlineChecks, 0);
  CHECK(!script.checkpointSeen);
}

void CheckOtherExceptionsKeepTheirPath() {
  int genericAborts = 0;
  // A non-token loop exception still enters the existing generic abort branch.
  try {
    throw std::runtime_error("ordinary loop exception");
  } catch (const Token&) {
    CHECK(false);
  } catch (...) {
    ++genericAborts;
  }
  CHECK_EQ(genericAborts, 1);

  int live = 0;
  int destroyed = 0;
  bool propagated = false;
  try {
    // Non-token boot exceptions still propagate with ordinary owner destruction;
    // GM10 does not redesign those exceptions into terminal lifecycle transport.
    std::unique_ptr<Owner> globals;
    std::unique_ptr<Owner> architecture;
    try {
      globals = std::make_unique<Owner>(live, destroyed);
      throw std::runtime_error("ordinary boot exception");
    } catch (const Token&) {
      CHECK(false);
    }
  } catch (const std::runtime_error&) {
    propagated = true;
    CHECK_EQ(live, 0);
    CHECK_EQ(destroyed, 1);
  }
  CHECK(propagated);
}
// CPU callback models only: the actual room/resident predicates have separate
// GM11a tests. These four independent outcomes exercise the production root gate,
// not a replacement implementation of the actual resource/registry checks.
struct Obligations {
  bool roomEnvironmentEmpty = true;
  bool roomGeometryEmpty = true;
  bool liquidEmpty = true;
  bool residentEmpty = true;
  int queries = 0;

  bool Allowed(Result checkpoint) {
    return PortH700::RootCleanupAllowed(checkpoint, [&] {
      ++queries;
      CHECK(PortH700::RootUnwindReady(checkpoint));
      return roomEnvironmentEmpty && roomGeometryEmpty && liquidEmpty && residentEmpty;
    });
  }
};

enum class Retirement { Architecture, Subsystems, Globals, Flush, Aram };
using Events = std::vector<Retirement>;

struct RetirementFailure {
  Retirement at;
};

struct TerminalOwner {
  TerminalOwner*& alias;
  Events& events;
  Retirement event;

  TerminalOwner(TerminalOwner*& alias, Events& events, Retirement event)
  : alias(alias), events(events), event(event) {
    CHECK(alias == nullptr);
    alias = this;
  }
  ~TerminalOwner() {
    CHECK(alias == nullptr); // clearing after reset() would already be too late
    events.push_back(event);
  }
};

struct TerminalRoot {
  PortH700::TerminalMilestones milestones;
  Obligations obligations;
  Events events;
  Events attempts;
  bool injectFailure = false;
  Retirement failureAt = Retirement::Architecture;
  TerminalOwner* architectureAlias = nullptr;
  TerminalOwner* globalsAlias = nullptr;
  // Like the real root, nullable owners and milestones outlive its typed try.
  std::unique_ptr<TerminalOwner> globals;
  std::unique_ptr<TerminalOwner> architecture;

  TerminalRoot() {
    CHECK(!milestones.subsystemsInitialized);
    CHECK(!milestones.subsystemsRetired);
    CHECK(!milestones.aramRetired);
    globals = std::make_unique<TerminalOwner>(globalsAlias, events, Retirement::Globals);
  }
  ~TerminalRoot() {
    // Test-scope release only, after all retention assertions. A production
    // rejected gate/throwing callback fail-stops instead of leaving this scope.
    architectureAlias = nullptr;
    globalsAlias = nullptr;
  }

  void ConstructArchitecture() {
    REQUIRE(milestones.subsystemsInitialized);
    architecture = std::make_unique<TerminalOwner>(architectureAlias, events,
                                                   Retirement::Architecture);
  }

  void Attempt(Retirement event) {
    attempts.push_back(event);
    if (injectFailure && failureAt == event) throw RetirementFailure{event};
  }

  void RetireArchitecture() {
    Attempt(Retirement::Architecture);
    architectureAlias = nullptr;
    architecture.reset();
  }
  void ShutdownSubsystems() {
    Attempt(Retirement::Subsystems);
    CHECK(!architecture);
    CHECK(architectureAlias == nullptr);
    CHECK(milestones.subsystemsInitialized);
    CHECK(!milestones.subsystemsRetired);
    CHECK(globals != nullptr);
    events.push_back(Retirement::Subsystems);
  }
  void RetireGlobals() {
    Attempt(Retirement::Globals);
    CHECK(!architecture);
    CHECK(!milestones.subsystemsInitialized || milestones.subsystemsRetired);
    globalsAlias = nullptr;
    globals.reset();
  }
  void Flush() {
    Attempt(Retirement::Flush);
    CHECK(!architecture);
    CHECK(!globals);
    events.push_back(Retirement::Flush);
  }
  void ShutdownAram() {
    Attempt(Retirement::Aram);
    CHECK(milestones.subsystemsInitialized);
    CHECK(milestones.subsystemsRetired);
    CHECK(!milestones.aramRetired);
    REQUIRE(!events.empty());
    CHECK_EQ(events.back(), Retirement::Flush);
    events.push_back(Retirement::Aram);
  }

  int Cleanup() {
    // Only the production helper decides order and milestone skips. No graphics,
    // healthy boundary or full lifecycle poll is provided by these callbacks.
    return PortH700::CleanupTerminalOwners(
        milestones, [&] { RetireArchitecture(); }, [&] { ShutdownSubsystems(); },
        [&] { RetireGlobals(); }, [&] { Flush(); }, [&] { ShutdownAram(); });
  }
};

enum class RootPhase { BeforeSubsystems, BeforeArchitecture, LiveArchitecture,
                       ResetAfterArchitecture, ExitAfterGlobals };

void CheckTerminalCleanup(RootPhase phase) {
  TerminalRoot root;
  if (phase != RootPhase::BeforeSubsystems) root.milestones.subsystemsInitialized = true;
  if (phase == RootPhase::LiveArchitecture || phase == RootPhase::ResetAfterArchitecture ||
      phase == RootPhase::ExitAfterGlobals)
    root.ConstructArchitecture();
  if (phase == RootPhase::ResetAfterArchitecture || phase == RootPhase::ExitAfterGlobals)
    root.RetireArchitecture(); // old arch gone; replacement has not been constructed
  if (phase == RootPhase::ExitAfterGlobals) {
    root.ShutdownSubsystems();
    root.milestones.subsystemsRetired = true; // healthy shutdown callback returned
    root.RetireGlobals();
  }
  const auto eventsBeforeThrow = root.events;
  const auto attemptsBeforeThrow = root.attempts;
  auto* const globalsBeforeThrow = root.globals.get();
  auto* const architectureBeforeThrow = root.architecture.get();
  PollScript script{{{Status::Pending, Stage::Render}, {Status::CpuQuiescent, Stage::Maps}}};
  int rootLocalsDestroyed = 0;
  int result = -1;
  bool caught = false;
  try {
    RootLocal local{script.checkpointSeen, rootLocalsDestroyed};
    const auto checkpoint = script.Prepare();
    REQUIRE(root.obligations.Allowed(checkpoint));
    CHECK(root.events == eventsBeforeThrow);
    CHECK(root.attempts == attemptsBeforeThrow);
    CHECK_EQ(rootLocalsDestroyed, 0);
    throw Token{"terminal root phase", checkpoint};
  } catch (const Token& token) {
    caught = true;
    CHECK_EQ(rootLocalsDestroyed, 1);
    CHECK(root.events == eventsBeforeThrow);
    CHECK(root.globals.get() == globalsBeforeThrow);
    CHECK(root.architecture.get() == architectureBeforeThrow);
    REQUIRE(root.obligations.Allowed(token.checkpoint));
    result = root.Cleanup();
  }
  CHECK(caught);
  CHECK_EQ(result, 1);
  CHECK_EQ(root.obligations.queries, 2); // pre-throw and catch independently gate
  CHECK_EQ(script.polls, 2); // cleanup never resumes polling towards full release
  CHECK_EQ(script.idles, 1);
  CHECK(!root.architecture);
  CHECK(!root.globals);
  CHECK(root.architectureAlias == nullptr);
  CHECK(root.globalsAlias == nullptr);
  const bool initialized = phase != RootPhase::BeforeSubsystems;
  CHECK_EQ(root.milestones.subsystemsInitialized, initialized);
  CHECK_EQ(root.milestones.subsystemsRetired, initialized);
  CHECK_EQ(root.milestones.aramRetired, initialized);

  Events expected;
  if (phase == RootPhase::LiveArchitecture || phase == RootPhase::ResetAfterArchitecture ||
      phase == RootPhase::ExitAfterGlobals)
    expected.push_back(Retirement::Architecture);
  if (initialized) expected.push_back(Retirement::Subsystems);
  expected.push_back(Retirement::Globals);
  expected.push_back(Retirement::Flush);
  if (initialized) expected.push_back(Retirement::Aram);
  CHECK(root.events == expected);

  // Both shutdown milestones (including an already retired ARAM) suppress a
  // second callback; nullable owner callbacks remain harmless, flush still runs.
  root.attempts.clear();
  REQUIRE(root.obligations.Allowed({Status::CpuQuiescent, Stage::Maps}));
  CHECK_EQ(root.Cleanup(), 1);
  expected.push_back(Retirement::Flush);
  CHECK(root.events == expected);
  const Events repeated{Retirement::Architecture, Retirement::Globals, Retirement::Flush};
  CHECK(root.attempts == repeated);
  CHECK_EQ(root.milestones.subsystemsRetired, initialized);
  CHECK_EQ(root.milestones.aramRetired, initialized);
  CHECK_EQ(script.polls, 2);
}

void CheckEachObligationBlocksTransport() {
  const std::vector<bool Obligations::*> guards{
      &Obligations::roomEnvironmentEmpty, &Obligations::roomGeometryEmpty,
      &Obligations::liquidEmpty, &Obligations::residentEmpty};
  for (const auto guard : guards) {
    TerminalRoot root;
    root.milestones.subsystemsInitialized = true;
    root.ConstructArchitecture();
    root.obligations.*guard = false;
    auto* const globals = root.globals.get();
    auto* const architecture = root.architecture.get();
    PollScript script{{{Status::Pending, Stage::Fifo}, {Status::Pending, Stage::Render},
                       {Status::Pending, Stage::Compiler}, {Status::CpuQuiescent, Stage::Maps}}};
    bool caught = false;
    bool reachedHold = false;
    int rootLocalsDestroyed = 0;
    try {
      RootLocal local{script.checkpointSeen, rootLocalsDestroyed};
      const auto checkpoint = script.Prepare();
      CHECK_EQ(checkpoint.status, Status::CpuQuiescent);
      CHECK_EQ(checkpoint.stage, Stage::Maps);
      CHECK_EQ(root.obligations.queries, 0);
      if (root.obligations.Allowed(checkpoint)) throw Token{"blocked obligation", checkpoint};
      reachedHold = true;
      CHECK_EQ(root.obligations.queries, 1);
      CHECK_EQ(rootLocalsDestroyed, 0); // fail-stop point, before any root unwind
      CHECK(root.globals.get() == globals);
      CHECK(root.architecture.get() == architecture);
      CHECK(root.globalsAlias == globals);
      CHECK(root.architectureAlias == architecture);
      CHECK(root.events.empty());
      CHECK(root.attempts.empty());
      CHECK(!root.milestones.subsystemsRetired);
      CHECK(!root.milestones.aramRetired);
      // Only the test now leaves scope; production must hold at this point.
    } catch (const Token&) {
      caught = true;
    }
    CHECK(!caught);
    CHECK(reachedHold);
    CHECK_EQ(script.polls, 4);
    CHECK_EQ(script.idles, 3);
    CHECK(root.events.empty());
    CHECK(root.attempts.empty());

    // A token is not cached permission: the catch must query actual obligations
    // again, and retain owners if any one outcome no longer permits cleanup.
    root.obligations.*guard = true;
    const Result checkpoint{Status::CpuQuiescent, Stage::Maps};
    REQUIRE(root.obligations.Allowed(checkpoint));
    int result = -1;
    try {
      throw Token{"catch recheck", checkpoint};
    } catch (const Token& token) {
      root.obligations.*guard = false;
      if (root.obligations.Allowed(token.checkpoint)) result = root.Cleanup();
    }
    CHECK_EQ(result, -1);
    CHECK_EQ(root.obligations.queries, 3);
    CHECK(root.globals.get() == globals);
    CHECK(root.architecture.get() == architecture);
    CHECK(root.globalsAlias == globals);
    CHECK(root.architectureAlias == architecture);
    CHECK(root.events.empty());
    CHECK(root.attempts.empty());
    CHECK(!root.milestones.subsystemsRetired);
    CHECK(!root.milestones.aramRetired);
  }
}

void CheckThrowingRetirementStopsCleanup() {
  const Events order{Retirement::Architecture, Retirement::Subsystems, Retirement::Globals,
                     Retirement::Flush, Retirement::Aram};
  for (size_t failure = 0; failure < order.size(); ++failure) {
    TerminalRoot root;
    root.milestones.subsystemsInitialized = true;
    root.ConstructArchitecture();
    root.injectFailure = true;
    root.failureAt = order[failure];
    PollScript script{{{Status::CpuQuiescent, Stage::Maps}}};
    const auto checkpoint = script.Prepare();
    REQUIRE(root.obligations.Allowed(checkpoint));
    int result = -1;
    bool caught = false;
    try {
      result = root.Cleanup();
    } catch (const RetirementFailure& error) {
      caught = true;
      CHECK_EQ(error.at, order[failure]);
    }
    CHECK(caught);
    CHECK_EQ(result, -1); // no swallowed failure or invented return1
    const Events attempted(order.begin(), order.begin() + failure + 1);
    const Events completed(order.begin(), order.begin() + failure);
    CHECK(root.attempts == attempted);
    CHECK(root.events == completed);
    CHECK(root.milestones.subsystemsInitialized);
    CHECK_EQ(root.milestones.subsystemsRetired, failure > 1);
    CHECK(!root.milestones.aramRetired); // set only AFTER successful ARAM return
    CHECK_EQ(root.architecture != nullptr, failure == 0);
    CHECK_EQ(root.globals != nullptr, failure <= 2);
    CHECK(root.architectureAlias == root.architecture.get());
    CHECK(root.globalsAlias == root.globals.get());
    CHECK_EQ(script.polls, 1);
    CHECK_EQ(script.idles, 0);
    // No retry: production fail-stops with exactly these later owners/milestones.
  }
}
} // namespace

int main() {
  CheckRootTransport(false, false); // partial boot: globals only
  CheckRootTransport(false, true);  // boot after architecture/preload return
  CheckRootTransport(true, true);   // inner loop catch rethrows to outer live owners
  CheckUnprovenPreparationDoesNotUnwind();
  CheckExactCheckpointOnly();
  CheckMainThreadRequiredBeforePoll();
  CheckOtherExceptionsKeepTheirPath();
  for (const auto phase : {RootPhase::BeforeSubsystems, RootPhase::BeforeArchitecture,
                           RootPhase::LiveArchitecture, RootPhase::ResetAfterArchitecture,
                           RootPhase::ExitAfterGlobals})
    CheckTerminalCleanup(phase);
  CheckEachObligationBlocksTransport();
  CheckThrowingRetirementStopsCleanup();
  return PORT_TEST_RESULT();
}
