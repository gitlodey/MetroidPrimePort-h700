#include "pattern.hpp"

#include <SDL3/SDL.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <dawn/native/DawnNative.h>
#include <dawn/native/OpenGLBackend.h>
#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
using namespace h700_probe;
using Clock = std::chrono::steady_clock;
constexpr uint64_t kWaitNs = 5'000'000'000ULL;

void require(bool ok, std::string_view message) {
  if (!ok) throw std::runtime_error(std::string(message));
}

std::string text(wgpu::StringView value) {
  return std::string(std::string_view(value));
}

bool extension(const char* list, std::string_view name) {
  if (!list) return false;
  std::string_view rest(list);
  while (!rest.empty()) {
    auto end = rest.find(' ');
    if (rest.substr(0, end) == name) return true;
    if (end == std::string_view::npos) break;
    rest.remove_prefix(end + 1);
  }
  return false;
}

struct Options {
  std::string mode;
  unsigned frames = 0;
  bool help = false;
  bool self_test = false;
};

Options options(int argc, char** argv) {
  Options out;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--help" && argc == 2) out.help = true;
    else if (arg == "--self-test" && argc == 2) out.self_test = true;
    else if (arg == "--mode" && i + 1 < argc && out.mode.empty()) out.mode = argv[++i];
    else if (arg == "--frames" && i + 1 < argc && out.frames == 0) {
      std::string_view value(argv[++i]);
      auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), out.frames);
      require(error == std::errc{} && end == value.data() + value.size() &&
                  out.frames >= 2 && out.frames <= 120,
              "--frames must be an integer from 2 to 120");
    } else throw std::runtime_error("Unknown, duplicated or incomplete argument");
  }
  if (out.help || out.self_test) return out;
  require(out.mode == "offscreen" || out.mode == "bridge",
          "Choose --mode offscreen or --mode bridge explicitly; both initialize SDL video");
  if (!out.frames) out.frames = out.mode == "bridge" ? 30 : 2;
  return out;
}

void help() {
  std::cout << "Usage: h700_graphics_probe --help | --self-test\n"
               "       h700_graphics_probe --mode offscreen|bridge [--frames 2..120]\n"
               "--help/--self-test never initialize SDL/EGL/Dawn.\n"
               "BOTH graphics modes create a 640x480 SDL2/Mali window and can occupy the screen.\n"
               "No game/assets/settings/saves/network; no clocks or firmware changes.\n"
               "Run graphics modes only in an agreed free device session, with an external timeout.\n"
               "Offscreen: WGSL pattern + GPU readback. Bridge: one persistent EGLImage, serial access.\n"
               "Not a game renderer test, threading stress test or FPS benchmark.\n";
}

void self_test() {
  require(extension("EGL_A EGL_B EGL_B_long", "EGL_B"), "extension token match");
  require(!extension("EGL_B_long EGL_AB", "EGL_B"), "extension substring rejection");
  require(!extension(nullptr, "EGL_A"), "empty extension list");
  require(expected_pixel(0, 0, 0) == Pixel{255, 255, 255, 255}, "white marker");
  require(expected_pixel(7, 7, 1) == Pixel{255, 0, 255, 255}, "changing marker");
  require(expected_pixel(8, 8, 0) == Pixel{255, 0, 0, 255}, "marker boundary");
  require(expected_pixel(32, 0, 0) == Pixel{0, 255, 0, 255}, "right boundary");
  require(expected_pixel(0, 32, 0) == Pixel{0, 0, 255, 255}, "lower boundary");
  require(expected_pixel(63, 63, 0) == Pixel{255, 255, 0, 255}, "yellow quadrant");
  require(pixel_matches({254, 1, 0, 255}, {255, 0, 0, 255}), "pixel tolerance");
  require(!pixel_matches({253, 0, 0, 255}, {255, 0, 0, 255}), "pixel mismatch");
  require(!pixel_matches({255, 0, 0, 0}, {255, 0, 0, 255}), "alpha mismatch");
  char name[] = "probe", mode[] = "--mode", offscreen[] = "offscreen";
  char count[] = "--frames", two[] = "2", bad[] = "2junk";
  char* valid[] = {name, mode, offscreen, count, two};
  require(options(5, valid).frames == 2, "valid options");
  char* invalid[] = {name, mode, offscreen, count, bad};
  bool rejected = false;
  try { (void)options(5, invalid); } catch (const std::exception&) { rejected = true; }
  require(rejected, "invalid numeric argument");
  rejected = false;
  try { (void)options(1, valid); } catch (const std::exception&) { rejected = true; }
  require(rejected, "no accidental graphics startup");
  std::cout << "PASS self-test (CPU-only pattern, extension and argument checks; no SDL/GPU)\n";
}

struct Diagnostics {
  std::atomic<unsigned> errors{0};
  std::atomic<bool> destroying{false};
};

struct Platform {
  static inline Platform* active = nullptr;
  SDL_Window* window = nullptr;
  SDL_GLContext context = nullptr;
  void* egl_library = nullptr;
  void* gles_library = nullptr;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext presenter = EGL_NO_CONTEXT;
  EGLSurface draw_surface = EGL_NO_SURFACE;
  EGLSurface read_surface = EGL_NO_SURFACE;
  GLuint texture = 0;
  GLuint framebuffer = 0;
  EGLImageKHR image = EGL_NO_IMAGE_KHR;
  PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
  bool sdl_started = false;
  int width = 0;
  int height = 0;

  static dawn::native::opengl::EGLFunctionPointerType resolve(const char* name) {
    if (!active) return nullptr;
    void* proc = dlsym(active->egl_library, name);
    if (!proc) proc = dlsym(active->gles_library, name);
    if (proc) return reinterpret_cast<dawn::native::opengl::EGLFunctionPointerType>(proc);
    return reinterpret_cast<dawn::native::opengl::EGLFunctionPointerType>(SDL_GL_GetProcAddress(name));
  }

  bool make_current() const {
    // Dawn switches EGL directly, leaving SDL's TLS cache pointing at presenter.
    // SDL_GL_MakeCurrent can incorrectly short-circuit on that cached identity.
    return context && eglMakeCurrent(display, draw_surface, read_surface, presenter) == EGL_TRUE &&
           eglGetCurrentContext() == presenter &&
           eglGetCurrentSurface(EGL_DRAW) == draw_surface &&
           eglGetCurrentSurface(EGL_READ) == read_surface;
  }

  void init() {
    require(SDL_GetVersion() == SDL_VERSION, "SDL runtime/header version mismatch");
    require(SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1"), "SDL signal hint failed");
    require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
    sdl_started = true;
    const char* driver = SDL_GetCurrentVideoDriver();
    require(driver && std::string_view(driver) == "sdl2", "Expected SDL3 shim driver sdl2");
    require(SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES) &&
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3) &&
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0) &&
                SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1), SDL_GetError());
    SDL_PropertiesID props = SDL_CreateProperties();
    require(props != 0, "SDL properties allocation failed");
    bool ok = SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "H700 Dawn probe (no game)") &&
              SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 640) &&
              SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 480) &&
              SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true) &&
              SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
    if (ok) window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    require(window != nullptr, SDL_GetError());
    context = SDL_GL_CreateContext(window);
    require(context != nullptr, SDL_GetError());
    display = eglGetCurrentDisplay();
    presenter = eglGetCurrentContext();
    draw_surface = eglGetCurrentSurface(EGL_DRAW);
    read_surface = eglGetCurrentSurface(EGL_READ);
    require(display != EGL_NO_DISPLAY && presenter != EGL_NO_CONTEXT, "No firmware EGL display/context");
    require(SDL_GetWindowSizeInPixels(window, &width, &height) && width > 0 && height > 0,
            "Cannot obtain drawable size");
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    require(renderer && std::string_view(renderer).find("Mali-G31") != std::string_view::npos,
            "Expected firmware Mali-G31, not a software/other renderer");
    require(version != nullptr, "Cannot query firmware GL version");
    std::cout << "BOOTSTRAP SDL=" << SDL_GetVersion() << " renderer=" << renderer
              << " GL=" << version << " drawable=" << width << "x" << height << '\n';
    const char* exts = eglQueryString(display, EGL_EXTENSIONS);
    const char* egl_version = eglQueryString(display, EGL_VERSION);
    std::cout << "EGL version=" << (egl_version ? egl_version : "unknown") << '\n';
    require(extension(exts, "EGL_EXT_create_context_robustness") ||
                (egl_version && std::string_view(egl_version).starts_with("1.5")),
            "Dawn requires EGL context robustness");
    require(extension(exts, "EGL_KHR_fence_sync") ||
                (egl_version && std::string_view(egl_version).starts_with("1.5")),
            "Conservative probe requires fence sync (reusable-only is not sufficient)");
    egl_library = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    gles_library = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    require(egl_library && gles_library, "Cannot open firmware EGL/GLES runtime sonames");
    active = this;
    require(glGetError() == GL_NO_ERROR, "GL error after SDL bootstrap");
  }

  void create_image_storage() {
    require(make_current(), "Cannot restore presenter for image creation");
    const char* exts = eglQueryString(display, EGL_EXTENSIONS);
    require(extension(exts, "EGL_KHR_image_base") && extension(exts, "EGL_KHR_gl_texture_2D_image"),
            "Firmware lacks EGLImage texture export extensions");
    create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(resolve("eglCreateImageKHR"));
    destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(resolve("eglDestroyImageKHR"));
    require(create_image && destroy_image && resolve("glEGLImageTargetTexture2DOES"),
            "Missing EGLImage entrypoints");
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kSize, kSize);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Presenter FBO incomplete");
    require(glGetError() == GL_NO_ERROR, "Presenter texture allocation failed");
    glFinish();
    const EGLint attrs[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    image = create_image(display, presenter, EGL_GL_TEXTURE_2D_KHR,
                         reinterpret_cast<EGLClientBuffer>(uintptr_t(texture)), attrs);
    require(image != EGL_NO_IMAGE_KHR, "eglCreateImageKHR failed");
    require(glGetError() == GL_NO_ERROR, "GL error creating EGLImage");
  }

  bool cleanup() noexcept {
    bool ok = true;
    if (context) {
      if (make_current()) {
        if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
        if (image != EGL_NO_IMAGE_KHR && destroy_image) {
          if (!destroy_image(display, image)) ok = false;
        }
        if (texture) glDeleteTextures(1, &texture);
        if (glGetError() != GL_NO_ERROR) ok = false;
      } else ok = false;
      framebuffer = 0;
      texture = 0;
      image = EGL_NO_IMAGE_KHR;
      // This pinned SDL2 backend owns the context through its window data;
      // SDL_GL_DestroyContext would leave that handle for a second deletion.
      if (display != EGL_NO_DISPLAY &&
          eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) ok = false;
      if (!SDL_GL_MakeCurrent(nullptr, nullptr) || eglGetCurrentContext() != EGL_NO_CONTEXT) ok = false;
      context = nullptr;
    }
    if (window) { SDL_DestroyWindow(window); window = nullptr; }
    if (sdl_started) { SDL_Quit(); sdl_started = false; }
    active = nullptr;
    if (gles_library) { dlclose(gles_library); gles_library = nullptr; }
    if (egl_library) { dlclose(egl_library); egl_library = nullptr; }
    return ok;
  }
  ~Platform() { (void)cleanup(); }
};

struct Core {
  std::shared_ptr<Diagnostics> diagnostics = std::make_shared<Diagnostics>();
  wgpu::Instance instance;
  wgpu::Adapter adapter;
  wgpu::Device device;
  wgpu::Queue queue;
  EGLContext observed_context = EGL_NO_CONTEXT;

  void wait(wgpu::Future future) {
    wgpu::FutureWaitInfo info{future, false};
    auto status = instance.WaitAny(1, &info, kWaitNs);
    require(status == wgpu::WaitStatus::Success && info.completed, "Dawn future wait failed or timed out");
    require(diagnostics->errors.load() == 0, "Dawn reported validation/device errors");
  }

  void completed() {
    struct State { bool called = false; wgpu::QueueWorkDoneStatus status{}; };
    auto state = std::make_shared<State>();
    auto future = queue.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly,
      [state](wgpu::QueueWorkDoneStatus status, wgpu::StringView message) {
        state->called = true;
        state->status = status;
        if (status != wgpu::QueueWorkDoneStatus::Success) std::cerr << "QUEUE " << text(message) << '\n';
      });
    wait(future);
    require(state->called && state->status == wgpu::QueueWorkDoneStatus::Success,
            "Submitted GPU work did not complete successfully");
  }

  void init() {

    dawn::native::DawnInstanceDescriptor native;
    native.backendValidationLevel = dawn::native::BackendValidationLevel::Full;
    const wgpu::InstanceFeatureName features[] = {wgpu::InstanceFeatureName::TimedWaitAny};
    wgpu::InstanceDescriptor descriptor{};
    descriptor.nextInChain = &native;
    descriptor.requiredFeatureCount = 1;
    descriptor.requiredFeatures = features;
    instance = wgpu::CreateInstance(&descriptor);
    require(bool(instance), "Cannot create Dawn instance with TimedWaitAny");
    dawn::native::opengl::RequestAdapterOptionsGetGLProc egl_options;
    egl_options.getProc = Platform::resolve;
    egl_options.display = Platform::active->display;
    wgpu::RequestAdapterOptions opts{};
    opts.nextInChain = &egl_options;
    opts.featureLevel = wgpu::FeatureLevel::Compatibility;
    opts.backendType = wgpu::BackendType::OpenGLES;
    struct AdapterState { wgpu::Adapter value; bool ok = false; };
    auto adapter_state = std::make_shared<AdapterState>();
    auto future = instance.RequestAdapter(&opts, wgpu::CallbackMode::WaitAnyOnly,
      [adapter_state](wgpu::RequestAdapterStatus status, wgpu::Adapter value, wgpu::StringView message) {
        adapter_state->ok = status == wgpu::RequestAdapterStatus::Success;
        adapter_state->value = std::move(value);
        if (!adapter_state->ok) std::cerr << "ADAPTER " << text(message) << '\n';
      });
    wait(future);
    adapter = std::move(adapter_state->value);
    require(adapter_state->ok && bool(adapter), "Cannot obtain hardware OpenGLES Compatibility adapter");
    wgpu::AdapterInfo info{};
    require(adapter.GetInfo(&info) == wgpu::Status::Success, "Adapter identity query failed");
    std::cout << "ADAPTER device=" << text(info.device) << " description=" << text(info.description) << '\n';
    require(info.backendType == wgpu::BackendType::OpenGLES && info.adapterType != wgpu::AdapterType::CPU,
            "Unexpected backend or software adapter");
    require(text(info.device).find("Mali") != std::string::npos ||
                text(info.description).find("Mali") != std::string::npos,
            "Dawn adapter identity does not identify Mali");
    wgpu::Limits supported{};
    require(adapter.GetLimits(&supported) == wgpu::Status::Success, "Adapter limits query failed");
    std::cout << "LIMITS maxTextureDimension2D=" << supported.maxTextureDimension2D
              << " maxImmediateSize=" << supported.maxImmediateSize
              << " maxComputeInvocationsPerWorkgroup=" << supported.maxComputeInvocationsPerWorkgroup << '\n';
    require(supported.maxTextureDimension2D != wgpu::kLimitU32Undefined &&
                supported.maxTextureDimension2D >= 4096 &&
                supported.maxImmediateSize != wgpu::kLimitU32Undefined && supported.maxImmediateSize >= 64,
            "Adapter found, but U's 4096 texture / 64-byte immediates baseline is unavailable");
    wgpu::Limits required{};
    required.maxTextureDimension2D = 4096;
    required.maxImmediateSize = 64;
    wgpu::DeviceDescriptor desc{};
    desc.requiredLimits = &required;
    auto diag = diagnostics;
    desc.SetUncapturedErrorCallback(
      [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message, Diagnostics* state) {
        ++state->errors;
        std::cerr << "VALIDATION " << text(message) << '\n';
      }, diagnostics.get());
    desc.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous,
      [diag](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
        if (!diag->destroying.load() || reason != wgpu::DeviceLostReason::Destroyed) {
          ++diag->errors;
          std::cerr << "DEVICE_LOST " << text(message) << '\n';
        }
      });
    struct DeviceState { wgpu::Device value; bool ok = false; };
    auto state = std::make_shared<DeviceState>();
    future = adapter.RequestDevice(&desc, wgpu::CallbackMode::WaitAnyOnly,
      [state](wgpu::RequestDeviceStatus status, wgpu::Device value, wgpu::StringView message) {
        state->ok = status == wgpu::RequestDeviceStatus::Success;
        state->value = std::move(value);
        if (!state->ok) std::cerr << "DEVICE " << text(message) << '\n';
      });
    wait(future);
    device = std::move(state->value);
    require(state->ok && bool(device), "Cannot create probe device with the selected baseline limits");
    queue = device.GetQueue();
    require(bool(queue), "Cannot obtain device queue");
    std::cout << "PASS adapter/device (not a full U capability or game test)\n";
  }

  void observe_context() {
    auto current = eglGetCurrentContext();
    require(current != EGL_NO_CONTEXT && current != Platform::active->presenter,
            "Cannot observe the dedicated Dawn context for teardown verification");
    if (observed_context == EGL_NO_CONTEXT) observed_context = current;
    require(observed_context == current, "Unexpected context change in the single-context probe");
  }

  void cleanup() noexcept {
    diagnostics->destroying = true;
    queue = nullptr;
    if (device) device.Destroy();
    device = nullptr;
    adapter = nullptr;
    if (instance) instance.ProcessEvents();
    instance = nullptr;
  }
  ~Core() { cleanup(); }
};

struct Scene {
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::RenderPipeline pipeline;
  wgpu::Buffer parameters;
  wgpu::BindGroup bindings;

  void init(Core& core, bool bridge) {
    wgpu::TextureDescriptor desc{};
    desc.label = "64x64 synthetic pattern";
    desc.dimension = wgpu::TextureDimension::e2D;
    desc.format = wgpu::TextureFormat::RGBA8Unorm;
    desc.size = {kSize, kSize, 1};
    desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    if (bridge) {
      dawn::native::opengl::ExternalImageDescriptorEGLImage external;
      const WGPUTextureDescriptor& c_descriptor = desc;
      external.cTextureDescriptor = &c_descriptor;
      external.isInitialized = false;
      external.image = Platform::active->image;
      texture = wgpu::Texture::Acquire(dawn::native::opengl::WrapExternalEGLImage(core.device.Get(), &external));
    } else texture = core.device.CreateTexture(&desc);
    require(bool(texture), "Texture creation/import failed");
    view = texture.CreateView();
    require(bool(view), "Texture view creation failed");
    wgpu::ShaderSourceWGSL source{};
    source.code = kShader;
    wgpu::ShaderModuleDescriptor shader_desc{};
    shader_desc.nextInChain = &source;
    auto shader = core.device.CreateShaderModule(&shader_desc);
    wgpu::ColorTargetState target{};
    target.format = wgpu::TextureFormat::RGBA8Unorm;
    target.writeMask = wgpu::ColorWriteMask::All;
    wgpu::FragmentState fragment{};
    fragment.module = shader;
    fragment.entryPoint = "fs_main";
    fragment.targetCount = 1;
    fragment.targets = &target;
    wgpu::RenderPipelineDescriptor render{};
    render.vertex.module = shader;
    render.vertex.entryPoint = "vs_main";
    render.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    render.primitive.frontFace = wgpu::FrontFace::CCW;
    render.primitive.cullMode = wgpu::CullMode::None;
    render.multisample.count = 1;
    render.fragment = &fragment;
    pipeline = core.device.CreateRenderPipeline(&render);
    wgpu::BufferDescriptor buffer{};
    buffer.size = 16;
    buffer.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    parameters = core.device.CreateBuffer(&buffer);
    auto layout = pipeline.GetBindGroupLayout(0);
    wgpu::BindGroupEntry entry{};
    entry.binding = 0;
    entry.buffer = parameters;
    entry.size = 16;
    wgpu::BindGroupDescriptor group{};
    group.layout = layout;
    group.entryCount = 1;
    group.entries = &entry;
    bindings = core.device.CreateBindGroup(&group);
    require(bool(pipeline) && bool(parameters) && bool(bindings), "Synthetic pipeline construction failed");
    require(core.diagnostics->errors.load() == 0, "Pipeline validation failed");
    core.observe_context();
  }

  void draw(Core& core, unsigned frame) {
    const uint32_t data[] = {frame, 0, 0, 0};
    core.queue.WriteBuffer(parameters, 0, data, sizeof(data));
    auto encoder = core.device.CreateCommandEncoder();
    wgpu::RenderPassColorAttachment color{};
    color.view = view;
    color.loadOp = wgpu::LoadOp::Clear;
    color.storeOp = wgpu::StoreOp::Store;
    color.clearValue = {0, 0, 0, 1};
    wgpu::RenderPassDescriptor pass_desc{};
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color;
    auto pass = encoder.BeginRenderPass(&pass_desc);
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, bindings);
    pass.Draw(3);
    pass.End();
    auto command = encoder.Finish();
    core.queue.Submit(1, &command);
    core.completed();
    core.observe_context();
  }

  void verify_readback(Core& core, unsigned frame) {
    wgpu::BufferDescriptor desc{};
    desc.size = uint64_t(kRowBytes) * kSize;
    desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    auto readback = core.device.CreateBuffer(&desc);
    auto encoder = core.device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo from{};
    from.texture = texture;
    from.aspect = wgpu::TextureAspect::All;
    wgpu::TexelCopyBufferInfo to{};
    to.buffer = readback;
    to.layout.bytesPerRow = kRowBytes;
    to.layout.rowsPerImage = kSize;
    wgpu::Extent3D extent{kSize, kSize, 1};
    encoder.CopyTextureToBuffer(&from, &to, &extent);
    auto command = encoder.Finish();
    core.queue.Submit(1, &command);
    struct State { bool ok = false; };
    auto state = std::make_shared<State>();
    auto future = readback.MapAsync(wgpu::MapMode::Read, 0, desc.size, wgpu::CallbackMode::WaitAnyOnly,
      [state](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        state->ok = status == wgpu::MapAsyncStatus::Success;
        if (!state->ok) std::cerr << "MAP " << text(message) << '\n';
      });
    core.wait(future);
    require(state->ok, "Readback mapping failed");
    const auto* bytes = static_cast<const uint8_t*>(readback.GetConstMappedRange(0, desc.size));
    require(bytes != nullptr, "Mapped data unavailable");
    bool good = true;
    for (uint32_t y = 0; y < kSize; ++y) {
      for (uint32_t x = 0; x < kSize; ++x) {
        Pixel actual{};
        std::memcpy(actual.data(), bytes + y * kRowBytes + x * 4, 4);
        if (!pixel_matches(actual, expected_pixel(x, y, frame))) good = false;
      }
    }
    readback.Unmap();
    require(good, "WGSL offscreen readback differs from the expected pattern");
  }
};

// Only read a few diagnostic pixels. Presentation itself remains an EGLImage/FBO GPU blit.
bool sample_presenter(Platform& platform, unsigned frame, bool flipped) {
  constexpr std::array<std::array<uint32_t, 2>, 7> points{{
      {4, 4}, {16, 16}, {48, 16}, {16, 48}, {48, 48}, {31, 24}, {32, 40}}};
  for (auto point : points) {
    Pixel actual{};
    auto [x, logical_y] = point;
    uint32_t y = flipped ? kSize - 1 - logical_y : logical_y;
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
    if (!pixel_matches(actual, expected_pixel(x, logical_y, frame))) return false;
  }
  require(platform.framebuffer != 0, "Missing presenter framebuffer");
  return true;
}

void present(Platform& platform, unsigned frame, int& orientation) {
  require(platform.make_current(), "Cannot restore SDL presenter context");
  glBindFramebuffer(GL_READ_FRAMEBUFFER, platform.framebuffer);
  require(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Read FBO became incomplete");
  if (orientation < 0) {
    bool normal = sample_presenter(platform, frame, false);
    bool flipped = sample_presenter(platform, frame, true);
    require(normal != flipped, "Imported image pixels mismatch or orientation is ambiguous");
    orientation = flipped ? 1 : 0;
    std::cout << "BRIDGE GL row 0 corresponds to WebGPU " << (flipped ? "last" : "first")
              << " row; display blit orientation is explicit\n";
  } else require(sample_presenter(platform, frame, orientation != 0), "Imported pixels/order changed unexpectedly");
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glBlitFramebuffer(0, 0, kSize, kSize, 0, orientation == 0 ? platform.height : 0,
                    platform.width, orientation == 0 ? 0 : platform.height,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
  require(glGetError() == GL_NO_ERROR, "Presenter GL sampling/blit error");
  require(SDL_GL_SwapWindow(platform.window), SDL_GetError());
  glFinish(); // Reverse ordering before Dawn writes this same image again.
  require(glGetError() == GL_NO_ERROR, "Presenter completion GL error");
}

void run_graphics(const Options& opts) {
  std::cout << "START mode=" << opts.mode << " frames=" << opts.frames
            << " (graphics probe only; no game or FPS measurement)\n";
  Platform platform;
  platform.init();
  const bool bridge = opts.mode == "bridge";
  if (bridge) platform.create_image_storage();
  EGLContext dawn_context = EGL_NO_CONTEXT;
  auto diagnostics = std::make_shared<Diagnostics>();
  {
    Core core;
    core.diagnostics = diagnostics;
    core.init();
    {
      Scene scene;
      scene.init(core, bridge);
      int orientation = -1;
      const auto deadline = Clock::now() + std::chrono::seconds(20);
      for (unsigned frame = 0; frame < opts.frames; ++frame) {
        require(Clock::now() < deadline, "Probe sequence deadline exceeded");
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
          require(event.type != SDL_EVENT_QUIT &&
                      !(event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE),
                  "Probe cancelled by user");
        }
        scene.draw(core, frame);
        if (bridge) present(platform, frame, orientation);
        else scene.verify_readback(core, frame);
        require(diagnostics->errors.load() == 0, "Dawn error during frame");
        if (bridge) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      dawn_context = core.observed_context;
    }
    core.cleanup();
  }
  require(platform.make_current(), "Cannot restore presenter after Dawn teardown");
  require(dawn_context != EGL_NO_CONTEXT && dawn_context != platform.presenter, "Missing Dawn teardown witness");
  (void)eglGetError();
  EGLint value = 0;
  EGLBoolean exists = eglQueryContext(platform.display, dawn_context, EGL_CONTEXT_CLIENT_VERSION, &value);
  EGLint error = eglGetError();
  require(exists == EGL_FALSE && error == EGL_BAD_CONTEXT,
          "Dawn context remains alive after full release; alias cleanup not demonstrated");
  require(eglGetCurrentContext() == platform.presenter, "Presenter context was lost during teardown");
  require(diagnostics->errors.load() == 0, "Unexpected error during Dawn teardown");
  require(platform.cleanup(), "SDL/EGL resource cleanup failed");
  std::cout << "PASS " << opts.mode << " frames=" << opts.frames
            << " pixels/ordering and dedicated-context teardown checked\n";
}
} // namespace

int main(int argc, char** argv) {
  std::cout.setf(std::ios::unitbuf);
  std::cerr.setf(std::ios::unitbuf);
  try {
    auto opts = options(argc, argv);
    if (opts.help) help();
    else if (opts.self_test) self_test();
    else run_graphics(opts);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
