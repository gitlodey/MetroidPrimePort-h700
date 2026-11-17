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
    // The real catch fail-stops here: no owner dtors, delayed flush, full lifecycle
    // poll or return to platform. Only this CPU harness ends its scope afterwards.
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
      CHECK_EQ(PortH700::RootUnwindReady(result),
               status == Status::CpuQuiescent && stage == Stage::Maps);
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
} // namespace

int main() {
  CheckRootTransport(false, false); // partial boot: globals only
  CheckRootTransport(false, true);  // boot after architecture/preload return
  CheckRootTransport(true, true);   // inner loop catch rethrows to outer live owners
  CheckUnprovenPreparationDoesNotUnwind();
  CheckExactCheckpointOnly();
  CheckMainThreadRequiredBeforePoll();
  CheckOtherExceptionsKeepTheirPath();
  return PORT_TEST_RESULT();
}
