#include "port_h700_profile.h"
#include "port_test.h"
#include <cstdio>
#include <map>
#include <string>

int main() {
  std::map<std::string, std::string> env;
  const auto lookup = [&](const char* key) -> const char* {
    const auto found = env.find(key);
    return found == env.end() ? nullptr : found->second.c_str();
  };
  REQUIRE(PortH700::ConflictingOverride(lookup) == nullptr);
  // Even scale1 is a pin when supplied through the original env path. Reject it
  // rather than accidentally allocating the full set of staging buffers.
  for (const char* name : {"MP_FRAME_BUFFERS", "MP_STORAGE_BUFFERS", "AURORA_FULL_DEVICE_BUFFERS",
                           "AURORA_STAGING_START_KIB", "AURORA_STAGING_START_MASK"}) {
    for (const char* value : {"", "0", "1", "12"}) {
      env = {{name, value}};
      REQUIRE(std::string(PortH700::ConflictingOverride(lookup)) == name);
    }
  }
  for (const char* name : {"MP_DAWN_ENABLE", "MP_DAWN_DISABLE", "MP_GRAPHICS_BACKEND",
                           "AURORA_PRESENT_EVERY", "MP_PIPELINE_THREADS", "MP_GL_VERSION",
                           "MP_GPU_POWER", "AURORA_VERTEX_FETCH_TEXTURE", "AURORA_NO_COMPUTE",
                           "MP_GPU_NO_CORE", "AURORA_NO_BC", "MP_RESIZE_WAIT", "MP_WGSL_OVERRIDE",
                           "MP_WGSL_DUMP", "AURORA_GPU_SELFTEST", "MP_GPU_SELFTEST", "AURORA_OVERDRAW", "MP_STORAGE_CLAMP",
                           "MP_MEM1_MB", "MP_ROOM_GEO_RESIDENT", "MP_ROOM_GEO_RESIDENT_MB",
                           "MP_PIPELINE_LAZY", "MP_PIPELINE_SEED", "MP_PIPELINE_PBR", "MP_GPU_DRIVER"}) {
    env = {{name, "1"}};
    REQUIRE(std::string(PortH700::ConflictingOverride(lookup)) == name);
  }
  env = {{"SDL_VIDEODRIVER", "sdl2"}, {"SDL3SHIM_SDL2_VIDEODRIVER", "mali"}, {"MP_OPENGLES", "1"},
         {"MP_USER_PATH", "/test/user"}, {"MP_AUDIO_STATS", "1"}, {"SDL_ROTATION", "270"},
         {"SDL_GAMECONTROLLERCONFIG_FILE", "/firmware/controllerdb"}};
  REQUIRE(PortH700::ConflictingOverride(lookup) == nullptr);
  for (const auto& [name, value] : {std::pair{"SDL_VIDEODRIVER", "dummy"},
                                  std::pair{"SDL_VIDEODRIVER", "sdl2,x11"},
                                  std::pair{"SDL3SHIM_SDL2_VIDEODRIVER", "offscreen"},
                                  std::pair{"MP_OPENGLES", "0"}}) {
    env = {{name, value}};
    REQUIRE(std::string(PortH700::ConflictingOverride(lookup)) == name);
  }

  AuroraConfig config{};
  config.desiredBackend = BACKEND_NULL;
  config.allowCpuAdapter = true;
  config.mem1Size = 256u * 1024u * 1024u;
  config.frameBufferScale = 12;
  config.frameStorageScale = 12;
  config.frameBuffersPinned = true;
  config.residentGeometryMiB = 256;
  const uint8_t seed = 1;
  config.pipelineCacheSeedData = &seed;
  config.pipelineCacheSeedSize = 1;
  config.userPath = "keep-user-settings";
  PortH700::ApplyConfig(config);
  REQUIRE(config.desiredBackend == BACKEND_OPENGLES && !config.allowCpuAdapter);
  REQUIRE(config.windowWidth == 640 && config.windowHeight == 480);
  REQUIRE(config.mem1Size == 24u * 1024u * 1024u);
  REQUIRE(config.frameBufferScale == 1 && config.frameStorageScale == 0);
  REQUIRE(!config.frameBuffersPinned && config.frameBuffersDeferred && config.residentGeometryMiB == 0);
  REQUIRE(config.pipelineCacheLazy && config.pipelineCacheNoSeed && config.pipelineCacheSkipPbr);
  REQUIRE(config.pipelineCacheSeedData == nullptr && config.pipelineCacheSeedSize == 0);
  REQUIRE(std::string(config.userPath) == "keep-user-settings");

  std::string enable, disable;
  REQUIRE(PortH700::ProtectDawnToggles(enable, disable) == nullptr);
  REQUIRE(disable == "skip_validation,disable_robustness");
  REQUIRE(PortH700::ProtectDawnToggles(enable, disable) == nullptr);
  REQUIRE(disable == "skip_validation,disable_robustness"); // idempotent, not duplicated per call
  for (const char* unsafe : {"skip_validation", "disable_robustness"}) {
    enable = std::string("unrelated,") + unsafe + ",other";
    REQUIRE(std::string(PortH700::ProtectDawnToggles(enable, disable)) == unsafe);
  }
  enable.clear();
  disable = "gl_allow_context_on_multi_threads";
  REQUIRE(std::string(PortH700::ProtectDawnToggles(enable, disable)) == "gl_allow_context_on_multi_threads");
  enable = "d3d11_disable_cpu_buffers"; // Aurora ignores the non-GL workaround, not a safety bypass
  disable.clear();
  REQUIRE(PortH700::ProtectDawnToggles(enable, disable) == nullptr);
  REQUIRE(PortH700::HasToggle(disable.c_str(), "skip_validation"));
  REQUIRE(PortH700::HasToggle(disable.c_str(), "disable_robustness"));
  std::puts("H700 unpinned allocation, display/override and Dawn safety policy passed");
}
