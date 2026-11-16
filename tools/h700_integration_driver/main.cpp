#include <aurora/aurora.h>
#include <aurora/main.h>

#include <cstdio>
#include <cstring>
#ifdef AURORA_H700
#include "../../extern/aurora/lib/h700/lifecycle.hpp"
#include "../../extern/aurora/lib/h700/presentation.hpp"
#include <chrono>
#include <thread>
#endif

namespace {
// Keep the real runtime linked even while graphics execution is intentionally
// unavailable. A help-only executable must not silently link an empty renderer.
auto (*volatile runtimeInitialize)(int, char**, const AuroraConfig*) = &aurora_initialize;
auto (*volatile runtimeShutdown)() = &aurora_shutdown;
#ifdef AURORA_H700
// IG12 will call this from its actual scenario boundary. No CLI option enters graphics yet.
int finish_generation_at_safe_points(std::chrono::milliseconds timeout) {
  using namespace aurora::h700;
  const auto state = presentation_state(); // owned results survive the backend's final reset
  request_generation_shutdown();
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    const auto result = poll_generation_shutdown();
    // Safe-point delivery, outside progress/recording/locks. No credit is held for this print.
    if (state) for (const auto& record : state->take_notifications()) {
      std::printf("generation=%llu frame=%llu slot=%u outcome=%u\n",
                  static_cast<unsigned long long>(record.descriptor.generation),
                  static_cast<unsigned long long>(record.descriptor.frame), record.descriptor.slot,
                  static_cast<unsigned>(record.outcome));
    }
    if (result.status != ShutdownStatus::Pending)
      return result.status == ShutdownStatus::Released ? 0 : 1;
    if (std::chrono::steady_clock::now() >= deadline) {
      std::fprintf(stderr, "Generation teardown incomplete at stage %u; ownership retained, no second generation\n",
                   static_cast<unsigned>(result.stage));
      return 1; // diagnostic exit status, NEVER a successful release claim
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}
auto (*volatile finishGeneration)(std::chrono::milliseconds) = &finish_generation_at_safe_points;
#endif
}

int main(int argc, char** argv) {
#ifdef AURORA_H700
  if (finishGeneration == nullptr) return 1;
#endif
  if (runtimeInitialize == nullptr || runtimeShutdown == nullptr) {
    return 1;
  }
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    std::puts("H700 integration driver\n"
              "  --help     Print this message without initializing SDL/EGL/Dawn\n"
              "  --version  Print the integration revision\n"
              "Graphics execution is not available until the integration lifecycle is complete.");
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::puts("H700 integration driver " H700_DRIVER_REVISION " (not device-validated)");
    return 0;
  }
  std::fputs("Graphics execution is not implemented at IG-01; no graphics were initialized.\n", stderr);
  return 2;
}
