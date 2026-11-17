#include <aurora/aurora.h>
#include <aurora/phase.hpp>
#include <aurora/time.hpp>

#ifdef AURORA_ENABLE_GX
#include "gfx/resources.hpp"
#include "gfx/overdraw.hpp"
#include "gfx/vis_query.hpp"
#include "gfx/frame.hpp"
#include "gfx/pipeline_cache.hpp"
#include "gfx/recording.hpp"
#include "gfx/texture.hpp"
#include "gfx/render_worker.hpp"
#include "gx/command_processor.hpp"
#include "gx/fifo.hpp"
#include "gx/gx.hpp"
#include "gx/texture.hpp"
#include "imgui.hpp"
#include "io.hpp"
#include "webgpu/gpu.hpp"
#include "webgpu/gpu_prof.hpp"
#ifdef AURORA_H700
#include "h700/presentation.hpp"
#include "h700/lifecycle.hpp"
#endif
#include <webgpu/webgpu_cpp.h>
#endif

#ifdef AURORA_ENABLE_RMLUI
#include "rmlui.hpp"
#endif

#include "input.hpp"
#include "internal.hpp"
#include "screenshot.hpp"
#include "thread.hpp"
#include "window.hpp"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_surface.h>
#include <magic_enum.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <chrono>
#include <cstdlib>
#include <filesystem>

#include "system_info.hpp"
#include "tracy/Tracy.hpp"

namespace aurora {
AuroraConfig g_config;
// aurora_set_graphics_quality request: msaa << 16 | anisotropy, 0 = none.
std::atomic<uint32_t> g_pendingQuality{0};
uint32_t g_sdlCustomEventsStart;
char g_gameName[4];

namespace {
constexpr Module Log{"aurora"};

#ifdef AURORA_ENABLE_GX
std::atomic_bool g_screenshotRequested;
// A PNG request names its file and is answered through g_pngState (0 pending, 1 saved, -1 failed).
std::mutex g_pngMutex;
std::string g_pngPath;
std::atomic_int g_pngState{0};
std::atomic_uint32_t g_pngWidth{0};
std::atomic_uint32_t g_pngHeight{0};

// GPU
using webgpu::g_device;
using webgpu::g_queue;
using webgpu::g_surface;

void record_screenshot(wgpu::CommandEncoder& encoder, std::vector<gfx::AfterSubmitCallback>& callbacks) {
  std::string pngPath;
  {
    std::lock_guard lock{g_pngMutex};
    pngPath = std::move(g_pngPath);
    g_pngPath.clear();
  }
  const bool png = !pngPath.empty();
  const auto& source = webgpu::present_source();
  const bool bgra = source.format == wgpu::TextureFormat::BGRA8Unorm ||
                    source.format == wgpu::TextureFormat::BGRA8UnormSrgb;
  const bool rgba = source.format == wgpu::TextureFormat::RGBA8Unorm ||
                    source.format == wgpu::TextureFormat::RGBA8UnormSrgb;
  if (!bgra && !rgba) {
    Log.warn("Screenshot unsupported for texture format {}", magic_enum::enum_name(source.format));
    if (png) {
      g_pngState.store(-1, std::memory_order_release);
    }
    return;
  }

  const uint32_t width = source.size.width;
  const uint32_t height = source.size.height;
  const uint32_t bytesPerRow = (width * 4 + 255) & ~255u;
  const uint64_t byteSize = static_cast<uint64_t>(bytesPerRow) * height;
  const wgpu::BufferDescriptor descriptor{
      .label = "Screenshot readback",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
      .size = byteSize,
  };
  auto readback = g_device.CreateBuffer(&descriptor);
  const wgpu::TexelCopyTextureInfo src{.texture = source.texture};
  const wgpu::TexelCopyBufferInfo dst{
      .layout =
          wgpu::TexelCopyBufferLayout{
              .bytesPerRow = bytesPerRow,
              .rowsPerImage = height,
          },
      .buffer = readback,
  };
  encoder.CopyTextureToBuffer(&src, &dst, &source.size);

  callbacks.emplace_back([readback, width, height, bytesPerRow, byteSize, bgra, png, pngPath]() {
    readback.MapAsync(
        wgpu::MapMode::Read, 0, byteSize, wgpu::CallbackMode::AllowSpontaneous,
        [readback, width, height, bytesPerRow, byteSize, bgra, png, pngPath](wgpu::MapAsyncStatus status,
                                                                          wgpu::StringView message) {
          if (status != wgpu::MapAsyncStatus::Success) {
            if (status != wgpu::MapAsyncStatus::CallbackCancelled && status != wgpu::MapAsyncStatus::Aborted) {
              Log.warn("Screenshot readback failed {}: {}", magic_enum::enum_name(status), message);
            }
            if (png) {
              g_pngState.store(-1, std::memory_order_release);
            }
            return;
          }

          const void* pixels = readback.GetConstMappedRange(0, byteSize);
          // The present source's alpha is not meaningful and is usually 0, so a surface with an
          // alpha channel saves an image every viewer shows as fully transparent (a 32-bit BMP
          // with an alpha mask, or a PNG whose alpha is 0). The X formats drop it: SDL writes a
          // 24-bit BMP and converts to opaque RGBA for PNG.
          SDL_Surface* surface = SDL_CreateSurfaceFrom(
              static_cast<int>(width), static_cast<int>(height),
              bgra ? SDL_PIXELFORMAT_BGRX32 : SDL_PIXELFORMAT_RGBX32, const_cast<void*>(pixels),
              static_cast<int>(bytesPerRow));
          if (surface == nullptr) {
            Log.warn("Unable to create screenshot surface: {}", SDL_GetError());
            if (png) {
              g_pngState.store(-1, std::memory_order_release);
            }
          } else if (png) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(pngPath).parent_path(), ec);
            if (!SDL_SavePNG(surface, pngPath.c_str())) {
              Log.warn("Unable to save screenshot {}: {}", pngPath, SDL_GetError());
              g_pngState.store(-1, std::memory_order_release);
            } else {
              Log.info("Screenshot saved to {}", pngPath);
              g_pngWidth.store(width, std::memory_order_relaxed);
              g_pngHeight.store(height, std::memory_order_relaxed);
              g_pngState.store(1, std::memory_order_release);
            }
            SDL_DestroySurface(surface);
          } else {
            std::error_code ec;
            std::filesystem::create_directories("screenshots", ec);
            const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
            const std::string path = "screenshots/metroid-prime-" + std::to_string(stamp) + ".bmp";
            if (!SDL_SaveBMP(surface, path.c_str())) {
              Log.warn("Unable to save screenshot {}: {}", path, SDL_GetError());
            } else {
              Log.info("Screenshot saved to {}", path);
            }
            SDL_DestroySurface(surface);
          }
          readback.Unmap();
        });
  });
}

uint32_t clamp_scissor_coord(double value, uint32_t maximum) noexcept {
  if (!std::isfinite(value)) {
    return 0;
  }
  return static_cast<uint32_t>(std::clamp(value, 0.0, static_cast<double>(maximum)));
}

void set_present_viewport(const wgpu::RenderPassEncoder& pass, const gfx::Viewport& viewport, uint32_t surfaceWidth,
                          uint32_t surfaceHeight) noexcept {
  pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height, viewport.znear, viewport.zfar);
  const auto scissorX = clamp_scissor_coord(std::floor(viewport.left), surfaceWidth);
  const auto scissorY = clamp_scissor_coord(std::floor(viewport.top), surfaceHeight);
  const auto scissorRight = clamp_scissor_coord(std::ceil(viewport.left + viewport.width), surfaceWidth);
  const auto scissorBottom = clamp_scissor_coord(std::ceil(viewport.top + viewport.height), surfaceHeight);
  pass.SetScissorRect(scissorX, scissorY, scissorRight - scissorX, scissorBottom - scissorY);
}
#endif

#if defined(AURORA_ENABLE_GX) && defined(AURORA_H700)
constexpr std::array PreferredBackendOrder{BACKEND_OPENGLES};
#elif defined(AURORA_ENABLE_GX)
constexpr std::array PreferredBackendOrder{
#ifdef ENABLE_BACKEND_WEBGPU
    BACKEND_WEBGPU,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D12
    BACKEND_D3D12,
#endif
#ifdef DAWN_ENABLE_BACKEND_METAL
    BACKEND_METAL,
#endif
#ifdef DAWN_ENABLE_BACKEND_VULKAN
    BACKEND_VULKAN,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D11
    BACKEND_D3D11,
#endif
// Desktop GL last among the GPU backends: for GPUs whose D3D and Vulkan drivers all fail (OpenGL 3.3
// hardware), before the software adapters of the second pass.
#ifdef DAWN_ENABLE_BACKEND_DESKTOP_GL
    BACKEND_OPENGL,
#endif
#ifdef DAWN_ENABLE_BACKEND_OPENGLES
    BACKEND_OPENGLES,
#endif
#ifdef DAWN_ENABLE_BACKEND_NULL
    BACKEND_NULL,
#endif
};
#else
constexpr std::array<AuroraBackend, 0> PreferredBackendOrder{};
#endif

bool g_initialFrame = false;
#if defined(AURORA_H700) && defined(AURORA_ENABLE_GX)
std::optional<h700::FrameDescriptor> g_nativeFrame;
h700::GenerationLifecycle g_generationLifecycle;
std::thread::id g_lifecycleMain;
bool g_lifecyclePolling = false;
bool g_gfxStarted = false;
bool g_fifoStarted = false;
bool g_uiStarted = false;
bool g_inputStarted = false;
bool g_shutdownRootsRequested = false;
bool g_shutdownTailCaptured = false;
gx::fifo::DrainRequest g_shutdownFifoDrain;
gfx::render_worker::FrameTail g_shutdownFrameTail;
#ifdef AURORA_ENABLE_RMLUI
bool g_rmlStarted = false;
#endif
void end_frame(bool fifoDrained) noexcept;
#endif

AuroraInfo initialize(int argc, char* argv[], const AuroraConfig& config) noexcept {
#if defined(AURORA_H700) && defined(AURORA_ENABLE_GX)
  if (!h700::can_start_generation() || !g_generationLifecycle.begin()) {
    Log.error("H700 initialization refused: previous generation failed or release is unknown/incomplete");
    return {.backend = BACKEND_AUTO};
  }
  g_lifecycleMain = std::this_thread::get_id();
  g_gfxStarted = g_fifoStarted = g_uiStarted = g_inputStarted = false;
  g_shutdownRootsRequested = g_shutdownTailCaptured = false;
  g_shutdownFifoDrain = {};
  g_shutdownFrameTail = {};
#ifdef AURORA_ENABLE_RMLUI
  g_rmlStarted = false;
#endif
  bool initialized = false;
  struct InitOutcome {
    bool& initialized;
    ~InitOutcome() { if (!initialized) g_generationLifecycle.fail(); }
  } initOutcome{initialized};
#endif
  g_config = config;
  Log.info("Aurora initializing");
  log_system_information();
  if (g_config.appName == nullptr) {
    g_config.appName = "Aurora";
  } else {
    g_config.appName = strdup(g_config.appName);
  }
  if (g_config.userPath == nullptr) {
    g_config.userPath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.userPath = strdup(g_config.userPath);
  }
  if (g_config.cachePath == nullptr) {
    g_config.cachePath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.cachePath = strdup(g_config.cachePath);
  }
  if (g_config.resourcesPath == nullptr) {
    g_config.resourcesPath = SDL_GetBasePath();
  } else {
    g_config.resourcesPath = strdup(g_config.resourcesPath);
  }
  if (g_config.msaa == 0) {
    g_config.msaa = 1;
  }
  if (g_config.maxTextureAnisotropy == 0) {
    g_config.maxTextureAnisotropy = 16;
  }
#if defined(AURORA_H700) && defined(AURORA_ENABLE_GX)
  if (!window::initialize()) {
    window::shutdown();
    return {.backend = BACKEND_AUTO};
  }
  g_sdlCustomEventsStart = SDL_RegisterEvents(2);
  if (g_sdlCustomEventsStart == static_cast<Uint32>(-1) || !window::initialize_event_watch()) {
    window::shutdown();
    return {.backend = BACKEND_AUTO};
  }
#else
  AURORA_ASSERT(window::initialize(), "Error initializing window");

  g_sdlCustomEventsStart = SDL_RegisterEvents(2);
  AURORA_ASSERT(g_sdlCustomEventsStart, "Failed to allocate user events: {}", SDL_GetError());
  AURORA_ASSERT(window::initialize_event_watch(), "Error initializing SDL event watch");
#endif

#ifdef AURORA_ENABLE_GX
#ifdef AURORA_H700
  const AuroraBackend selectedBackend = BACKEND_OPENGLES;
  const bool requestedBackend = config.desiredBackend == BACKEND_AUTO || config.desiredBackend == BACKEND_OPENGLES;
  if (!requestedBackend || config.allowCpuAdapter || !window::create_window(BACKEND_OPENGLES) ||
      !webgpu::initialize(BACKEND_OPENGLES, false)) {
    // No fallback/retry, nor destruction of a display still borrowed by a live Dawn group.
    // The driver calls the same status-bearing lifecycle for partial init; never retry here.
    Log.error("H700 initialization failed; graphics runtime remains unavailable");
    return {.backend = BACKEND_AUTO};
  }
#else
  /* Attempt to create a window using the calling application's desired backend */
  AuroraBackend selectedBackend = config.desiredBackend;
  bool windowCreated = false;
  if (selectedBackend != BACKEND_AUTO && window::create_window(selectedBackend)) {
    if (webgpu::initialize(selectedBackend, config.allowCpuAdapter)) {
      windowCreated = true;
    } else {
      window::destroy_window();
    }
  }

  // Without a usable GPU (a virtual machine without 3D acceleration, a broken driver), a software
  // adapter such as llvmpipe or WARP still draws a picture, slowly; the Null backend draws nothing.
  // So unless CPU adapters are allowed from the start, a second pass takes them before Null.
  const int passes = config.allowCpuAdapter ? 1 : 2;
  for (int pass = 0; pass < passes && !windowCreated; ++pass) {
    const bool allowCpu = config.allowCpuAdapter || pass == 1;
    for (const auto backendType : PreferredBackendOrder) {
      if (pass + 1 < passes && backendType == BACKEND_NULL) {
        continue;
      }
      selectedBackend = backendType;
      if (!window::create_window(selectedBackend)) {
        continue;
      }
      if (webgpu::initialize(selectedBackend, allowCpu)) {
        windowCreated = true;
        break;
      } else {
        window::destroy_window();
      }
    }
  }

  AURORA_ASSERT(windowCreated, "Error creating window: {}", SDL_GetError());

  if (webgpu::g_backendType == wgpu::BackendType::Null && config.noGraphicsMessage != nullptr) {
    Log.error("No graphics device could be started; exiting");
    // No parent: the window isn't shown yet, and a box owned by a hidden window may not show either.
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, config.appName, config.noGraphicsMessage, nullptr);
    window::destroy_window();
    SDL_Quit();
    std::exit(1);
  }

#endif
  // Initialize SDL_Renderer for ImGui when we can't use a Dawn backend
  if (webgpu::g_backendType == wgpu::BackendType::Null) {
    AURORA_ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
  }
#else
  AuroraBackend selectedBackend = BACKEND_NULL;
  AURORA_ASSERT(window::create_window(BACKEND_NULL), "Error creating window: {}", SDL_GetError());
  AURORA_ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
#endif

  window::show_window();
  thread::set_current({
      .name = "Main thread",
      .affinity = thread::Affinity::SharedCache,
  });

#ifdef AURORA_ENABLE_GX
#ifdef AURORA_H700
  g_gfxStarted = true; // includes partially created bootstrap holders
#endif
  gfx::initialize();
  if (!gx::fifo::init()) {
#ifdef AURORA_H700
    if (auto state = h700::presentation_state()) state->fail("FIFO initialization failed");
#endif
    Log.error("FIFO initialization failed; resources retained for quiescence");
    return {.backend = BACKEND_AUTO};
  }
#ifdef AURORA_H700
  g_fifoStarted = true;
#endif
  imgui::create_context();
#endif
  const auto size = window::get_window_size();
  Log.info("Using framebuffer size {}x{} scale {}", size.fb_width, size.fb_height, size.scale);
#ifdef AURORA_ENABLE_GX
  if (g_config.imGuiInitCallback != nullptr) {
    g_config.imGuiInitCallback(&size);
  }
  imgui::initialize();
#ifdef AURORA_H700
  g_uiStarted = true;
#endif
#endif

#ifdef AURORA_ENABLE_RMLUI
  rmlui::initialize(size);
#if defined(AURORA_H700) && defined(AURORA_ENABLE_GX)
  g_rmlStarted = true;
#endif
#endif

  g_initialFrame = true;
  g_config.desiredBackend = selectedBackend;
#if defined(AURORA_H700) && defined(AURORA_ENABLE_GX)
  const auto state = h700::presentation_state();
  if (!state || state->snapshot().phase != h700::PresentationPhase::Running ||
      !gfx::render_worker::wait_healthy()) {
    Log.error("H700 initialization became terminal; retaining created owners for ordered shutdown");
    return {.backend = BACKEND_AUTO};
  }
  initialized = true;
#endif
  return {
      .backend = selectedBackend,
      .userPath = g_config.userPath,
      .cachePath = g_config.cachePath,
      .window = window::get_sdl_window(),
      .windowSize = size,
  };
}

#if defined(AURORA_H700) && defined(AURORA_ENABLE_GX)
void request_h700_shutdown() noexcept {
  if (g_lifecycleMain != std::thread::id{} && g_lifecycleMain != std::this_thread::get_id()) return;
  g_generationLifecycle.request_stop();
  if (g_generationLifecycle.stage() != h700::ShutdownStage::Producers || g_shutdownRootsRequested) return;
  g_shutdownRootsRequested = true;
  if (auto state = h700::presentation_state()) state->begin_draining();
  gfx::render_worker::begin_draining(); // roots only; accepted downstream work remains admissible
  if (g_gfxStarted) gfx::begin_pipeline_cache_draining();
  if (g_fifoStarted) gx::fifo::request_drain(g_shutdownFifoDrain);
}

h700::ShutdownResult poll_h700_shutdown(h700::ShutdownGoal goal) noexcept;

h700::ShutdownResult poll_h700_shutdown() noexcept {
  return poll_h700_shutdown(h700::ShutdownGoal::Full);
}

h700::ShutdownResult poll_h700_shutdown(h700::ShutdownGoal goal) noexcept {
  using Stage = h700::ShutdownStage;
  using Status = h700::ShutdownStatus;
  if (g_lifecycleMain != std::thread::id{} && g_lifecycleMain != std::this_thread::get_id())
    return {Status::Unknown, g_generationLifecycle.stage()};
  if (g_lifecyclePolling) return {Status::Unknown, g_generationLifecycle.stage()};
  g_lifecyclePolling = true;
  struct PollScope { ~PollScope() { g_lifecyclePolling = false; } } scope;
  request_h700_shutdown();
  if (g_generationLifecycle.stage() == Stage::Done || g_generationLifecycle.stage() == Stage::Unknown)
    return g_generationLifecycle.result();
  if (goal == h700::ShutdownGoal::CpuQuiescence) {
    // This is terminal transport preparation, never a healthy reset/restart boundary.
    g_generationLifecycle.fail();
    h700::forbid_next_generation();
    // Hold before even main progress can retire callbacks. A late request after Maps
    // entry remains Pending, not CPU proof with already-retired ownership.
    if (g_generationLifecycle.stage() >= Stage::Maps) return g_generationLifecycle.result(goal);
  }
  const auto unknown = [](const char* reason) {
    Log.error("H700 teardown unknown: {}. Retaining remaining ownership; no next generation", reason);
    if (auto state = h700::presentation_state()) state->fail(reason);
    h700::forbid_next_generation();
    g_generationLifecycle.unknown();
    return g_generationLifecycle.result();
  };
  if (auto state = h700::presentation_state()) {
    if (state->snapshot().first_error) g_generationLifecycle.fail();
    if (webgpu::g_instance) (void)h700::main_progress_once();
    if (state->snapshot().first_error) g_generationLifecycle.fail();
  }
  const auto stage = g_generationLifecycle.stage();
  switch (stage) {
  case Stage::Running: return g_generationLifecycle.result();
  case Stage::Producers: {
    if (g_fifoStarted && !g_generationLifecycle.failed()) {
      const auto drain = gx::fifo::poll_drain(g_shutdownFifoDrain);
      if (drain == gx::fifo::PollStatus::Pending) return g_generationLifecycle.result();
      if (drain == gx::fifo::PollStatus::Failed) {
        g_generationLifecycle.fail();
        if (auto state = h700::presentation_state()) state->fail("FIFO tail drain failed during shutdown");
      }
    }
    if (g_nativeFrame) {
      if (!g_generationLifecycle.failed() && gfx::recording_active()) {
        // FIFO has acknowledged its whole tail. Retain final recording jobs instead of
        // waiting for render capacity on main; normal accepted frames are not cancelled.
        if (!g_shutdownFrameTail.capture([] { end_frame(true); })) return unknown("Frame tail capture failed");
        g_shutdownTailCaptured = true;
      } else { (void)h700::abort_frame(*g_nativeFrame); g_nativeFrame.reset(); }
    }
    if (g_shutdownTailCaptured) {
      const auto tail = g_shutdownFrameTail.poll();
      if (tail == gfx::render_worker::Outcome::Pending) return g_generationLifecycle.result();
      if (tail != gfx::render_worker::Outcome::Completed) {
        g_generationLifecycle.fail();
        if (auto state = h700::presentation_state()) state->fail("Accepted frame tail admission failed");
        gfx::render_worker::notify_failure();
        (void)g_shutdownFrameTail.poll(); // explicit failure cancellation, never a healthy drain substitute
      }
    }
    break;
  }
  case Stage::Fifo:
    if (g_fifoStarted) {
      gx::fifo::request_shutdown();
      const auto result = gx::fifo::poll_shutdown();
      if (result == gx::fifo::PollStatus::Pending) return g_generationLifecycle.result();
      if (result == gx::fifo::PollStatus::Failed) return unknown("FIFO owner-thread unbind failed");
    }
    g_fifoStarted = false;
    break;
  case Stage::Render: {
    // Only now may downstream admission close: FIFO and the main producer have finished.
    gfx::render_worker::request_shutdown();
    const auto result = gfx::render_worker::poll_shutdown();
    if (result == gfx::render_worker::Outcome::Pending) return g_generationLifecycle.result();
    if (result != gfx::render_worker::Outcome::Completed) return unknown("Render owner-thread unbind failed");
    break;
  }
  case Stage::Compiler:
    if (g_gfxStarted) {
      gfx::request_pipeline_cache_shutdown();
      const auto result = gfx::poll_pipeline_cache_shutdown();
      if (result == gfx::PipelineShutdownStatus::Pending) return g_generationLifecycle.result();
      if (result == gfx::PipelineShutdownStatus::Failed) return unknown("Compiler owner-thread unbind/join failed");
    }
    break;
  case Stage::Maps:
    if (!g_generationLifecycle.begin_map_retirement(goal)) return g_generationLifecycle.result(goal);
    if (g_gfxStarted && !gfx::retire_frame_resources_after_quiescence()) return g_generationLifecycle.result();
    break;
  case Stage::Native: {
    // One final future covers ALL queue operations, including bootstrap with zero native frames.
    // No Submit is added. On failure retain the obligation through Destroy/callback settlement.
    const auto queue = webgpu::poll_h700_queue_drain();
    if (queue == webgpu::QueueDrainStatus::Failed) g_generationLifecycle.fail();
    if (!g_generationLifecycle.failed() &&
        (queue == webgpu::QueueDrainStatus::Pending || !h700::native_frames_drained()))
      return g_generationLifecycle.result();
    break;
  }
  case Stage::Holders:
#ifdef AURORA_ENABLE_RMLUI
    if (g_rmlStarted) { rmlui::shutdown(); g_rmlStarted = false; }
#endif
    if (g_uiStarted) { imgui::shutdown(); g_uiStarted = false; }
    if (g_gfxStarted) {
      if (!gfx::release_frame_holders_after_quiescence()) return unknown("Frame/bootstrap holders not retired");
      g_gfxStarted = false;
    }
    if (!webgpu::release_h700_app_holders()) return g_generationLifecycle.result();
    break;
  case Stage::Device: {
    const auto result = webgpu::poll_h700_device_release();
    if (result == webgpu::DeviceReleaseStatus::Pending) return g_generationLifecycle.result();
    if (result == webgpu::DeviceReleaseStatus::Unknown) return unknown("Dawn final release/context proof failed");
    break;
  }
  case Stage::Exports:
    if (!h700::retire_native_after_dawn_release() || !h700::release_exports())
      return unknown("Native failure ownership or export teardown incomplete");
    break;
  case Stage::Window:
    if (g_inputStarted) { input::shutdown(); g_inputStarted = false; }
    if (!window::can_destroy_window()) return unknown("Window/display release refused");
    window::shutdown(); // SDL owns presenter/context/surface; resolver/libraries released last
    if (h700::presentation_state()) return unknown("SDL/resolver final release incomplete");
    g_initialFrame = false;
    if (g_generationLifecycle.failed()) h700::forbid_next_generation();
    break;
  case Stage::Done: case Stage::Unknown: return g_generationLifecycle.result();
  }
  (void)g_generationLifecycle.advance(stage, goal);
  return g_generationLifecycle.result(goal);
}
#endif

void shutdown() noexcept {
#if defined(AURORA_H700) && defined(AURORA_ENABLE_GX)
  request_h700_shutdown();
  while (true) {
    const auto result = poll_h700_shutdown();
    if (result.status != h700::ShutdownStatus::Pending) {
      if (result.status != h700::ShutdownStatus::Released) Log.error("H700 shutdown ended with failure/unknown status");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
#else
#ifdef AURORA_ENABLE_GX
  if (!gx::fifo::shutdown()) {
    Log.error("FIFO processor/callback still owns resources; shutdown is incomplete");
    return;
  }
  gfx::render_worker::synchronize();
#ifdef AURORA_ENABLE_RMLUI
  rmlui::shutdown();
#endif
  imgui::shutdown();
  gfx::shutdown();
  webgpu::shutdown();
#endif
  input::shutdown();
  window::shutdown();
#endif
}

const AuroraEvent* update() noexcept {
  ZoneScoped;
  phase::set(phase::Main, "window event pump");
  if (g_initialFrame) {
    g_initialFrame = false;
    input::initialize();
#if defined(AURORA_H700) && defined(AURORA_ENABLE_GX)
    g_inputStarted = true;
#endif
  }
#ifdef AURORA_ENABLE_GX
  gx::update();
#endif
  const AuroraEvent* events = window::poll_events();
  phase::set(phase::Main, "game update");
  return events;
}

#ifdef AURORA_ENABLE_GX
// Returns false (after dropping the surface where needed) when no frame can be drawn
// because the surface is changing or gone.
static bool service_surface(const char* caller) noexcept {
#ifdef AURORA_H700
  return webgpu::presentation_ready() && window::is_presentable();
#else
  // Android keeps the native window across surfaceChanged. Releasing the surface here
  // would rebuild it on that same window while the old swapchain is still connected,
  // which fails with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR (a lost device). Skip the frame.
  if (window::is_surface_changing()) {
    return false;
  }
  const bool invalidated = window::consume_surface_invalidated();
  if (!window::is_presentable() || (invalidated && webgpu::surface_window_changed())) {
    static int sNotPresentable = 0;
    if (sNotPresentable++ < 3) {
      Log.warn("{}: not presentable - surfaceReady={} backgrounded={} invalidated={}", caller,
               window::is_surface_ready(), window::is_backgrounded(), invalidated);
    }
    webgpu::release_surface();
    return false;
  }
  return true;
#endif
}
#endif

void release_lost_surface() noexcept {
#ifdef AURORA_ENABLE_GX
  if (g_surface) {
    service_surface("release_lost_surface");
  }
#endif
}

bool begin_frame() noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
#ifdef AURORA_H700
  (void)h700::main_progress_once();
#endif
  {
    if (!service_surface("begin_frame")) {
      return false;
    }
    if (window::is_paused()) {
      static int sPaused = 0;
      if (sPaused++ < 3) {
        Log.warn("begin_frame: paused - surfaceReady={} backgrounded={}", window::is_surface_ready(),
                 window::is_backgrounded());
      }
      return false;
    }
#ifndef AURORA_H700
    if (!g_surface) {
      webgpu::refresh_surface(true);
      if (!g_surface) {
        return false;
      }
    }
#endif
    if (const uint32_t quality = g_pendingQuality.exchange(0, std::memory_order_acq_rel); quality != 0) {
      webgpu::set_quality(quality >> 16, static_cast<uint16_t>(quality & 0xFFFF));
    }
  }

#ifdef AURORA_H700
  if (g_nativeFrame) {
    if (auto state = h700::presentation_state()) state->fail("H700 begin_frame while another frame is recording");
    return false;
  }
#endif
  if (!gx::fifo::begin_frame()) return false;
#ifdef AURORA_H700
  const auto nativeFrame = h700::acquire_frame(); // reserve native credit BEFORE CPU recording/admission
  if (!nativeFrame) {
    (void)gx::fifo::end_frame();
    return false;
  }
  g_nativeFrame.emplace(*nativeFrame);
#endif
  phase::set(phase::Main, "begin_frame (waiting for a frame slot)");
  if (!gfx::begin_frame()) {
    (void)gx::fifo::end_frame();
#ifdef AURORA_H700
    if (!h700::abort_frame(*g_nativeFrame)) {
      if (auto state = h700::presentation_state()) state->fail("H700 native admission rollback failed");
    }
    g_nativeFrame.reset();
#endif
    return false;
  }
  // A failed staging-buffer acquisition must not leave an ImGui frame open.
  imgui::new_frame(window::get_window_size());
#endif
  return true;
}

void end_frame(bool fifoDrained = false) noexcept {
  ZoneScoped;
  // The frames just after an EFB resize, timed for its log line (webgpu::resize_swapchain).
  struct ResizeFrameTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    bool timed = webgpu::g_resizeFramesToTime > 0;
    ~ResizeFrameTimer() {
      if (timed) {
        const int index = 3 - webgpu::g_resizeFramesToTime--;
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        Log.info("EFB resize: frame {} after it took {:.1f} ms to record, submit and present", index, ms);
      }
    }
  } resizeFrameTimer;
#ifdef AURORA_ENABLE_GX
#ifdef AURORA_H700
  const auto nativeFrame = g_nativeFrame;
  g_nativeFrame.reset();
  if (!nativeFrame) {
    if (auto state = h700::presentation_state()) state->fail("H700 end_frame without a native reservation");
    return;
  }
#endif
  phase::set(phase::Main, "end_frame (recording and queueing the frame)");
  if ((!fifoDrained && !gx::fifo::drain()) || !gx::fifo::end_frame()) {
    // The FIFO processor may still reference the active recorder/packet. IG-11
    // retires those only after producer and downstream quiescence, never here.
    gfx::render_worker::notify_failure();
#ifdef AURORA_H700
    (void)h700::abort_frame(*nativeFrame);
    if (auto state = h700::presentation_state()) state->fail("FIFO frame drain failed before Submit");
#endif
    return;
  }
  gx::texture::end_frame();
  gfx::finish();
  auto imguiDrawData = imgui::freeze();

  const auto& presentSource = webgpu::present_source();
  const auto viewport = webgpu::calculate_present_viewport(webgpu::g_graphicsConfig.surfaceConfiguration.width,
                                                           webgpu::g_graphicsConfig.surfaceConfiguration.height,
                                                           presentSource.size.width, presentSource.size.height);

  wgpu::BindGroup rmlBindGroup;
  bool rmlOverlay = false;
#if AURORA_ENABLE_RMLUI
  if (rmlui::is_initialized()) {
    auto rmlFrame = rmlui::record_frame(viewport);
    rmlBindGroup = std::move(rmlFrame.bindGroup);
    rmlOverlay = rmlFrame.overlay;
  }
#endif

  gfx::end_frame([rmlBindGroup = std::move(rmlBindGroup), rmlOverlay, viewport,
#ifdef AURORA_H700
                  nativeFrame = *nativeFrame, // immutable value: the CPU packet is cleared before this callback
#endif
                  imguiDrawData = std::move(imguiDrawData)](
                     wgpu::CommandEncoder& encoder, std::vector<gfx::AfterSubmitCallback> afterSubmitCallbacks) {
    if (g_screenshotRequested.exchange(false, std::memory_order_acq_rel)) {
      record_screenshot(encoder, afterSubmitCallbacks);
    }
    wgpu::Texture currentTexture;
    wgpu::TextureView currentView;
#ifdef AURORA_H700
    const auto* native = h700::native_slot(nativeFrame);
    if (!native) {
      (void)h700::abort_frame(nativeFrame);
      if (auto state = h700::presentation_state()) state->fail("H700 native composition target unavailable");
      return;
    }
    currentTexture = native->texture;
    currentView = native->view;
#else
    auto surfaceStatus = wgpu::SurfaceGetCurrentTextureStatus::Error;
    bool acquireAttempted = false;
    // Port: AURORA_PRESENT_EVERY=<n> presents one frame in n (test runs on Xvfb, where each present
    // is a CPU copy under MESA_VK_WSI_DEBUG=sw). The frame is still rendered; screenshots read the EFB.
    static const uint32_t presentEvery = [] {
      const char* env = std::getenv("AURORA_PRESENT_EVERY");
      const long n = env != nullptr ? std::strtol(env, nullptr, 10) : 1;
      return n > 1 ? static_cast<uint32_t>(n) : 1u;
    }();
    static uint32_t presentCounter = 0;
    const bool skipPresent = presentEvery > 1 && (presentCounter++ % presentEvery) != 0;
    if (skipPresent) {
      gfx::after_present();
    }
    {
      window::SurfaceLock surfaceLock;
      if (!skipPresent && window::is_presentable() && g_surface) {
        ZoneScopedN("Acquire texture");
        phase::set(phase::Render, "surface GetCurrentTexture");
        wgpu::SurfaceTexture surfaceTexture;
        g_surface.GetCurrentTexture(&surfaceTexture);
        phase::set(phase::Render, "present blit recording");
        acquireAttempted = true;
        surfaceStatus = surfaceTexture.status;
        if (surfaceStatus == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal) {
          currentTexture = std::move(surfaceTexture.texture);
          currentView = currentTexture.CreateView();
        }
      }
    }

#endif
    const bool canPresent = currentTexture && currentView;
#ifdef AURORA_H700
    if (!canPresent) {
      (void)h700::abort_frame(nativeFrame);
      if (auto state = h700::presentation_state()) state->fail("H700 native composition view missing");
      return;
    }
#endif
    if (canPresent) {
      wgpu::BindGroup presentBindGroup;
      if (rmlBindGroup && !rmlOverlay) {
        presentBindGroup = rmlBindGroup;
      } else {
        const auto& resampledSource = webgpu::resample_present_source(encoder, viewport);
        presentBindGroup = webgpu::create_copy_bind_group(resampledSource);
      }
      {
        const std::array attachments{
            wgpu::RenderPassColorAttachment{
                .view = currentView,
                .loadOp = wgpu::LoadOp::Clear,
                .storeOp = wgpu::StoreOp::Store,
            },
        };
        const wgpu::RenderPassDescriptor renderPassDescriptor{
            .label = "EFB copy render pass",
            .colorAttachmentCount = attachments.size(),
            .colorAttachments = attachments.data(),
            .timestampWrites = webgpu::gpu_prof::pass_writes("Present blit"),
        };
        const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
        // Copy EFB -> XFB (swapchain)
        pass.SetPipeline(webgpu::g_CopyPipeline);
        pass.SetBindGroup(0, presentBindGroup, 0, nullptr);
        set_present_viewport(pass, viewport, webgpu::g_graphicsConfig.surfaceConfiguration.width,
                             webgpu::g_graphicsConfig.surfaceConfiguration.height);

        pass.Draw(3);
        if (rmlBindGroup && rmlOverlay) {
          pass.SetPipeline(webgpu::g_CopyPremultipliedAlphaPipeline);
          pass.SetBindGroup(0, rmlBindGroup, 0, nullptr);
          pass.Draw(3);
        }
        // ImGui draws in the same pass: a second pass would write the whole swapchain image out
        // and read it back in, which costs a tile-based (mobile) GPU a full-screen round trip.
        pass.SetViewport(0.f, 0.f, static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.width),
                         static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.height), 0.f, 1.f);
        imgui::render(pass, imguiDrawData);
        pass.End();
      }
    }
#ifndef AURORA_H700
    else if (!skipPresent) {
      Log.info("Skipping present; window not presentable");
    }
#endif
    webgpu::gpu_prof::frame_end(encoder);
    gfx::overdraw::frame_end(encoder);
    gfx::vis_query::detail::frame_end(encoder);
#ifdef AURORA_H700
    if (!h700::diagnostic_copy_native(nativeFrame, encoder)) return;
#endif
    const wgpu::CommandBufferDescriptor cmdBufDescriptor{.label = "Redraw command buffer"};
    const auto buffer = encoder.Finish(&cmdBufDescriptor);
    {
      ZoneScopedN("Queue Submit");
      phase::set(phase::Render, "queue submit");
#ifdef AURORA_H700
      if (!h700::submit_frame(nativeFrame, buffer)) return;
#else
      g_queue.Submit(1, &buffer);
#endif
    }
    gfx::sample_dawn_memory();
    phase::set(phase::Render, "after submit");
    webgpu::gpu_prof::after_submit();
    gfx::overdraw::after_submit();
    gfx::vis_query::detail::after_submit();
#ifndef AURORA_H700
    if (canPresent && g_surface) {
      ZoneScopedN("Present");
      wgpu::ConvertibleStatus status = wgpu::Status::Error;
      {
        window::SurfaceLock surfaceLock;
        if (window::is_presentable()) {
          phase::set(phase::Render, "surface Present");
          status = g_surface.Present();
        }
      }
      phase::set(phase::Render, "after present");
      if (status) {
        gfx::after_present();
      } else {
        Log.warn("Surface present failed");
        webgpu::release_surface();
      }
    } else if (g_surface && acquireAttempted) {
      // Without an attempt, surfaceStatus is only the Error placeholder: the window was briefly
      // not presentable (an Android surfaceChanged, say). Dropping the surface then made
      // begin_frame build a second VkSurface on the same ANativeWindow while the old swapchain
      // was still connected, which fails with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR (device lost).
      switch (surfaceStatus) {
      case wgpu::SurfaceGetCurrentTextureStatus::Timeout:
        Log.warn("Surface texture acquisition timed out");
        break;
      case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
      case wgpu::SurfaceGetCurrentTextureStatus::Outdated:
        Log.info("Surface texture is {}, reconfiguring swapchain", magic_enum::enum_name(surfaceStatus));
        window::push_custom_event(window::CustomEvent::RefreshSurface);
        break;
      case wgpu::SurfaceGetCurrentTextureStatus::Lost:
        Log.warn("Surface texture is {}, releasing surface", magic_enum::enum_name(surfaceStatus));
        webgpu::release_surface();
        break;
      case wgpu::SurfaceGetCurrentTextureStatus::Error:
        Log.warn("Surface texture is {}, dropping surface", magic_enum::enum_name(surfaceStatus));
        g_surface = {};
        window::set_surface_held(false);
        break;
      default:
        if (!window::is_presentable()) {
          webgpu::release_surface();
        } else {
          Log.error("Failed to get surface texture: {}", magic_enum::enum_name(surfaceStatus));
        }
        break;
      }
    }
#endif
    for (auto& callback : afterSubmitCallbacks) {
      if (callback) {
        callback();
      }
    }
    gfx::after_submit();

    TracyPlotConfig("aurora: lastVertSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastUniformSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastIndexSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastStorageSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastTextureUploadSize", tracy::PlotFormatType::Memory, false, true, 0);

    const auto& stats = gfx::detail::resources().stats;
    TracyPlot("aurora: queuedPipelines", static_cast<int64_t>(stats.queuedPipelines));
    TracyPlot("aurora: createdPipelines", static_cast<int64_t>(stats.createdPipelines));
    TracyPlot("aurora: drawCallCount", static_cast<int64_t>(stats.drawCallCount));
    TracyPlot("aurora: mergedDrawCallCount", static_cast<int64_t>(stats.mergedDrawCallCount));
    TracyPlot("aurora: lastVertSize", static_cast<int64_t>(stats.lastVertSize));
    TracyPlot("aurora: lastUniformSize", static_cast<int64_t>(stats.lastUniformSize));
    TracyPlot("aurora: lastIndexSize", static_cast<int64_t>(stats.lastIndexSize));
    TracyPlot("aurora: lastStorageSize", static_cast<int64_t>(stats.lastStorageSize));
    TracyPlot("aurora: lastTextureUploadSize", static_cast<int64_t>(stats.lastTextureUploadSize));
  }
#ifdef AURORA_H700
  , [nativeFrame = *nativeFrame] { (void)h700::abort_frame(nativeFrame); }
#endif
  );
#ifdef AURORA_H700
  (void)h700::main_progress_once();
#endif

#endif
}
} // namespace

#if defined(AURORA_H700) && defined(AURORA_ENABLE_GX)
bool h700::initialization_ready() noexcept {
  if (g_lifecycleMain != std::this_thread::get_id() ||
      g_generationLifecycle.stage() != h700::ShutdownStage::Running || g_generationLifecycle.failed() ||
      !g_gfxStarted || !g_fifoStarted || !g_uiStarted || !g_initialFrame ||
      !window::get_sdl_window() || !h700::presenter_ready() || !webgpu::presentation_ready()) return false;
  const auto state = h700::presentation_state();
  if (!state) return false;
  const auto snapshot = state->snapshot();
  return snapshot.phase == h700::PresentationPhase::Running && !snapshot.first_error &&
         gfx::render_worker::wait_healthy();
}
bool h700::game_work_healthy() noexcept {
  if (g_lifecycleMain != std::this_thread::get_id() || g_lifecyclePolling ||
      g_generationLifecycle.stage() != h700::ShutdownStage::Running || g_generationLifecycle.failed() ||
      !g_gfxStarted || !g_fifoStarted || !g_uiStarted || gx::fifo::failed()) return false;
  const auto state = h700::presentation_state();
  if (!state) return false;
  const auto snapshot = state->snapshot();
  return snapshot.phase == h700::PresentationPhase::Running && !snapshot.first_error &&
         gfx::render_worker::root_admission_open();
}
bool h700::synchronize_game_work() noexcept {
  // Never acknowledge an unfinished recorder as a completed game-owner boundary.
  if (!h700::game_work_healthy() || g_nativeFrame || gx::fifo::frame_active()) return false;
  if (!gx::fifo::drain() || !h700::game_work_healthy()) return false;
  // The ordered worker marker follows callback return AND release of the preceding job's
  // captures. Queue-empty/GXDrawDone alone do not provide this ownership evidence.
  return gfx::render_worker::synchronize() == gfx::render_worker::Outcome::Completed &&
         h700::game_work_healthy();
}
void h700::request_generation_shutdown() noexcept { request_h700_shutdown(); }
h700::ShutdownResult h700::poll_generation_shutdown() noexcept { return poll_h700_shutdown(); }
h700::ShutdownResult h700::poll_generation_cpu_quiescence() noexcept {
  return poll_h700_shutdown(h700::ShutdownGoal::CpuQuiescence);
}
#endif

void request_screenshot() noexcept {
#ifdef AURORA_ENABLE_GX
  g_screenshotRequested.store(true, std::memory_order_release);
#endif
}
void request_screenshot_png(const char* path) noexcept {
#ifdef AURORA_ENABLE_GX
  {
    std::lock_guard lock{g_pngMutex};
    g_pngPath = path != nullptr ? path : "";
  }
  g_pngState.store(0, std::memory_order_release);
  g_screenshotRequested.store(true, std::memory_order_release);
#endif
}

int screenshot_png_state(uint32_t* width, uint32_t* height) noexcept {
#ifdef AURORA_ENABLE_GX
  const int state = g_pngState.load(std::memory_order_acquire);
  if (state > 0) {
    if (width != nullptr) *width = g_pngWidth.load(std::memory_order_relaxed);
    if (height != nullptr) *height = g_pngHeight.load(std::memory_order_relaxed);
  }
  return state;
#else
  return -1;
#endif
}

bool frame_buffer_size_for_scale(float scale, uint32_t* width, uint32_t* height) noexcept {
  return window::get_frame_buffer_size_for_scale(scale, width, height);
}

uint32_t max_texture_dimension() noexcept {
  wgpu::Limits limits{};
  if (!webgpu::g_device || !webgpu::g_device.GetLimits(&limits)) {
    return 0;
  }
  return limits.maxTextureDimension2D;
}
} // namespace aurora

// C API bindings
AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config) {
  return aurora::initialize(argc, argv, *config);
}
void aurora_shutdown() { aurora::shutdown(); }
const AuroraEvent* aurora_update() { return aurora::update(); }
bool aurora_begin_frame() { return aurora::begin_frame(); }
void aurora_release_lost_surface() { aurora::release_lost_surface(); }
bool aurora_is_suspended() {
  return aurora::window::is_backgrounded() || !aurora::window::is_surface_ready() ||
         aurora::window::is_surface_changing();
}
uint64_t aurora_joystick_update_ns() { return aurora::window::joystick_update_ns(); }
void aurora_end_frame() { aurora::end_frame(); }
void aurora_set_background_pipelines_paused(bool paused) { aurora::gfx::set_background_pipelines_paused(paused); }
void aurora_disable_config_persistence() { aurora::io::disable_config_persistence(); }
AuroraBackend aurora_get_backend() { return aurora::g_config.desiredBackend; }
const char* aurora_get_gpu_driver() {
  static std::string driver;
  const wgpu::StringView description = aurora::webgpu::g_adapterInfo.description;
  driver = description.IsUndefined() ? std::string() : std::string(description.data, description.length);
  return driver.c_str();
}
bool aurora_vulkan_library_failed() { return aurora::webgpu::g_vulkanLibraryFailed; }
const AuroraBackend* aurora_get_available_backends(size_t* count) {
  if (count != nullptr) {
    *count = aurora::PreferredBackendOrder.size();
  }
  return aurora::PreferredBackendOrder.data();
}
void aurora_set_log_level(AuroraLogLevel level) { aurora::g_config.logLevel = level; }
void aurora_set_pause_on_focus_lost(bool value) { aurora::g_config.pauseOnFocusLost = value; }
void aurora_set_background_input(bool value) {
  aurora::g_config.allowJoystickBackgroundEvents = value;
  aurora::window::set_background_input(value);
}
void aurora_set_resampler(AuroraSampler sampler) {
#ifdef AURORA_ENABLE_GX
  aurora::webgpu::set_resampler(sampler);
#else
  (void)sampler;
#endif
}

void aurora_set_present_sharpness(float sharpness) {
#ifdef AURORA_ENABLE_GX
  aurora::webgpu::set_present_sharpness(sharpness);
#else
  (void)sharpness;
#endif
}
void aurora_set_graphics_quality(uint32_t msaa, uint16_t maxTextureAnisotropy) {
  aurora::g_pendingQuality.store(std::min(msaa, 4u) << 16 | maxTextureAnisotropy, std::memory_order_release);
}
void aurora_set_per_pixel_lighting(bool enabled) {
#ifdef AURORA_ENABLE_GX
  aurora::gx::set_per_pixel_lighting(enabled);
#else
  (void)enabled;
#endif
}
void aurora_set_timescale(float scale) { aurora::time::set_scale(scale); }
float aurora_get_timescale() { return aurora::time::scale(); }
