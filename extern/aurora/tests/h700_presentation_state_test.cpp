#include "h700/presentation_state.hpp"
#include "h700/presentation_timing.hpp"

#include <gtest/gtest.h>

#include <latch>
#include <thread>
#include <type_traits>

namespace aurora::h700 {
namespace {

TEST(H700PresentationTiming, WallMillisecondsKeepNestedEnvelopesSeparate) {
  using T = FrameWallTimings;
  using namespace std::chrono_literals;
  T timing;
  timing.acquired = T::Clock::time_point{} + 1s;
  timing.submit_begin = timing.acquired + 10ms;
  timing.queue_returned = timing.submit_begin + 3ms;
  timing.submit_returned = timing.submit_begin + 5ms;
  timing.completion_observed = timing.submit_returned + 200ms;
  const auto present_begin = timing.completion_observed + 2ms;
  const auto sample = timing.sample(present_begin, present_begin + 4ms, present_begin + 19ms, present_begin + 20ms);
  EXPECT_DOUBLE_EQ(sample[T::Prepare], 10);
  EXPECT_DOUBLE_EQ(sample[T::Submit], 5);
  EXPECT_DOUBLE_EQ(sample[T::Queue], 3);
  EXPECT_DOUBLE_EQ(sample[T::CompletionObserved], 200);
  EXPECT_DOUBLE_EQ(sample[T::Present], 20);
  EXPECT_DOUBLE_EQ(sample[T::Swap], 15);
}

TEST(H700PresentationTiming, FixedSummaryReportsIndependentMeansMaximaAndResets) {
  FrameWallSummary summary;
  EXPECT_EQ(summary.mean(), FrameWallTimings::Sample{});
  summary.record({1, 2, 3, 4, 5, 6});
  summary.record({3, 4, 5, 6, 7, 8});
  EXPECT_EQ(summary.frames, 2);
  EXPECT_EQ(summary.mean(), (FrameWallTimings::Sample{2, 3, 4, 5, 6, 7}));
  EXPECT_EQ(summary.maximum, (FrameWallTimings::Sample{3, 4, 5, 6, 7, 8}));
  summary = {};
  EXPECT_EQ(summary.frames, 0);
  EXPECT_EQ(summary.mean(), FrameWallTimings::Sample{});
  EXPECT_EQ(summary.maximum, FrameWallTimings::Sample{});
}

TEST(H700PresentationTiming, GLApiTimingRequiresExactOptInAndNonnegativeByteRanges) {
  EXPECT_FALSE(gl_api_timing_enabled(nullptr));
  for (const char* value : {"", "0", "true", "01", "1 ", " 1", "11"}) {
    EXPECT_FALSE(gl_api_timing_enabled(value)) << value;
  }
  EXPECT_TRUE(gl_api_timing_enabled("1"));
  EXPECT_EQ(requested_gl_buffer_bytes(-1), 0);
  EXPECT_EQ(requested_gl_buffer_bytes(0), 0);
  EXPECT_EQ(requested_gl_buffer_bytes(4096), 4096);
}

struct ProbeClock {
  using duration = std::chrono::nanoseconds;
  using time_point = std::chrono::time_point<ProbeClock, duration>;
  inline static duration elapsed{};
  inline static size_t reads = 0;
  static time_point now() { ++reads; return time_point(elapsed); }
};

TEST(H700PresentationTiming, ProbeOffCallsOnceWithoutClockOrRecording) {
  ProbeClock::elapsed = {};
  ProbeClock::reads = 0;
  size_t calls = 0, records = 0;
  EXPECT_EQ(probe_wall_call<ProbeClock>(false, [&] { ++calls; return 53; },
      [&](uint64_t, int) { ++records; }), 53);
  probe_wall_call<ProbeClock>(false, [&] { ++calls; }, [&](uint64_t) { ++records; });
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(records, 0);
  EXPECT_EQ(ProbeClock::reads, 0);
}

TEST(H700PresentationTiming, ProbeOnMeasuresOnlyCallAndPreservesVoidAndValueResults) {
  using namespace std::chrono_literals;
  ProbeClock::elapsed = {};
  ProbeClock::reads = 0;
  size_t calls = 0, records = 0;
  const auto result = probe_wall_call<ProbeClock>(true, [&] {
    ++calls;
    ProbeClock::elapsed += 150ms;
    return 73;
  }, [&](uint64_t wall_ns, int returned) {
    ++records;
    EXPECT_EQ(wall_ns, 150'000'000);
    EXPECT_EQ(returned, 73);
    EXPECT_EQ(ProbeClock::reads, 2);
    ProbeClock::elapsed += 100ms; // aggregation is outside the measured call
  });
  EXPECT_EQ(result, 73);
  probe_wall_call<ProbeClock>(true, [&] { ++calls; ProbeClock::elapsed += 7ms; }, [&](uint64_t wall_ns) {
    ++records;
    EXPECT_EQ(wall_ns, 7'000'000);
  });
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(records, 2);
  EXPECT_EQ(ProbeClock::reads, 4);
}

TEST(H700PresentationTiming, WaitApiFieldsDoNotExtendTheContiguousGlWrapperTable) {
  using T = GLApiWallSummary;
  EXPECT_EQ(T::WrappedGLCount, static_cast<size_t>(T::MakeCurrent));
  EXPECT_EQ(T::WrappedGLCount, static_cast<size_t>(T::BindBufferRange) + 1);
  EXPECT_STREQ(T::Names[T::WrappedGLCount - 1], "glBindBufferRange");
  EXPECT_EQ(T::DawnClientWaitFields, (std::array{T::ClientWaitSync, T::ClientWaitSyncKHR}));
  for (const auto field : {T::ProcessEvents, T::ForwardWaitAny, T::ReversePoll, T::DeviceLostPoll,
                           T::ClientWaitSync, T::ClientWaitSyncKHR, T::PresenterClientWaitSyncKHR}) {
    EXPECT_GT(static_cast<size_t>(field), T::WrappedGLCount);
  }
  EXPECT_STREQ(T::Names[T::ProcessEvents], "ProcessEvents");
  EXPECT_STREQ(T::Names[T::ForwardWaitAny], "forwardWaitAny");
  EXPECT_STREQ(T::Names[T::ReversePoll], "reversePoll");
  EXPECT_STREQ(T::Names[T::DeviceLostPoll], "deviceLostPoll");
  EXPECT_STREQ(T::Names[T::ClientWaitSync], "eglClientWaitSync");
  EXPECT_STREQ(T::Names[T::ClientWaitSyncKHR], "eglClientWaitSyncKHR");
  EXPECT_STREQ(T::Names[T::PresenterClientWaitSyncKHR], "presenterEglClientWaitSyncKHR");
}

TEST(H700PresentationTiming, WaitSummaryKeepsBoundariesFlagsTimeoutsOutcomesAndPresenterSeparate) {
  using T = GLApiWallSummary;
  using O = T::WaitOutcome;
  T summary;
  summary.record(T::ProcessEvents, 83'000'000);
  summary.record_wait(T::ForwardWaitAny, 150'000'000, 0, 0, O::TimedOut);
  summary.record_wait(T::ForwardWaitAny, 7, 0, 0, O::Signaled);
  summary.record(T::ReversePoll, 30);
  summary.record(T::DeviceLostPoll, 40);
  summary.record_wait(T::ClientWaitSync, 11, T::SyncFlushCommandsBit, 0, O::TimedOut);
  summary.record_wait(T::ClientWaitSyncKHR, 17, 0, UINT64_MAX, O::Signaled);
  summary.record_wait(T::ClientWaitSyncKHR, 3, T::SyncFlushCommandsBit | 4, 1, O::Other);
  summary.record_wait(T::PresenterClientWaitSyncKHR, 19, 0, 0, O::Signaled);
  const auto samples = summary.sample_and_reset();
  EXPECT_EQ(samples[T::ProcessEvents].calls, 1);
  EXPECT_EQ(samples[T::ProcessEvents].total_ns, 83'000'000);
  EXPECT_TRUE(samples[T::ProcessEvents].wait.empty());
  EXPECT_EQ(samples[T::ReversePoll].total_ns, 30);
  EXPECT_EQ(samples[T::DeviceLostPoll].total_ns, 40);
  const auto& forward = samples[T::ForwardWaitAny];
  EXPECT_EQ(forward.calls, 2);
  EXPECT_EQ(forward.total_ns, 150'000'007);
  EXPECT_EQ(forward.max_ns, 150'000'000);
  EXPECT_EQ(forward.wait.timeout_zero, 2);
  EXPECT_EQ(forward.wait.timeout_nonzero, 0);
  EXPECT_EQ(forward.wait.timed_out, 1);
  EXPECT_EQ(forward.wait.signaled, 1);
  EXPECT_EQ(forward.wait.other, 0);
  EXPECT_EQ(forward.wait.flags_or, 0);
  EXPECT_EQ(forward.wait.flush_calls, 0);
  const auto& core = samples[T::ClientWaitSync];
  EXPECT_EQ(core.calls, 1);
  EXPECT_EQ(core.total_ns, 11);
  EXPECT_EQ(core.wait.timeout_zero, 1);
  EXPECT_EQ(core.wait.flush_calls, 1);
  EXPECT_EQ(core.wait.flags_or, T::SyncFlushCommandsBit);
  EXPECT_EQ(core.wait.timed_out, 1);
  const auto& khr = samples[T::ClientWaitSyncKHR];
  EXPECT_EQ(khr.calls, 2);
  EXPECT_EQ(khr.total_ns, 20);
  EXPECT_EQ(khr.max_ns, 17);
  EXPECT_EQ(khr.wait.timeout_zero, 0);
  EXPECT_EQ(khr.wait.timeout_nonzero, 2);
  EXPECT_EQ(khr.wait.flags_or, T::SyncFlushCommandsBit | 4);
  EXPECT_EQ(khr.wait.flush_calls, 1);
  EXPECT_EQ(khr.wait.timed_out, 0);
  EXPECT_EQ(khr.wait.signaled, 1);
  EXPECT_EQ(khr.wait.other, 1);
  const auto& presenter = samples[T::PresenterClientWaitSyncKHR];
  EXPECT_EQ(presenter.calls, 1);
  EXPECT_EQ(presenter.total_ns, 19);
  EXPECT_EQ(presenter.wait.timeout_zero, 1);
  EXPECT_EQ(presenter.wait.flush_calls, 0);
  EXPECT_EQ(presenter.wait.signaled, 1);
  for (const auto field : T::DawnClientWaitFields) EXPECT_EQ(samples[field].bytes, 0);
  EXPECT_TRUE(samples[T::MakeCurrent].empty());
  for (const auto& sample : summary.sample_and_reset()) EXPECT_TRUE(sample.empty());
}

TEST(H700PresentationTiming, MetadataOnlyWindowEdgesAreNotReportedAsEmpty) {
  GLApiWallSummary::Sample sample;
  EXPECT_TRUE(sample.empty());
  sample.wait.timed_out = 1;
  EXPECT_FALSE(sample.empty());
  sample = {};
  sample.wait.flags_or = 4;
  EXPECT_FALSE(sample.empty());
}

TEST(H700PresentationTiming, GLApiSummaryKeepsIndependentCountsBytesMaximaAndResets) {
  using T = GLApiWallSummary;
  T summary;
  for (const auto& sample : summary.sample_and_reset()) EXPECT_TRUE(sample.empty());
  summary.record(T::MapBufferRange, 30, 4096);
  summary.record(T::MapBufferRange, 10, 1024);
  summary.record(T::GetError, 7);
  summary.record(T::Uniform1uiv, 11);
  summary.record(T::Uniform1uiv, 3);
  summary.record(T::UseProgram, 17);
  summary.record(T::BindBufferRange, 19, requested_gl_buffer_bytes(256));
  summary.record(T::BindBufferRange, 5, requested_gl_buffer_bytes(128));
  const auto samples = summary.sample_and_reset();
  EXPECT_EQ(samples[T::MapBufferRange].calls, 2);
  EXPECT_EQ(samples[T::MapBufferRange].total_ns, 40);
  EXPECT_EQ(samples[T::MapBufferRange].max_ns, 30);
  EXPECT_EQ(samples[T::MapBufferRange].bytes, 5120);
  EXPECT_EQ(samples[T::GetError].calls, 1);
  EXPECT_EQ(samples[T::GetError].total_ns, 7);
  EXPECT_EQ(samples[T::GetError].max_ns, 7);
  EXPECT_EQ(samples[T::GetError].bytes, 0);
  EXPECT_STREQ(T::Names[T::Uniform1uiv], "glUniform1uiv");
  EXPECT_EQ(samples[T::Uniform1uiv].calls, 2);
  EXPECT_EQ(samples[T::Uniform1uiv].total_ns, 14);
  EXPECT_EQ(samples[T::Uniform1uiv].max_ns, 11);
  EXPECT_EQ(samples[T::Uniform1uiv].bytes, 0);
  EXPECT_STREQ(T::Names[T::UseProgram], "glUseProgram");
  EXPECT_EQ(samples[T::UseProgram].calls, 1);
  EXPECT_EQ(samples[T::UseProgram].total_ns, 17);
  EXPECT_EQ(samples[T::UseProgram].max_ns, 17);
  EXPECT_EQ(samples[T::UseProgram].bytes, 0);
  EXPECT_STREQ(T::Names[T::BindBufferRange], "glBindBufferRange");
  EXPECT_EQ(samples[T::BindBufferRange].calls, 2);
  EXPECT_EQ(samples[T::BindBufferRange].total_ns, 24);
  EXPECT_EQ(samples[T::BindBufferRange].max_ns, 19);
  EXPECT_EQ(samples[T::BindBufferRange].bytes, 384);
  EXPECT_TRUE(samples[T::MakeCurrent].empty());
  for (const auto& sample : summary.sample_and_reset()) EXPECT_TRUE(sample.empty());
  summary.record(T::MapBufferRange, 3, 8);
  const auto next = summary.sample_and_reset();
  EXPECT_EQ(next[T::MapBufferRange].calls, 1);
  EXPECT_EQ(next[T::MapBufferRange].total_ns, 3);
  EXPECT_EQ(next[T::MapBufferRange].max_ns, 3);
  EXPECT_EQ(next[T::MapBufferRange].bytes, 8);
}

TEST(H700PresentationTiming, BufferSubDataTargetCountersAreIndependentSubsets) {
  using T = GLApiWallSummary;
  T summary;
  summary.record_buffer_sub_data(T::UniformBufferTarget, 7, 64);
  summary.record_buffer_sub_data(T::UniformBufferTarget, 11, 4);
  summary.record_buffer_sub_data(T::ArrayBufferTarget, 23, 256);
  summary.record_buffer_sub_data(0, 5, 0);
  const auto samples = summary.sample_and_reset();
  EXPECT_EQ(samples[T::BufferSubData].calls, 4);
  EXPECT_EQ(samples[T::BufferSubData].total_ns, 46);
  EXPECT_EQ(samples[T::BufferSubData].max_ns, 23);
  EXPECT_EQ(samples[T::BufferSubData].bytes, 324);
  EXPECT_EQ(samples[T::BufferSubDataUniform].calls, 2);
  EXPECT_EQ(samples[T::BufferSubDataUniform].total_ns, 18);
  EXPECT_EQ(samples[T::BufferSubDataUniform].max_ns, 11);
  EXPECT_EQ(samples[T::BufferSubDataUniform].bytes, 68);
  EXPECT_EQ(samples[T::BufferSubDataArray].calls, 1);
  EXPECT_EQ(samples[T::BufferSubDataArray].total_ns, 23);
  EXPECT_EQ(samples[T::BufferSubDataArray].bytes, 256);
  EXPECT_STREQ(T::Names[T::BufferSubDataUniform], "glBufferSubData_uniform");
  EXPECT_STREQ(T::Names[T::BufferSubDataArray], "glBufferSubData_array");
  for (const auto& sample : summary.sample_and_reset()) EXPECT_TRUE(sample.empty());
}

TEST(H700PresentationTiming, GLApiConcurrentRecordingAndSamplingDoNotLoseTotals) {
  using T = GLApiWallSummary;
  constexpr size_t Writers = 3;
  constexpr uint64_t CallsPerWriter = 1024;
  T summary;
  std::latch start(Writers + 1);
  std::atomic<size_t> done{0};
  std::array<std::thread, Writers> workers;
  for (auto& worker : workers) {
    worker = std::thread([&] {
      start.arrive_and_wait();
      for (uint64_t i = 1; i <= CallsPerWriter; ++i) {
        summary.record(T::BufferSubData, i, 64);
        summary.record_wait(T::ClientWaitSyncKHR, i, i % 2 ? T::SyncFlushCommandsBit : 0, i % 2,
                             i % 2 ? T::WaitOutcome::Signaled : T::WaitOutcome::TimedOut);
      }
      done.fetch_add(1, std::memory_order_release);
    });
  }
  T::Sample total, waitTotal;
  const auto collect = [&] {
    const auto samples = summary.sample_and_reset();
    const auto& sample = samples[T::BufferSubData];
    total.calls += sample.calls;
    total.total_ns += sample.total_ns;
    total.max_ns = std::max(total.max_ns, sample.max_ns);
    total.bytes += sample.bytes;
    const auto& wait = samples[T::ClientWaitSyncKHR];
    waitTotal.calls += wait.calls;
    waitTotal.total_ns += wait.total_ns;
    waitTotal.max_ns = std::max(waitTotal.max_ns, wait.max_ns);
    waitTotal.wait.timeout_zero += wait.wait.timeout_zero;
    waitTotal.wait.timeout_nonzero += wait.wait.timeout_nonzero;
    waitTotal.wait.flags_or |= wait.wait.flags_or;
    waitTotal.wait.flush_calls += wait.wait.flush_calls;
    waitTotal.wait.timed_out += wait.wait.timed_out;
    waitTotal.wait.signaled += wait.wait.signaled;
    waitTotal.wait.other += wait.wait.other;
  };
  start.arrive_and_wait();
  while (done.load(std::memory_order_acquire) != Writers) {
    collect();
    std::this_thread::yield();
  }
  for (auto& worker : workers) worker.join();
  collect();
  EXPECT_EQ(total.calls, Writers * CallsPerWriter);
  EXPECT_EQ(total.total_ns, Writers * CallsPerWriter * (CallsPerWriter + 1) / 2);
  EXPECT_EQ(total.max_ns, CallsPerWriter);
  EXPECT_EQ(total.bytes, Writers * CallsPerWriter * 64);
  EXPECT_EQ(waitTotal.calls, total.calls);
  EXPECT_EQ(waitTotal.total_ns, total.total_ns);
  EXPECT_EQ(waitTotal.max_ns, CallsPerWriter);
  EXPECT_EQ(waitTotal.wait.timeout_zero, Writers * CallsPerWriter / 2);
  EXPECT_EQ(waitTotal.wait.timeout_nonzero, Writers * CallsPerWriter / 2);
  EXPECT_EQ(waitTotal.wait.flags_or, T::SyncFlushCommandsBit);
  EXPECT_EQ(waitTotal.wait.flush_calls, Writers * CallsPerWriter / 2);
  EXPECT_EQ(waitTotal.wait.timed_out, Writers * CallsPerWriter / 2);
  EXPECT_EQ(waitTotal.wait.signaled, Writers * CallsPerWriter / 2);
  EXPECT_EQ(waitTotal.wait.other, 0);
  for (const auto& sample : summary.sample_and_reset()) EXPECT_TRUE(sample.empty());
}

static_assert(!std::is_copy_assignable_v<FrameDescriptor>);
static_assert(!std::is_move_assignable_v<FrameDescriptor>);

using Phase = PresentationPhase;
using Status = SlotStatus;
using Outcome = PresentationNotification::Outcome;

class H700PresentationStateTest : public ::testing::Test {
protected:
  PresentationState state{7};

  FrameDescriptor reserve(std::uint64_t frame) {
    auto descriptor = state.try_reserve(frame);
    EXPECT_TRUE(descriptor.has_value());
    return descriptor.value();
  }

  void publish(const FrameDescriptor& descriptor, CompletionId forward) {
    ASSERT_TRUE(state.mark_submitted(descriptor));
    ASSERT_TRUE(state.publish_submitted(descriptor, forward));
  }

  void read(const FrameDescriptor& descriptor, CompletionId forward) {
    ASSERT_TRUE(state.complete_forward(descriptor, forward));
    const auto reading = state.try_begin_read();
    ASSERT_TRUE(reading.has_value());
    EXPECT_EQ(*reading, descriptor);
  }

  void retire(const FrameDescriptor& descriptor, CompletionId reverse) {
    ASSERT_TRUE(state.begin_retiring(descriptor, reverse));
    ASSERT_TRUE(state.complete_swap(descriptor));
    ASSERT_TRUE(state.complete_reverse(descriptor, reverse));
    ASSERT_TRUE(state.recycle(descriptor));
  }
};

TEST_F(H700PresentationStateTest, ExactlyTwoCreditsAndAbortBeforeSubmit) {
  const auto a = reserve(0);
  const auto b = reserve(1);
  EXPECT_NE(a.slot, b.slot);
  EXPECT_FALSE(state.try_reserve(2));
  EXPECT_FALSE(state.try_begin_read());
  EXPECT_FALSE(state.try_stop());
  ASSERT_TRUE(state.abort_reservation(a));
  EXPECT_FALSE(state.abort_reservation(a));
  EXPECT_FALSE(state.try_reserve(0));
  const auto c = reserve(2); // rejected third acquisition did not consume this frame ID
  EXPECT_EQ(c.slot, a.slot);
  EXPECT_FALSE(state.abort_reservation(a));
  EXPECT_EQ(state.snapshot().slots[c.slot].status, Status::Reserved);
  ASSERT_TRUE(state.abort_reservation(b));
  ASSERT_TRUE(state.abort_reservation(c));
  const auto notifications = state.take_notifications();
  ASSERT_EQ(notifications.size(), 3u);
  EXPECT_EQ(notifications[0].descriptor, a);
  EXPECT_EQ(notifications[0].outcome, Outcome::Aborted);
  EXPECT_TRUE(state.take_notifications().empty());
}

TEST_F(H700PresentationStateTest, SubmitBoundaryCannotBeRolledBackBeforeFuturePublication) {
  const auto a = reserve(0);
  EXPECT_FALSE(state.publish_submitted(a, 10));
  ASSERT_TRUE(state.mark_submitted(a));
  EXPECT_FALSE(state.mark_submitted(a));
  EXPECT_FALSE(state.abort_reservation(a));
  EXPECT_FALSE(state.try_begin_read());
  EXPECT_FALSE(state.complete_forward(a, 10));
  EXPECT_FALSE(state.publish_submitted(a, 0));
  ASSERT_TRUE(state.publish_submitted(a, 10));
  const auto snapshot = state.snapshot();
  EXPECT_EQ(snapshot.slots[a.slot].status, Status::Submitted);
  EXPECT_EQ(snapshot.slots[a.slot].forward, 10u);
  EXPECT_EQ(snapshot.mailbox_size, 1u);
}

TEST_F(H700PresentationStateTest, RejectsStaleGenerationWrongFrameAndInvalidSlot) {
  const auto a = reserve(0);
  publish(a, 10);
  const FrameDescriptor invalid[] = {
      {6, a.frame, a.slot}, {8, a.frame, a.slot}, {7, a.frame + 1, a.slot}, {7, a.frame, 2}};
  for (const auto& descriptor : invalid) {
    EXPECT_FALSE(state.abort_reservation(descriptor));
    EXPECT_FALSE(state.mark_submitted(descriptor));
    EXPECT_FALSE(state.publish_submitted(descriptor, 11));
    EXPECT_FALSE(state.complete_forward(descriptor, 10));
    EXPECT_FALSE(state.begin_retiring(descriptor, 20));
    EXPECT_FALSE(state.complete_reverse(descriptor, 20));
    EXPECT_FALSE(state.complete_swap(descriptor));
    EXPECT_FALSE(state.recycle(descriptor));
    EXPECT_FALSE(state.quarantine(descriptor, "stale error"));
  }
  const auto snapshot = state.snapshot();
  EXPECT_EQ(snapshot.phase, Phase::Running);
  EXPECT_FALSE(snapshot.first_error);
  EXPECT_EQ(snapshot.mailbox_size, 1u);
  EXPECT_EQ(snapshot.slots[a.slot].status, Status::Submitted);
  read(a, 10);
  retire(a, 20);

  PresentationState next_generation{8};
  const auto next = next_generation.try_reserve(0);
  ASSERT_TRUE(next);
  EXPECT_FALSE(next_generation.abort_reservation(a));
  EXPECT_EQ(next_generation.snapshot().slots[next->slot].status, Status::Reserved);
}

TEST_F(H700PresentationStateTest, ReadRequiresForwardSuccessAndRecycleRequiresReverseAndSwap) {
  const auto a = reserve(0);
  const auto b = reserve(1);
  EXPECT_FALSE(state.recycle(a));
  EXPECT_FALSE(state.begin_retiring(a, 20));
  EXPECT_FALSE(state.complete_swap(a));
  EXPECT_FALSE(state.complete_reverse(a, 20));
  publish(a, 10);
  EXPECT_FALSE(state.try_begin_read());
  EXPECT_FALSE(state.complete_forward(a, 11));
  EXPECT_FALSE(state.try_begin_read());
  ASSERT_TRUE(state.complete_forward(a, 10));
  EXPECT_FALSE(state.recycle(a));
  ASSERT_TRUE(state.try_begin_read());
  EXPECT_EQ(state.snapshot().slots[a.slot].status, Status::Reading);
  EXPECT_FALSE(state.try_reserve(2));
  EXPECT_FALSE(state.recycle(a));
  EXPECT_FALSE(state.complete_swap(a));
  EXPECT_FALSE(state.begin_retiring(a, 0));
  ASSERT_TRUE(state.begin_retiring(a, 20));
  EXPECT_FALSE(state.recycle(a));
  EXPECT_FALSE(state.complete_reverse(a, 21));
  ASSERT_TRUE(state.complete_swap(a));
  EXPECT_FALSE(state.recycle(a));
  EXPECT_FALSE(state.try_reserve(2));
  ASSERT_TRUE(state.complete_reverse(a, 20));
  ASSERT_TRUE(state.recycle(a));
  const auto c = reserve(2);
  EXPECT_EQ(c.slot, a.slot);
  EXPECT_FALSE(state.recycle(a));
  EXPECT_FALSE(state.complete_forward(a, 10));
  EXPECT_FALSE(state.complete_swap(a));
  EXPECT_EQ(state.snapshot().slots[c.slot].status, Status::Reserved);
  EXPECT_TRUE(state.abort_reservation(b));
}

TEST_F(H700PresentationStateTest, ReverseSuccessAloneDoesNotRecycle) {
  const auto a = reserve(0);
  publish(a, 10);
  read(a, 10);
  ASSERT_TRUE(state.begin_retiring(a, 20));
  ASSERT_TRUE(state.complete_reverse(a, 20));
  EXPECT_FALSE(state.recycle(a));
  EXPECT_TRUE(state.take_notifications().empty());
  ASSERT_TRUE(state.complete_swap(a));
  EXPECT_TRUE(state.recycle(a));
}

TEST_F(H700PresentationStateTest, MailboxFollowsPublicationNotReservationOrCompletionOrder) {
  const auto a = reserve(0);
  const auto b = reserve(1);
  publish(b, 11);
  publish(a, 10);
  ASSERT_TRUE(state.front_submission());
  EXPECT_EQ(state.front_submission()->descriptor, b);
  EXPECT_EQ(state.front_submission()->forward, 11u);
  ASSERT_TRUE(state.complete_forward(a, 10)); // second arrival completes first
  EXPECT_FALSE(state.try_begin_read());
  EXPECT_EQ(state.snapshot().mailbox_size, 2u);
  EXPECT_FALSE(state.publish_submitted(b, 12)); // no overwrite, including a duplicate future
  read(b, 11);
  EXPECT_EQ(state.front_submission()->descriptor, a);
  const auto reading = state.try_begin_read();
  ASSERT_TRUE(reading);
  EXPECT_EQ(*reading, a);
  EXPECT_FALSE(state.front_submission());
  retire(b, 21);
  retire(a, 20);

  const auto c = reserve(2);
  publish(c, 12); // ring wraps without preserving old entries
  EXPECT_EQ(state.front_submission()->descriptor, c);
  read(c, 12);
  retire(c, 22);
}

TEST_F(H700PresentationStateTest, DuplicateEventsNeverDoublePublishNotifyOrRelease) {
  const auto a = reserve(0);
  publish(a, 10);
  EXPECT_FALSE(state.publish_submitted(a, 10));
  EXPECT_FALSE(state.publish_submitted(a, 11));
  ASSERT_TRUE(state.complete_forward(a, 10));
  EXPECT_FALSE(state.complete_forward(a, 10));
  ASSERT_TRUE(state.try_begin_read());
  EXPECT_FALSE(state.try_begin_read());
  EXPECT_FALSE(state.publish_submitted(a, 10));
  ASSERT_TRUE(state.begin_retiring(a, 20));
  EXPECT_FALSE(state.begin_retiring(a, 20));
  ASSERT_TRUE(state.complete_reverse(a, 20));
  EXPECT_FALSE(state.complete_reverse(a, 20));
  ASSERT_TRUE(state.complete_swap(a));
  EXPECT_FALSE(state.complete_swap(a));
  ASSERT_TRUE(state.recycle(a));
  EXPECT_FALSE(state.recycle(a));
  EXPECT_FALSE(state.complete_reverse(a, 20));
  const auto notifications = state.take_notifications();
  ASSERT_EQ(notifications.size(), 1u);
  EXPECT_EQ(notifications[0].descriptor, a);
  EXPECT_EQ(notifications[0].outcome, Outcome::Presented);
  const auto b = reserve(1);
  EXPECT_EQ(b.slot, a.slot);
  EXPECT_FALSE(state.complete_reverse(a, 20));
  EXPECT_EQ(state.snapshot().slots[b.slot].status, Status::Reserved);
}

TEST_F(H700PresentationStateTest, DrainingAcceptsPendingRenderPublicationsAndRetiresAllWork) {
  const auto a = reserve(0);
  const auto b = reserve(1);
  publish(a, 10);
  std::latch draining{1};
  std::thread worker([&] {
    draining.wait();
    EXPECT_TRUE(state.mark_submitted(b));
    EXPECT_TRUE(state.publish_submitted(b, 11));
  });
  state.begin_draining();
  EXPECT_EQ(state.snapshot().phase, Phase::Draining);
  EXPECT_FALSE(state.try_reserve(2));
  EXPECT_FALSE(state.try_stop());
  EXPECT_FALSE(state.stop_after_teardown());
  draining.count_down();
  worker.join(); // publication needs neither main progress nor notification dispatch
  EXPECT_EQ(state.snapshot().mailbox_size, 2u);
  read(a, 10);
  retire(a, 20);
  EXPECT_FALSE(state.try_stop());
  read(b, 11);
  retire(b, 21);
  ASSERT_TRUE(state.try_stop());
  EXPECT_EQ(state.snapshot().phase, Phase::Stopped);
  const auto notifications = state.take_notifications();
  ASSERT_EQ(notifications.size(), 2u);
  EXPECT_EQ(notifications[0].descriptor, a);
  EXPECT_EQ(notifications[1].descriptor, b);
  EXPECT_EQ(notifications[0].outcome, Outcome::Presented);
  EXPECT_EQ(notifications[1].outcome, Outcome::Presented);
}

TEST_F(H700PresentationStateTest, DrainingAlsoPreservesSubmitWithoutPublishedFuture) {
  const auto a = reserve(0);
  ASSERT_TRUE(state.mark_submitted(a));
  state.begin_draining();
  EXPECT_FALSE(state.try_stop());
  ASSERT_TRUE(state.publish_submitted(a, 10));
  read(a, 10);
  retire(a, 20);
  EXPECT_TRUE(state.try_stop());
}

TEST_F(H700PresentationStateTest, NotificationsAreOwnedAndIndependentOfCreditsAndWorker) {
  const auto a = reserve(0);
  publish(a, 10);
  read(a, 10);
  ASSERT_TRUE(state.begin_retiring(a, 20));
  ASSERT_TRUE(state.complete_swap(a));
  // A record alone is not a credit. Conversely, dispatch is not needed for reverse retirement.
  EXPECT_FALSE(state.recycle(a));
  ASSERT_TRUE(state.complete_reverse(a, 20));
  ASSERT_TRUE(state.recycle(a));
  const auto b = reserve(1);
  EXPECT_EQ(b.slot, a.slot);
  std::thread worker([&] { publish(b, 11); });
  worker.join(); // a's notification is still queued
  read(b, 11);
  retire(b, 21);
  auto notifications = state.take_notifications(); // main safe-point; no callbacks in helper
  ASSERT_EQ(notifications.size(), 2u);
  EXPECT_EQ(notifications[0].descriptor, a);
  EXPECT_EQ(notifications[1].descriptor, b);
  EXPECT_EQ(notifications[0].outcome, Outcome::Presented);
  EXPECT_EQ(notifications[1].outcome, Outcome::Presented);
  EXPECT_TRUE(state.take_notifications().empty());
  // Holding/processing those records outside the state lock cannot retain or corrupt a slot.
  const auto c = reserve(2);
  EXPECT_TRUE(state.abort_reservation(c));
  EXPECT_EQ(notifications[0].descriptor.frame, 0u);
  EXPECT_EQ(notifications[1].descriptor.frame, 1u);
}

TEST_F(H700PresentationStateTest, FailedPreservesPendingOwnershipAndFirstError) {
  const auto a = reserve(0);
  const auto b = reserve(1);
  ASSERT_TRUE(state.mark_submitted(a));
  state.fail("device lost");
  state.begin_draining();
  state.fail("later error");
  EXPECT_FALSE(state.try_reserve(2));
  EXPECT_FALSE(state.mark_submitted(b));
  EXPECT_FALSE(state.abort_reservation(a));
  EXPECT_FALSE(state.try_stop());
  ASSERT_TRUE(state.publish_submitted(a, 10)); // accepted submit's late future is not lost
  EXPECT_TRUE(state.abort_reservation(b)); // still provably pre-submit
  const auto snapshot = state.snapshot();
  EXPECT_EQ(snapshot.phase, Phase::Failed);
  EXPECT_EQ(snapshot.first_error, "device lost");
  EXPECT_EQ(snapshot.slots[a.slot].status, Status::Submitted);
  EXPECT_EQ(snapshot.slots[a.slot].forward, 10u);
  EXPECT_EQ(snapshot.mailbox_size, 1u);
  EXPECT_EQ(snapshot.mailbox[0]->descriptor, a);
  EXPECT_FALSE(state.try_begin_read());
  ASSERT_TRUE(state.quarantine(a, "unknown forward completion"));
  EXPECT_FALSE(state.complete_forward(a, 10));
  EXPECT_FALSE(state.recycle(a));
  EXPECT_FALSE(state.try_stop());
  EXPECT_EQ(state.snapshot().first_error, "device lost");
}

TEST_F(H700PresentationStateTest, QuarantinedSubmitAcceptsLateFutureWithoutOverwritingMailbox) {
  const auto a = reserve(0);
  const auto b = reserve(1);
  publish(b, 11);
  ASSERT_TRUE(state.mark_submitted(a));
  ASSERT_TRUE(state.quarantine(a, "future registration failed"));
  ASSERT_TRUE(state.publish_submitted(a, 10));
  EXPECT_FALSE(state.publish_submitted(a, 12));
  EXPECT_FALSE(state.quarantine(a, "duplicate"));
  EXPECT_FALSE(state.abort_reservation(a));
  EXPECT_FALSE(state.recycle(a));
  const auto snapshot = state.snapshot();
  EXPECT_EQ(snapshot.slots[a.slot].status, Status::Quarantined);
  EXPECT_EQ(snapshot.slots[a.slot].forward, 10u);
  ASSERT_EQ(snapshot.mailbox_size, 2u);
  EXPECT_EQ(snapshot.mailbox[0]->descriptor, b);
  EXPECT_EQ(snapshot.mailbox[1]->descriptor, a);
  EXPECT_EQ(snapshot.mailbox[1]->forward, 10u);
  EXPECT_EQ(snapshot.first_error, "future registration failed");
  const auto notifications = state.take_notifications();
  ASSERT_EQ(notifications.size(), 1u);
  EXPECT_EQ(notifications[0].outcome, Outcome::Quarantined);
}

TEST_F(H700PresentationStateTest, UnknownReverseNeverReusesAndFailureStopRetainsEvidence) {
  const auto a = reserve(0);
  const auto b = reserve(1);
  publish(a, 10);
  publish(b, 11);
  read(a, 10);
  ASSERT_TRUE(state.begin_retiring(a, 20));
  ASSERT_TRUE(state.complete_swap(a));
  ASSERT_TRUE(state.quarantine(a, "reverse wait failed"));
  EXPECT_FALSE(state.complete_reverse(a, 20));
  EXPECT_FALSE(state.recycle(a));
  EXPECT_FALSE(state.try_reserve(2));
  EXPECT_FALSE(state.try_stop());
  // Simulate only the CPU wrapper's post-teardown attestation, NOT an actual GPU teardown.
  ASSERT_TRUE(state.stop_after_teardown());
  const auto snapshot = state.snapshot();
  EXPECT_EQ(snapshot.phase, Phase::Stopped);
  EXPECT_EQ(snapshot.slots[a.slot].status, Status::Quarantined);
  EXPECT_EQ(snapshot.slots[a.slot].reverse, 20u);
  EXPECT_FALSE(snapshot.slots[a.slot].reverse_complete);
  EXPECT_TRUE(snapshot.slots[a.slot].swap_succeeded);
  EXPECT_EQ(snapshot.slots[b.slot].status, Status::Submitted);
  EXPECT_EQ(snapshot.slots[b.slot].forward, 11u);
  ASSERT_EQ(snapshot.mailbox_size, 1u);
  EXPECT_EQ(snapshot.mailbox[0]->descriptor, b);
  EXPECT_FALSE(state.complete_forward(b, 11));
  EXPECT_FALSE(state.publish_submitted(b, 12));
  EXPECT_FALSE(state.mark_submitted(b));
  EXPECT_FALSE(state.try_begin_read());
  EXPECT_FALSE(state.begin_retiring(a, 21));
  EXPECT_FALSE(state.abort_reservation(b));
  EXPECT_FALSE(state.complete_swap(a));
  EXPECT_FALSE(state.quarantine(b, "too late"));
  EXPECT_FALSE(state.stop_after_teardown());
  state.begin_draining();
  state.fail("too late");
  EXPECT_EQ(state.snapshot().phase, Phase::Stopped);
  EXPECT_EQ(state.snapshot().first_error, "reverse wait failed");
  const auto notifications = state.take_notifications();
  ASSERT_EQ(notifications.size(), 1u); // swap succeeded: no duplicate frame result on quarantine
  EXPECT_EQ(notifications[0].outcome, Outcome::Presented);
}

TEST_F(H700PresentationStateTest, FailedWithOnlyPreSubmitWorkCanAbortAndStopNormally) {
  const auto a = reserve(0);
  EXPECT_FALSE(state.quarantine(a, "not submitted"));
  state.fail("recording failed");
  ASSERT_TRUE(state.abort_reservation(a));
  ASSERT_TRUE(state.try_stop());
  EXPECT_FALSE(state.try_reserve(1));
  EXPECT_EQ(state.snapshot().first_error, "recording failed");
  const auto notifications = state.take_notifications();
  ASSERT_EQ(notifications.size(), 1u);
  EXPECT_EQ(notifications[0].outcome, Outcome::Aborted);
}

TEST_F(H700PresentationStateTest, FailureDuringDrainingPreservesBothMailboxEntries) {
  const auto a = reserve(0);
  const auto b = reserve(1);
  publish(a, 10);
  publish(b, 11);
  state.begin_draining();
  ASSERT_TRUE(state.quarantine(a, "forward failed"));
  state.fail("secondary failure");
  const auto snapshot = state.snapshot();
  EXPECT_EQ(snapshot.phase, Phase::Failed);
  EXPECT_EQ(snapshot.first_error, "forward failed");
  ASSERT_EQ(snapshot.mailbox_size, 2u);
  EXPECT_EQ(snapshot.mailbox[0]->forward, 10u);
  EXPECT_EQ(snapshot.mailbox[1]->forward, 11u);
  EXPECT_EQ(state.front_submission()->descriptor, a);
  EXPECT_FALSE(state.try_begin_read());
  EXPECT_FALSE(state.try_stop());
}

TEST_F(H700PresentationStateTest, ConcurrentErrorsPreserveAnOwnedFirstRecord) {
  std::string error = "first error";
  state.fail(error);
  error.assign("caller storage changed");
  std::thread first([&] { state.fail("worker error"); });
  std::thread second([&] { state.fail("device callback error"); });
  first.join();
  second.join();
  EXPECT_EQ(state.snapshot().first_error, "first error");
  EXPECT_EQ(state.snapshot().phase, Phase::Failed);
  EXPECT_TRUE(state.try_stop());
}

TEST(H700PresentationState, ZeroGenerationIsTerminalAndEmptyDrainCanStop) {
  PresentationState invalid{0};
  EXPECT_EQ(invalid.snapshot().phase, Phase::Failed);
  EXPECT_TRUE(invalid.snapshot().first_error);
  EXPECT_FALSE(invalid.try_reserve(0));
  EXPECT_TRUE(invalid.try_stop());
  PresentationState empty{1};
  EXPECT_FALSE(empty.try_stop());
  EXPECT_FALSE(empty.stop_after_teardown());
  empty.begin_draining();
  empty.begin_draining();
  EXPECT_TRUE(empty.try_stop());
  EXPECT_FALSE(empty.try_reserve(0));
}

} // namespace
} // namespace aurora::h700
