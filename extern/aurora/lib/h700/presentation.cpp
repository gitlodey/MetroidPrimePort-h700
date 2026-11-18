#include "presentation.hpp"
#include "diagnostics.hpp"
#include "presentation_timing.hpp"

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "../gfx/frame.hpp"
#include "../gfx/render_worker.hpp"
#include "../window.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>
#include <string_view>
#include <type_traits>

namespace aurora::h700 {
namespace {
constexpr Module Log{"aurora::h700"};
// Full event history is for the bounded IG diagnostic, not retail playtime. Context records
// and current bindings below remain authoritative and are updated even without this trace.
constexpr size_t MaxContextEvents = 65536;
struct DiagnosticMap {
  std::atomic_bool called{false};
  std::atomic<wgpu::MapAsyncStatus> status{wgpu::MapAsyncStatus::Error};
};
struct NativeFlight {
  wgpu::Buffer diagnostic_buffer;
  wgpu::Future diagnostic_future{};
  std::shared_ptr<DiagnosticMap> diagnostic_map;
  explicit NativeFlight(FrameDescriptor value) : descriptor(value) {}
  const FrameDescriptor descriptor;
  wgpu::Future forward{}; // written before the published release-store
  std::atomic<bool> published = false;
  std::atomic<bool> callback_called = false;
  std::atomic<wgpu::QueueWorkDoneStatus> status = wgpu::QueueWorkDoneStatus::Error;
  bool forward_observed = false; // remaining fields are main-only
  EGLSyncKHR reverse = EGL_NO_SYNC_KHR;
  bool reverse_wait_failed = false;
  FrameWallTimings wall; // worker timestamps use the published release/acquire; observation/presentation are main-only
};
struct GLProcCache {
  std::once_flag once;
  void* original = nullptr; // written by the resolver once, before exposing its wrapper
};
struct Backend {
  explicit Backend(uint64_t id) : generation(id), state(std::make_shared<PresentationState>(id)) {}
  uint64_t generation;
  std::shared_ptr<PresentationState> state;
  std::thread::id main = std::this_thread::get_id();
  SDL_Window* window = nullptr;
  SDL_GLContext sdl_context = nullptr; // owned by the pinned SDL2 mali window data
  EGLDisplay display = EGL_NO_DISPLAY; // borrowed from SDL, never eglTerminate here
  EGLContext presenter = EGL_NO_CONTEXT;
  EGLSurface draw = EGL_NO_SURFACE;
  EGLSurface read = EGL_NO_SURFACE;
  void* egl_library = nullptr;
  void* gles_library = nullptr;
  PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
  PFNEGLCREATESYNCKHRPROC create_sync = nullptr;
  PFNEGLCLIENTWAITSYNCKHRPROC client_wait_sync = nullptr;
  PFNEGLDESTROYSYNCKHRPROC destroy_sync = nullptr;
  std::array<NativeSlot, 2> slots;
  std::mutex flights_mutex; // never held across a Dawn/GL/EGL call or a wait
  std::array<std::shared_ptr<NativeFlight>, 2> flights;
  uint64_t next_frame = 0;
  FrameWallSummary frame_wall;
  FrameWallTimings::Clock::time_point last_wall_report = FrameWallTimings::Clock::now();
  const bool gl_timing_enabled = gl_api_timing_enabled(std::getenv("MP_H700_GL_TIMING"));
  GLApiWallSummary gl_wall;
  std::array<GLProcCache, GLApiWallSummary::WrappedGLCount> gl_procs;
  std::array<GLProcCache, GLApiWallSummary::DawnClientWaitFields.size()> egl_wait_procs;
  PFNEGLMAKECURRENTPROC make_current = nullptr;
  bool progressing = false;
  bool presenter_ready = false;
  bool dawn_attempted = false;
  bool dawn_released = true;
  bool import_attempted = false;
  bool imports_ready = false;
  bool exports_released = false;
  bool window_release_ready = false;
  std::mutex observer_mutex;
  std::vector<ContextRecord> contexts;
  std::vector<ContextBinding> bindings;
  std::vector<ContextEvent> events;
  bool trustworthy = true;
};
// Installed before RequestAdapter, removed only after final native release and SDL teardown.
// No resolver callback may overlap that boundary (IG-11 quiescence contract).
std::unique_ptr<Backend> backend;
uint64_t next_generation = 1;
bool allow_next_generation = true;
std::array<std::optional<NativePending>, 2> retired_flights;

bool fail(std::string message) {
  Log.error("{}", message);
  if (backend) backend->state->fail(std::move(message));
  return false;
}
bool main_thread() {
  return backend && backend->main == std::this_thread::get_id() && SDL_IsMainThread();
}
bool require_main() {
  return main_thread() || fail("H700 presenter operation outside SDL main thread");
}
bool has_extension(const char* text, std::string_view name) {
  for (std::string_view rest = text ? text : ""; !rest.empty();) {
    const auto end = rest.find(' ');
    if (rest.substr(0, end) == name) return true;
    if (end == std::string_view::npos) break;
    rest.remove_prefix(end + 1);
  }
  return false;
}
void* raw_proc(const char* name) {
  if (!backend) return nullptr;
  void* proc = backend->egl_library ? dlsym(backend->egl_library, name) : nullptr;
  if (!proc && backend->gles_library) proc = dlsym(backend->gles_library, name);
  if (!proc) proc = reinterpret_cast<void*>(eglGetProcAddress(name));
  return proc;
}
using GLWall = GLApiWallSummary;

template <GLWall::Field Api, typename Proc, typename... Args>
auto timed_driver_call(Proc proc, uint64_t bytes, Args... args) -> std::invoke_result_t<Proc, Args...> {
  const auto begin = FrameWallTimings::Clock::now();
  if constexpr (std::is_void_v<std::invoke_result_t<Proc, Args...>>) {
    proc(args...);
    const auto returned = FrameWallTimings::Clock::now();
    backend->gl_wall.record(Api, static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(returned - begin).count()), bytes);
  } else {
    const auto result = proc(args...);
    const auto returned = FrameWallTimings::Clock::now();
    backend->gl_wall.record(Api, static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(returned - begin).count()), bytes);
    return result;
  }
}

template <GLWall::Field Api, typename Call>
auto timed_progress_call(Call call) -> std::invoke_result_t<Call> {
  return probe_wall_call(backend->gl_timing_enabled, call,
      [](uint64_t wall_ns, const auto&...) { backend->gl_wall.record(Api, wall_ns); });
}

GLWall::WaitOutcome egl_wait_outcome(EGLint result) {
  static_assert(EGL_TIMEOUT_EXPIRED == EGL_TIMEOUT_EXPIRED_KHR &&
                EGL_CONDITION_SATISFIED == EGL_CONDITION_SATISFIED_KHR &&
                EGL_SYNC_FLUSH_COMMANDS_BIT == GLWall::SyncFlushCommandsBit &&
                EGL_SYNC_FLUSH_COMMANDS_BIT_KHR == GLWall::SyncFlushCommandsBit);
  if (result == EGL_TIMEOUT_EXPIRED_KHR) return GLWall::WaitOutcome::TimedOut;
  if (result == EGL_CONDITION_SATISFIED_KHR) return GLWall::WaitOutcome::Signaled;
  return GLWall::WaitOutcome::Other;
}

template <GLWall::Field Api, typename Proc, typename Sync, typename Time>
EGLint timed_egl_wait_call(Proc proc, EGLDisplay display, Sync sync, EGLint flags, Time timeout) {
  return probe_wall_call(backend->gl_timing_enabled, [=] { return proc(display, sync, flags, timeout); },
      [=](uint64_t wall_ns, EGLint result) {
        backend->gl_wall.record_wait(Api, wall_ns, static_cast<uint32_t>(flags), static_cast<uint64_t>(timeout),
                                     egl_wait_outcome(result));
      });
}

EGLint EGLAPIENTRY timed_client_wait_sync(EGLDisplay display, EGLSync sync, EGLint flags, EGLTime timeout) {
  const auto proc = reinterpret_cast<PFNEGLCLIENTWAITSYNCPROC>(backend->egl_wait_procs[0].original);
  return timed_egl_wait_call<GLWall::ClientWaitSync>(proc, display, sync, flags, timeout);
}
EGLint EGLAPIENTRY timed_client_wait_sync_khr(EGLDisplay display, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout) {
  const auto proc = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(backend->egl_wait_procs[1].original);
  return timed_egl_wait_call<GLWall::ClientWaitSyncKHR>(proc, display, sync, flags, timeout);
}

template <GLWall::Field Api, typename Proc, typename... Args>
auto timed_gl_call(uint64_t bytes, Args... args) -> std::invoke_result_t<Proc, Args...> {
  // Only expose these wrappers after resolving a non-null real entry point. No lookups,
  // error queries, logging or allocations belong inside the measured driver interval.
  return timed_driver_call<Api>(reinterpret_cast<Proc>(backend->gl_procs[Api].original), bytes, args...);
}

void* GL_APIENTRY timed_map_buffer_range(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
  return timed_gl_call<GLWall::MapBufferRange, PFNGLMAPBUFFERRANGEPROC>(
      requested_gl_buffer_bytes(length), target, offset, length, access);
}
GLboolean GL_APIENTRY timed_unmap_buffer(GLenum target) {
  return timed_gl_call<GLWall::UnmapBuffer, PFNGLUNMAPBUFFERPROC>(0, target);
}
void GL_APIENTRY timed_buffer_data(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
  timed_gl_call<GLWall::BufferData, PFNGLBUFFERDATAPROC>(requested_gl_buffer_bytes(size), target, size, data, usage);
}
void GL_APIENTRY timed_buffer_sub_data(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
  static_assert(GL_UNIFORM_BUFFER == GLWall::UniformBufferTarget && GL_ARRAY_BUFFER == GLWall::ArrayBufferTarget);
  const auto proc = reinterpret_cast<PFNGLBUFFERSUBDATAPROC>(backend->gl_procs[GLWall::BufferSubData].original);
  const auto begin = FrameWallTimings::Clock::now();
  proc(target, offset, size, data);
  const auto returned = FrameWallTimings::Clock::now();
  backend->gl_wall.record_buffer_sub_data(target, static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(returned - begin).count()), requested_gl_buffer_bytes(size));
}
void GL_APIENTRY timed_copy_buffer_sub_data(GLenum read, GLenum write, GLintptr read_offset,
                                           GLintptr write_offset, GLsizeiptr size) {
  timed_gl_call<GLWall::CopyBufferSubData, PFNGLCOPYBUFFERSUBDATAPROC>(
      requested_gl_buffer_bytes(size), read, write, read_offset, write_offset, size);
}
void GL_APIENTRY timed_tex_sub_image_2d(GLenum target, GLint level, GLint x, GLint y, GLsizei width,
                                       GLsizei height, GLenum format, GLenum type, const void* pixels) {
  timed_gl_call<GLWall::TexSubImage2D, PFNGLTEXSUBIMAGE2DPROC>(
      0, target, level, x, y, width, height, format, type, pixels);
}
void GL_APIENTRY timed_tex_image_2d(GLenum target, GLint level, GLint internal_format, GLsizei width,
                                   GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels) {
  timed_gl_call<GLWall::TexImage2D, PFNGLTEXIMAGE2DPROC>(
      0, target, level, internal_format, width, height, border, format, type, pixels);
}
void GL_APIENTRY timed_tex_storage_2d(GLenum target, GLsizei levels, GLenum internal_format,
                                     GLsizei width, GLsizei height) {
  timed_gl_call<GLWall::TexStorage2D, PFNGLTEXSTORAGE2DPROC>(0, target, levels, internal_format, width, height);
}
void GL_APIENTRY timed_draw_arrays(GLenum mode, GLint first, GLsizei count) {
  timed_gl_call<GLWall::DrawArrays, PFNGLDRAWARRAYSPROC>(0, mode, first, count);
}
void GL_APIENTRY timed_draw_elements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
  timed_gl_call<GLWall::DrawElements, PFNGLDRAWELEMENTSPROC>(0, mode, count, type, indices);
}
void GL_APIENTRY timed_draw_arrays_instanced(GLenum mode, GLint first, GLsizei count, GLsizei instances) {
  timed_gl_call<GLWall::DrawArraysInstanced, PFNGLDRAWARRAYSINSTANCEDPROC>(0, mode, first, count, instances);
}
void GL_APIENTRY timed_draw_elements_instanced(GLenum mode, GLsizei count, GLenum type,
                                              const void* indices, GLsizei instances) {
  timed_gl_call<GLWall::DrawElementsInstanced, PFNGLDRAWELEMENTSINSTANCEDPROC>(0, mode, count, type, indices, instances);
}
// GLES3 headers stop at 3.0; Dawn may load this 3.2/desktop entry point by its core name.
using DrawElementsInstancedBaseVertexProc = void (GL_APIENTRY *)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLint);
void GL_APIENTRY timed_draw_elements_instanced_base_vertex(GLenum mode, GLsizei count, GLenum type,
                                                          const void* indices, GLsizei instances, GLint base_vertex) {
  timed_gl_call<GLWall::DrawElementsInstancedBaseVertex, DrawElementsInstancedBaseVertexProc>(
      0, mode, count, type, indices, instances, base_vertex);
}
void GL_APIENTRY timed_blit_framebuffer(GLint src_x0, GLint src_y0, GLint src_x1, GLint src_y1,
                                       GLint dst_x0, GLint dst_y0, GLint dst_x1, GLint dst_y1,
                                       GLbitfield mask, GLenum filter) {
  timed_gl_call<GLWall::BlitFramebuffer, PFNGLBLITFRAMEBUFFERPROC>(
      0, src_x0, src_y0, src_x1, src_y1, dst_x0, dst_y0, dst_x1, dst_y1, mask, filter);
}
void GL_APIENTRY timed_flush() {
  timed_gl_call<GLWall::Flush, PFNGLFLUSHPROC>(0);
}
GLenum GL_APIENTRY timed_get_error() {
  return timed_gl_call<GLWall::GetError, PFNGLGETERRORPROC>(0);
}
void GL_APIENTRY timed_uniform_1uiv(GLint location, GLsizei count, const GLuint* value) {
  timed_gl_call<GLWall::Uniform1uiv, PFNGLUNIFORM1UIVPROC>(0, location, count, value);
}
void GL_APIENTRY timed_use_program(GLuint program) {
  timed_gl_call<GLWall::UseProgram, PFNGLUSEPROGRAMPROC>(0, program);
}
void GL_APIENTRY timed_bind_buffer_range(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
  timed_gl_call<GLWall::BindBufferRange, PFNGLBINDBUFFERRANGEPROC>(
      requested_gl_buffer_bytes(size), target, index, buffer, offset, size);
}

template <typename GLProc>
dawn::native::opengl::EGLFunctionPointerType checked_gl_wrapper(GLProc wrapper) {
  return reinterpret_cast<dawn::native::opengl::EGLFunctionPointerType>(wrapper);
}
// Explicit parameter types also check every wrapper's GL calling ABI at compile time.
const std::array<dawn::native::opengl::EGLFunctionPointerType, GLWall::WrappedGLCount> TimedGLWrappers{
    checked_gl_wrapper<PFNGLMAPBUFFERRANGEPROC>(timed_map_buffer_range),
    checked_gl_wrapper<PFNGLUNMAPBUFFERPROC>(timed_unmap_buffer),
    checked_gl_wrapper<PFNGLBUFFERDATAPROC>(timed_buffer_data),
    checked_gl_wrapper<PFNGLBUFFERSUBDATAPROC>(timed_buffer_sub_data),
    checked_gl_wrapper<PFNGLCOPYBUFFERSUBDATAPROC>(timed_copy_buffer_sub_data),
    checked_gl_wrapper<PFNGLTEXSUBIMAGE2DPROC>(timed_tex_sub_image_2d),
    checked_gl_wrapper<PFNGLTEXIMAGE2DPROC>(timed_tex_image_2d),
    checked_gl_wrapper<PFNGLTEXSTORAGE2DPROC>(timed_tex_storage_2d),
    checked_gl_wrapper<PFNGLDRAWARRAYSPROC>(timed_draw_arrays),
    checked_gl_wrapper<PFNGLDRAWELEMENTSPROC>(timed_draw_elements),
    checked_gl_wrapper<PFNGLDRAWARRAYSINSTANCEDPROC>(timed_draw_arrays_instanced),
    checked_gl_wrapper<PFNGLDRAWELEMENTSINSTANCEDPROC>(timed_draw_elements_instanced),
    checked_gl_wrapper<DrawElementsInstancedBaseVertexProc>(timed_draw_elements_instanced_base_vertex),
    checked_gl_wrapper<PFNGLBLITFRAMEBUFFERPROC>(timed_blit_framebuffer),
    checked_gl_wrapper<PFNGLFLUSHPROC>(timed_flush),
    checked_gl_wrapper<PFNGLGETERRORPROC>(timed_get_error),
    checked_gl_wrapper<PFNGLUNIFORM1UIVPROC>(timed_uniform_1uiv),
    checked_gl_wrapper<PFNGLUSEPROGRAMPROC>(timed_use_program),
    checked_gl_wrapper<PFNGLBINDBUFFERRANGEPROC>(timed_bind_buffer_range)};
static_assert(TimedGLWrappers.size() == GLWall::MakeCurrent);
// EGL 1.5 and KHR have distinct typed entry points; never resolve one through the other's name.
const std::array<dawn::native::opengl::EGLFunctionPointerType, GLWall::DawnClientWaitFields.size()> TimedEGLWaitWrappers{
    checked_gl_wrapper<PFNEGLCLIENTWAITSYNCPROC>(timed_client_wait_sync),
    checked_gl_wrapper<PFNEGLCLIENTWAITSYNCKHRPROC>(timed_client_wait_sync_khr)};

void observation_failure(const char* reason) {
  if (backend->trustworthy) {
    Log.error("{}; context events={}/{}, contexts={}, bindings={}; teardown cannot be certified",
              reason, backend->events.size(), MaxContextEvents, backend->contexts.size(), backend->bindings.size());
  }
  backend->trustworthy = false;
  backend->state->fail(reason);
}
ContextRecord* live_context(EGLContext context) {
  if (context == EGL_NO_CONTEXT) return nullptr;
  for (auto it = backend->contexts.rbegin(); it != backend->contexts.rend(); ++it) {
    if (it->context == context && !it->destroyed) return &*it;
  }
  return nullptr;
}
void append_event(const ContextEvent& event) {
  if (!diagnostic::current()) return;
  if (backend->events.size() == MaxContextEvents) {
    observation_failure("H700 EGL context event trace exhausted");
  } else {
    backend->events.push_back(event);
  }
}
void retire_pending() {
  for (auto& context : backend->contexts) {
    if (context.destroy_requested && !context.destroyed &&
        std::none_of(backend->bindings.begin(), backend->bindings.end(), [&](const ContextBinding& binding) {
          return binding.context == context.context;
        })) context.destroyed = true;
  }
}
void record_binding(EGLContext context, EGLSurface draw, EGLSurface read) {
  const auto thread = std::this_thread::get_id();
  std::erase_if(backend->bindings, [&](const ContextBinding& binding) { return binding.thread == thread; });
  if (context != EGL_NO_CONTEXT) backend->bindings.push_back({thread, context, draw, read});
  retire_pending();
}
EGLContext EGLAPIENTRY observed_create(EGLDisplay display, EGLConfig config, EGLContext share, const EGLint* attrs) {
  const auto proc = reinterpret_cast<PFNEGLCREATECONTEXTPROC>(raw_proc("eglCreateContext"));
  const auto result = proc ? proc(display, config, share, attrs) : EGL_NO_CONTEXT;
  std::lock_guard lock(backend->observer_mutex);
  append_event({ContextEvent::Operation::Create, std::this_thread::get_id(), display, result, share,
                EGL_NO_SURFACE, EGL_NO_SURFACE, reinterpret_cast<uintptr_t>(result)});
  if (result != EGL_NO_CONTEXT) {
    auto* parent = live_context(share);
    if (display != backend->display || live_context(result) || (share != EGL_NO_CONTEXT && !parent)) {
      observation_failure("H700 EGL create returned an unexpected display, context or share group");
    }
    const auto group = parent ? parent->group : backend->contexts.size() + 1;
    backend->contexts.push_back({result, share, group});
  } else if (!proc) observation_failure("H700 EGL create entry point missing");
  // A real failed create owns no context. Dawn may attempt optional compiler contexts;
  // retain the zero return, but let RequestAdapter/RequestDevice decide bootstrap success.
  return result;
}
EGLBoolean EGLAPIENTRY observed_make_current(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context) {
  // Do not hold our lock during driver calls. Each thread serializes its own current binding;
  // successful destroy of another thread's current context only marks it pending below.
  const auto proc = backend->make_current;
  const auto result = !proc ? EGL_FALSE : backend->gl_timing_enabled
      ? timed_driver_call<GLWall::MakeCurrent>(proc, 0, display, draw, read, context)
      : proc(display, draw, read, context);
  std::lock_guard lock(backend->observer_mutex);
  append_event({ContextEvent::Operation::MakeCurrent, std::this_thread::get_id(), display, context, EGL_NO_CONTEXT,
                draw, read, result});
  if (result == EGL_TRUE) {
    if (display != backend->display ||
        (context != EGL_NO_CONTEXT && context != backend->presenter && !live_context(context)))
      observation_failure("H700 EGL make-current used an unexpected display or context");
    record_binding(context, draw, read);
  } else observation_failure("H700 EGL make-current failed");
  return result;
}
EGLBoolean EGLAPIENTRY observed_destroy(EGLDisplay display, EGLContext context) {
  const auto proc = reinterpret_cast<PFNEGLDESTROYCONTEXTPROC>(raw_proc("eglDestroyContext"));
  const auto result = proc ? proc(display, context) : EGL_FALSE;
  std::lock_guard lock(backend->observer_mutex);
  append_event({ContextEvent::Operation::Destroy, std::this_thread::get_id(), display, context, EGL_NO_CONTEXT,
                EGL_NO_SURFACE, EGL_NO_SURFACE, result});
  auto* record = live_context(context);
  if (result == EGL_TRUE && display == backend->display && record && !record->destroy_requested) {
    record->destroy_requested = true;
    retire_pending();
  } else observation_failure("H700 EGL destroy failed or targeted an unexpected context");
  return result;
}
EGLBoolean EGLAPIENTRY observed_release_thread() {
  const auto proc = reinterpret_cast<PFNEGLRELEASETHREADPROC>(raw_proc("eglReleaseThread"));
  const auto result = proc ? proc() : EGL_FALSE;
  std::lock_guard lock(backend->observer_mutex);
  append_event({ContextEvent::Operation::ReleaseThread, std::this_thread::get_id(), backend->display,
                EGL_NO_CONTEXT, EGL_NO_CONTEXT, EGL_NO_SURFACE, EGL_NO_SURFACE, result});
  if (result == EGL_TRUE) record_binding(EGL_NO_CONTEXT, EGL_NO_SURFACE, EGL_NO_SURFACE);
  else observation_failure("H700 EGL release-thread failed");
  return result;
}
__eglMustCastToProperFunctionPointerType EGLAPIENTRY observed_get_proc(const char* name) {
  return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(resolve(name));
}
bool contexts_retired() {
  std::lock_guard lock(backend->observer_mutex);
  return backend->trustworthy &&
         std::all_of(backend->contexts.begin(), backend->contexts.end(), [](const ContextRecord& c) { return c.destroyed; }) &&
         std::none_of(backend->bindings.begin(), backend->bindings.end(), [](const ContextBinding& b) {
           return b.context != backend->presenter;
         });
}
} // namespace

bool can_start_generation() { return !backend && allow_next_generation; }
void forbid_next_generation() { allow_next_generation = false; }
bool on_main_thread() { return main_thread(); }

bool prepare_sdl() {
  if (!can_start_generation()) return fail("H700 previous generation not cleanly released");
  if (!SDL_IsMainThread() || SDL_GetVersion() != SDL_VERSION) return fail("H700 requires SDL main thread/matched shim");
  // The pinned shim reads this with libc getenv(), not SDL_GetHint().
  if (::setenv("SDL_VIDEODRIVER", "sdl2", 1) != 0 || ::setenv("SDL3SHIM_SDL2_VIDEODRIVER", "mali", 1) != 0 ||
      !SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "sdl2", SDL_HINT_OVERRIDE)) {
    return fail("Cannot select SDL3-over-SDL2 mali video path");
  }
  return true;
}

bool initialize_presenter(SDL_Window* window) {
  if (!can_start_generation() || !SDL_IsMainThread() || !window || next_generation == 0)
    return fail("H700 generation cannot be retried");
  for (auto& retired : retired_flights) retired.reset();
  backend = std::make_unique<Backend>(next_generation++);
  backend->window = window;
  const char* driver = SDL_GetCurrentVideoDriver();
  if (!driver || std::string_view(driver) != "sdl2") return fail("H700 requires SDL shim video driver sdl2");
  backend->egl_library = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
  backend->gles_library = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
  if (!backend->egl_library || !backend->gles_library) return fail("Cannot retain firmware EGL/GLES libraries");
  backend->make_current = reinterpret_cast<PFNEGLMAKECURRENTPROC>(raw_proc("eglMakeCurrent"));
  backend->sdl_context = SDL_GL_CreateContext(window);
  if (!backend->sdl_context) return fail(std::string("Cannot create mali presenter: ") + SDL_GetError());
  backend->display = eglGetCurrentDisplay();
  backend->presenter = eglGetCurrentContext();
  backend->draw = eglGetCurrentSurface(EGL_DRAW);
  backend->read = eglGetCurrentSurface(EGL_READ);
  int width = 0, height = 0;
  if (backend->display == EGL_NO_DISPLAY || backend->presenter == EGL_NO_CONTEXT ||
      backend->draw == EGL_NO_SURFACE || backend->read == EGL_NO_SURFACE ||
      !SDL_GetWindowSizeInPixels(window, &width, &height) || width != NativeWidth || height != NativeHeight) {
    return fail("H700 requires real EGL display/context/surfaces and 640x480 drawable");
  }
  const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  if (!renderer || std::string_view(renderer).find("Mali-G31") == std::string_view::npos || !version ||
      !std::string_view(version).starts_with("OpenGL ES 3.")) return fail("H700 requires hardware Mali-G31 GLES3");
  const char* extensions = eglQueryString(backend->display, EGL_EXTENSIONS);
  const char* egl_version = eglQueryString(backend->display, EGL_VERSION);
  const bool egl15 = egl_version && std::string_view(egl_version).starts_with("1.5");
  if ((!egl15 && !has_extension(extensions, "EGL_EXT_create_context_robustness")) ||
      (!egl15 && !has_extension(extensions, "EGL_KHR_fence_sync")) ||
      !has_extension(extensions, "EGL_KHR_image_base") || !has_extension(extensions, "EGL_KHR_gl_texture_2D_image")) {
    return fail("H700 EGL lacks robustness/fence/image export extensions");
  }
  backend->create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(resolve("eglCreateImageKHR"));
  backend->destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(resolve("eglDestroyImageKHR"));
  if (!backend->create_image || !backend->destroy_image || !resolve("glEGLImageTargetTexture2DOES")) {
    return fail("H700 EGLImage entrypoints missing");
  }
  backend->create_sync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(resolve("eglCreateSyncKHR"));
  // Keep the presenter raw: its separately labelled probe must not also enter the Dawn wrapper.
  backend->client_wait_sync = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(raw_proc("eglClientWaitSyncKHR"));
  backend->destroy_sync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(resolve("eglDestroySyncKHR"));
  if (!backend->create_sync || !backend->client_wait_sync || !backend->destroy_sync) {
    return fail("H700 reverse fence entrypoints missing");
  }
  if (glGetError() != GL_NO_ERROR) return fail("GL error after H700 presenter bootstrap");
  for (auto& slot : backend->slots) {
    glGenTextures(1, &slot.source);
    glBindTexture(GL_TEXTURE_2D, slot.source);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, NativeWidth, NativeHeight);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &slot.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, slot.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.source, 0);
    const auto framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    const auto allocation_error = glGetError();
    if (!slot.source || !slot.framebuffer || framebuffer_status != GL_FRAMEBUFFER_COMPLETE ||
        allocation_error != GL_NO_ERROR) return fail("H700 native slot allocation/FBO failed");
    // Initialization only, not a per-frame synchronization or transport mechanism.
    glFinish();
    const EGLint attrs[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    slot.image = backend->create_image(backend->display, backend->presenter, EGL_GL_TEXTURE_2D_KHR,
                                     reinterpret_cast<EGLClientBuffer>(uintptr_t(slot.source)), attrs);
    if (slot.image == EGL_NO_IMAGE_KHR || glGetError() != GL_NO_ERROR) return fail("H700 EGLImage export failed");
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (!SDL_GL_SetSwapInterval(1)) return fail("Cannot set H700 presenter swap interval");
  backend->presenter_ready = true;
  Log.info("H700 generation {}: {} / {}, two permanent 640x480 RGBA8 exports", backend->generation, renderer, version);
  if (backend->gl_timing_enabled) {
    Log.info("H700 GL API wall enabled gen={} (MP_H700_GL_TIMING=1): Dawn-resolved GL/EGL ClientWait + "
             "observed eglMakeCurrent + progress/poll envelopes; presenter ClientWait counted separately; "
             "wait outcomes/timeout classes/flags are call subsets, envelopes overlap API wall; "
             "requested_buffer_bytes counts buffer range/allocation arguments only "
             "(BindBufferRange is not upload traffic), zero for other APIs; "
             "CPU wall, not GPU time", backend->generation);
  }
  return true;
}

std::shared_ptr<PresentationState> presentation_state() { return backend ? backend->state : nullptr; }
bool presenter_ready() { return backend && backend->presenter_ready && !backend->exports_released; }
bool imports_ready() {
  if (!presenter_ready() || !backend->imports_ready) return false;
  const auto phase = backend->state->snapshot().phase;
  return phase == PresentationPhase::Running || phase == PresentationPhase::Draining;
}
EGLDisplay external_display() { return backend ? backend->display : EGL_NO_DISPLAY; }

dawn::native::opengl::EGLFunctionPointerType resolve(const char* name) {
  if (!backend || !name) return nullptr;
  using Proc = dawn::native::opengl::EGLFunctionPointerType;
  if (std::strcmp(name, "eglCreateContext") == 0) return reinterpret_cast<Proc>(observed_create);
  if (std::strcmp(name, "eglMakeCurrent") == 0) return reinterpret_cast<Proc>(observed_make_current);
  if (std::strcmp(name, "eglDestroyContext") == 0) return reinterpret_cast<Proc>(observed_destroy);
  if (std::strcmp(name, "eglReleaseThread") == 0) return reinterpret_cast<Proc>(observed_release_thread);
  if (std::strcmp(name, "eglGetProcAddress") == 0) return reinterpret_cast<Proc>(observed_get_proc);
  if (backend->gl_timing_enabled) {
    for (size_t i = 0; i < TimedGLWrappers.size(); ++i) {
      if (std::strcmp(name, GLWall::Names[i]) != 0) continue;
      auto& cached = backend->gl_procs[i];
      std::call_once(cached.once, [&] { cached.original = raw_proc(name); });
      // A missing real function stays missing; never advertise a synthetic GL capability.
      return cached.original ? TimedGLWrappers[i] : nullptr;
    }
    for (size_t i = 0; i < TimedEGLWaitWrappers.size(); ++i) {
      if (std::strcmp(name, GLWall::Names[GLWall::DawnClientWaitFields[i]]) != 0) continue;
      auto& cached = backend->egl_wait_procs[i];
      std::call_once(cached.once, [&] { cached.original = raw_proc(name); });
      return cached.original ? TimedEGLWaitWrappers[i] : nullptr;
    }
  }
  return reinterpret_cast<Proc>(raw_proc(name));
}

bool begin_dawn() {
  if (!require_main() || !presenter_ready() || backend->dawn_attempted) return fail("H700 Dawn bootstrap is single-attempt");
  backend->dawn_attempted = true;
  backend->dawn_released = false;
  return true;
}

bool import_slots(const wgpu::Device& device) {
  if (!require_main() || !device || !presenter_ready() || !backend->dawn_attempted || backend->import_attempted) {
    return fail("H700 import requires one new live device; retry forbidden");
  }
  backend->import_attempted = true; // set BEFORE the first irreversible alias allocation
  for (auto& slot : backend->slots) {
    wgpu::TextureDescriptor descriptor{};
    descriptor.label = "H700 permanent native output";
    descriptor.dimension = wgpu::TextureDimension::e2D;
    descriptor.format = NativeFormat;
    descriptor.size = {NativeWidth, NativeHeight, 1};
    descriptor.mipLevelCount = 1;
    descriptor.sampleCount = 1;
    descriptor.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    dawn::native::opengl::ExternalImageDescriptorEGLImage external;
    const WGPUTextureDescriptor& c_descriptor = descriptor;
    external.cTextureDescriptor = &c_descriptor;
    external.isInitialized = false;
    external.image = slot.image;
    slot.texture = wgpu::Texture::Acquire(dawn::native::opengl::WrapExternalEGLImage(device.Get(), &external));
    if (!slot.texture) return fail("H700 EGLImage import failed; retire whole Dawn generation, do not retry");
    if (const auto observation = diagnostic::current()) {
      ++observation->imports;
      observation->storage(static_cast<unsigned>(&slot - backend->slots.data()), reinterpret_cast<uintptr_t>(slot.image),
          slot.source, slot.framebuffer, slot.texture.GetWidth(), slot.texture.GetHeight());
    }
    slot.view = slot.texture.CreateView();
    if (!slot.view) return fail("H700 imported texture view failed; retire whole Dawn generation");
    if (backend->state->snapshot().phase == PresentationPhase::Failed) return false;
  }
  backend->imports_ready = true;
  return restore_presenter();
}

const NativeSlot* native_slot(const FrameDescriptor& descriptor) {
  if (!imports_ready() || descriptor.generation != backend->generation || descriptor.slot >= backend->slots.size()) return nullptr;
  const auto ownership = backend->state->snapshot();
  const auto& current = ownership.slots[descriptor.slot].descriptor;
  if (!current || *current != descriptor) return nullptr;
  return &backend->slots[descriptor.slot];
}

bool restore_presenter() {
  if (!require_main() || !backend->sdl_context || backend->exports_released) return false;
  if (observed_make_current(backend->display, backend->draw, backend->read, backend->presenter) != EGL_TRUE ||
      eglGetCurrentDisplay() != backend->display || eglGetCurrentContext() != backend->presenter ||
      eglGetCurrentSurface(EGL_DRAW) != backend->draw || eglGetCurrentSurface(EGL_READ) != backend->read) {
    return fail("Cannot restore raw H700 presenter EGL binding");
  }
  return true;
}

namespace {
std::shared_ptr<NativeFlight> flight_for(const FrameDescriptor& descriptor) {
  if (!backend || descriptor.generation != backend->generation || descriptor.slot >= backend->flights.size()) return {};
  const auto ownership = backend->state->snapshot();
  const auto& current = ownership.slots[descriptor.slot].descriptor;
  if (!current || *current != descriptor) return {};
  std::lock_guard lock(backend->flights_mutex);
  const auto& flight = backend->flights[descriptor.slot];
  return flight && flight->descriptor == descriptor ? flight : nullptr;
}
void quarantine_frame(const FrameDescriptor& descriptor, std::string error) {
  if (!backend->state->quarantine(descriptor, error)) backend->state->fail(std::move(error));
}
std::array<std::shared_ptr<NativeFlight>, 2> flights_snapshot() {
  std::lock_guard lock(backend->flights_mutex);
  return backend->flights;
}
void poll_forward(const std::shared_ptr<NativeFlight>& flight) {
  if (!flight->published.load(std::memory_order_acquire) || flight->forward_observed || !flight->forward.id) return;
  wgpu::FutureWaitInfo info{.future = flight->forward};
  const auto wait = probe_wall_call(backend->gl_timing_enabled,
      [&] { return webgpu::g_instance.WaitAny(1, &info, 0); },
      [](uint64_t wall_ns, wgpu::WaitStatus result) {
        const auto outcome = result == wgpu::WaitStatus::TimedOut ? GLWall::WaitOutcome::TimedOut
            : result == wgpu::WaitStatus::Success ? GLWall::WaitOutcome::Signaled : GLWall::WaitOutcome::Other;
        backend->gl_wall.record_wait(GLWall::ForwardWaitAny, wall_ns, 0, 0, outcome);
      });
  if (wait == wgpu::WaitStatus::TimedOut) return;
  if (wait != wgpu::WaitStatus::Success || !info.completed || !flight->callback_called.load(std::memory_order_acquire)) {
    quarantine_frame(flight->descriptor, "H700 forward wait unresolved; future/callback retained");
    return;
  }
  flight->forward_observed = true;
  if (flight->status.load(std::memory_order_relaxed) != wgpu::QueueWorkDoneStatus::Success) {
    quarantine_frame(flight->descriptor, "H700 forward work-done wait/status failed");
    return;
  }
  flight->wall.completion_observed = FrameWallTimings::Clock::now();
  const auto ownership = backend->state->snapshot();
  const auto& slot = ownership.slots[flight->descriptor.slot];
  if (!slot.descriptor || *slot.descriptor != flight->descriptor) {
    backend->state->fail("H700 stale forward completion");
  } else if (slot.status == SlotStatus::Submitted &&
             !backend->state->complete_forward(flight->descriptor, flight->forward.id)) {
    quarantine_frame(flight->descriptor, "H700 forward transition failed");
  }
  // A quarantined publication is still observed and retained, never converted into Ready.
}
bool poll_diagnostic_pixels(const std::shared_ptr<NativeFlight>& flight, bool teardown = false) {
  if (!teardown && !flight->published.load(std::memory_order_acquire)) return false;
  if (!flight->diagnostic_buffer) return true;
  if (!flight->diagnostic_future.id) {
    if (teardown) { flight->diagnostic_buffer = {}; return true; }
    if (!flight->forward_observed || flight->status.load() != wgpu::QueueWorkDoneStatus::Success) return false;
    flight->diagnostic_map = std::make_shared<DiagnosticMap>();
    flight->diagnostic_future = flight->diagnostic_buffer.MapAsync(wgpu::MapMode::Read, 0,
        diagnostic::Points.size() * 256, wgpu::CallbackMode::WaitAnyOnly,
        [result = flight->diagnostic_map](wgpu::MapAsyncStatus status, wgpu::StringView) {
          result->status.store(status, std::memory_order_relaxed);
          result->called.store(true, std::memory_order_release);
        });
  }
  wgpu::FutureWaitInfo info{.future = flight->diagnostic_future};
  const auto wait = webgpu::g_instance.WaitAny(1, &info, 0);
  if (wait == wgpu::WaitStatus::TimedOut) return false;
  if (wait != wgpu::WaitStatus::Success || !info.completed || !flight->diagnostic_map->called.load(std::memory_order_acquire)) {
    quarantine_frame(flight->descriptor, "Diagnostic map callback ownership unresolved");
    return false;
  }
  if (flight->diagnostic_map->status.load() == wgpu::MapAsyncStatus::Success) {
    if (!teardown) {
      diagnostic::Pixels pixels{.frame = flight->descriptor.frame, .presenter = false};
      const auto* bytes = static_cast<const uint8_t*>(flight->diagnostic_buffer.GetConstMappedRange());
      if (!bytes) return fail("Diagnostic map returned no pixels");
      for (size_t i = 0; i < pixels.values.size(); ++i) std::memcpy(pixels.values[i].data(), bytes + i * 256, 4);
      if (const auto observation = diagnostic::current()) observation->pixels(pixels);
    }
    flight->diagnostic_buffer.Unmap();
  } else if (!teardown) {
    quarantine_frame(flight->descriptor, "Diagnostic map failed");
  }
  flight->diagnostic_buffer = {};
  flight->diagnostic_future = {}; // WaitAnyOnly callback has actually returned
  flight->diagnostic_map.reset();
  return true;
}
void recycle_safe(const std::shared_ptr<NativeFlight>& flight) {
  if (flight_for(flight->descriptor) != flight) { backend->state->fail("Stale safe-credit descriptor"); return; }
  const auto owned = backend->state->snapshot().slots[flight->descriptor.slot];
  if (owned.status != SlotStatus::Retiring || !owned.reverse_complete || !owned.swap_succeeded) return;
  if (const auto observation = diagnostic::current(); observation && observation->hold_credit(flight->descriptor.frame)) return;
  if (!backend->state->recycle(flight->descriptor)) {
    quarantine_frame(flight->descriptor, "H700 safe recycle transition failed");
    return;
  }
  std::lock_guard lock(backend->flights_mutex);
  if (backend->flights[flight->descriptor.slot] == flight) backend->flights[flight->descriptor.slot].reset();
}
void poll_reverse(const std::shared_ptr<NativeFlight>& flight) {
  if (flight->reverse == EGL_NO_SYNC_KHR) { recycle_safe(flight); return; }
  if (flight->reverse_wait_failed) return;
  // Both the generation/frame/slot and current ownership must match before touching its fence.
  if (flight_for(flight->descriptor) != flight) {
    flight->reverse_wait_failed = true;
    backend->state->fail("H700 stale reverse fence descriptor");
    return;
  }
  const auto before = backend->state->snapshot();
  const auto& owned = before.slots[flight->descriptor.slot];
  if ((owned.status == SlotStatus::Retiring && !owned.reverse) ||
      (owned.reverse && *owned.reverse != reinterpret_cast<uintptr_t>(flight->reverse))) {
    flight->reverse_wait_failed = true;
    quarantine_frame(flight->descriptor, "H700 reverse fence identity mismatch");
    return;
  }
  const auto result = probe_wall_call(backend->gl_timing_enabled,
      [&] { return backend->client_wait_sync(backend->display, flight->reverse, 0, 0); },
      [](uint64_t wall_ns, EGLint result) {
        backend->gl_wall.record_wait(GLWall::PresenterClientWaitSyncKHR, wall_ns, 0, 0, egl_wait_outcome(result));
      });
  if (result == EGL_TIMEOUT_EXPIRED_KHR) return;
  if (result != EGL_CONDITION_SATISFIED_KHR ||
      backend->destroy_sync(backend->display, flight->reverse) != EGL_TRUE) {
    flight->reverse_wait_failed = true;
    quarantine_frame(flight->descriptor, "H700 reverse fence wait/release failed");
    return; // keep the actual handle for IG-11, never reuse after a failed wait
  }
  flight->reverse = EGL_NO_SYNC_KHR;
  const auto ownership = backend->state->snapshot();
  const auto& slot = ownership.slots[flight->descriptor.slot];
  if (slot.status != SlotStatus::Retiring) return; // quarantine remains owned until generation teardown
  if (!slot.reverse || !backend->state->complete_reverse(flight->descriptor, *slot.reverse)) {
    quarantine_frame(flight->descriptor, "H700 reverse completion/recycle transition failed");
    return;
  }
  recycle_safe(flight);
}
void report_frame_wall(const NativeFlight& flight, FrameWallTimings::Clock::time_point present_begin,
                       FrameWallTimings::Clock::time_point swap_begin, FrameWallTimings::Clock::time_point swap_returned) {
  const auto now = FrameWallTimings::Clock::now();
  backend->frame_wall.record(flight.wall.sample(present_begin, swap_begin, swap_returned, now));
  if (now - backend->last_wall_report < std::chrono::seconds(2)) return;
  const auto api_samples = backend->gl_timing_enabled ? backend->gl_wall.sample_and_reset() : GLWall::Samples{};
  const auto mean = backend->frame_wall.mean();
  const auto& maximum = backend->frame_wall.maximum;
  using T = FrameWallTimings;
  Log.info("H700 frame wall gen={} last={} frames={} window_ms={:.1f} mean/max: "
           "prepare_ms={:.2f}/{:.2f} submit_ms={:.2f}/{:.2f} queue_ms={:.2f}/{:.2f} "
           "completion_observed_ms={:.2f}/{:.2f} present_ms={:.2f}/{:.2f} swap_ms={:.2f}/{:.2f}; "
           "overlapping wall envelopes, not GPU time",
           flight.descriptor.generation, flight.descriptor.frame, backend->frame_wall.frames,
           std::chrono::duration<double, std::milli>(now - backend->last_wall_report).count(),
           mean[T::Prepare], maximum[T::Prepare], mean[T::Submit], maximum[T::Submit],
           mean[T::Queue], maximum[T::Queue], mean[T::CompletionObserved], maximum[T::CompletionObserved],
           mean[T::Present], maximum[T::Present], mean[T::Swap], maximum[T::Swap]);
  if (backend->gl_timing_enabled) {
    Log.info("H700 GL API wall gen={} last={} window_ms={:.1f}; completed-call CPU wall, approximate report edges; "
             "thread sums overlap, not GPU time", flight.descriptor.generation, flight.descriptor.frame,
             std::chrono::duration<double, std::milli>(now - backend->last_wall_report).count());
    for (size_t i = 0; i < api_samples.size(); ++i) {
      const auto& sample = api_samples[i];
      if (sample.empty()) continue;
      Log.info("H700 GL API wall gen={} last={} api={} calls={} total_ns={} max_ns={} requested_buffer_bytes={}",
               flight.descriptor.generation, flight.descriptor.frame, GLWall::Names[i], sample.calls,
               sample.total_ns, sample.max_ns, sample.bytes);
      if (!sample.wait.empty()) {
        const auto& wait = sample.wait;
        Log.info("H700 wait outcomes gen={} last={} api={} timeout0={} timeout_nonzero={} flags_or={:#x} "
                 "flush_calls={} timed_out={} success_or_signaled={} other={}; subsets of API calls, not extra work",
                 flight.descriptor.generation, flight.descriptor.frame, GLWall::Names[i], wait.timeout_zero,
                 wait.timeout_nonzero, wait.flags_or, wait.flush_calls, wait.timed_out, wait.signaled, wait.other);
      }
    }
  }
  backend->frame_wall = {};
  backend->last_wall_report = now;
}
void present_ready() {
  if (!window::is_presentable()) return;
  if (const auto front = backend->state->front_submission()) {
    const auto flight = flight_for(front->descriptor);
    if (flight && flight->diagnostic_buffer) return; // real forward + map completion before reading/credit retirement
  }
  const auto descriptor = backend->state->try_begin_read();
  if (!descriptor) return;
  const auto flight = flight_for(*descriptor);
  const auto* slot = native_slot(*descriptor);
  const auto present_begin = FrameWallTimings::Clock::now();
  if (!flight || !slot || !restore_presenter()) {
    quarantine_frame(*descriptor, "H700 cannot acquire the matching native presenter target");
    return;
  }
  if (glGetError() != GL_NO_ERROR) {
    quarantine_frame(*descriptor, "H700 GL error before native blit");
    return;
  }
  if (const auto observation = diagnostic::current()) {
    // Initial dimensions were checked at import; no Dawn call may disturb this raw presenter binding.
    observation->storage(descriptor->slot,reinterpret_cast<uintptr_t>(slot->image),slot->source,slot->framebuffer,
                         NativeWidth,NativeHeight);
    if (!observation->healthy()) { quarantine_frame(*descriptor,"Diagnostic native storage changed"); return; }
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, slot->framebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    quarantine_frame(*descriptor, "H700 native read framebuffer incomplete");
    return;
  }
  // The measured Mali bridge maps GL row 0 to WebGPU's first row. Flip explicitly
  // into the bottom-origin default framebuffer; the IG-14 pixel oracle checks this path.
  glBlitFramebuffer(0, 0, NativeWidth, NativeHeight, 0, NativeHeight, NativeWidth, 0,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
  if (glGetError() != GL_NO_ERROR) {
    quarantine_frame(*descriptor, "H700 native blit failed");
    return;
  }
  if (const auto observation = diagnostic::current()) {
    diagnostic::Pixels pixels{.frame = descriptor->frame, .presenter = true};
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    for (size_t i = 0; i < diagnostic::Points.size(); ++i) {
      const auto point = diagnostic::Points[i];
      glReadPixels(point.x, NativeHeight - 1 - point.y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels.values[i].data());
    }
    if (glGetError() != GL_NO_ERROR) { quarantine_frame(*descriptor, "Diagnostic after-blit readback failed"); return; }
    observation->pixels(pixels); // owned data only, never a driver/user callback from progress
  }
  const EGLint attrs[] = {EGL_NONE};
  flight->reverse = backend->create_sync(backend->display, EGL_SYNC_FENCE_KHR, attrs);
  if (flight->reverse == EGL_NO_SYNC_KHR ||
      !backend->state->begin_retiring(*descriptor, reinterpret_cast<uintptr_t>(flight->reverse))) {
    quarantine_frame(*descriptor, "H700 reverse fence creation/transition failed");
    return;
  }
  glFlush(); // orders the presenter read before Dawn may write this slot again
  if (glGetError() != GL_NO_ERROR) {
    quarantine_frame(*descriptor, "H700 presenter flush failed");
    return;
  }
  // The pinned SDL2 shim returns true unconditionally; only the actual EGL return
  // can attest a successful swap. SDL still owns this display/surface unchanged.
  const auto swap_begin = FrameWallTimings::Clock::now();
  if (eglSwapBuffers(backend->display, backend->draw) != EGL_TRUE) {
    const EGLint error = eglGetError();
    quarantine_frame(*descriptor, fmt::format("H700 eglSwapBuffers failed: EGL error {:#x}", error));
    return;
  }
  const auto swap_returned = FrameWallTimings::Clock::now();
  if (!backend->state->complete_swap(*descriptor)) {
    quarantine_frame(*descriptor, "H700 swap completion transition failed");
    return;
  }
  report_frame_wall(*flight, present_begin, swap_begin, swap_returned);
  gfx::after_present(); // statistics only, never a game callback
}
} // namespace

std::optional<FrameDescriptor> acquire_frame() {
  if (!require_main() || !imports_ready() || backend->progressing) return std::nullopt;
  if (backend->next_frame == 2) if (const auto observation = diagnostic::current()) observation->third_acquire();
  while (backend->state->snapshot().phase == PresentationPhase::Running) {
    if (backend->next_frame == UINT64_MAX) {
      fail("H700 frame identity exhausted");
      return std::nullopt;
    }
    if (const auto descriptor = backend->state->try_reserve(backend->next_frame)) {
      auto flight = std::make_shared<NativeFlight>(*descriptor);
      {
        std::lock_guard lock(backend->flights_mutex);
        if (backend->flights[descriptor->slot]) {
          backend->state->fail("H700 native transport record would be overwritten");
          (void)backend->state->abort_reservation(*descriptor);
          return std::nullopt;
        }
        backend->flights[descriptor->slot] = std::move(flight);
      }
      ++backend->next_frame;
      return descriptor;
    }
    if (!main_progress_once()) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return std::nullopt;
}

bool abort_frame(const FrameDescriptor& descriptor) {
  const auto flight = flight_for(descriptor);
  if (!flight || flight->published.load(std::memory_order_acquire)) return false;
  if (!backend->state->abort_reservation(descriptor)) return false;
  std::lock_guard lock(backend->flights_mutex);
  if (backend->flights[descriptor.slot] == flight) backend->flights[descriptor.slot].reset();
  return true;
}

bool diagnostic_copy_native(const FrameDescriptor& descriptor, const wgpu::CommandEncoder& encoder) {
  const auto observation = diagnostic::current();
  if (!observation) return true;
  const auto flight = flight_for(descriptor);
  const auto* slot = native_slot(descriptor);
  if (!flight || !slot || flight->diagnostic_buffer) return fail("Diagnostic native copy descriptor mismatch");
  const wgpu::BufferDescriptor desc{.label = "IG12 native samples", .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
                                    .size = diagnostic::Points.size() * 256};
  flight->diagnostic_buffer = diagnostic::dawn_call(diagnostic::Kind::ResourceBuffer, descriptor.frame,
      [&] { return webgpu::g_device.CreateBuffer(&desc); });
  if (!flight->diagnostic_buffer) return fail("Cannot create diagnostic sample buffer");
  for (size_t i = 0; i < diagnostic::Points.size(); ++i) {
    const auto point = diagnostic::Points[i];
    const wgpu::TexelCopyTextureInfo from{.texture = slot->texture, .origin = {point.x, point.y, 0}};
    const wgpu::TexelCopyBufferInfo to{.layout = {.offset = i * 256, .bytesPerRow = 256, .rowsPerImage = 1},
                                     .buffer = flight->diagnostic_buffer};
    const wgpu::Extent3D size{1, 1, 1};
    encoder.CopyTextureToBuffer(&from, &to, &size);
  }
  return true;
}

bool submit_frame(const FrameDescriptor& descriptor, const wgpu::CommandBuffer& commands) {
  const auto flight = flight_for(descriptor);
  if (!flight || !commands || !gfx::render_worker::is_worker_thread()) {
    if (backend) {
      (void)abort_frame(descriptor);
      backend->state->fail("H700 invalid native render submission");
    }
    return false;
  }
  if (!backend->state->mark_submitted(descriptor)) {
    (void)abort_frame(descriptor);
    backend->state->fail("H700 native Submit boundary rejected");
    return false;
  }
  flight->wall.submit_begin = FrameWallTimings::Clock::now();
  webgpu::g_queue.Submit(1, &commands); // irreversible boundary: no rollback after mark_submitted
  flight->wall.queue_returned = FrameWallTimings::Clock::now();
  flight->forward = webgpu::g_queue.OnSubmittedWorkDone(
      wgpu::CallbackMode::WaitAnyOnly,
      [flight](wgpu::QueueWorkDoneStatus status, wgpu::StringView) {
        flight->status.store(status, std::memory_order_relaxed);
        flight->callback_called.store(true, std::memory_order_release);
      });
  if (const auto observation = diagnostic::current()) observation->event(diagnostic::Kind::Submitted, descriptor.frame, false);
  const bool published = backend->state->publish_submitted(descriptor, flight->forward.id);
  flight->wall.submit_returned = FrameWallTimings::Clock::now();
  flight->published.store(true, std::memory_order_release);
  // Even after synchronous loss/error, the real future stays published/owned for main/IG-11.
  if (!published || !flight->forward.id || backend->state->snapshot().phase == PresentationPhase::Failed) {
    quarantine_frame(descriptor, "H700 Submit/work-done publication failed");
    return false;
  }
  return true;
}

bool main_progress_once() {
  if (!require_main()) return false;
  if (backend->progressing) return fail("H700 recursive main progress");
  backend->progressing = true;
  struct ProgressScope { ~ProgressScope() { backend->progressing = false; } } scope;
  if (!webgpu::g_instance) return false;
  if (const auto observation = diagnostic::current()) {
    observation->progress();
    if (observation->failed.load()) backend->state->fail("IG12 diagnostic failure/deadline; own gates released");
  }
  // Direct calls, in the same order: never enqueue behind the worker we may be waiting for.
  timed_progress_call<GLWall::ProcessEvents>([] { webgpu::g_instance.ProcessEvents(); });
  timed_progress_call<GLWall::DeviceLostPoll>([] { webgpu::poll_h700_device_events(); });
  if (backend->state->snapshot().phase == PresentationPhase::Failed)
    if (const auto observation = diagnostic::current()) observation->cancel(); // release own gates on real Dawn failure too
  const auto flights = flights_snapshot();
  for (const auto& flight : flights) if (flight) poll_forward(flight);
  if (backend->state->snapshot().phase != PresentationPhase::Failed)
    for (const auto& flight : flights) if (flight) (void)poll_diagnostic_pixels(flight);
  for (const auto& flight : flights) if (flight)
    timed_progress_call<GLWall::ReversePoll>([&] { poll_reverse(flight); });
  present_ready(); // PresentationState forbids new reads in Failed, but retirement above continues
  for (const auto& flight : flights_snapshot()) if (flight)
    timed_progress_call<GLWall::ReversePoll>([&] { poll_reverse(flight); });
  const auto phase = backend->state->snapshot().phase;
  return phase == PresentationPhase::Running || phase == PresentationPhase::Draining;
}

std::array<std::optional<NativePending>, 2> pending_native_frames() {
  std::array<std::optional<NativePending>, 2> result;
  if (!backend || !require_main()) return result;
  const auto flights = flights_snapshot();
  for (size_t i = 0; i < flights.size(); ++i) {
    if (const auto& flight = flights[i]) {
      const bool published = flight->published.load(std::memory_order_acquire);
      result[i].emplace(NativePending{flight->descriptor, published ? flight->forward : wgpu::Future{}, published,
                                     flight->forward_observed, flight->callback_called.load(std::memory_order_acquire),
                                     flight->status.load(std::memory_order_relaxed), flight->reverse, flight->reverse_wait_failed});
    }
  }
  return result;
}

ContextSnapshot context_snapshot() {
  if (!backend) return {{}, {}, {}, true};
  std::lock_guard lock(backend->observer_mutex);
  return {backend->contexts, backend->bindings, backend->events, backend->trustworthy};
}

bool unbind_current_dawn() {
  if (!backend) return true;
  const auto current = eglGetCurrentContext();
  {
    std::lock_guard lock(backend->observer_mutex);
    if (current == EGL_NO_CONTEXT || current == backend->presenter) return true;
    if (!live_context(current)) return fail("Cannot unbind unobserved EGL context");
  }
  return observed_make_current(backend->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
}

bool native_frames_drained() {
  if (!backend) return true;
  if (!require_main()) return false;
  const auto flights = flights_snapshot();
  return std::none_of(flights.begin(), flights.end(), [](const auto& flight) { return bool(flight); });
}

bool settle_native_callbacks() {
  if (!backend) return true;
  if (!require_main()) return false;
  bool settled = true;
  for (const auto& flight : flights_snapshot()) {
    if (!flight) continue;
    if (!poll_diagnostic_pixels(flight, true)) settled = false;
    if (!flight->published.load(std::memory_order_acquire)) continue;
    if (flight->callback_called.load(std::memory_order_acquire)) continue;
    if (!webgpu::g_instance || !flight->forward.id) {
      fail("Cannot settle native work-done callback; retaining generation/future");
      return false;
    }
    wgpu::FutureWaitInfo info{.future = flight->forward};
    const auto wait = webgpu::g_instance.WaitAny(1, &info, 0);
    if (wait != wgpu::WaitStatus::Success || !info.completed || !flight->callback_called.load(std::memory_order_acquire))
      settled = false;
    // A failed work-done status never becomes a successful forward event or a reusable credit.
  }
  return settled;
}

std::array<std::optional<NativePending>, 2> retired_native_frames() {
  std::array<std::optional<NativePending>, 2> result;
  for (size_t i = 0; i < result.size(); ++i) if (retired_flights[i]) result[i].emplace(*retired_flights[i]);
  return result;
}

bool retire_native_after_dawn_release() {
  if (!backend) return true;
  if (!require_main() || !backend->dawn_released || !contexts_retired()) return false;
  const auto flights = flights_snapshot();
  if (std::none_of(flights.begin(), flights.end(), [](const auto& flight) { return bool(flight); })) return true;
  if (backend->state->snapshot().phase != PresentationPhase::Failed) return false;
  for (const auto& flight : flights)
    if (flight && flight->published.load(std::memory_order_acquire) &&
        !flight->callback_called.load(std::memory_order_acquire)) return false;
  if (!restore_presenter()) return false;
  // Failure teardown only, never per-frame: settle presenter reads even if their fence/swap
  // failed before acknowledgement. Whole Dawn share-group destruction is proven above.
  if (glGetError() != GL_NO_ERROR) return fail("GL error before failed-generation presenter retirement");
  glFinish();
  if (glGetError() != GL_NO_ERROR) return fail("Presenter teardown completion unknown; exports retained");
  for (const auto& flight : flights) {
    if (!flight) continue;
    const auto retired_reverse = flight->reverse;
    if (flight->reverse != EGL_NO_SYNC_KHR) {
      if (backend->destroy_sync(backend->display, flight->reverse) != EGL_TRUE)
        return fail("Failed-generation reverse sync release unknown; exports retained");
      flight->reverse = EGL_NO_SYNC_KHR;
    }
    retired_flights[flight->descriptor.slot].emplace(NativePending{
        flight->descriptor, flight->forward, flight->published.load(std::memory_order_acquire), flight->forward_observed,
        flight->callback_called.load(std::memory_order_acquire), flight->status.load(std::memory_order_relaxed),
        retired_reverse, flight->reverse_wait_failed, true});
    std::lock_guard lock{backend->flights_mutex};
    if (backend->flights[flight->descriptor.slot] == flight) backend->flights[flight->descriptor.slot].reset();
  }
  // PresentationState retains quarantined slot/future/mailbox metadata; no recycle/complete_swap.
  return true;
}

bool release_dawn_imports() {
  if (!backend) return true;
  if (!require_main()) return false;
  backend->state->begin_draining();
  backend->imports_ready = false;
  for (auto& slot : backend->slots) {
    slot.view = {};
    slot.texture = {};
  }
  return true;
}

bool acknowledge_dawn_release() {
  if (!backend) return true;
  if (!require_main()) return false;
  const bool importsRemaining = std::any_of(backend->slots.begin(), backend->slots.end(), [](const NativeSlot& s) {
    return bool(s.texture) || bool(s.view);
  });
  if (importsRemaining || !contexts_retired()) {
    {
      std::lock_guard lock(backend->observer_mutex);
      const auto liveContexts = std::count_if(backend->contexts.begin(), backend->contexts.end(), [](const ContextRecord& c) {
        return !c.destroyed;
      });
      const auto dawnBindings = std::count_if(backend->bindings.begin(), backend->bindings.end(), [](const ContextBinding& b) {
        return b.context != backend->presenter;
      });
      Log.error("H700 Dawn release proof: observer trustworthy={}, context events={}/{}, live contexts={}, "
                "Dawn bindings={}, imports remaining={}", backend->trustworthy, backend->events.size(), MaxContextEvents,
                liveContexts, dawnBindings, importsRemaining);
    }
    return fail("Dawn release incomplete: imports, contexts or current bindings remain");
  }
  backend->dawn_released = true;
  return true;
}

bool release_exports() {
  if (!backend) return true;
  if (!require_main()) return false;
  if (backend->exports_released) return true;
  if (!backend->dawn_released || !contexts_retired()) return fail("Refusing exports release before entire Dawn share group");
  {
    std::lock_guard lock(backend->flights_mutex);
    if (std::any_of(backend->flights.begin(), backend->flights.end(), [](const auto& flight) { return bool(flight); })) {
      return fail("Refusing exports release with pending native ownership/futures/reverse fences (IG-11 retirement required)");
    }
  }
  const bool has_exports = std::any_of(backend->slots.begin(), backend->slots.end(), [](const NativeSlot& s) {
    return s.source || s.framebuffer || s.image != EGL_NO_IMAGE_KHR;
  });
  if (has_exports) {
    if (!restore_presenter()) return false;
    bool ok = true;
    if (glGetError() != GL_NO_ERROR) {
      fail("GL error before H700 exports release");
      ok = false;
    }
    for (auto& slot : backend->slots) {
      if (slot.framebuffer) {
        glDeleteFramebuffers(1, &slot.framebuffer);
        if (glGetError() != GL_NO_ERROR) {
          fail("H700 FBO release failed");
          ok = false;
          continue;
        }
        slot.framebuffer = 0;
      }
      if (slot.image != EGL_NO_IMAGE_KHR) {
        if (backend->destroy_image(backend->display, slot.image) != EGL_TRUE) {
          fail("H700 EGLImage release failed");
          ok = false;
          continue;
        }
        slot.image = EGL_NO_IMAGE_KHR;
      }
      if (slot.source) {
        glDeleteTextures(1, &slot.source);
        if (glGetError() != GL_NO_ERROR) {
          fail("H700 source texture release failed");
          ok = false;
        } else slot.source = 0;
      }
    }
    if (!ok) return false;
  }
  backend->exports_released = true;
  backend->presenter_ready = false;
  return true;
}

bool prepare_window_destruction() {
  if (!backend) return true;
  if (!require_main() || !release_exports()) return false;
  if (backend->window_release_ready) return true;
  if (backend->display != EGL_NO_DISPLAY &&
      observed_make_current(backend->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
    return fail("Cannot unbind presenter before SDL window destruction");
  }
  // Pinned SDL2 mali destroys its GL context with the window. Explicit SDL_GL_DestroyContext
  // would double-delete it. Only clear SDL's stale TLS after a real EGL unbind.
  if (!SDL_GL_MakeCurrent(nullptr, nullptr) || eglGetCurrentContext() != EGL_NO_CONTEXT) {
    return fail("Cannot clear SDL GL binding before window destruction");
  }
  backend->window_release_ready = true;
  return true;
}

bool finish_shutdown() {
  if (!backend) return true;
  if (!require_main() || !backend->window_release_ready || !backend->dawn_released || !contexts_retired()) {
    return fail("H700 shutdown incomplete; retaining resolver/libraries and forbidding next generation");
  }
  if (SDL_WasInit(SDL_INIT_VIDEO) != 0) return fail("H700 resolver release must follow SDL video shutdown");
  if (!backend->exports_released || !native_frames_drained() || eglGetCurrentContext() != EGL_NO_CONTEXT)
    return fail("H700 final ownership/binding proof incomplete");
  const auto ownership = backend->state->snapshot();
  if (ownership.phase != PresentationPhase::Failed &&
      (ownership.mailbox_size || std::any_of(ownership.slots.begin(), ownership.slots.end(),
                                            [](const auto& slot) { return slot.status != SlotStatus::Free; })))
    return fail("H700 logical ownership remains at final resolver boundary");
  bool ok = true;
  if (backend->gles_library) {
    if (dlclose(backend->gles_library) != 0) ok = false;
    else backend->gles_library = nullptr;
  }
  if (backend->egl_library) {
    if (dlclose(backend->egl_library) != 0) ok = false;
    else backend->egl_library = nullptr;
  }
  if (!ok) return fail("H700 runtime library release failed");
  if (backend->state->snapshot().phase == PresentationPhase::Failed) {
    allow_next_generation = false;
    if (!backend->state->stop_after_teardown()) return false;
  } else if (!backend->state->try_stop()) {
    return fail("H700 presentation ownership not stopped after teardown");
  }
  backend.reset();
  return true;
}
} // namespace aurora::h700
