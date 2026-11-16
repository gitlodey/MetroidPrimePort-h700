#pragma once

#include "../../extern/aurora/lib/h700/diagnostics.hpp"
#include "../../extern/aurora/lib/h700/lifecycle.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace h700_driver {
using aurora::h700::diagnostic::Pixel;
using aurora::h700::diagnostic::Point;
// Main-owned exit-safety proof: set BEFORE initialize, clear only after actual full release.
inline bool generationOwnersLive = false;
inline bool generation_released(aurora::h700::ShutdownStatus status) {
  return status == aurora::h700::ShutdownStatus::Released || status == aurora::h700::ShutdownStatus::FailedReleased;
}
[[noreturn]] inline void fail_stop(const char* reason) noexcept {
  std::fprintf(stderr, "IG12 fail-stop: %s; ownership unproven, no release/PASS claim; bypassing static teardown\n", reason);
  try {
    if (const auto control = aurora::h700::diagnostic::current()) control->cancel();
  } catch (...) { std::fputs("Unable to open own diagnostic gates; fail-stop still required\n", stderr); }
  std::fflush(nullptr);
  std::_Exit(EXIT_FAILURE);
}
// Same presence/value rules as the actual consumers. Lookup injection keeps CPU tests GPU-free.
template <class Lookup> const char* conflicting_override(Lookup lookup) {
  for (const char* name : {"MP_DAWN_ENABLE","MP_DAWN_DISABLE","AURORA_PRESENT_EVERY","MP_PIPELINE_THREADS",
       "MP_GL_VERSION","MP_GPU_POWER","AURORA_VERTEX_FETCH_TEXTURE","AURORA_NO_COMPUTE","MP_GPU_NO_CORE",
       "AURORA_NO_BC","MP_RESIZE_WAIT","MP_WGSL_OVERRIDE","MP_WGSL_DUMP","AURORA_GPU_SELFTEST",
       "AURORA_OVERDRAW","MP_STORAGE_CLAMP"}) {
    if (const char* value = lookup(name); value && *value) return name;
  }
  for (const char* name : {"AURORA_STAGING_START_KIB","AURORA_STAGING_START_MASK","AURORA_FULL_DEVICE_BUFFERS"}) {
    if (lookup(name) != nullptr) return name;
  }
  return nullptr;
}
inline Pixel frame_marker(unsigned generation, uint64_t frame) {
  return {static_cast<uint8_t>(frame), static_cast<uint8_t>(generation * 64), static_cast<uint8_t>(255 - frame), 255};
}
// Independent math, not the renderer's fog LUT, uniforms or generated shader.
inline Pixel expected(unsigned generation, uint64_t frame, uint32_t width, Point point) {
  if ((width != 640 && width != 320) || generation < 1 || generation > 2 || frame >= 120)
    throw std::invalid_argument("Invalid oracle generation/frame/EFB width");
  if (point.y == 200 && (point.x == 80 || point.x == 240 || point.x == 400 || point.x == 560)) {
    const double x = std::floor((point.x + 0.5) * width / 640.0);
    const double offset = 2.0 * (x + 0.5) / width - 1.0;
    const double k = (frame & 1) ? 2.0 : 1.0;
    const double fog = std::clamp(0.25 * std::sqrt(offset * offset + k * k) / k, 0.0, 1.0);
    const auto value = static_cast<uint8_t>(std::lround(255.0 * (1.0 - fog)));
    return {value, value, value, 255};
  }
  if (point.x == 24 && point.y == 24) return {0,255,255,255};
  if (point.x == 80 && point.y == 40) return {255,0,0,255};
  if (point.x == 560 && point.y == 40) return {0,255,0,255};
  if (point.x == 80 && point.y == 440) return {0,0,255,255};
  if (point.x == 560 && point.y == 440) return {255,255,0,255};
  if (point.x == 320 && point.y == 440) return frame_marker(generation, frame);
  throw std::invalid_argument("Unknown oracle point");
}
inline bool matches(Pixel actual, Pixel wanted, Point point) {
  for (size_t c = 0; c < 4; ++c) {
    const int tolerance = point.y == 200 && c < 3 ? 1 : 0;
    if (std::abs(int(actual[c]) - int(wanted[c])) > tolerance) return false;
  }
  return true;
}
inline bool real_overlap(const std::vector<aurora::h700::diagnostic::Event>& events) {
  using namespace aurora::h700::diagnostic;
  struct Interval { Kind kind; uint64_t start, end; std::thread::id thread; };
  std::vector<Interval> intervals;
  for (size_t i = 0; i < events.size(); ++i) {
    const auto& begin = events[i];
    if (!begin.begin || (begin.kind != Kind::Compiler && begin.kind != Kind::ResourceTexture && begin.kind != Kind::RenderEncoder)) continue;
    for (size_t j = i + 1; j < events.size(); ++j) if (!events[j].begin && events[j].kind == begin.kind &&
        events[j].key == begin.key && events[j].thread == begin.thread) {
      intervals.push_back({begin.kind, begin.ns, events[j].ns, begin.thread}); break;
    }
  }
  for (const auto& c : intervals) for (const auto& r : intervals) for (const auto& e : intervals)
    if (c.kind == Kind::Compiler && r.kind == Kind::ResourceTexture && e.kind == Kind::RenderEncoder &&
        c.thread != r.thread && c.thread != e.thread && r.thread != e.thread &&
        std::max({c.start,r.start,e.start}) < std::min({c.end,r.end,e.end})) return true;
  return false; // barriers and artificial wait intervals NEVER count
}
int run_scene(const std::string& directory);
} // namespace h700_driver
