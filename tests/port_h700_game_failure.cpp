#include "port_h700_game_failure.h"
#include "port_h700_terminal.h"
#include "port_test.h"

#include <initializer_list>

namespace {
using Failure = PortH700::GameFailure;
using Result = aurora::h700::ShutdownResult;
using Stage = aurora::h700::ShutdownStage;
using Status = aurora::h700::ShutdownStatus;

static_assert(noexcept(PortH700::GameFailurePending()));
static_assert(noexcept(PortH700::GetGameFailure()));
static_assert(noexcept(PortH700::GameFailureSite()));
static_assert(noexcept(PortH700::NoteGameFailure(Failure::Backend, "static site")));
static_assert(noexcept(PortH700::GameWorkMayContinue(true, "static site")));

// Exercise the production continuation predicate, not a second implementation
// of its latch/health rule. Actual boot/frame/root bindings are checked by Python.
void CheckContinuation(bool expected) {
  for (const char* site : {"boot: after PAKs", "boot: before renderer",
                           "frame: after draw", "frame: after AsyncIdle",
                           "frame: after submission/pulse", "renderer: before reflection retirement"}) {
    int continued = 0;
    if (PortH700::GameWorkMayContinue(true, site)) ++continued;
    CHECK_EQ(continued, expected ? 1 : 0);
  }
}

struct RootProbe {
  int polls = 0;
  int idles = 0;
  int obligationQueries = 0;
  int transports = 0;
  bool checkpointHeld = false;

  // Only the existing GM11b preparation and exact guard can authorize transport.
  // Unknown/Pending represents a retained root here, not a fake unwind exception.
  void Run(bool backendHealthy, Result result, bool obligationsEmpty) {
    if (PortH700::GameWorkMayContinue(backendHealthy, "root safe-point")) return;
    const auto checkpoint = PortH700::PrepareRootUnwind(
        [] { return true; },
        [&] {
          ++polls;
          checkpointHeld = PortH700::RootUnwindReady(result);
          return result;
        },
        [&] { ++idles; },
        [] { return true; });
    if (!PortH700::RootUnwindReady(checkpoint)) return;
    if (!PortH700::RootCleanupAllowed(checkpoint, [&] {
          CHECK(checkpointHeld);
          ++obligationQueries;
          return obligationsEmpty;
        })) return;
    try {
      throw PortH700::RootTerminalToken{"root safe-point", checkpoint};
    } catch (const PortH700::RootTerminalToken& terminal) {
      CHECK(checkpointHeld);
      CHECK(PortH700::RootCleanupAllowed(terminal.checkpoint, [&] {
        ++obligationQueries;
        return obligationsEmpty;
      }));
      ++transports;
    }
  }
};

void CheckHealthy() {
  CHECK(!PortH700::GameFailurePending());
  CHECK_EQ(PortH700::GetGameFailure(), Failure::None);
  CHECK(PortH700::GameFailureSite() == nullptr);
  PortH700::NoteGameFailure(Failure::None, "not a failure");
  CheckContinuation(true);
  CHECK(PortH700::GameFailureSite() == nullptr);
  RootProbe root;
  root.Run(true, {Status::CpuQuiescent, Stage::Maps}, true);
  CHECK_EQ(root.polls, 0);
  CHECK_EQ(root.obligationQueries, 0);
  CHECK_EQ(root.transports, 0);
}

void CheckFirstFailureAndStickyContinuation() {
  static const char firstSite[] = "actual backend failure observation";
  CHECK(!PortH700::GameWorkMayContinue(false, firstSite));
  CHECK(PortH700::GameFailurePending());
  CHECK_EQ(PortH700::GetGameFailure(), Failure::Backend);
  CHECK(PortH700::GameFailureSite() == firstSite);
  PortH700::NoteGameFailure(Failure::None, "cannot clear");
  PortH700::NoteGameFailure(Failure::Backend, "cannot replace");
  CHECK(!PortH700::GameWorkMayContinue(false, "later backend failure"));
  CheckContinuation(false); // even if the backend query is healthy again
  CHECK_EQ(PortH700::GetGameFailure(), Failure::Backend);
  CHECK(PortH700::GameFailureSite() == firstSite);
}

void CheckLatchDoesNotAuthorizeUnwind() {
  for (const auto result : {Result{Status::Pending, Stage::Fifo},
                            Result{Status::Unknown, Stage::Unknown},
                            Result{Status::CpuQuiescent, Stage::Compiler},
                            Result{Status::Released, Stage::Done}}) {
    RootProbe root;
    root.Run(true, result, true); // latch alone must still request the actual poll
    CHECK_EQ(root.polls, 1);
    CHECK_EQ(root.idles, 0);
    CHECK_EQ(root.obligationQueries, 0);
    CHECK_EQ(root.transports, 0);
  }
  for (const bool backendHealthy : {false, true}) {
    RootProbe blocked;
    blocked.Run(backendHealthy, {Status::CpuQuiescent, Stage::Maps}, false);
    CHECK_EQ(blocked.polls, 1);
    CHECK_EQ(blocked.obligationQueries, 1);
    CHECK_EQ(blocked.transports, 0);

    RootProbe permitted;
    permitted.Run(backendHealthy, {Status::CpuQuiescent, Stage::Maps}, true);
    CHECK_EQ(permitted.polls, 1);
    CHECK_EQ(permitted.obligationQueries, 2); // before throw AND in catch
    CHECK_EQ(permitted.transports, 1);
  }
  RootProbe unhealthyRoot;
  unhealthyRoot.Run(false, {Status::CpuQuiescent, Stage::Maps}, true);
  CHECK_EQ(unhealthyRoot.polls, 1);
  CHECK_EQ(unhealthyRoot.obligationQueries, 2);
  CHECK_EQ(unhealthyRoot.transports, 1);
  CHECK(PortH700::GameFailurePending()); // transport never resets a generation's failure
}
} // namespace

int main() {
  // Deliberately no reset API: run the healthy phase before the first sticky failure.
  CheckHealthy();
  CheckFirstFailureAndStickyContinuation();
  CheckLatchDoesNotAuthorizeUnwind();
  return PORT_TEST_RESULT();
}
