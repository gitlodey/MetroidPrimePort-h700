#include <aurora/aurora.h>
#include <aurora/main.h>

#include <cstdio>
#include <cstring>

namespace {
// Keep the real runtime linked even while graphics execution is intentionally
// unavailable. A help-only executable must not silently link an empty renderer.
auto (*volatile runtimeInitialize)(int, char**, const AuroraConfig*) = &aurora_initialize;
auto (*volatile runtimeShutdown)() = &aurora_shutdown;
}

int main(int argc, char** argv) {
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
