#pragma once

#include "presentation_state.hpp"
#include "../webgpu/wgpu.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <dawn/native/OpenGLBackend.h>

#include <memory>
#include <thread>
#include <vector>

struct SDL_Window;

namespace aurora::h700 {

inline constexpr uint32_t NativeWidth = 640;
inline constexpr uint32_t NativeHeight = 480;
inline constexpr wgpu::TextureFormat NativeFormat = wgpu::TextureFormat::RGBA8Unorm;

struct NativeSlot {
  GLuint source = 0;
  GLuint framebuffer = 0;
  EGLImageKHR image = EGL_NO_IMAGE_KHR;
  wgpu::Texture texture;
  wgpu::TextureView view;
};

struct ContextRecord {
  EGLContext context;
  EGLContext share;
  uint64_t group;
  bool destroy_requested = false;
  bool destroyed = false;
};

struct ContextEvent {
  enum class Operation { Create, MakeCurrent, Destroy, ReleaseThread };
  Operation operation;
  std::thread::id thread;
  EGLDisplay display;
  EGLContext context;
  EGLContext share;
  EGLSurface draw;
  EGLSurface read;
  // Create returns a context; other operations return EGLBoolean (stored without consuming eglGetError).
  uintptr_t result;
};

struct ContextBinding {
  std::thread::id thread;
  EGLContext context;
  EGLSurface draw;
  EGLSurface read;
};

struct ContextSnapshot {
  std::vector<ContextRecord> contexts;
  std::vector<ContextBinding> bindings;
  std::vector<ContextEvent> events;
  bool trustworthy;
};

// SDL video init/window and every presenter/export operation belong to the SDL main thread.
bool can_start_generation(); // false after failed/unknown teardown; no live recovery
void forbid_next_generation(); // main-only terminal gate, including failure before presenter bootstrap
bool on_main_thread();
bool prepare_sdl();
bool initialize_presenter(SDL_Window* window);
std::shared_ptr<PresentationState> presentation_state();
bool presenter_ready();
bool imports_ready();
EGLDisplay external_display();
dawn::native::opengl::EGLFunctionPointerType resolve(const char* name);
// Exactly one attempt per generation, including failed requests/imports. No live-device retry.
bool begin_dawn();
bool import_slots(const wgpu::Device& device);
const NativeSlot* native_slot(const FrameDescriptor& descriptor);
bool restore_presenter(); // raw eglMakeCurrent, never SDL's cached fast path

// Main acquires BEFORE gfx recording. The returned value must be captured independently
// of the CPU FramePacket, which is cleared before the end-frame callback runs.
std::optional<FrameDescriptor> acquire_frame();
bool abort_frame(const FrameDescriptor& descriptor);
// Render worker only: mark the irreversible boundary, Submit, then publish the real
// WaitAnyOnly future. Never waits for main, forward completion or a free native slot.
bool submit_frame(const FrameDescriptor& descriptor, const wgpu::CommandBuffer& commands);
// Opt-in IG12 samples in this same frame command buffer; no extra Submit/CPU presentation path.
bool diagnostic_copy_native(const FrameDescriptor& descriptor, const wgpu::CommandEncoder& encoder);
// Main only, nonrecursive and lock-free at entry: internal events/completions, blit/swap,
// reverse polling. No game callbacks/event pump/recording/resize/shutdown.
bool main_progress_once();

struct NativePending {
  FrameDescriptor descriptor;
  wgpu::Future forward;
  bool published;
  bool forward_observed;
  bool callback_called;
  wgpu::QueueWorkDoneStatus forward_status;
  EGLSyncKHR reverse;
  bool reverse_wait_failed;
  bool retired_after_teardown = false;
};
// IG-11 inventory, not a release/drain claim. Failed/late publications and reverse
// fences remain owned here. Exports cannot be released while these records remain.
std::array<std::optional<NativePending>, 2> pending_native_frames();
bool native_frames_drained();
bool settle_native_callbacks(); // producer joins first; poll real WaitAnyOnly callbacks, no fake completion
bool retire_native_after_dawn_release(); // failed flights need whole-group proof + presenter teardown completion
std::array<std::optional<NativePending>, 2> retired_native_frames();

ContextSnapshot context_snapshot();
bool unbind_current_dawn(); // must run on EACH owning thread, not just main

// IG-11 supplies quiescence/app-holder retirement BEFORE these operations. Merely dropping
// our wrappers is not proof of alias deletion: the entire observed Dawn share group must die.
bool release_dawn_imports();
bool acknowledge_dawn_release(); // only after queue/device/adapter/instance + callback captures released
bool release_exports(); // refuses while any Dawn context/binding or reference obligation remains
bool prepare_window_destruction(); // exports first, raw unbind, then clear SDL TLS; window owns GL context
bool finish_shutdown(); // only AFTER SDL window/video destruction; resolver/libraries released last

} // namespace aurora::h700
