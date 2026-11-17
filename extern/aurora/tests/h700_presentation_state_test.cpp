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
