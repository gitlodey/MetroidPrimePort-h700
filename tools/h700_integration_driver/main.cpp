#include <aurora/aurora.h>
#include <aurora/main.h>
#include "scene.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#ifdef AURORA_H700
#include "../../extern/aurora/lib/h700/lifecycle.hpp"
#include "../../extern/aurora/lib/h700/presentation.hpp"
#endif

namespace {
auto (*volatile runtimeInitialize)(int,char**,const AuroraConfig*) = &aurora_initialize;
auto (*volatile runtimeShutdown)() = &aurora_shutdown;
#ifdef AURORA_H700
int scene_exception(const char* message) {
  std::fprintf(stderr,"Diagnostic exception: %s\n",message);
  if (h700_driver::generationOwnersLive)
    h700_driver::fail_stop("exception with live generation owners; shutdown stage unproven");
  if (const auto control=aurora::h700::diagnostic::current()) control->cancel();
  aurora::h700::diagnostic::install({});
  return 1; // no initialization attempted, or actual Released/FailedReleased already observed
}
#endif
}
int main(int argc, char** argv) {
  if (!runtimeInitialize || !runtimeShutdown) return 1; // keep real Aurora linked, including the non-H700 compile profile
  if (argc==2 && std::strcmp(argv[1],"--help")==0) {
    std::puts("H700 integration driver\n"
              "  --help / --version           No SDL/EGL/Dawn initialization\n"
              "  --run --test-dir /fresh/path Fixed IG12 two-generation scenario (GPU; IG14 permission required)\n"
              "App deadline 60 seconds; external 90-second watchdog is required. No assets/cards/disc discovery.");
    return 0;
  }
  if (argc==2 && std::strcmp(argv[1],"--version")==0) {
    std::puts("H700 integration driver " H700_DRIVER_REVISION " (not device-validated)"); return 0;
  }
  if (argc!=4 || std::strcmp(argv[1],"--run") || std::strcmp(argv[2],"--test-dir") || !argv[3][0]) {
    std::fputs("Use --run --test-dir /fresh/path; invalid arguments, no graphics initialized\n",stderr); return 2;
  }
  // A diagnostic run must not silently inherit a different renderer, validation or shader profile.
  if (const char* name=h700_driver::conflicting_override([](const char* key) { return std::getenv(key); })) {
    std::fprintf(stderr,"Conflicting runtime override %s; no graphics initialized\n",name); return 2;
  }
#ifdef AURORA_H700
  try { return h700_driver::run_scene(argv[3]); }
  catch (const std::exception& error) { return scene_exception(error.what()); }
  catch (...) { return scene_exception("non-standard exception"); }
#else
  std::fputs("The graphics scenario requires the H700 profile; no graphics initialized\n",stderr); return 2;
#endif
}
