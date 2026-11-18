#pragma once

#include "resources.hpp"
#include "frame_packet.hpp"

#include <optional>

#ifdef AURORA_H700
#include "../h700/presentation_state.hpp"
#endif

namespace aurora::gfx::detail {

inline constexpr size_t FrameSlotCount = 2;
inline constexpr size_t StagingBufferCount = FrameSlotCount + 3;

const wgpu::Buffer& staging_buffer(size_t slot);

struct RegisteredDrawType {
  DrawCallback draw = nullptr;
  void* userdata = nullptr;
};

struct RegisteredEncoderTaskType {
  EncoderTaskCallback callback = nullptr;
  void* userdata = nullptr;
  EncoderTaskCompletionCallback afterSubmit = nullptr;
};

std::optional<RegisteredDrawType> find_runtime_draw_type(DrawTypeId id);
std::optional<RegisteredEncoderTaskType> find_runtime_encoder_task_type(EncoderTaskId id);

} // namespace aurora::gfx::detail

namespace aurora::gfx {
// The 8-bit display EFB (g_frameBuffer), whichever target scene_render_target_layout() reports.
RenderTargetLayout display_render_target_layout() noexcept;
void initialize();
bool shutdown(); // blocking compatibility wrapper
bool release_frame_holders_after_quiescence(); // no worker waits; false retains holders
#ifdef AURORA_H700
// Immutable native reservation; its completion/reuse is independent of the CPU packet slot.
bool begin_frame(std::optional<h700::FrameDescriptor> nativeFrame = std::nullopt);
#else
bool begin_frame();
#endif
bool end_frame(EndFrameCallback callback, std::function<void()> preSubmitAbort = {});
// Accepted packets are never cleared while queued/running work can still reference them.
bool frame_work_failed(size_t slot) noexcept;
void fail_frame_work(size_t slot) noexcept;
size_t pending_frame_packets() noexcept;
size_t pending_staging_maps() noexcept;
size_t pending_internal_maps() noexcept; // owner thread, or after producer/worker quiescence
// Main only, no external locks: after FIFO/render joins, process existing callbacks, unmap,
// reset per-frame/readback holders. False retains ownership; no extra Submit, no device release.
bool retire_frame_resources_after_quiescence();
// IG-11 only, after upstream producer quiescence, worker join and map retirement.
bool retire_cancelled_frames_after_quiescence();
bool try_gpu_synchronize();
uint32_t current_frame() noexcept;
void after_submit() noexcept;
bool gpu_synchronize();
void after_present() noexcept;
float calculate_fps() noexcept;
} // namespace aurora::gfx
