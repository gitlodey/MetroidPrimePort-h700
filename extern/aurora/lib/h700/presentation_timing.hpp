#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

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

constexpr bool producer_flush_enabled(const char* value, const char* bench) noexcept {
  return value && std::string_view(value) == "1" && bench &&
         (std::string_view(bench) == "1" || std::string_view(bench) == "menu");
}

// Kept EGL-header-free for the CPU policy tests; checked against the real enums at the wrapper.
inline constexpr uint32_t ProducerFenceSyncType = 0x30f9;
inline constexpr uint32_t ProducerNativeFenceSyncType = 0x3144;
inline constexpr uint32_t ProducerSyncNone = 0x3038;

template <typename Attribute>
constexpr bool producer_sync_attributes_empty(const Attribute* attrs) noexcept {
  // Reject every nonempty list, including native-fence FD imports, without scanning it.
  return !attrs || attrs[0] == static_cast<Attribute>(ProducerSyncNone);
}

constexpr bool producer_sync_eligible(uint32_t type, bool created, bool empty_attrs) noexcept {
  return created && empty_attrs && (type == ProducerFenceSyncType || type == ProducerNativeFenceSyncType);
}

inline uint64_t requested_gl_buffer_bytes(std::ptrdiff_t size) {
  return size > 0 ? static_cast<uint64_t>(size) : 0;
}

// Call/record are stack-owned functors. OFF must neither read the clock nor update counters.
template <typename Clock = FrameWallTimings::Clock, typename Call, typename Record>
auto probe_wall_call(bool enabled, Call call, Record record) -> std::invoke_result_t<Call> {
  if (!enabled) return call();
  const auto begin = Clock::now();
  if constexpr (std::is_void_v<std::invoke_result_t<Call>>) {
    call();
    const auto returned = Clock::now();
    record(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(returned - begin).count()));
  } else {
    decltype(auto) result = call();
    const auto returned = Clock::now();
    record(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(returned - begin).count()), result);
    return result;
  }
}

struct GLApiWallSummary {
  enum Field : size_t {
    MapBufferRange, UnmapBuffer, BufferData, BufferSubData, CopyBufferSubData,
    TexSubImage2D, TexImage2D, TexStorage2D, DrawArrays, DrawElements,
    DrawArraysInstanced, DrawElementsInstanced, DrawElementsInstancedBaseVertex,
    BlitFramebuffer, Flush, GetError, Uniform1uiv, UseProgram, BindBufferRange, MakeCurrent,
    BufferSubDataUniform, BufferSubDataArray,
    ProcessEvents, ForwardWaitAny, ReversePoll, DeviceLostPoll,
    ClientWaitSync, ClientWaitSyncKHR, PresenterClientWaitSyncKHR,
    CreateSync, CreateSyncKHR, ProducerFlush, Count
  };
  static constexpr std::array<const char*, Count> Names{
      "glMapBufferRange", "glUnmapBuffer", "glBufferData", "glBufferSubData", "glCopyBufferSubData",
      "glTexSubImage2D", "glTexImage2D", "glTexStorage2D", "glDrawArrays", "glDrawElements",
      "glDrawArraysInstanced", "glDrawElementsInstanced", "glDrawElementsInstancedBaseVertex",
      "glBlitFramebuffer", "glFlush", "glGetError", "glUniform1uiv", "glUseProgram", "glBindBufferRange",
      "eglMakeCurrent", "glBufferSubData_uniform", "glBufferSubData_array",
      "ProcessEvents", "forwardWaitAny", "reversePoll", "deviceLostPoll",
      "eglClientWaitSync", "eglClientWaitSyncKHR", "presenterEglClientWaitSyncKHR",
      "eglCreateSync", "eglCreateSyncKHR", "producerGlFlush"};
  // Only the contiguous GL entry points before MakeCurrent index TimedGLWrappers/gl_procs.
  static constexpr size_t WrappedGLCount = MakeCurrent;
  static constexpr std::array<Field, 2> DawnClientWaitFields{ClientWaitSync, ClientWaitSyncKHR};
  static constexpr std::array<Field, 2> DawnCreateSyncFields{CreateSync, CreateSyncKHR};
  static constexpr uint32_t UniformBufferTarget = 0x8a11;
  static constexpr uint32_t ArrayBufferTarget = 0x8892;
  static constexpr uint32_t SyncFlushCommandsBit = 0x1;
  enum class WaitOutcome { TimedOut, Signaled, Other };
  struct WaitSample {
    uint64_t timeout_zero = 0, timeout_nonzero = 0, flags_or = 0, flush_calls = 0;
    uint64_t timed_out = 0, signaled = 0, other = 0;
    bool empty() const {
      return timeout_zero == 0 && timeout_nonzero == 0 && flags_or == 0 && flush_calls == 0 &&
             timed_out == 0 && signaled == 0 && other == 0;
    }
  };
  struct SyncSample {
    uint64_t fence_calls = 0, native_fence_calls = 0, other_type_calls = 0;
    uint64_t created = 0, failed = 0, eligible = 0;
    bool empty() const {
      return fence_calls == 0 && native_fence_calls == 0 && other_type_calls == 0 &&
             created == 0 && failed == 0 && eligible == 0;
    }
  };
  struct Sample {
    uint64_t calls = 0, total_ns = 0, max_ns = 0, bytes = 0;
    WaitSample wait{};
    SyncSample sync{};
    bool empty() const {
      return calls == 0 && total_ns == 0 && max_ns == 0 && bytes == 0 && wait.empty() && sync.empty();
    }
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

  void record_wait(Field api, uint64_t wall_ns, uint32_t flags, uint64_t timeout, WaitOutcome outcome) {
    record(api, wall_ns);
    auto& counter = counters[api];
    (timeout == 0 ? counter.timeout_zero : counter.timeout_nonzero).fetch_add(1, std::memory_order_relaxed);
    if (flags != 0) counter.flags_or.fetch_or(flags, std::memory_order_relaxed);
    if (flags & SyncFlushCommandsBit) counter.flush_calls.fetch_add(1, std::memory_order_relaxed);
    auto& result = outcome == WaitOutcome::TimedOut ? counter.timed_out
        : outcome == WaitOutcome::Signaled ? counter.signaled : counter.other;
    result.fetch_add(1, std::memory_order_relaxed);
  }

  void record_sync(Field api, uint64_t wall_ns, uint32_t type, bool created, bool eligible) {
    record(api, wall_ns);
    auto& counter = counters[api];
    auto& type_calls = type == ProducerFenceSyncType ? counter.fence_calls
        : type == ProducerNativeFenceSyncType ? counter.native_fence_calls : counter.other_type_calls;
    type_calls.fetch_add(1, std::memory_order_relaxed);
    (created ? counter.created : counter.failed).fetch_add(1, std::memory_order_relaxed);
    if (eligible) counter.eligible.fetch_add(1, std::memory_order_relaxed);
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
                   counter.bytes.exchange(0, std::memory_order_relaxed),
                   {counter.timeout_zero.exchange(0, std::memory_order_relaxed),
                    counter.timeout_nonzero.exchange(0, std::memory_order_relaxed),
                    counter.flags_or.exchange(0, std::memory_order_relaxed),
                    counter.flush_calls.exchange(0, std::memory_order_relaxed),
                    counter.timed_out.exchange(0, std::memory_order_relaxed),
                    counter.signaled.exchange(0, std::memory_order_relaxed),
                    counter.other.exchange(0, std::memory_order_relaxed)},
                   {counter.fence_calls.exchange(0, std::memory_order_relaxed),
                    counter.native_fence_calls.exchange(0, std::memory_order_relaxed),
                    counter.other_type_calls.exchange(0, std::memory_order_relaxed),
                    counter.created.exchange(0, std::memory_order_relaxed),
                    counter.failed.exchange(0, std::memory_order_relaxed),
                    counter.eligible.exchange(0, std::memory_order_relaxed)}};
    }
    return result;
  }

private:
  struct Counters {
    std::atomic<uint64_t> calls{0}, total_ns{0}, max_ns{0}, bytes{0};
    std::atomic<uint64_t> timeout_zero{0}, timeout_nonzero{0}, flags_or{0}, flush_calls{0};
    std::atomic<uint64_t> timed_out{0}, signaled{0}, other{0};
    std::atomic<uint64_t> fence_calls{0}, native_fence_calls{0}, other_type_calls{0};
    std::atomic<uint64_t> created{0}, failed{0}, eligible{0};
  };
  std::array<Counters, Count> counters{};
};

// The EGL wrapper calls this before returning to Dawn's scoped context owner. Timing covers
// only the original create and the actual eager flush, not eligibility/ledger checks.
template <typename Clock = FrameWallTimings::Clock, typename Create, typename IsProducer, typename Flush>
auto producer_sync_call(bool enabled, bool timing, GLApiWallSummary& summary, GLApiWallSummary::Field api,
                        uint32_t type, bool empty_attrs, Create create, IsProducer is_producer, Flush flush)
    -> std::invoke_result_t<Create> {
  if (!enabled && !timing) return create();
  uint64_t wall_ns = 0;
  const auto result = probe_wall_call<Clock>(timing, create,
      [&](uint64_t ns, const auto&) { wall_ns = ns; });
  const bool created = static_cast<bool>(result);
  const bool eligible = producer_sync_eligible(type, created, empty_attrs) && is_producer();
  if (timing) summary.record_sync(api, wall_ns, type, created, eligible);
  if (enabled && eligible) {
    probe_wall_call<Clock>(timing, flush,
        [&](uint64_t ns) { summary.record(GLApiWallSummary::ProducerFlush, ns); });
  }
  return result;
}

} // namespace aurora::h700
