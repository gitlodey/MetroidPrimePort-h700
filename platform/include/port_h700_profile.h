#ifndef METROID_PRIME_PORT_PORT_H700_PROFILE_H
#define METROID_PRIME_PORT_PORT_H700_PROFILE_H

#include <aurora/aurora.h>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

// Startup policy for the approved retail H700 path. No SDL, IO or GPU calls:
// the same policy is exercised by the CPU tests before game owners exist.
namespace PortH700 {

inline constexpr int kPressureModerateMiB = 192;
inline constexpr int kPressureSevereMiB = 128;

template <class Lookup>
const char* ConflictingOverride(Lookup lookup) {
  // These consumers use presence, or can pin/replace the allocation policy.
  for (const char* name : {"MP_FRAME_BUFFERS", "MP_STORAGE_BUFFERS", "AURORA_FULL_DEVICE_BUFFERS",
                           "AURORA_STAGING_START_KIB", "AURORA_STAGING_START_MASK"}) {
    if (lookup(name) != nullptr) return name;
  }
  // No diagnostic renderer, shader, threading or cache policy overrides in the
  // first game profile. Audio/input/path/logging variables are not filtered here.
  for (const char* name : {"MP_DAWN_ENABLE", "MP_DAWN_DISABLE", "MP_GRAPHICS_BACKEND",
                           "AURORA_PRESENT_EVERY", "MP_PIPELINE_THREADS", "MP_GL_VERSION",
                           "MP_GPU_POWER", "AURORA_VERTEX_FETCH_TEXTURE", "AURORA_NO_COMPUTE",
                           "MP_GPU_NO_CORE", "AURORA_NO_BC", "MP_RESIZE_WAIT", "MP_WGSL_OVERRIDE",
                           "MP_WGSL_DUMP", "AURORA_GPU_SELFTEST", "MP_GPU_SELFTEST", "AURORA_OVERDRAW", "MP_STORAGE_CLAMP",
                           "MP_MEM1_MB", "MP_ROOM_GEO_RESIDENT", "MP_ROOM_GEO_RESIDENT_MB",
                           "MP_PIPELINE_LAZY", "MP_PIPELINE_SEED", "MP_PIPELINE_PBR", "MP_GPU_DRIVER",
                                                      "MP_PIPELINE_TRIM_MIB", "MP_PIPELINE_TRIM_SEVERE_MIB", "MP_PIPELINE_EVICT",
                                                      "MP_TEXTURE_PRESSURE_MIB", "MP_TEXTURE_PRESSURE_SEVERE_MIB"}) {
    if (const char* value = lookup(name); value != nullptr && value[0] != '\0') return name;
  }
  for (const auto& [name, expected] : {std::pair{"SDL_VIDEODRIVER", "sdl2"},
                                     std::pair{"SDL3SHIM_SDL2_VIDEODRIVER", "mali"},
                                     std::pair{"MP_OPENGLES", "1"}}) {
    if (const char* value = lookup(name); value != nullptr && std::string_view(value) != expected) return name;
  }
  return nullptr;
}

inline void ApplyConfig(AuroraConfig& config) {
  config.desiredBackend = BACKEND_OPENGLES;
  config.allowCpuAdapter = false;
  config.windowWidth = 640;
  config.windowHeight = 480;
  config.mem1Size = MEM1_DEFAULT_SIZE;
  config.frameBufferScale = 1;
  config.frameStorageScale = 0; // follows frameBufferScale; NOT an explicit pin
  config.residentGeometryMiB = 0;
  config.frameBuffersPinned = false;
  config.frameBuffersDeferred = true;
  config.pipelineCacheLazy = true;
  config.pipelineCacheNoSeed = true;
  config.pipelineCacheSeedData = nullptr;
  config.pipelineCacheSeedSize = 0;
  config.pipelineCacheSkipPbr = true;
  config.pipelineCacheSkipPbrReason = "H700 retail profile";
}

inline bool HasToggle(const char* text, std::string_view name) {
  for (std::string_view rest = text != nullptr ? text : ""; !rest.empty();) {
    const size_t comma = rest.find(',');
    if (rest.substr(0, comma) == name) return true;
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
  }
  return false;
}

inline const char* ProtectDawnToggles(const std::string& enable, std::string& disable) {
  for (const char* name : {"skip_validation", "disable_robustness"}) {
    if (HasToggle(enable.c_str(), name)) return name;
  }
  if (HasToggle(disable.c_str(), "gl_allow_context_on_multi_threads")) return "gl_allow_context_on_multi_threads";
  // These disabled toggles remove Aurora's NDEBUG performance defaults. The
  // environment is checked separately because it is applied after config.
  for (const char* name : {"skip_validation", "disable_robustness"}) {
    if (!HasToggle(disable.c_str(), name)) {
      if (!disable.empty()) disable += ',';
      disable += name;
    }
  }
  return nullptr;
}

} // namespace PortH700
#endif
