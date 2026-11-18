#include "frame.hpp"
#include "../gx/decoded_geometry.hpp"
#include "../gx/command_processor.hpp"
#include "overdraw.hpp"

#include "depth_peek.hpp"
#include "pipeline_cache.hpp"
#include "bloom.hpp"
#include "shadow.hpp"
#include "actor_shadow.hpp"
#include "volfog.hpp"
#include "thermal.hpp"
#include "xray.hpp"
#include <bit>
#include <aurora/vfx.hpp>
#include <aurora/envfx.hpp>
#include <aurora/water.hpp>
#include "probe.hpp"
#include "recording.hpp"
#include "render_worker.hpp"
#include "map_completion.hpp"
#include "vis_query.hpp"
#include "../gx/fifo.hpp"
#include "resource_cache.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "texture_replacement.hpp"
#include "../gx/gx.hpp"
#ifdef AURORA_ENABLE_RMLUI
#include "../rmlui/pipeline.hpp"
#endif
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"
#include "../h700/wait.hpp"
#ifdef AURORA_H700
#include "../h700/presentation.hpp"
#include "../h700/diagnostics.hpp"
#include "../h700/frame_dest_ring.hpp"
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <tracy/Tracy.hpp>
#include <magic_enum.hpp>

namespace aurora::gfx {
using namespace detail;
using webgpu::g_device;
using webgpu::g_instance;
using webgpu::g_queue;

// Set once when the device is created (webgpu::initialize); see resources.hpp.
bool g_noCompute = false;
bool g_vertexFetchTexture = false;

namespace {
constexpr Module Log{"aurora::gfx"};

Resources g_resources;
#ifdef AURORA_H700
using FrameDestBindings = h700::FrameDestRingBindings<wgpu::Buffer, wgpu::BindGroup>;
std::array<FrameDestBindings, h700::FrameDestRingSlots> g_frameDestRing;
h700::FrameDestRingState g_frameDestRingState;
std::array<std::vector<std::shared_ptr<void>>, h700::FrameDestRingSlots> g_geometrySlotPins;
uint64_t g_geometryGeneration = 0;
std::optional<uint64_t> g_geometryLastFrame;
#endif

enum class BufferMapState {
  Unmapped,
  Mapping,
  Mapped,
  Failed,
};

FrameBufferSizes g_bufferSizes;
std::array<std::atomic<uint64_t>, 4> g_stagingGrowthWant{};
// aurora_reserve_frame_buffers() asked for the held limits; begin_frame takes them.
std::atomic<bool> g_reserveHeldFrameBuffers{false};
// They were taken: a re-initialisation (device loss) keeps them.
bool g_heldFrameBuffersTaken = false;
constexpr uint32_t StorageGrowthCapScale = 6;
std::array<wgpu::Buffer, StagingBufferCount> g_stagingBuffers;
std::array<std::atomic<BufferMapState>, StagingBufferCount> g_mappingStates;
uint32_t g_frameIndex = UINT32_MAX;

std::array<FramePacket, FrameSlotCount> g_framePackets;
std::array<std::atomic_bool, FrameSlotCount> g_frameFailed{};
std::array<std::atomic_bool, FrameSlotCount> g_frameOwned{};
std::array<std::atomic<std::shared_ptr<h700::MapSlotOwnership>>, StagingBufferCount> g_mapOwnership;
std::array<std::atomic<std::shared_ptr<MapCompletion>>, StagingBufferCount> g_mapCompletions;
std::thread::id g_frameOwner;
bool g_mapsRetiring = false;
#ifdef AURORA_H700
bool g_frameInitialized = false;
#endif

bool staging_callback_pending(size_t slot) {
  const auto completion = g_mapCompletions[slot].load(std::memory_order_acquire);
  return completion && completion->pending();
}
uint64_t g_nextFrameId = 1;
render_worker::FrameSlotPool g_frameSlots{FrameSlotCount};
render_worker::FrameSlotPool g_stagingSlots{StagingBufferCount};

// The uniform buffer is bound as a MaxUniformSize window, and some passes bind more of it.
constexpr uint64_t MinDeviceUniformSize = 64u << 10;

struct SharedBuffer {
  StagingKind kind;
  wgpu::Buffer Resources::*member;
  wgpu::BufferUsage usage;
  const char* label;
  const char* name;
};
const std::array<SharedBuffer, 4> SharedBuffers{{
    {StagingKind::Vertex, &Resources::vertexBuffer,
     wgpu::BufferUsage::Storage | wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc,
     "Shared Vertex Buffer", "vertex"},
    {StagingKind::Uniform, &Resources::uniformBuffer,
     wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc, "Shared Uniform Buffer",
     "uniform"},
    {StagingKind::Index, &Resources::indexBuffer,
     wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc, "Shared Index Buffer",
     "index"},
    {StagingKind::Storage, &Resources::storageBuffer,
     wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc, "Shared Storage Buffer",
     "storage"},
}};

// Texture vertex fetch: one fetch texture row is 4096 R32Uint texels (16 KiB), so a buffer's
// bytes are its size / 16 KiB rows of the texture the shaders read it through.
constexpr uint32_t FetchTextureWidth = 4096;

// The allocation size a shared buffer needs: whole fetch texture rows for the kinds the shaders
// read as textures, so its texture covers it exactly.
uint64_t fetch_aligned_buffer_size(StagingKind kind, uint64_t bytes) noexcept {
  if (!g_vertexFetchTexture || (kind != StagingKind::Vertex && kind != StagingKind::Storage)) {
    return bytes;
  }
  return AURORA_ALIGN(bytes, FetchTextureRowBytes);
}

// Also covers the whole-buffer fragment binding after growth/resident/deferred sizing.
void validate_mixed_buffer_size(uint64_t bytes, bool storageBinding, const char* label) {
  if constexpr (g_mixedFetch) {
    if (bytes > g_resources.limits.maxBufferSize ||
        (storageBinding && bytes > g_resources.limits.maxStorageBufferBindingSize)) {
      FATAL("H700 mixed buffer {} ({} bytes) exceeds granted buffer/binding limits {}/{}", label, bytes,
            g_resources.limits.maxBufferSize, g_resources.limits.maxStorageBufferBindingSize);
    }
  }
}

// Creates the fetch texture mirroring `kind`'s device buffer of `bytes` bytes.
void create_fetch_texture(StagingKind kind, uint64_t bytes) {
  const bool vertex = kind == StagingKind::Vertex;
  wgpu::Texture& texture = vertex ? g_resources.vertexFetchTexture : g_resources.storageFetchTexture;
  wgpu::TextureView& view = vertex ? g_resources.vertexFetchTextureView : g_resources.storageFetchTextureView;
  const char* label = vertex ? "Vertex fetch texture" : "Storage fetch texture";
  const uint64_t rows = AURORA_ALIGN(bytes, FetchTextureRowBytes) / FetchTextureRowBytes;
  const uint32_t maxRows = g_resources.limits.maxTextureDimension2D;
  if (maxRows != 0 && rows > maxRows) {
    // The shaders address the data by row, so it cannot go in a smaller texture.
    FATAL("{} needs {} rows of {} bytes, more than this device's maxTextureDimension2D of {}; "
          "lower frameBufferScale or leave AURORA_VERTEX_FETCH_TEXTURE unset",
          label, rows, FetchTextureRowBytes, maxRows);
  }
  const wgpu::TextureDescriptor descriptor{
      .label = label,
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = {FetchTextureWidth, static_cast<uint32_t>(rows), 1},
      .format = wgpu::TextureFormat::R32Uint,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  texture = g_device.CreateTexture(&descriptor);
  view = texture.CreateView();
}

void create_static_bind_group() {
  std::array<wgpu::BindGroupEntry, g_mixedFetch ? 4 : 2> entries{};
  for (size_t i = 0; i < entries.size(); ++i) {
    entries[i].binding = static_cast<uint32_t>(i);
  }
  if (g_vertexFetchTexture || g_mixedFetch) {
    entries[0].textureView = g_resources.vertexFetchTextureView;
    entries[1].textureView = g_resources.storageFetchTextureView;
  } else {
    entries[0].buffer = g_resources.vertexBuffer;
    entries[1].buffer = g_resources.storageBuffer;
  }
  if constexpr (g_mixedFetch) {
    entries[MixedFragmentStorageBinding].buffer = g_resources.storageBuffer;
  }
#ifdef AURORA_H700
  entries[MixedFetchSamplerBinding].sampler = g_resources.fetchSampler;
#endif
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .label = "Static bind group",
      .layout = g_resources.staticBindGroupLayout,
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  g_resources.staticBindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
}

void create_uniform_bind_group() {
  const std::array entries{
      wgpu::BindGroupEntry{
          .binding = 0,
          .buffer = g_resources.uniformBuffer,
          .size = gx::MaxUniformSize,
      },
  };
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .label = "Uniform bind group",
      .layout = g_resources.uniformBindGroupLayout,
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  g_resources.uniformBindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  if constexpr (g_mixedFetch) {
    const std::array drawEntries{
        entries[0],
        wgpu::BindGroupEntry{
            .binding = 1,
            .buffer = g_resources.uniformBuffer,
            .size = sizeof(gx::DrawImmediateData),
        },
    };
    const wgpu::BindGroupDescriptor drawDescriptor{
        .label = "Draw uniform bind group",
        .layout = g_resources.drawUniformBindGroupLayout,
        .entryCount = drawEntries.size(),
        .entries = drawEntries.data(),
    };
    g_resources.drawUniformBindGroup = g_device.CreateBindGroup(&drawDescriptor);
  }
}

#ifdef AURORA_H700
void log_frame_dest_ring_fallback(h700::FrameDestRingFallback reason) {
  Log.info("H700 frame destination ring fallback: reason={} canonical_uniform={} canonical_storage={} "
           "(owners retained until guarded release)", h700::frame_dest_ring_reason(reason),
           static_cast<const void*>(g_resources.uniformBuffer.Get()),
           static_cast<const void*>(g_resources.storageBuffer.Get()));
}

void fallback_frame_dest_ring(h700::FrameDestRingFallback reason) {
  AURORA_ASSERT(render_worker::is_worker_thread(), "Frame destination fallback must run on render worker");
  // Current aliases already are canonical. Restoring slot 0 would lose the active frame's data.
  if (g_frameDestRingState.fallback(reason)) log_frame_dest_ring_fallback(reason);
}

void create_frame_dest_ring_bind_groups(FrameDestBindings& slot) {
  const auto staticEntries = h700::frame_dest_ring_static_entries(
      g_resources.vertexFetchTextureView.Get(), g_resources.storageFetchTextureView.Get(),
      slot.storageBuffer.Get(), g_resources.fetchSampler.Get());
  constexpr char StaticLabel[] = "Native destination static bind group";
  const WGPUBindGroupDescriptor staticDescriptor{
      .label = {StaticLabel, sizeof(StaticLabel) - 1},
      .layout = g_resources.staticBindGroupLayout.Get(),
      .entryCount = staticEntries.size(),
      .entries = staticEntries.data(),
  };
  slot.staticBindGroup = wgpu::BindGroup::Acquire(wgpuDeviceCreateBindGroup(g_device.Get(), &staticDescriptor));
  const auto uniformEntries = h700::frame_dest_ring_uniform_entries(slot.uniformBuffer.Get(), gx::MaxUniformSize);
  constexpr char UniformLabel[] = "Native destination uniform bind group";
  const WGPUBindGroupDescriptor uniformDescriptor{
      .label = {UniformLabel, sizeof(UniformLabel) - 1},
      .layout = g_resources.uniformBindGroupLayout.Get(),
      .entryCount = uniformEntries.size(),
      .entries = uniformEntries.data(),
  };
  slot.uniformBindGroup = wgpu::BindGroup::Acquire(wgpuDeviceCreateBindGroup(g_device.Get(), &uniformDescriptor));
  const auto drawEntries = h700::frame_dest_ring_uniform_entries<true>(
      slot.uniformBuffer.Get(), gx::MaxUniformSize, sizeof(gx::DrawImmediateData));
  constexpr char DrawLabel[] = "Native destination draw uniform bind group";
  const WGPUBindGroupDescriptor drawDescriptor{
      .label = {DrawLabel, sizeof(DrawLabel) - 1},
      .layout = g_resources.drawUniformBindGroupLayout.Get(),
      .entryCount = drawEntries.size(),
      .entries = drawEntries.data(),
  };
  slot.drawUniformBindGroup = wgpu::BindGroup::Acquire(wgpuDeviceCreateBindGroup(g_device.Get(), &drawDescriptor));
}

bool initialize_frame_dest_ring() {
  if (!h700::frame_dest_ring_requested(std::getenv("MP_H700_FRAME_DEST_RING"), std::getenv("MP_H700_BENCH")))
    return true;
  const auto reason = h700::frame_dest_ring_eligibility(
      g_mixedFetch && g_vertexFetchTexture && h700::imports_ready(), g_config.residentGeometryMiB, g_bufferSizes,
      g_resources.uniformBuffer.GetSize(), g_resources.storageBuffer.GetSize(), g_resources.limits);
  if (reason != h700::FrameDestRingFallback::None) {
    log_frame_dest_ring_fallback(reason);
    return true;
  }
  g_frameDestRing[0] = {g_resources.uniformBuffer, g_resources.storageBuffer, g_resources.staticBindGroup,
                       g_resources.uniformBindGroup, g_resources.drawUniformBindGroup};
  const auto descriptors = h700::frame_dest_ring_buffer_descriptors();
  for (const auto& descriptor : descriptors) {
    validate_mixed_buffer_size(descriptor.size,
        (descriptor.usage & wgpu::BufferUsage::Storage) != wgpu::BufferUsage::None, "native destination");
  }
  auto& second = g_frameDestRing[1];
  second.uniformBuffer = g_device.CreateBuffer(&descriptors[0]);
  second.storageBuffer = g_device.CreateBuffer(&descriptors[1]);
  if (second.uniformBuffer && second.storageBuffer) create_frame_dest_ring_bind_groups(second);
  if (!second.uniformBuffer || !second.storageBuffer || !second.staticBindGroup || !second.uniformBindGroup ||
      !second.drawUniformBindGroup || !render_worker::wait_healthy()) {
    Log.error("H700 frame destination ring resource creation failed; retaining holders for guarded release");
    if (auto state = h700::presentation_state()) state->fail("H700 frame destination ring resource creation failed");
    render_worker::notify_failure();
    return false;
  }
  // Both storage buffers start zeroed; the original full mirror initialization remains in place.
  // Only destinations/groups are duplicated, never staging memory or texture mirrors.
  g_frameDestRingState = h700::FrameDestRingState{true};
  Log.info("H700 frame destination ring enabled: native_slots={} uniform_bytes={} storage_bytes={} extra_bytes={}",
           h700::FrameDestRingSlots, h700::FrameDestRingBufferBytes, h700::FrameDestRingBufferBytes,
           2 * h700::FrameDestRingBufferBytes);
  return true;
}

bool select_frame_dest_ring(const std::optional<h700::FrameDescriptor>& nativeFrame, size_t frameSlot) {
  AURORA_ASSERT(render_worker::is_worker_thread(), "Frame destination selection must run on render worker");
  const auto decision = g_frameDestRingState.select(nativeFrame);
  if (decision.route == h700::FrameDestRingRoute::Legacy) return true;
  if (decision.route == h700::FrameDestRingRoute::Fallback) {
    log_frame_dest_ring_fallback(g_frameDestRingState.fallback_reason());
    return true;
  }
  if (decision.route == h700::FrameDestRingRoute::ContractError || !h700::native_slot(*nativeFrame)) {
    Log.error("H700 frame destination ring contract failed: {} generation={} frame={} native_slot={} cpu_slot={}",
              decision.error ? decision.error : "native reservation is not current",
              nativeFrame ? nativeFrame->generation : 0, nativeFrame ? nativeFrame->frame : 0,
              nativeFrame ? int(nativeFrame->slot) : -1, frameSlot);
    fail_frame_work(frameSlot);
    return false;
  }
  h700::apply_frame_dest_ring_bindings(g_resources, g_frameDestRing[decision.slot]);
  if (decision.logSelection) {
    Log.info("H700 frame destination ring select: generation={} frame={} native_slot={} cpu_slot={} uniform={} storage={}",
             nativeFrame->generation, nativeFrame->frame, decision.slot, frameSlot,
             static_cast<const void*>(g_resources.uniformBuffer.Get()),
             static_cast<const void*>(g_resources.storageBuffer.Get()));
  }
  return true;
}
#endif

struct RuntimeDrawType {
  std::string label;
  DrawCallback draw = nullptr;
  void* userdata = nullptr;
  uint32_t generation = 1;
};

struct RuntimeEncoderTaskType {
  std::string label;
  EncoderTaskCallback callback = nullptr;
  void* userdata = nullptr;
  EncoderTaskCompletionCallback afterSubmit = nullptr;
  uint32_t generation = 1;
};

constexpr uint32_t draw_type_index(DrawTypeId id) { return static_cast<uint32_t>(id & 0xFFFFFFFFu); }
constexpr uint32_t draw_type_generation(DrawTypeId id) { return static_cast<uint32_t>(id >> 32); }
constexpr DrawTypeId make_draw_type_id(uint32_t index, uint32_t generation) {
  return static_cast<DrawTypeId>(generation) << 32 | index;
}

std::vector<RuntimeDrawType> g_runtimeDrawTypes;
std::vector<uint32_t> g_freeDrawTypeSlots;
std::vector<RuntimeEncoderTaskType> g_runtimeEncoderTaskTypes;
std::vector<uint32_t> g_freeEncoderTaskTypeSlots;
std::mutex g_runtimeTypeMutex;

// Requires g_runtimeTypeMutex held.
const RuntimeDrawType* find_runtime_draw_type_locked(DrawTypeId id) {
  const auto idx = draw_type_index(id);
  if (id == InvalidDrawType || idx >= g_runtimeDrawTypes.size()) {
    return nullptr;
  }
  const auto& slot = g_runtimeDrawTypes[idx];
  if (slot.generation != draw_type_generation(id) || slot.draw == nullptr) {
    return nullptr;
  }
  return &slot;
}

// Requires g_runtimeTypeMutex held.
const RuntimeEncoderTaskType* find_runtime_encoder_task_type_locked(EncoderTaskId id) {
  const auto idx = draw_type_index(id);
  if (id == InvalidEncoderTask || idx >= g_runtimeEncoderTaskTypes.size()) {
    return nullptr;
  }
  const auto& slot = g_runtimeEncoderTaskTypes[idx];
  if (slot.generation != draw_type_generation(id) || slot.callback == nullptr) {
    return nullptr;
  }
  return &slot;
}

using PresentClock = std::chrono::steady_clock;
constexpr auto PresentFpsWindow = std::chrono::seconds{1};
std::mutex g_presentStatsMutex;
std::deque<PresentClock::time_point> g_presentTimes;
std::atomic_bool g_processEventsQueued = false;
std::atomic_int64_t g_lastPresentNs = 0;
std::atomic_int64_t g_presentPeriodNs = 0;
std::atomic_int64_t g_cpuFrameTimeNs = 0;
PresentClock::time_point g_cpuFrameStart;
constexpr auto FrameStartSafetyMargin = std::chrono::milliseconds{2};
constexpr auto MaxPacingSample = std::chrono::milliseconds{250};
constexpr uint32_t PacingEmaWeight = 8;

int64_t timestamp_ns(PresentClock::time_point time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

int64_t duration_ns(PresentClock::duration duration) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

void update_ema(std::atomic_int64_t& value, int64_t sample) {
  if (sample <= 0 || sample > duration_ns(MaxPacingSample)) {
    return;
  }

  int64_t current = value.load(std::memory_order_acquire);
  while (true) {
    const int64_t next = current == 0 ? sample : current + (sample - current) / static_cast<int64_t>(PacingEmaWeight);
    if (value.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
      return;
    }
  }
}

void prune_present_times(PresentClock::time_point now) {
  while (!g_presentTimes.empty() && g_presentTimes.front() + PresentFpsWindow < now) {
    g_presentTimes.pop_front();
  }
}

void process_events() {
  ZoneScopedN("ProcessEvents");
  if (g_instance) {
    g_instance.ProcessEvents();
  }
}

void enqueue_process_events() {
  if (render_worker::is_worker_thread()) {
    process_events();
    return;
  }

  bool expected = false;
  if (!g_processEventsQueued.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
    return;
  }

  render_worker::enqueue_work([] {
    process_events();
    g_processEventsQueued.store(false, std::memory_order_release);
  }, [] { g_processEventsQueued.store(false, std::memory_order_release); });
}

bool wait_for_gpu_progress(std::chrono::nanoseconds sleepDuration) {
  if (!render_worker::progress_wait()) return false;
#ifndef AURORA_H700
  if (render_worker::is_idle()) enqueue_process_events();
#endif
  std::this_thread::sleep_for(sleepDuration);
  return render_worker::wait_healthy();
}

void pace_frame_start() {
  ZoneScopedN("Frame start pacing");
  if (g_frameSlots.free_count() == FrameSlotCount) {
    return;
  }

  const int64_t lastPresentNs = g_lastPresentNs.load(std::memory_order_acquire);
  const int64_t presentPeriodNs = g_presentPeriodNs.load(std::memory_order_acquire);
  const int64_t cpuFrameTimeNs = g_cpuFrameTimeNs.load(std::memory_order_acquire);
  if (lastPresentNs == 0 || presentPeriodNs == 0 || cpuFrameTimeNs == 0) {
    return;
  }

  const int64_t safetyMarginNs = duration_ns(FrameStartSafetyMargin);
  const int64_t targetStartNs = lastPresentNs + presentPeriodNs - cpuFrameTimeNs - safetyMarginNs;
  int64_t nowNs = timestamp_ns(PresentClock::now());
  if (targetStartNs <= nowNs) {
    return;
  }

  const double initialWaitMs = static_cast<double>(targetStartNs - nowNs) / 1'000'000.0;
  TracyPlot("aurora: frameStartPaceWaitMs", initialWaitMs);
  while (nowNs < targetStartNs) {
    const int64_t remainingNs = targetStartNs - nowNs;
    const auto sleepDuration =
        remainingNs > 1'000'000 ? std::chrono::milliseconds{1} : std::chrono::nanoseconds{remainingNs};
    if (!wait_for_gpu_progress(sleepDuration)) return;
    nowNs = timestamp_ns(PresentClock::now());
  }
}

std::shared_ptr<h700::MapSlotOwnership> map_staging_buffer(size_t slot, bool releaseSlotOnCompletion = false) {
  if (g_mapsRetiring) return {};
  auto expected = BufferMapState::Unmapped;
  if (!g_mappingStates[slot].compare_exchange_strong(expected, BufferMapState::Mapping, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
    return g_mapOwnership[slot].load(std::memory_order_acquire);
  }
  auto ownership = std::make_shared<h700::MapSlotOwnership>(releaseSlotOnCompletion, [slot] { g_stagingSlots.release(slot); });
  g_mapOwnership[slot].store(ownership, std::memory_order_release);
  auto completion = std::make_shared<MapCompletion>();
  g_mapCompletions[slot].store(completion, std::memory_order_release);
  g_stagingBuffers[slot].MapAsync(
      wgpu::MapMode::Write, 0, g_bufferSizes.staging(), wgpu::CallbackMode::AllowSpontaneous,
      [slot, ownership, completion](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        if (g_mapCompletions[slot].load(std::memory_order_acquire) != completion || !completion->begin_completion())
          return; // duplicate or an old request: never mutate a replacement buffer/generation
#ifdef AURORA_H700
        if (status != wgpu::MapAsyncStatus::Success) {
          g_mappingStates[slot].store(BufferMapState::Failed, std::memory_order_release);
          if (auto state = h700::presentation_state()) state->fail(fmt::format("Staging map failed: {} {}", magic_enum::enum_name(status), message));
          render_worker::notify_failure();
          ownership->complete();
          (void)completion->finish_completion(false);
          return;
        }
#else
        if (status == wgpu::MapAsyncStatus::CallbackCancelled || status == wgpu::MapAsyncStatus::Aborted) {
          Log.warn("Buffer mapping {}: {}", magic_enum::enum_name(status), message);
          g_mappingStates[slot].store(BufferMapState::Unmapped, std::memory_order_release);
          ownership->complete();
          (void)completion->finish_completion(false);
          return;
        }
        AURORA_ASSERT(status == wgpu::MapAsyncStatus::Success, "Buffer mapping failed: {} {}",
                      magic_enum::enum_name(status), message);
#endif
        g_mappingStates[slot].store(BufferMapState::Mapped, std::memory_order_release);
        ownership->complete();
        (void)completion->finish_completion(true);
      });
  return ownership;
}
} // namespace

const FrameBufferSizes& frame_buffer_sizes() noexcept { return g_bufferSizes; }

Range resident_region(ResidentBuffer kind) noexcept {
  switch (kind) {
  case ResidentBuffer::Vertex:
    return {0, uint32_t(g_bufferSizes.residentVertex)};
  case ResidentBuffer::Index:
    return {0, uint32_t(g_bufferSizes.residentIndex)};
  case ResidentBuffer::Storage:
    return {0, uint32_t(g_bufferSizes.residentStorage)};
  }
  return {};
}

namespace detail {

Resources& resources() noexcept { return g_resources; }

const wgpu::Buffer& staging_buffer(size_t slot) { return g_stagingBuffers[slot]; }

std::optional<RegisteredDrawType> find_runtime_draw_type(DrawTypeId id) {
  std::lock_guard lock{g_runtimeTypeMutex};
  const auto* slot = find_runtime_draw_type_locked(id);
  if (slot == nullptr) {
    return std::nullopt;
  }
  return RegisteredDrawType{.draw = slot->draw, .userdata = slot->userdata};
}

std::optional<RegisteredEncoderTaskType> find_runtime_encoder_task_type(EncoderTaskId id) {
  std::lock_guard lock{g_runtimeTypeMutex};
  const auto* slot = find_runtime_encoder_task_type_locked(id);
  if (slot == nullptr) {
    return std::nullopt;
  }
  return RegisteredEncoderTaskType{
      .callback = slot->callback,
      .userdata = slot->userdata,
      .afterSubmit = slot->afterSubmit,
  };
}

void upload_fetch_texture_rows(const wgpu::CommandEncoder& encoder, StagingKind kind, uint64_t lo, uint64_t hi) {
  if (!g_vertexFetchTexture || hi <= lo || (kind != StagingKind::Vertex && kind != StagingKind::Storage)) {
    return;
  }
  const bool vertex = kind == StagingKind::Vertex;
  const wgpu::Texture& texture = vertex ? g_resources.vertexFetchTexture : g_resources.storageFetchTexture;
  if (!texture) {
    return;
  }
  const uint64_t firstRow = lo / FetchTextureRowBytes;
  const uint64_t lastRow = (hi + FetchTextureRowBytes - 1) / FetchTextureRowBytes;
  const wgpu::TexelCopyBufferInfo source{
      .layout =
          wgpu::TexelCopyBufferLayout{
              .offset = firstRow * FetchTextureRowBytes,
              .bytesPerRow = FetchTextureRowBytes,
              .rowsPerImage = static_cast<uint32_t>(lastRow - firstRow),
          },
      .buffer = vertex ? g_resources.vertexBuffer : g_resources.storageBuffer,
  };
  const wgpu::TexelCopyTextureInfo destination{
      .texture = texture,
      .origin =
          wgpu::Origin3D{
              .x = 0,
              .y = static_cast<uint32_t>(firstRow),
              .z = 0,
          },
  };
  const wgpu::Extent3D size{
      .width = FetchTextureWidth,
      .height = static_cast<uint32_t>(lastRow - firstRow),
      .depthOrArrayLayers = 1,
  };
  encoder.CopyBufferToTexture(&source, &destination, &size);
}

} // namespace detail

wgpu::Device device() noexcept { return g_device; }

wgpu::Queue queue() noexcept { return g_queue; }

wgpu::TextureFormat color_format() noexcept { return webgpu::g_graphicsConfig.surfaceConfiguration.format; }

wgpu::TextureFormat depth_format() noexcept { return webgpu::g_graphicsConfig.depthFormat; }

uint32_t sample_count() noexcept { return webgpu::g_graphicsConfig.msaaSamples; }

static RenderTargetLayout efb_render_target_layout(const webgpu::TextureWithSampler& color) noexcept {
  RenderTargetLayout layout{
      .colorAttachmentCount = 1,
      .colorAttachments = {{{
          .semantic = ColorAttachmentSemantic::SceneColor,
          .format = color.format,
          .width = color.size.width,
          .height = color.size.height,
      }}},
      .depthStencilFormat = webgpu::g_graphicsConfig.depthFormat,
      .sampleCount = webgpu::g_graphicsConfig.msaaSamples,
  };
  finalize_render_target_layout(layout);
  return layout;
}

// The target EFB passes draw into now: the HDR scene while it is active (set_scene_hdr), else the
// display EFB. color_format() stays the surface format either way.
RenderTargetLayout scene_render_target_layout() noexcept {
  return efb_render_target_layout(scene_hdr_active() ? webgpu::g_sceneHdr : webgpu::g_frameBuffer);
}

RenderTargetLayout display_render_target_layout() noexcept { return efb_render_target_layout(webgpu::g_frameBuffer); }

bool uses_reversed_z() noexcept { return gx::UseReversedZ; }

DrawTypeId register_draw_type(const DrawTypeDescriptor& desc) {
  if (desc.draw == nullptr) {
    Log.warn("register_draw_type: draw callback is null");
    return InvalidDrawType;
  }

  std::lock_guard lock{g_runtimeTypeMutex};
  uint32_t idx;
  if (!g_freeDrawTypeSlots.empty()) {
    idx = g_freeDrawTypeSlots.back();
    g_freeDrawTypeSlots.pop_back();
  } else {
    idx = static_cast<uint32_t>(g_runtimeDrawTypes.size());
    g_runtimeDrawTypes.emplace_back();
  }
  auto& slot = g_runtimeDrawTypes[idx];
  slot.label = desc.label != nullptr ? desc.label : "";
  slot.draw = desc.draw;
  slot.userdata = desc.userdata;
  return make_draw_type_id(idx, slot.generation);
}

void unregister_draw_type(DrawTypeId type) noexcept {
  std::lock_guard lock{g_runtimeTypeMutex};
  if (find_runtime_draw_type_locked(type) == nullptr) {
    return;
  }
  const auto idx = draw_type_index(type);
  auto& slot = g_runtimeDrawTypes[idx];
  slot.label.clear();
  slot.draw = nullptr;
  slot.userdata = nullptr;
  ++slot.generation;
  g_freeDrawTypeSlots.push_back(idx);
}

EncoderTaskId register_encoder_task_type(const EncoderTaskDescriptor& desc) {
  if (desc.callback == nullptr) {
    Log.warn("register_encoder_task_type: callback is null");
    return InvalidEncoderTask;
  }

  std::lock_guard lock{g_runtimeTypeMutex};
  uint32_t idx;
  if (!g_freeEncoderTaskTypeSlots.empty()) {
    idx = g_freeEncoderTaskTypeSlots.back();
    g_freeEncoderTaskTypeSlots.pop_back();
  } else {
    idx = static_cast<uint32_t>(g_runtimeEncoderTaskTypes.size());
    g_runtimeEncoderTaskTypes.emplace_back();
  }
  auto& slot = g_runtimeEncoderTaskTypes[idx];
  slot.label = desc.label != nullptr ? desc.label : "";
  slot.callback = desc.callback;
  slot.userdata = desc.userdata;
  slot.afterSubmit = desc.afterSubmit;
  return make_draw_type_id(idx, slot.generation);
}

void unregister_encoder_task_type(EncoderTaskId type) noexcept {
  std::lock_guard lock{g_runtimeTypeMutex};
  if (find_runtime_encoder_task_type_locked(type) == nullptr) {
    return;
  }
  const auto idx = draw_type_index(type);
  auto& slot = g_runtimeEncoderTaskTypes[idx];
  slot.label.clear();
  slot.callback = nullptr;
  slot.userdata = nullptr;
  slot.afterSubmit = nullptr;
  ++slot.generation;
  g_freeEncoderTaskTypeSlots.push_back(idx);
}

void initialize() {
  if (pending_internal_maps() != 0) {
    Log.error("Frame initialization refused: previous map callbacks remain owned");
    render_worker::notify_failure();
    return;
  }
#ifdef AURORA_H700
  if (g_frameDestRing[0].uniformBuffer || g_frameDestRing[0].storageBuffer ||
      g_frameDestRing[1].uniformBuffer || g_frameDestRing[1].storageBuffer) {
    Log.error("Frame initialization refused: native destination owners require guarded release");
    render_worker::notify_failure();
    return;
  }
  g_frameInitialized = true;
#endif
  g_frameOwner = std::this_thread::get_id();
  g_mapsRetiring = false;
  g_frameIndex = 0;
  g_processEventsQueued.store(false, std::memory_order_release);
  g_lastPresentNs.store(0, std::memory_order_release);
  g_presentPeriodNs.store(0, std::memory_order_release);
  g_cpuFrameTimeNs.store(0, std::memory_order_release);
  g_cpuFrameStart = {};
  {
    std::lock_guard lock{g_presentStatsMutex};
    g_presentTimes.clear();
  }
#ifdef AURORA_H700
  auto presentation = h700::presentation_state();
  render_worker::set_wait_hooks({
      .main = std::this_thread::get_id(),
      .healthy = [presentation] { const auto phase = presentation->snapshot().phase;
        return phase == h700::PresentationPhase::Running || phase == h700::PresentationPhase::Draining; },
      .progress = [] { (void)h700::main_progress_once(); },
      .admitRoot = [presentation] { return presentation->snapshot().phase == h700::PresentationPhase::Running; },
  });
#endif
  render_worker::initialize();
  // This appears to take a while and blocks the render thread for periods of time
  // render_worker::set_event_pump([] {
  //   if (g_instance) {
  //     g_instance.ProcessEvents();
  //   }
  // });
  depth_peek::initialize();
  tex_copy_conv::initialize();
  tex_palette_conv::initialize();

  // For uniform & storage buffer offset alignments
  g_device.GetLimits(&g_resources.limits);
  if constexpr (g_mixedFetch) {
    if (!g_vertexFetchTexture) {
      FATAL("H700 mixed fetch requires both vertex texture mirrors");
    }
  }

  // The vertex and array buffers are bound whole as storage, and one staging buffer holds
  // a frame of everything, so the scale stops where the device does.
  {
    const uint64_t maxBinding = g_resources.limits.maxStorageBufferBindingSize;
    const uint64_t maxBuffer = g_resources.limits.maxBufferSize;
    uint32_t scale = std::max<uint32_t>(g_config.frameBufferScale, 1);
    for (;; --scale) {
      g_bufferSizes = {
          .vertex = VertexBufferSize * scale,
          .uniform = scale > 1 ? UniformBufferSize * 2 : UniformBufferSize,
          // Not scaled: the heaviest room measured drew 0.1 MiB of indices, and each
          // scaled MiB here costs one per staging buffer plus the device copy.
          .index = IndexBufferSize,
          .storage = StorageBufferSize * scale,
      };
      if (scale == 1 || (g_bufferSizes.vertex <= maxBinding && g_bufferSizes.storage <= maxBinding &&
                         g_bufferSizes.staging() <= maxBuffer)) {
        break;
      }
    }
    if (scale != std::max<uint32_t>(g_config.frameBufferScale, 1)) {
      Log.warn("Frame buffer scale {} is beyond this device, using {}", g_config.frameBufferScale, scale);
    }
    // The arrays can be scaled apart from the rest: frameStorageScale 0 follows the scale.
    uint32_t storageScale = g_config.frameStorageScale != 0 ? g_config.frameStorageScale : scale;
    // Test hook: start the growable storage smaller to force an overflow.
    if (g_config.frameStorageScale == 0) {
      if (const char* start = std::getenv("AURORA_STORAGE_START_SCALE")) {
        storageScale = std::max<uint32_t>(1, std::min<uint32_t>(storageScale, std::strtoul(start, nullptr, 10)));
      }
    }
    const uint32_t wantedStorageScale = storageScale;
    while (storageScale > scale) {
      g_bufferSizes.storage = StorageBufferSize * storageScale;
      if (g_bufferSizes.storage <= maxBinding && g_bufferSizes.staging() <= maxBuffer) {
        break;
      }
      --storageScale;
    }
    g_bufferSizes.storage = StorageBufferSize * storageScale;
    if (storageScale != wantedStorageScale) {
      Log.warn("Frame storage scale {} is beyond this device, using {}", wantedStorageScale, storageScale);
    }
    // Unless the scale was pinned, an overflow can double the staging storage later, up to
    // 6x. The device buffer sets that much aside now so the resident region stays put.
    g_bufferSizes.storageReserve = g_bufferSizes.storage;
    g_bufferSizes.storageCap = 0;
    if (g_config.frameStorageScale == 0 && g_config.residentGeometryMiB != 0) {
      uint32_t capScale = StorageGrowthCapScale;
      while (capScale > storageScale) {
        FrameBufferSizes probe = g_bufferSizes;
        probe.storage = StorageBufferSize * capScale;
        if (probe.storage <= maxBinding && probe.staging() <= maxBuffer) {
          break;
        }
        --capScale;
      }
      if (capScale > storageScale) {
        g_bufferSizes.storageCap = StorageBufferSize * capScale;
        g_bufferSizes.storageReserve = g_bufferSizes.storageCap;
      }
    }
    // The vertices likewise: a frame drawing skinned models twice more (the Remastered suit
    // shell) went past 2x with the room geometry kept resident. Only the limit moves; the
    // device and staging buffers still grow to it when a frame needs more.
    uint64_t vertexReserve = g_bufferSizes.vertex;
    if (g_config.frameStorageScale == 0 && g_config.residentGeometryMiB != 0) {
      for (uint32_t capScale = StorageGrowthCapScale; capScale > scale; --capScale) {
        FrameBufferSizes probe = g_bufferSizes;
        probe.vertex = VertexBufferSize * capScale;
        probe.storage = g_bufferSizes.storageReserve;
        if (probe.vertex <= maxBinding && probe.staging() <= maxBuffer) {
          vertexReserve = probe.vertex;
          break;
        }
      }
    }
    for (auto& want : g_stagingGrowthWant) {
      want.store(0, std::memory_order_release);
    }
    // The resident regions follow a frame's data in the device buffers, which are bound
    // whole, so they take what the device leaves after it: two fifths each to vertices and
    // arrays, a fifth to indices.
    const uint64_t resident = uint64_t(g_config.residentGeometryMiB) * 1024 * 1024;
    if (resident != 0) {
      const auto fit = [](uint64_t want, uint64_t frame, uint64_t limit) -> uint64_t {
        const uint64_t room = limit > frame ? limit - frame : 0;
        // Offsets into it are u32, and every allocation is 256-aligned.
        return std::min({want, room, uint64_t(UINT32_MAX) - frame}) & ~uint64_t(255);
      };
      const uint64_t bound = std::min(maxBinding, maxBuffer);
      g_bufferSizes.residentVertex = fit(resident / 5 * 2, vertexReserve, bound);
      g_bufferSizes.residentStorage = fit(resident / 5 * 2, g_bufferSizes.storageReserve, bound);
      g_bufferSizes.residentIndex = fit(resident / 5, g_bufferSizes.index, maxBuffer);
      Log.info("Resident geometry: {} MiB of vertices, {} MiB of arrays, {} MiB of indices",
               g_bufferSizes.residentVertex >> 20, g_bufferSizes.residentStorage >> 20,
               g_bufferSizes.residentIndex >> 20);
    }
    // What a frame can hold is what is sized above (the device buffers' per-frame part). The
    // staging buffers, which there are StagingBufferCount of, start smaller unless the scale
    // was pinned: a frame that needs more spills (see SpillBuffer), and the arrays' staging
    // storage grows after an overflow where it can.
    g_bufferSizes.maxVertex = vertexReserve;
    g_bufferSizes.maxUniform = g_bufferSizes.uniform;
    g_bufferSizes.maxIndex = g_bufferSizes.index;
    g_bufferSizes.maxStorage = g_bufferSizes.storageReserve;
    g_bufferSizes.scaledStorage = g_bufferSizes.storage;
    g_bufferSizes.scaledVertex = g_bufferSizes.vertex;
    g_bufferSizes.textureUpload = TextureUploadSize;
    if (!g_config.frameBuffersPinned) {
      g_bufferSizes.vertex = std::min(g_bufferSizes.vertex, StartVertexSize);
      g_bufferSizes.uniform = std::min(g_bufferSizes.uniform, StartUniformSize);
      g_bufferSizes.index = std::min(g_bufferSizes.index, StartIndexSize);
      g_bufferSizes.storage = std::min(g_bufferSizes.storage, StartStorageSize);
      g_bufferSizes.textureUpload = StartTextureUploadSize;
    }
    // Test hook: AURORA_STAGING_START_KIB=<n> sets the vertex, uniform, index and storage
    // staging buffers to n KiB (at most their frame limit) to force the spill path.
    if (const char* kib = std::getenv("AURORA_STAGING_START_KIB")) {
      const uint64_t size = std::max<uint64_t>(256, (std::strtoull(kib, nullptr, 10) << 10) & ~uint64_t(255));
      const char* maskEnv = std::getenv("AURORA_STAGING_START_MASK"); // bits: 1 vertex, 2 uniform, 4 index, 8 storage
      const unsigned mask = maskEnv != nullptr ? unsigned(std::strtoul(maskEnv, nullptr, 0)) : 0xFu;
      if (mask & 1u) g_bufferSizes.vertex = std::min(g_bufferSizes.maxVertex, size);
      if (mask & 2u) g_bufferSizes.uniform = std::min(g_bufferSizes.maxUniform, size);
      if (mask & 4u) g_bufferSizes.index = std::min(g_bufferSizes.maxIndex, size);
      if (mask & 8u) g_bufferSizes.storage = std::min(g_bufferSizes.maxStorage, size);
    }
    // The growable cap is relative to the staging size, so it keeps its meaning; but a cap the
    // staging storage already has is no cap.
    if (g_bufferSizes.storageCap != 0 && g_bufferSizes.storageCap <= g_bufferSizes.storage) {
      g_bufferSizes.storageCap = 0;
    }
    // The device buffers' per-frame part starts where staging does, unless the scale was pinned,
    // and grows like it when a frame needs more (grow_frame_buffer). The uniform one holds at
    // least the bound window. Android takes the full frame limit up front: a mid-frame grow
    // there was followed by a device loss on an Adreno 618 (issue #165), and with robustness
    // off nothing catches a bad access.
    // AURORA_FULL_DEVICE_BUFFERS=1 takes the Android path elsewhere, to test it.
#if defined(__ANDROID__)
    const bool fullDeviceBuffers = true;
#else
    const bool fullDeviceBuffers = std::getenv("AURORA_FULL_DEVICE_BUFFERS") != nullptr;
#endif
    // Taking those limits whole costs memory a game without room geometry never uses, so
    // frameBuffersDeferred runs at what a scale of 1 takes until aurora_reserve_frame_buffers()
    // (reserve_held_frame_buffers, between two frames, never mid-frame). Once taken they stay
    // taken over a device loss.
    if (fullDeviceBuffers && g_config.frameBuffersDeferred && !g_config.frameBuffersPinned &&
        !g_heldFrameBuffersTaken && g_config.residentGeometryMiB == 0 &&
        std::getenv("AURORA_STAGING_START_KIB") == nullptr) {
      auto& s = g_bufferSizes;
      if (s.maxVertex > VertexBufferSize || s.maxUniform > UniformBufferSize || s.maxIndex > IndexBufferSize ||
          s.maxStorage > StorageBufferSize) {
        s.heldVertex = s.maxVertex;
        s.heldUniform = s.maxUniform;
        s.heldIndex = s.maxIndex;
        s.heldStorage = s.maxStorage;
        s.maxVertex = std::min(s.maxVertex, VertexBufferSize);
        s.maxUniform = std::min(s.maxUniform, UniformBufferSize);
        s.maxIndex = std::min(s.maxIndex, IndexBufferSize);
        s.maxStorage = std::min(s.maxStorage, StorageBufferSize);
        s.vertex = std::min(s.vertex, s.maxVertex);
        s.uniform = std::min(s.uniform, s.maxUniform);
        s.index = std::min(s.index, s.maxIndex);
        s.storage = std::min(s.storage, s.maxStorage);
        Log.info("Frame buffers: scaled limits held back until room geometry asks for them");
      }
    }
    if (g_config.frameBuffersPinned || (fullDeviceBuffers && std::getenv("AURORA_STAGING_START_KIB") == nullptr)) {
      g_bufferSizes.deviceVertex = g_bufferSizes.maxVertex;
      g_bufferSizes.deviceUniform = g_bufferSizes.maxUniform;
      g_bufferSizes.deviceIndex = g_bufferSizes.maxIndex;
      g_bufferSizes.deviceStorage = g_bufferSizes.maxStorage;
    } else {
      g_bufferSizes.deviceVertex = g_bufferSizes.vertex;
      g_bufferSizes.deviceUniform =
          std::min(g_bufferSizes.maxUniform, std::max<uint64_t>(g_bufferSizes.uniform, MinDeviceUniformSize));
      g_bufferSizes.deviceIndex = g_bufferSizes.index;
      g_bufferSizes.deviceStorage = g_bufferSizes.storage;
    }
    Log.info("Frame buffers: staging {}/{}/{}/{}+{} MiB (vertex/uniform/index/storage+textures), device {}/{}/{}/{} "
             "KiB, frame limit {}/{}/{}/{} MiB",
             g_bufferSizes.vertex >> 20, g_bufferSizes.uniform >> 20, g_bufferSizes.index >> 20,
             g_bufferSizes.storage >> 20, g_bufferSizes.textureUpload >> 20, g_bufferSizes.deviceVertex >> 10,
             g_bufferSizes.deviceUniform >> 10, g_bufferSizes.deviceIndex >> 10, g_bufferSizes.deviceStorage >> 10,
             g_bufferSizes.maxVertex >> 20, g_bufferSizes.maxUniform >> 20, g_bufferSizes.maxIndex >> 20,
             g_bufferSizes.maxStorage >> 20);
  }

  const auto createBuffer = [](wgpu::Buffer& out, wgpu::BufferUsage usage, uint64_t size, const char* label) {
    if (size <= 0) {
      return;
    }
    validate_mixed_buffer_size(size, (usage & wgpu::BufferUsage::Storage) != wgpu::BufferUsage::None, label);
    const wgpu::BufferDescriptor descriptor{
        .label = label,
        .usage = usage,
        .size = size,
    };
    out = g_device.CreateBuffer(&descriptor);
  };
  for (const auto& buffer : SharedBuffers) {
    createBuffer(g_resources.*buffer.member, buffer.usage,
                 fetch_aligned_buffer_size(buffer.kind,
                                           g_bufferSizes.base(buffer.kind) + g_bufferSizes.device(buffer.kind)),
                 buffer.label);
  }
  if (g_vertexFetchTexture) {
    const uint64_t vertexBytes = g_bufferSizes.base(StagingKind::Vertex) + g_bufferSizes.deviceVertex;
    const uint64_t storageBytes = g_bufferSizes.base(StagingKind::Storage) + g_bufferSizes.deviceStorage;
    create_fetch_texture(StagingKind::Vertex, vertexBytes);
    create_fetch_texture(StagingKind::Storage, storageBytes);
    // The new buffers are zeroed, and a read past the data has to see that (the storage buffers
    // read 0 there), so the textures start as their copies rather than as undefined content.
    const wgpu::CommandEncoderDescriptor encoderDescriptor{
        .label = "Vertex fetch textures",
    };
    auto encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
    upload_fetch_texture_rows(encoder, StagingKind::Vertex, 0, vertexBytes);
    upload_fetch_texture_rows(encoder, StagingKind::Storage, 0, storageBytes);
    auto commands = encoder.Finish();
    g_queue.Submit(1, &commands);
    Log.info("Texture vertex fetch: {} rows of vertices, {} rows of arrays", vertexBytes / FetchTextureRowBytes,
             storageBytes / FetchTextureRowBytes);
  }
  for (size_t i = 0; i < g_stagingBuffers.size(); ++i) {
    const auto label = fmt::format("Staging Buffer {}", i);
    createBuffer(g_stagingBuffers[i], wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc, g_bufferSizes.staging(),
                 label.c_str());
  }
  for (auto& state : g_mappingStates) {
    state.store(BufferMapState::Unmapped, std::memory_order_release);
  }
  for (size_t slot = 0; slot < g_stagingBuffers.size(); ++slot) {
    map_staging_buffer(slot);
  }

#ifdef AURORA_H700
  const auto fetchSamplerDescriptor = fetch_sampler_descriptor();
  g_resources.fetchSampler = g_device.CreateSampler(&fetchSamplerDescriptor);
#endif
  {
    const auto layoutEntries = static_bind_group_layout_entries(g_vertexFetchTexture);
    const wgpu::BindGroupLayoutDescriptor layoutDesc{
        .label = "Static bind group layout",
        .entryCount = layoutEntries.size(),
        .entries = layoutEntries.data(),
    };
    g_resources.staticBindGroupLayout = g_device.CreateBindGroupLayout(&layoutDesc);
    create_static_bind_group();
  }

  {
    const auto layoutEntries = uniform_bind_group_layout_entries();
    const wgpu::BindGroupLayoutDescriptor layoutDesc{
        .label = "Uniform bind group layout",
        .entryCount = layoutEntries.size(),
        .entries = layoutEntries.data(),
    };
    g_resources.uniformBindGroupLayout = g_device.CreateBindGroupLayout(&layoutDesc);
    if constexpr (g_mixedFetch) {
      const auto drawEntries = uniform_bind_group_layout_entries<true>(sizeof(gx::DrawImmediateData));
      const wgpu::BindGroupLayoutDescriptor drawDesc{
          .label = "Draw uniform bind group layout",
          .entryCount = drawEntries.size(),
          .entries = drawEntries.data(),
      };
      g_resources.drawUniformBindGroupLayout = g_device.CreateBindGroupLayout(&drawDesc);
    }
    create_uniform_bind_group();
  }
#ifdef AURORA_H700
  if (!initialize_frame_dest_ring()) return;
#endif

  gx::initialize();
  if (gx::decoded::enabled())
    Log.info("H700 PERF12 retail geometry enabled: source_budget_mib=32 gpu_budget_mib=64 scratch_mib=16; native slots required");
#ifdef AURORA_ENABLE_RMLUI
  rmlui::initialize_pipeline();
#endif
  initialize_pipeline_cache();
}

bool shutdown() {
#ifdef AURORA_H700
  if (!g_frameInitialized) return true;
#endif
  render_worker::synchronize();
  if (!render_worker::shutdown()) return false;
  if (!render_worker::is_quiescent()) return false; // producer-side rejected work may still be unwinding
  while (!retire_frame_resources_after_quiescence()) {
    if (!render_worker::progress_wait()) {
      Log.error("Frame retirement incomplete; maps/packets/holders retained for IG-11");
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
#ifdef AURORA_H700
  if (!join_pipeline_cache_workers()) return false;
#endif
  return release_frame_holders_after_quiescence();
}

bool release_frame_holders_after_quiescence() {
#ifdef AURORA_H700
  if (!g_frameInitialized) return true;
  if (!gx::fifo::is_quiescent() || !render_worker::is_quiescent() || !pipeline_cache_threads_joined() ||
      pending_internal_maps() != 0) return false;
#endif
  g_processEventsQueued.store(false, std::memory_order_release);
  g_lastPresentNs.store(0, std::memory_order_release);
  g_presentPeriodNs.store(0, std::memory_order_release);
  g_cpuFrameTimeNs.store(0, std::memory_order_release);
  g_cpuFrameStart = {};
  {
    std::lock_guard lock{g_presentStatsMutex};
    g_presentTimes.clear();
  }
#ifdef AURORA_H700
  if (!release_pipeline_cache_holders()) return false;
#else
  if (!shutdown_pipeline_cache()) return false;
#endif
  if (!vis_query::detail::shutdown()) return false;
  depth_peek::shutdown();
  tex_copy_conv::shutdown();
  probe::shutdown();
  bloom::shutdown();
  volfog::shutdown();
  xray::shutdown();
  thermal::shutdown();
  shadow::shutdown();
  actor_shadow::shutdown();
  vfx::shutdown();
  water::shutdown();
  envfx::shutdown();
  tex_palette_conv::shutdown();
  texture_replacement::shutdown();
#ifdef AURORA_H700
  // Same guarded holder phase as destinations: packet reset/afterSubmit are too early.
  g_geometrySlotPins.fill({});
  for (auto& packet : g_framePackets) packet.geometryPins.clear();
  g_geometryGeneration = 0;
  g_geometryLastFrame.reset();
#endif
  gx::decoded::shutdown();
  gx::fifo::clear_immutable_bindings();
  gx::shutdown();
#ifdef AURORA_ENABLE_RMLUI
  rmlui::shutdown_pipeline();
#endif

  shutdown_resource_cache();
  {
    std::lock_guard lock{g_runtimeTypeMutex};
    g_runtimeDrawTypes.clear();
    g_freeDrawTypeSlots.clear();
    g_runtimeEncoderTaskTypes.clear();
    g_freeEncoderTaskTypeSlots.clear();
  }
  g_resources.vertexBuffer = {};
  g_resources.uniformBuffer = {};
  g_resources.indexBuffer = {};
  g_resources.storageBuffer = {};
  g_resources.vertexFetchTexture = {};
  g_resources.vertexFetchTextureView = {};
  g_resources.storageFetchTexture = {};
  g_resources.storageFetchTextureView = {};
  g_stagingBuffers.fill({}); // map retirement above unmapped and settled every request
  for (auto& packet : g_framePackets) {
    packet = {};
  }
  for (auto& owned : g_frameOwned) owned.store(false, std::memory_order_release);
  shutdown_recording();
  g_resources.staticBindGroup = {};
  g_resources.staticBindGroupLayout = {};
#ifdef AURORA_H700
  g_resources.fetchSampler = {};
#endif
  g_resources.uniformBindGroup = {};
  g_resources.uniformBindGroupLayout = {};
  g_resources.drawUniformBindGroup = {};
  g_resources.drawUniformBindGroupLayout = {};
#ifdef AURORA_H700
  g_frameDestRing.fill({});
  g_frameDestRingState = h700::FrameDestRingState{};
#endif
  g_frameIndex = UINT32_MAX;
  g_frameSlots.reset();
  g_stagingSlots.reset();
  for (auto& state : g_mappingStates) {
    state.store(BufferMapState::Unmapped, std::memory_order_release);
  }
#ifdef AURORA_H700
  g_frameInitialized = false;
#endif
  return true;
}

bool wait_for_staging_buffer(size_t slot) {
  ZoneScopedN("Wait for buffer map");
  const auto initial = g_mappingStates[slot].load(std::memory_order_acquire);
  if (initial == BufferMapState::Mapped && !staging_callback_pending(slot) && render_worker::wait_healthy()) return true;
  if (initial == BufferMapState::Failed || !render_worker::wait_healthy()) {
    if (initial == BufferMapState::Mapping || staging_callback_pending(slot)) {
      if (auto owner = g_mapOwnership[slot].load(std::memory_order_acquire)) owner->cancel_wait();
    } else g_stagingSlots.release(slot);
    return false;
  }
  const auto ownership = map_staging_buffer(slot);
  while (true) {
    const auto mappingState = g_mappingStates[slot].load(std::memory_order_acquire);
    if (mappingState == BufferMapState::Mapped && !staging_callback_pending(slot) && render_worker::wait_healthy()) return true;
    if (mappingState == BufferMapState::Unmapped || mappingState == BufferMapState::Failed ||
        !wait_for_gpu_progress(std::chrono::milliseconds{1})) {
      if (ownership) ownership->cancel_wait();
      else g_stagingSlots.release(slot);
      return false;
    }
  }
}

std::optional<size_t> acquire_frame_slot() {
  ZoneScopedN("Acquire frame slot");
  const auto waitStart = PresentClock::now();
  while (render_worker::wait_healthy()) {
    if (const auto slot = g_frameSlots.try_acquire()) {
      const auto waitDuration = PresentClock::now() - waitStart;
      const double waitMs = std::chrono::duration<double, std::milli>{waitDuration}.count();
      TracyPlot("aurora: frameSlotWaitMs", waitMs);
      return *slot;
    }
    if (!wait_for_gpu_progress(std::chrono::microseconds{100})) break;
  }
  return std::nullopt;
}

std::optional<size_t> acquire_mapped_staging_buffer() {
  ZoneScopedN("Acquire mapped staging buffer");
  while (render_worker::wait_healthy()) {
    if (auto slot = g_stagingSlots.try_acquire()) {
      if (wait_for_staging_buffer(*slot)) {
        return *slot;
      }
      return std::nullopt; // map completion owns a cancelled waiter's release
    }
    if (!wait_for_gpu_progress(std::chrono::microseconds{100})) break;
  }
  return std::nullopt;
}

void request_staging_growth(StagingKind kind, uint64_t peak) noexcept {
  auto& want = g_stagingGrowthWant[static_cast<size_t>(kind)];
  uint64_t seen = want.load(std::memory_order_relaxed);
  while (peak > seen && !want.compare_exchange_weak(seen, peak, std::memory_order_acq_rel)) {
  }
}

bool grow_frame_buffer(StagingKind kind, uint64_t need, size_t frameSlot) {
  ZoneScopedN("Grow frame buffer");
  // On the render worker, after everything queued before: what this frame encoded so far reads
  // the old buffer, then the copy makes the new one hold all of it (resident region included,
  // which stays at the same offsets) before anything encoded later reads or writes it.
  const auto accepted = render_worker::enqueue_work([kind, need, frameSlot] {
    // Resolve only after the ordered BeginFrame job created it. A cancelled waiter
    // leaves the packet owned, and this running job holds its own encoder reference.
    const auto encoder = g_framePackets[frameSlot].encoder;
    if (!encoder) { fail_frame_work(frameSlot); return; }
    const auto& shared = SharedBuffers[static_cast<size_t>(kind)];
    uint64_t& device = kind == StagingKind::Vertex    ? g_bufferSizes.deviceVertex
                       : kind == StagingKind::Uniform ? g_bufferSizes.deviceUniform
                       : kind == StagingKind::Index   ? g_bufferSizes.deviceIndex
                                                      : g_bufferSizes.deviceStorage;
    if (need <= device) {
      return;
    }
#ifdef AURORA_H700
    fallback_frame_dest_ring(h700::frame_dest_ring_growth_reason(kind));
#endif
    const uint64_t base = g_bufferSizes.base(kind);
    const uint64_t grown = std::min(std::bit_ceil(need), g_bufferSizes.limit(kind));
    const wgpu::BufferDescriptor descriptor{
        .label = shared.label,
        .usage = shared.usage,
        .size = fetch_aligned_buffer_size(kind, base + grown),
    };
    validate_mixed_buffer_size(descriptor.size, kind == StagingKind::Storage, shared.label);
    wgpu::Buffer buffer = g_device.CreateBuffer(&descriptor);
    wgpu::Buffer& current = g_resources.*shared.member;
    encoder.CopyBufferToBuffer(current, 0, buffer, 0, base + device);
    Log.info("frame {} device buffer grown from {} to {} KiB during frame {} (needed {} KiB)", shared.name,
             device >> 10, grown >> 10, g_frameIndex, need >> 10);
    current = std::move(buffer);
    device = grown;
    if (g_vertexFetchTexture && (kind == StagingKind::Vertex || kind == StagingKind::Storage)) {
      // The fetch texture is the new buffer, so it is the size of the buffer rather than of the
      // part in use; the buffer copy above already put the old contents at the same offsets.
      create_fetch_texture(kind, base + grown);
      upload_fetch_texture_rows(encoder, kind, 0, base + grown);
    }
    if (kind == StagingKind::Uniform) {
      create_uniform_bind_group();
    } else if (kind != StagingKind::Index) {
      create_static_bind_group();
    }
    // Cached bind groups that hold the old buffer are no longer looked up; drop them soon so
    // the old buffer goes too.
    expire_unused_bind_groups_soon();
  });
  // The shutdown tail retains these ordered growth jobs ahead of its encoding. CPU
  // recording uses base/limit, not the not-yet-grown device size; no waiter may depend
  // on a tail that has not been admitted yet. Ordinary recording keeps its old sync.
  return accepted == render_worker::Outcome::Completed &&
         (render_worker::capturing_frame_tail() || try_gpu_synchronize());
}

namespace {
// Grows the staging buffers a frame spilled out of, once every frame in flight is done with them.
bool grow_staging(const std::array<uint64_t, 4>& want) {
  ZoneScopedN("Grow staging");
  if (!try_gpu_synchronize()) return false;
  std::array<std::optional<render_worker::SlotLease>, StagingBufferCount> leases;
  // Holding every staging slot means no frame or pending map uses a buffer.
  for (size_t n = 0; n < StagingBufferCount; ++n) {
    const auto slot = g_stagingSlots.acquire_wait();
    if (!slot) return false;
    leases[n].emplace(g_stagingSlots, *slot);
  }
  struct Kind {
    const char* name;
    uint64_t& size;
    uint64_t limit;
  };
  const Kind kinds[] = {
      {"vertex", g_bufferSizes.vertex, g_bufferSizes.maxVertex},
      {"uniform", g_bufferSizes.uniform, g_bufferSizes.maxUniform},
      {"index", g_bufferSizes.index, g_bufferSizes.maxIndex},
      {"storage", g_bufferSizes.storage, g_bufferSizes.maxStorage},
  };
  for (size_t k = 0; k < 4; ++k) {
    if (want[k] <= kinds[k].size) {
      continue;
    }
    const uint64_t grown = std::min(std::bit_ceil(want[k]), kinds[k].limit);
    Log.info("frame {} staging buffers grown from {} to {} KiB after a frame spilled out of them (peak {} KiB)",
             kinds[k].name, kinds[k].size >> 10, grown >> 10, want[k] >> 10);
    kinds[k].size = grown;
  }
  for (size_t i = 0; i < g_stagingBuffers.size(); ++i) {
    // A buffer still mapping completes with an error; wait it out before replacing it.
    while (g_mappingStates[i].load(std::memory_order_acquire) == BufferMapState::Mapping || staging_callback_pending(i)) {
      if (!wait_for_gpu_progress(std::chrono::milliseconds{1})) return false;
    }
    if (!render_worker::wait_healthy()) return false;
    if (g_mappingStates[i].load(std::memory_order_acquire) == BufferMapState::Mapped) {
      g_stagingBuffers[i].Unmap();
    }
    g_mappingStates[i].store(BufferMapState::Unmapped, std::memory_order_release);
    const wgpu::BufferDescriptor descriptor{
        .label = "Staging Buffer",
        .usage = wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc,
        .size = g_bufferSizes.staging(),
    };
    g_stagingBuffers[i] = g_device.CreateBuffer(&descriptor);
  }
  return render_worker::wait_healthy(); // leases release every acquired slot, also on partial failure
}

// Takes the limits frameBuffersDeferred held back. With every staging slot held no frame is in
// flight, so the device buffers are replaced whole at the new limits between two frames, with
// nothing of any frame to copy: the mid-frame grow lost an Adreno 618's device (issue #165).
bool reserve_held_frame_buffers() {
  ZoneScopedN("Reserve frame buffers");
  auto& s = g_bufferSizes;
  if (s.heldVertex == 0) return true;
  if (!try_gpu_synchronize()) return false;
  std::array<std::optional<render_worker::SlotLease>, StagingBufferCount> leases;
  for (size_t n = 0; n < StagingBufferCount; ++n) {
    const auto slot = g_stagingSlots.acquire_wait();
    if (!slot) return false;
    leases[n].emplace(g_stagingSlots, *slot);
  }
  s.maxVertex = s.deviceVertex = s.heldVertex;
  s.maxUniform = s.deviceUniform = s.heldUniform;
  s.maxIndex = s.deviceIndex = s.heldIndex;
  s.maxStorage = s.deviceStorage = s.heldStorage;
  s.heldVertex = s.heldUniform = s.heldIndex = s.heldStorage = 0;
  g_heldFrameBuffersTaken = true;
  const auto accepted = render_worker::enqueue_work([] {
#ifdef AURORA_H700
    fallback_frame_dest_ring(h700::FrameDestRingFallback::DeferredReplacement);
#endif
    for (const auto& buffer : SharedBuffers) {
      const wgpu::BufferDescriptor descriptor{
          .label = buffer.label,
          .usage = buffer.usage,
          .size = fetch_aligned_buffer_size(buffer.kind,
                                            g_bufferSizes.base(buffer.kind) + g_bufferSizes.device(buffer.kind)),
      };
      validate_mixed_buffer_size(descriptor.size, buffer.kind == StagingKind::Storage, buffer.label);
      g_resources.*buffer.member = g_device.CreateBuffer(&descriptor);
    }
    if (g_vertexFetchTexture) {
      const uint64_t vertexBytes = g_bufferSizes.base(StagingKind::Vertex) + g_bufferSizes.deviceVertex;
      const uint64_t storageBytes = g_bufferSizes.base(StagingKind::Storage) + g_bufferSizes.deviceStorage;
      create_fetch_texture(StagingKind::Vertex, vertexBytes);
      create_fetch_texture(StagingKind::Storage, storageBytes);
      const wgpu::CommandEncoderDescriptor encoderDescriptor{
          .label = "Vertex fetch textures",
      };
      auto encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
      upload_fetch_texture_rows(encoder, StagingKind::Vertex, 0, vertexBytes);
      upload_fetch_texture_rows(encoder, StagingKind::Storage, 0, storageBytes);
      auto commands = encoder.Finish();
      g_queue.Submit(1, &commands);
    }
    create_uniform_bind_group();
    create_static_bind_group();
    // Cached bind groups that hold the old buffers are no longer looked up.
    expire_unused_bind_groups_soon();
  });
  if (accepted != render_worker::Outcome::Completed || !try_gpu_synchronize()) return false;
  Log.info("Frame buffers: took the held limits, device {}/{}/{}/{} MiB (vertex/uniform/index/storage)",
           s.deviceVertex >> 20, s.deviceUniform >> 20, s.deviceIndex >> 20, s.deviceStorage >> 20);
  return true;
}
} // namespace

#ifdef AURORA_H700
bool begin_frame(std::optional<h700::FrameDescriptor> nativeFrame) {
#else
bool begin_frame() {
#endif
  ZoneScoped;
  if (!render_worker::root_admission_open()) return false;
  {
    std::array<uint64_t, 4> want{};
    bool grow = false;
    const uint64_t sizes[] = {g_bufferSizes.vertex, g_bufferSizes.uniform, g_bufferSizes.index, g_bufferSizes.storage};
    for (size_t k = 0; k < 4; ++k) {
      want[k] = g_stagingGrowthWant[k].exchange(0, std::memory_order_acq_rel);
      grow = grow || want[k] > sizes[k];
    }
    if (grow) {
      if (!grow_staging(want)) return false;
    }
  }
  if (g_reserveHeldFrameBuffers.exchange(false, std::memory_order_acq_rel)) {
    if (!reserve_held_frame_buffers()) return false;
  }
  // pace_frame_start();
  const auto acquiredFrame = acquire_frame_slot();
  if (!acquiredFrame) return false;
  const size_t frameSlot = *acquiredFrame;
  render_worker::SlotLease frameLease(g_frameSlots, frameSlot);
  const auto stagingSlot = acquire_mapped_staging_buffer();
  if (!stagingSlot) return false;
  render_worker::SlotLease stagingLease(g_stagingSlots, *stagingSlot);

  auto& frame = g_framePackets[frameSlot];
  frame = {};
  frame.frameId = g_nextFrameId++;
  frame.frameIndex = g_frameIndex;
  frame.stagingBuffer = *stagingSlot;
#ifdef AURORA_H700
  if (gx::decoded::enabled() && nativeFrame && nativeFrame->slot < g_geometrySlotPins.size())
    frame.geometryNativeSlot = nativeFrame->slot;
#endif
  size_t bufferOffset = 0;
  const auto& stagingBuf = g_stagingBuffers[*stagingSlot];
  const auto mapBuffer = [&](ByteBuffer& buf, uint64_t size) {
    if (size <= 0) {
      return;
    }
    buf = ByteBuffer{static_cast<u8*>(stagingBuf.GetMappedRange(bufferOffset, size)), static_cast<size_t>(size)};
    bufferOffset += size;
  };
  mapBuffer(frame.verts, g_bufferSizes.vertex);
  mapBuffer(frame.uniforms, g_bufferSizes.uniform);
  mapBuffer(frame.indices, g_bufferSizes.index);
  mapBuffer(frame.storage, g_bufferSizes.storage);
  if constexpr (UseTextureBuffer) {
    mapBuffer(frame.textureUpload, g_bufferSizes.textureUpload);
  }

  g_frameFailed[frameSlot].store(false, std::memory_order_release);
  g_frameOwned[frameSlot].store(true, std::memory_order_release);
  const auto accepted = render_worker::enqueue_begin_frame(frame.frameId, [frameSlot
#ifdef AURORA_H700
      , nativeFrame
#endif
  ] {
#ifdef AURORA_H700
    if (!select_frame_dest_ring(nativeFrame, frameSlot)) return;
    if (gx::decoded::enabled() && nativeFrame) {
      if (!h700::native_slot(*nativeFrame) || nativeFrame->generation == 0 ||
          nativeFrame->slot >= g_geometrySlotPins.size() ||
          (g_geometryLastFrame && (nativeFrame->generation != g_geometryGeneration ||
                                  nativeFrame->frame <= *g_geometryLastFrame))) {
        Log.error("PERF12 invalid native geometry reservation");
        fail_frame_work(frameSlot);
        render_worker::notify_failure();
        return;
      }
      // native_slot proves the existing completion/reverse-fence reservation is current.
      // FIFO/render jobs are ordered; the previous packet's pins transferred before this.
      g_geometrySlotPins[nativeFrame->slot].clear();
      g_geometryGeneration = nativeFrame->generation;
      g_geometryLastFrame = nativeFrame->frame;
    }
#endif
    constexpr wgpu::CommandEncoderDescriptor EncoderDescriptor{.label = "Redraw encoder"};
#ifdef AURORA_H700
    g_framePackets[frameSlot].encoder = h700::diagnostic::dawn_call(h700::diagnostic::Kind::RenderEncoder,
        g_framePackets[frameSlot].frameIndex, [&] { return g_device.CreateCommandEncoder(&EncoderDescriptor); });
#else
    g_framePackets[frameSlot].encoder = g_device.CreateCommandEncoder(&EncoderDescriptor);
#endif
    webgpu::gpu_prof::frame_begin(g_framePackets[frameSlot].encoder);
    overdraw::frame_begin();
  }, [frameSlot] { fail_frame_work(frameSlot); });
  if (accepted != render_worker::Outcome::Completed) {
    frame = {};
    g_frameOwned[frameSlot].store(false, std::memory_order_release);
    return false;
  }
  frameLease.disarm();
  stagingLease.disarm();
  // After acceptance, even a cancelled waiter cannot release a packet an executing job owns.
  if (!render_worker::wait_healthy()) return false;
  begin_recording(frame, frameSlot);
  begin_pipeline_frame();
  g_cpuFrameStart = PresentClock::now();
  return true;
}

bool end_frame(EndFrameCallback callback, std::function<void()> preSubmitAbort) {
  ZoneScoped;
  if (g_cpuFrameStart.time_since_epoch().count() != 0) {
    const auto cpuFrameTime = PresentClock::now() - g_cpuFrameStart;
    update_ema(g_cpuFrameTimeNs, duration_ns(cpuFrameTime));
    const double cpuFrameTimeMs = std::chrono::duration<double, std::milli>{cpuFrameTime}.count();
    TracyPlot("aurora: cpuFrameTimeMs", cpuFrameTimeMs);
  }
  const auto recorded = end_recording();
  auto& frame = *recorded.packet;
  const size_t frameSlot = recorded.frameSlot;
  const uint64_t frameId = frame.frameId;
  end_pipeline_frame();
  ++g_frameIndex;

  const size_t stagingSlot = frame.stagingBuffer;
  auto cancel = [frameSlot, abort = std::move(preSubmitAbort)] {
    fail_frame_work(frameSlot);
    if (abort) abort(); // internal native reservation rollback, not a success callback
  };
  const auto accepted = render_worker::enqueue_end_frame(frameId,
      [frameSlot, stagingSlot, callback = std::move(callback), cancel]() mutable {
    if (frame_work_failed(frameSlot)) { cancel(); return; }
    auto& packet = g_framePackets[frameSlot];
    const uint32_t finishedFrame = packet.frameIndex;
    g_stagingBuffers[stagingSlot].Unmap();
    g_mappingStates[stagingSlot].store(BufferMapState::Unmapped, std::memory_order_release);
    auto encoder = std::move(packet.encoder);
    const auto stats = packet.stats;
    auto afterSubmitCallbacks = std::move(packet.afterSubmitCallbacks);
#ifdef AURORA_H700
    if (!packet.geometryPins.empty()) {
      AURORA_ASSERT(packet.geometryNativeSlot < g_geometrySlotPins.size(), "PERF12 pins without native owner");
      auto& pins = g_geometrySlotPins[packet.geometryNativeSlot];
      AURORA_ASSERT(pins.empty(), "PERF12 native slot reused before guarded begin");
      pins = std::move(packet.geometryPins);
    }
#endif
    packet = {};
    g_resources.stats.drawCallCount = stats.drawCallCount;
    g_resources.stats.mergedDrawCallCount = stats.mergedDrawCallCount;
    g_resources.stats.renderPassCount = stats.renderPassCount;
    g_resources.stats.lastVertSize = stats.lastVertSize;
    g_resources.stats.lastUniformSize = stats.lastUniformSize;
    g_resources.stats.lastIndexSize = stats.lastIndexSize;
    g_resources.stats.lastStorageSize = stats.lastStorageSize;
    g_resources.stats.lastTextureUploadSize = stats.lastTextureUploadSize;
    if (callback) {
      callback(encoder, std::move(afterSubmitCallbacks));
    }
    g_frameOwned[frameSlot].store(false, std::memory_order_release);
    g_frameSlots.release(frameSlot);
    expire_cached_bind_groups(finishedFrame);
    if (render_worker::wait_healthy()) map_staging_buffer(stagingSlot, true);
    process_events();
  }, cancel);
  return accepted == render_worker::Outcome::Completed;
}

uint32_t current_frame() noexcept { return g_frameIndex; }

void after_submit() noexcept {
  depth_peek::after_submit();
  bloom::after_submit();
}

bool try_gpu_synchronize() {
  if (render_worker::is_quiescent()) return render_worker::wait_healthy();
  return render_worker::synchronize() == render_worker::Outcome::Completed;
}
bool gpu_synchronize() { return try_gpu_synchronize(); }

void synchronize() { (void)try_gpu_synchronize(); } // public ABI; internal mutation sites use the status-bearing path

bool frame_work_failed(size_t slot) noexcept {
  return slot >= FrameSlotCount || g_frameFailed[slot].load(std::memory_order_acquire) || !render_worker::wait_healthy();
}
void fail_frame_work(size_t slot) noexcept {
  if (slot < FrameSlotCount) g_frameFailed[slot].store(true, std::memory_order_release);
#ifdef AURORA_H700
  if (auto state = h700::presentation_state()) state->fail("Render frame continuation failed or was rejected");
#endif
  render_worker::notify_failure();
}
size_t pending_staging_maps() noexcept {
  size_t count = 0;
  for (size_t i = 0; i < StagingBufferCount; ++i)
    if (g_mappingStates[i].load(std::memory_order_acquire) == BufferMapState::Mapping || staging_callback_pending(i)) ++count;
  return count;
}

size_t pending_internal_maps() noexcept {
  return pending_staging_maps() + webgpu::gpu_prof::pending_maps() + vis_query::detail::pending_maps();
}

bool retire_frame_resources_after_quiescence() {
  if (g_frameOwner != std::thread::id{} && g_frameOwner != std::this_thread::get_id()) return false;
  if (!gx::fifo::is_quiescent() || !render_worker::is_quiescent()) return false;
  g_mapsRetiring = true;
  // Lifecycle poll: callbacks must still retire after terminal failure. No new map or Submit.
  if (g_instance) g_instance.ProcessEvents();
  webgpu::gpu_prof::poll_maps();
  vis_query::detail::poll_maps();
  if (pending_internal_maps() != 0) return false;
  for (size_t i = 0; i < StagingBufferCount; ++i) {
    if (g_mappingStates[i].load(std::memory_order_acquire) == BufferMapState::Mapped) g_stagingBuffers[i].Unmap();
    if (auto completion = g_mapCompletions[i].load(std::memory_order_acquire)) (void)completion->retire();
    g_mapCompletions[i].store({}, std::memory_order_release);
    g_mapOwnership[i].store({}, std::memory_order_release);
    g_mappingStates[i].store(BufferMapState::Unmapped, std::memory_order_release);
  }
  // Reset an abandoned recorder without finish()/enqueue: its FIFO/worker users have joined.
  shutdown_recording();
  if (!retire_cancelled_frames_after_quiescence()) return false;
  return webgpu::gpu_prof::shutdown() && vis_query::detail::shutdown();
}
size_t pending_frame_packets() noexcept {
  size_t count = 0;
  for (const auto& owned : g_frameOwned) if (owned.load(std::memory_order_acquire)) ++count;
  return count;
}
bool retire_cancelled_frames_after_quiescence() {
  if (!gx::fifo::is_quiescent() || !render_worker::is_quiescent() || recording_active()) return false;
  for (size_t i = 0; i < FrameSlotCount; ++i) {
    if (!g_frameOwned[i].load(std::memory_order_acquire)) {
      g_framePackets[i] = {};
      g_frameFailed[i].store(false, std::memory_order_release);
      continue;
    }
    const auto staging = g_framePackets[i].stagingBuffer;
    if (g_mappingStates[staging].load(std::memory_order_acquire) == BufferMapState::Mapping || staging_callback_pending(staging))
      return false;
    g_framePackets[i] = {};
    g_frameOwned[i].store(false, std::memory_order_release);
    g_frameFailed[i].store(false, std::memory_order_release);
    g_frameSlots.release(i);
    g_stagingSlots.release(staging);
  }
  return true;
}

void after_present() noexcept {
  const auto now = PresentClock::now();
  const int64_t nowNs = timestamp_ns(now);
  const int64_t previousPresentNs = g_lastPresentNs.exchange(nowNs, std::memory_order_acq_rel);
  if (previousPresentNs != 0) {
    update_ema(g_presentPeriodNs, nowNs - previousPresentNs);
    const double presentPeriodMs = static_cast<double>(g_presentPeriodNs.load(std::memory_order_acquire)) / 1'000'000.0;
    TracyPlot("aurora: presentPeriodMs", presentPeriodMs);
  }
  std::lock_guard lock{g_presentStatsMutex};
  g_presentTimes.push_back(now);
  prune_present_times(now);
}

float calculate_fps() noexcept {
  const auto now = PresentClock::now();
  std::lock_guard lock{g_presentStatsMutex};
  prune_present_times(now);
  if (g_presentTimes.size() < 2) {
    return 0.f;
  }

  const auto elapsed = std::chrono::duration<float>(g_presentTimes.back() - g_presentTimes.front()).count();
  if (elapsed <= 0.f) {
    return 0.f;
  }
  return static_cast<float>(g_presentTimes.size() - 1) / elapsed;
}
} // namespace aurora::gfx

const AuroraStats* aurora_get_stats() { return &aurora::gfx::detail::resources().stats; }

uint32_t aurora_get_frame_buffer_scale() {
  return static_cast<uint32_t>(aurora::gfx::frame_buffer_sizes().scaledVertex / aurora::gfx::VertexBufferSize);
}
void aurora_reserve_frame_buffers() {
  aurora::gfx::g_reserveHeldFrameBuffers.store(true, std::memory_order_release);
}
uint32_t aurora_get_frame_storage_scale() {
  return static_cast<uint32_t>(aurora::gfx::frame_buffer_sizes().scaledStorage / aurora::gfx::StorageBufferSize);
}
uint32_t aurora_get_resident_geometry_mib() {
  const auto& sizes = aurora::gfx::frame_buffer_sizes();
  return static_cast<uint32_t>((sizes.residentVertex + sizes.residentIndex + sizes.residentStorage) >> 20);
}
float aurora_get_fps() { return aurora::gfx::calculate_fps(); }
void aurora_get_texture_support(bool* bc, bool* astc) {
  *bc = aurora::webgpu::g_bcTexturesSupported;
  *astc = aurora::webgpu::g_astcTexturesSupported;
}
