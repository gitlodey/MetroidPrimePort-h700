#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace aurora::h700 {

struct FrameWallTimings {
  using Clock = std::chrono::steady_clock;
  enum Field : size_t { Prepare, Submit, Queue, CompletionObserved, Present, Swap, Count };
  using Sample = std::array<double, Count>;

  Clock::time_point acquired = Clock::now();
  Clock::time_point submit_begin{}, queue_returned{}, submit_returned{}, completion_observed{};

  Sample sample(Clock::time_point present_begin, Clock::time_point swap_begin,
                Clock::time_point swap_returned, Clock::time_point presented) const {
    const auto ms = [](auto begin, auto end) { return std::chrono::duration<double, std::milli>(end - begin).count(); };
    // Queue/Swap are subsets. Completion includes main-thread polling delay, not just GPU work.
    return {ms(acquired, submit_begin), ms(submit_begin, submit_returned), ms(submit_begin, queue_returned),
            ms(submit_returned, completion_observed), ms(present_begin, presented), ms(swap_begin, swap_returned)};
  }
};

struct FrameWallSummary {
  size_t frames = 0;
  FrameWallTimings::Sample total{}, maximum{};

  void record(const FrameWallTimings::Sample& sample) {
    ++frames;
    for (size_t i = 0; i < sample.size(); ++i) {
      total[i] += sample[i];
      maximum[i] = std::max(maximum[i], sample[i]);
    }
  }
  FrameWallTimings::Sample mean() const {
    auto result = total;
    if (frames != 0) for (auto& value : result) value /= frames;
    return result;
  }
};

inline bool gl_api_timing_enabled(const char* value) {
  return value && value[0] == '1' && value[1] == '\0';
}

inline uint64_t requested_gl_buffer_bytes(std::ptrdiff_t size) {
  return size > 0 ? static_cast<uint64_t>(size) : 0;
}

struct GLApiWallSummary {
  enum Field : size_t {
    MapBufferRange, UnmapBuffer, BufferData, BufferSubData, CopyBufferSubData,
    TexSubImage2D, TexImage2D, TexStorage2D, DrawArrays, DrawElements,
    DrawArraysInstanced, DrawElementsInstanced, DrawElementsInstancedBaseVertex,
    BlitFramebuffer, Flush, GetError, Uniform1uiv, UseProgram, BindBufferRange, MakeCurrent,
    BufferSubDataUniform, BufferSubDataArray, Count
  };
  static constexpr std::array<const char*, Count> Names{
      "glMapBufferRange", "glUnmapBuffer", "glBufferData", "glBufferSubData", "glCopyBufferSubData",
      "glTexSubImage2D", "glTexImage2D", "glTexStorage2D", "glDrawArrays", "glDrawElements",
      "glDrawArraysInstanced", "glDrawElementsInstanced", "glDrawElementsInstancedBaseVertex",
      "glBlitFramebuffer", "glFlush", "glGetError", "glUniform1uiv", "glUseProgram", "glBindBufferRange",
      "eglMakeCurrent", "glBufferSubData_uniform", "glBufferSubData_array"};
  static constexpr uint32_t UniformBufferTarget = 0x8a11;
  static constexpr uint32_t ArrayBufferTarget = 0x8892;
  struct Sample {
    uint64_t calls = 0, total_ns = 0, max_ns = 0, bytes = 0;
    bool empty() const { return calls == 0 && total_ns == 0 && max_ns == 0 && bytes == 0; }
  };
  using Samples = std::array<Sample, Count>;

  void record(Field api, uint64_t wall_ns, uint64_t bytes = 0) {
    auto& counter = counters[api];
    counter.calls.fetch_add(1, std::memory_order_relaxed);
    counter.total_ns.fetch_add(wall_ns, std::memory_order_relaxed);
    if (bytes != 0) counter.bytes.fetch_add(bytes, std::memory_order_relaxed);
    auto maximum = counter.max_ns.load(std::memory_order_relaxed);
    while (maximum < wall_ns &&
           !counter.max_ns.compare_exchange_weak(maximum, wall_ns, std::memory_order_relaxed)) {}
  }

  void record_buffer_sub_data(uint32_t target, uint64_t wall_ns, uint64_t bytes) {
    record(BufferSubData, wall_ns, bytes);
    // Target counters are subsets of BufferSubData, not additional driver calls.
    if (target == UniformBufferTarget) record(BufferSubDataUniform, wall_ns, bytes);
    else if (target == ArrayBufferTarget) record(BufferSubDataArray, wall_ns, bytes);
  }

  Samples sample_and_reset() {
    Samples result{};
    // Independent atomic exchanges keep driver threads lock-free. Concurrent records may
    // straddle adjacent report windows; cumulative calls/bytes/time are never discarded.
    for (size_t i = 0; i < Count; ++i) {
      auto& counter = counters[i];
      result[i] = {counter.calls.exchange(0, std::memory_order_relaxed),
                   counter.total_ns.exchange(0, std::memory_order_relaxed),
                   counter.max_ns.exchange(0, std::memory_order_relaxed),
                   counter.bytes.exchange(0, std::memory_order_relaxed)};
    }
    return result;
  }

private:
  struct Counters {
    std::atomic<uint64_t> calls{0}, total_ns{0}, max_ns{0}, bytes{0};
  };
  std::array<Counters, Count> counters{};
};

} // namespace aurora::h700
