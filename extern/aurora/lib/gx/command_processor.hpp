#pragma once

#include "../internal.hpp"

#include <atomic>
#include <cstdint>

namespace aurora::gx::fifo {

inline constexpr uint32_t MaxImmutableListDepth = 16;

struct ProcessResult {
  uint32_t bytesProcessed;
  bool drawDone;
  // The outer shortcall is parsed but NOT retired while an immutable list is
  // suspended at DRAW_DONE. Resume it before processing any subsequent FIFO bytes.
  uint32_t pendingCallBytes = 0;
};

// Process GX FIFO commands until the next draw done event or end of buffer.
// bytesProcessed excludes a pending shortcall. Its callback must run outside the
// FIFO buffer lock; only resume_immutable_list may then retire that shortcall.
ProcessResult process(const uint8_t* data, uint32_t size) noexcept;
ProcessResult resume_immutable_list(uint32_t pendingCallBytes) noexcept;
void clear_draw_cache() noexcept;
// The frame's vertex array copies (see command_processor.cpp's sFrameArrays); the recording
// calls it when the frame's storage ends.
void clear_frame_arrays() noexcept;
// Guarded CPU-only release after FIFO/render/compiler quiescence. Drops bound
// and suspended-list views without resuming commands or releasing GPU holders.
void clear_immutable_bindings() noexcept;

} // namespace aurora::gx::fifo

namespace aurora::gx::testing {
// The vertex array overrun check (MP_GX_CHECK_ARRAYS, see command_processor.cpp): whether it runs,
// and how many overruns it has found. Tests set the flag rather than the environment.
extern std::atomic<bool> vertexArrayCheck;
extern std::atomic<uint32_t> vertexArrayOverruns;
void set_vertex_array_check(bool enabled) noexcept;
} // namespace aurora::gx::testing
