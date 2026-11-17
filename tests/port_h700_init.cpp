#include "port_h700_init.h"
#include "port_test.h"

#include <vector>

namespace {
using Result = aurora::h700::ShutdownResult;
using Stage = aurora::h700::ShutdownStage;
using Status = aurora::h700::ShutdownStatus;

AuroraInfo ValidInfo() {
  // Opaque identity only: the policy must never dereference the SDL window.
  static int windowIdentity;
  AuroraInfo info{};
  info.backend = BACKEND_OPENGLES;
  info.window = reinterpret_cast<SDL_Window*>(&windowIdentity);
  info.windowSize.native_fb_width = 640;
  info.windowSize.native_fb_height = 480;
  return info;
}

struct ShutdownScript {
  std::vector<Result> results;
  int expireAfterIdles = 3;
  int requests = 0;
  int polls = 0;
  int idles = 0;
  int deadlineChecks = 0;

  Result Run() {
    return PortH700::PollInitFailureShutdown(
        [&] {
          CHECK_EQ(requests, 0);
          CHECK_EQ(polls, 0);
          CHECK_EQ(idles, 0);
          ++requests;
        },
        [&] {
          CHECK_EQ(requests, 1);
          REQUIRE(static_cast<size_t>(polls) < results.size());
          return results[polls++];
        },
        [&] { ++idles; },
        [&] {
          ++deadlineChecks;
          return idles >= expireAfterIdles;
        });
  }
};

void CheckAcceptance() {
  const auto valid = ValidInfo();
  CHECK(PortH700::InitAccepted(valid, true));
  CHECK(!PortH700::InitAccepted(valid, false)); // returned backend/window alone are insufficient
  CHECK(!PortH700::InitAccepted(AuroraInfo{}, false));
  CHECK(!PortH700::InitAccepted(AuroraInfo{}, true));

  for (const auto backend : {BACKEND_AUTO, BACKEND_D3D11, BACKEND_D3D12, BACKEND_METAL,
                             BACKEND_VULKAN, BACKEND_OPENGL, BACKEND_WEBGPU, BACKEND_NULL}) {
    auto info = valid;
    info.backend = backend;
    CHECK(!PortH700::InitAccepted(info, true));
  }
  auto info = valid;
  info.window = nullptr;
  CHECK(!PortH700::InitAccepted(info, true));

  for (const uint32_t width : {0u, 480u, 639u, 641u, 1280u}) {
    info = valid;
    info.windowSize.native_fb_width = width;
    CHECK(!PortH700::InitAccepted(info, true));
  }
  for (const uint32_t height : {0u, 479u, 481u, 640u, 960u}) {
    info = valid;
    info.windowSize.native_fb_height = height;
    CHECK(!PortH700::InitAccepted(info, true));
  }
  // Logical window and scaled GX dimensions must not stand in for native output.
  info = valid;
  info.windowSize.width = 320;
  info.windowSize.height = 240;
  info.windowSize.fb_width = 320;
  info.windowSize.fb_height = 240;
  CHECK(PortH700::InitAccepted(info, true));
  info.windowSize.native_fb_width = 320;
  info.windowSize.native_fb_height = 240;
  info.windowSize.width = info.windowSize.fb_width = 640;
  info.windowSize.height = info.windowSize.fb_height = 480;
  CHECK(!PortH700::InitAccepted(info, true));
}

void CheckPreOwnerBootFailure() {
  // An early return may contain no window even though lifecycle has partial owners.
  const AuroraInfo earlyBoot{};
  CHECK(!PortH700::InitAccepted(earlyBoot, false));
  ShutdownScript partial{{{Status::Pending, Stage::Producers},
                          {Status::Pending, Stage::Window},
                          {Status::FailedReleased, Stage::Done}}};
  const auto result = partial.Run();
  CHECK_EQ(result.status, Status::FailedReleased);
  CHECK_EQ(result.stage, Stage::Done);
  CHECK_EQ(partial.requests, 1);
  CHECK_EQ(partial.polls, 3);
  CHECK_EQ(partial.idles, 2);

  // No runtime owners were created: the real lifecycle can already be Done.
  ShutdownScript noOwners{{{Status::Released, Stage::Done}}, 0};
  const auto emptyResult = noOwners.Run();
  CHECK_EQ(emptyResult.status, Status::Released);
  CHECK_EQ(emptyResult.stage, Stage::Done);
  CHECK_EQ(noOwners.requests, 1);
  CHECK_EQ(noOwners.polls, 1);
  CHECK_EQ(noOwners.idles, 0);
  CHECK_EQ(noOwners.deadlineChecks, 0);
}

void CheckPendingRelease() {
  for (const auto terminal : {Status::Released, Status::FailedReleased}) {
    // Known release wins even if it arrives at the deadline; no second poll/cleanup.
    ShutdownScript script{{{Status::Pending, Stage::Fifo},
                            {Status::Pending, Stage::Compiler},
                            {terminal, Stage::Done},
                            {Status::Unknown, Stage::Unknown}}, 2};
    const auto result = script.Run();
    CHECK_EQ(result.status, terminal);
    CHECK_EQ(result.stage, Stage::Done);
    CHECK_EQ(script.requests, 1);
    CHECK_EQ(script.polls, 3);
    CHECK_EQ(script.idles, 2);
    CHECK_EQ(script.deadlineChecks, 2);
  }
}

void CheckUnprovenOwnership() {
  for (const auto stage : {Stage::Producers, Stage::Render, Stage::Maps, Stage::Native}) {
    // Unknown must not be polled onward into an invented Released status.
    ShutdownScript unknown{{{Status::Pending, stage},
                             {Status::Unknown, stage},
                             {Status::Released, Stage::Done}}};
    const auto result = unknown.Run();
    CHECK_EQ(result.status, Status::Unknown);
    CHECK_EQ(result.stage, stage);
    CHECK_EQ(unknown.requests, 1);
    CHECK_EQ(unknown.polls, 2);
    CHECK_EQ(unknown.idles, 1);
    CHECK_EQ(unknown.deadlineChecks, 1);
  }
  ShutdownScript immediateUnknown{{{Status::Unknown, Stage::Unknown}}, 0};
  const auto unknown = immediateUnknown.Run();
  CHECK_EQ(unknown.status, Status::Unknown);
  CHECK_EQ(unknown.stage, Stage::Unknown);
  CHECK_EQ(immediateUnknown.requests, 1);
  CHECK_EQ(immediateUnknown.polls, 1);
  CHECK_EQ(immediateUnknown.idles, 0);

  for (const int expireAfter : {0, 2}) {
    ShutdownScript pending{{{Status::Pending, Stage::Render},
                             {Status::Pending, Stage::Compiler},
                             {Status::Pending, Stage::Native},
                             {Status::Released, Stage::Done}}, expireAfter};
    const auto result = pending.Run();
    CHECK_EQ(result.status, Status::Pending); // timeout is NOT FailedReleased/Released
    CHECK_EQ(result.stage, expireAfter == 0 ? Stage::Render : Stage::Native);
    CHECK_EQ(pending.requests, 1);
    CHECK_EQ(pending.polls, expireAfter + 1);
    CHECK_EQ(pending.idles, expireAfter);
    CHECK_EQ(pending.deadlineChecks, expireAfter + 1);
  }
}
} // namespace

int main() {
  CheckAcceptance();
  CheckPreOwnerBootFailure();
  CheckPendingRelease();
  CheckUnprovenOwnership();
  return PORT_TEST_RESULT();
}
