#pragma once

#include "../internal.hpp"

#include <atomic>
#include <cstring>

namespace aurora::gx::fifo {

namespace detail {
extern std::atomic_bool sFailed;
extern bool sWritesOpen;
extern uint8_t* sBufferData;
extern uint32_t sBufferSize;
extern uint32_t sBufferCapacity;
extern bool sInDisplayList;
extern uint8_t* sDlBuffer;
extern uint32_t sDlSize;
extern uint32_t sDlWritePos;
} // namespace detail

enum class ProcessingMode : uint8_t {
  // Process FIFO synchronously at drain()
  Drain,
  // Process FIFO synchronously at publish() (useful for profiling)
  Inline,
  // Process FIFO asynchronously on a worker thread; drain() synchronizes
  Thread
};
ProcessingMode processing_mode() noexcept;

bool init();
// IG-11: finish accepted producers/drain BEFORE shutdown, keeping downstream admission open.
// This joins the worker, not an unpublished tail. False retains processor/callback inputs.
bool shutdown();
enum class PollStatus { Pending, Complete, Failed };
struct DrainRequest {
  uint64_t target = 0;
  bool requested = false;
  bool complete = false;
};
void request_drain(DrainRequest& request); // freeze/publish accepted tail once; no buffer lock/wait
PollStatus poll_drain(DrainRequest& request); // try-lock only after actual processed watermark
void request_shutdown();
PollStatus poll_shutdown(); // join only after worker exit/unbind acknowledgement
bool is_quiescent() noexcept; // worker joined only; not FIFO/frame/compiler/map retirement
bool failed() noexcept;
uint64_t published_bytes() noexcept;
uint64_t processed_bytes() noexcept;

bool begin_frame() noexcept;
bool end_frame() noexcept;

// Whether a frame is currently being recorded, i.e. publish() will be honored.
bool frame_active() noexcept;

// Out-of-line slow path: grows internal buffer then appends data
bool write_data_grow(const void* data, uint32_t length);

// Main producer only, outside display-list recording. Appends a complete record
// or nothing, never publishes. True transfers accepted ownership even if a later
// backend failure leaves the record unprocessed; root must then retire its ticket.
bool write_checked(const void* data, uint32_t length) noexcept;
// One GX_AURORA REGISTER/RELEASE_IMMUTABLE_SOURCE record: u8 opcode, BE u16 command,
// BE u64 non-reused source ID. Rejection leaves all FIFO bytes/watermarks unchanged.
bool enqueue_ownership(uint16_t command, uint64_t sourceId) noexcept;

inline void write_data(const void* data, const uint32_t length) {
  if (detail::sFailed.load(std::memory_order_acquire) || !detail::sWritesOpen) return;
  if (!detail::sInDisplayList)
    LIKELY {
      if (length <= detail::sBufferCapacity - detail::sBufferSize)
        LIKELY {
          std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
          detail::sBufferSize += length;
          return;
        }
      write_data_grow(data, length);
    }
  else if (length <= detail::sDlSize - detail::sDlWritePos) {
    std::memcpy(detail::sDlBuffer + detail::sDlWritePos, data, length);
    detail::sDlWritePos += length;
  }
}

inline void write_u8(const uint8_t val) {
  if (detail::sFailed.load(std::memory_order_acquire) || !detail::sWritesOpen) return;
  if (!detail::sInDisplayList)
    LIKELY {
      if (detail::sBufferSize < detail::sBufferCapacity)
        LIKELY {
          detail::sBufferData[detail::sBufferSize++] = val;
          return;
        }
      write_data_grow(&val, 1);
    }
  else if (detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = val;
  }
}

inline void write_u16(const uint16_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u32(const uint32_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u64(const uint64_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_f32(const float val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

// Overwrites an unpublished u32 previously written at the given offset.
void patch_u32(uint32_t offset, uint32_t val);

// Marks a complete draw and publishes when the configured draw batch is full.
void finish_draw() noexcept;

// Makes commands written so far available to the FIFO processor.
void publish() noexcept;

using DrawDoneCallback = void (*)();
DrawDoneCallback set_draw_done_callback(DrawDoneCallback callback) noexcept;

// Display list recording
void begin_display_list(uint8_t* buf, uint32_t size);
uint32_t end_display_list();
bool in_display_list();

// Includes the unpublished tail. False preserves the buffer and watermarks for quiescence;
// callers must not proceed with recorder/state mutation as if the range had completed.
bool drain();

// Internal buffer inspection
const uint8_t* get_buffer_data();
uint32_t get_buffer_size();
bool clear_buffer();

#ifdef AURORA_FIFO_TESTING
namespace testing {
bool try_buffer_lock();
}
#endif

} // namespace aurora::gx::fifo
