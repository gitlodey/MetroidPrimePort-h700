#include "../lib/gfx/render_worker.hpp"
#include "../lib/h700/lifecycle.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace std::chrono_literals;
using aurora::gfx::render_worker::BoundedQueue;
using aurora::gfx::render_worker::FrameSlotPool;
using aurora::gfx::render_worker::ItemType;
using aurora::gfx::render_worker::QueueItem;

class RenderWorkerTest : public testing::Test {
protected:
  void SetUp() override { ASSERT_TRUE(aurora::gfx::render_worker::prepare_bootstrap()); }
  void TearDown() override {
    aurora::gfx::render_worker::set_wait_hooks({});
    aurora::gfx::render_worker::shutdown();
    EXPECT_TRUE(aurora::gfx::render_worker::prepare_bootstrap());
  }
};

TEST(RenderWorkerQueue, PreservesOrdering) {
  BoundedQueue queue{4};
  ASSERT_TRUE(queue.push(QueueItem{.type = ItemType::BeginFrame, .frameId = 1}));
  ASSERT_TRUE(queue.push(QueueItem{.type = ItemType::EncodePass, .frameId = 1, .passIndex = 7}));
  ASSERT_TRUE(queue.push(QueueItem{.type = ItemType::EndFrame, .frameId = 1}));

  bool closed = false;
  auto first = queue.pop_for(0ms, closed);
  auto second = queue.pop_for(0ms, closed);
  auto third = queue.pop_for(0ms, closed);

  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(first->type, ItemType::BeginFrame);
  EXPECT_EQ(second->type, ItemType::EncodePass);
  EXPECT_EQ(second->passIndex, 7u);
  EXPECT_EQ(third->type, ItemType::EndFrame);
  EXPECT_FALSE(closed);
}

TEST(RenderWorkerQueue, PushBlocksWhenFull) {
  BoundedQueue queue{1};
  ASSERT_TRUE(queue.push(QueueItem{.type = ItemType::BeginFrame}));

  std::atomic_bool pushed = false;
  auto future = std::async(std::launch::async, [&] {
    const bool result = queue.push(QueueItem{.type = ItemType::EncodePass});
    pushed.store(result, std::memory_order_release);
  });

  std::this_thread::sleep_for(20ms);
  EXPECT_FALSE(pushed.load(std::memory_order_acquire));

  bool closed = false;
  ASSERT_TRUE(queue.pop_for(0ms, closed).has_value());
  future.wait();
  EXPECT_TRUE(pushed.load(std::memory_order_acquire));
}

TEST_F(RenderWorkerTest, SyncWaitsForPriorWork) {
  std::vector<int> order;
  aurora::gfx::render_worker::initialize();
  aurora::gfx::render_worker::enqueue_begin_frame(1, [&] { order.push_back(1); });
  aurora::gfx::render_worker::enqueue_encode_pass(1, 0, [&] { order.push_back(2); });
  aurora::gfx::render_worker::synchronize();

  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_TRUE(aurora::gfx::render_worker::is_idle());
}

TEST_F(RenderWorkerTest, ShutdownCompletesQueuedWork) {
  std::atomic_int count = 0;
  aurora::gfx::render_worker::initialize();
  aurora::gfx::render_worker::enqueue_begin_frame(1, [&] { count.fetch_add(1, std::memory_order_acq_rel); });
  aurora::gfx::render_worker::shutdown();

  EXPECT_EQ(count.load(std::memory_order_acquire), 1);
  EXPECT_TRUE(aurora::gfx::render_worker::is_idle());
}

TEST_F(RenderWorkerTest, ShutdownPollKeepsStalledJobAliveAndReturnsToDriverDeadline) {
  namespace rw = aurora::gfx::render_worker;
  using namespace aurora::h700;
  std::atomic_bool allowReturn{false};
  std::promise<void> entered;
  auto payload = std::make_shared<int>(42);
  std::weak_ptr<int> witness = payload;
  struct Unblock {
    std::atomic_bool& flag;
    ~Unblock() { flag.store(true, std::memory_order_release); aurora::gfx::render_worker::shutdown(); }
  } unblock{allowReturn};
  rw::initialize();
  ASSERT_EQ(rw::enqueue_work([&entered, &allowReturn, payload] {
    entered.set_value();
    while (!allowReturn.load(std::memory_order_acquire)) std::this_thread::yield();
    EXPECT_EQ(*payload, 42);
  }), rw::Outcome::Completed);
  ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
  payload.reset();
  GenerationLifecycle lifecycle;
  ASSERT_TRUE(lifecycle.begin());
  lifecycle.request_stop();
  ASSERT_TRUE(lifecycle.advance(ShutdownStage::Producers));
  ASSERT_TRUE(lifecycle.advance(ShutdownStage::Fifo));
  rw::request_shutdown();
  const auto deadline = std::chrono::steady_clock::now() + 10ms;
  size_t polls = 0;
  do {
    EXPECT_EQ(rw::poll_shutdown(), rw::Outcome::Pending);
    EXPECT_EQ(lifecycle.result().status, ShutdownStatus::Pending);
    EXPECT_EQ(lifecycle.stage(), ShutdownStage::Render);
    EXPECT_FALSE(rw::is_quiescent());
    EXPECT_FALSE(witness.expired());
    ++polls;
  } while (std::chrono::steady_clock::now() < deadline);
  EXPECT_GT(polls, 1u); // driver reached its deadline without releasing/cancelling the running job
  allowReturn.store(true, std::memory_order_release);
  auto result = rw::Outcome::Pending;
  const auto joinDeadline = std::chrono::steady_clock::now() + 2s;
  while ((result = rw::poll_shutdown()) == rw::Outcome::Pending && std::chrono::steady_clock::now() < joinDeadline)
    std::this_thread::yield();
  EXPECT_EQ(result, rw::Outcome::Completed);
  EXPECT_TRUE(rw::is_quiescent());
  EXPECT_TRUE(witness.expired());
  EXPECT_TRUE(lifecycle.advance(ShutdownStage::Render));
  EXPECT_EQ(rw::poll_shutdown(), rw::Outcome::Completed);
}

TEST_F(RenderWorkerTest, AcceptedFrameTailRetainsFullQueueAdmissionAndExecutesOnceInOrder) {
  namespace rw = aurora::gfx::render_worker;
  std::atomic_bool allowReturn{false};
  std::promise<void> entered;
  std::atomic_size_t completed{0};
  auto payload = std::make_shared<int>(7);
  std::weak_ptr<int> witness = payload;
  std::vector<int> order;
  int cancelled = 0;
  rw::FrameTail tail;
  struct Unblock {
    std::atomic_bool& flag;
    ~Unblock() { flag.store(true, std::memory_order_release); aurora::gfx::render_worker::shutdown(); }
  } unblock{allowReturn};
  rw::initialize();
  rw::enqueue_work([&] {
    entered.set_value();
    while (!allowReturn.load(std::memory_order_acquire)) std::this_thread::yield();
  });
  ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
  for (size_t i = 0; i < 256; ++i) ASSERT_EQ(rw::enqueue_work([&] { ++completed; }), rw::Outcome::Completed);
  rw::begin_draining();
  ASSERT_TRUE(tail.capture([&] {
    EXPECT_TRUE(rw::capturing_frame_tail());
    EXPECT_EQ(rw::enqueue_encode_pass(1, 0, [&, payload] { order.push_back(*payload); }, [&] { ++cancelled; }), rw::Outcome::Completed);
    EXPECT_EQ(rw::enqueue_end_frame(1, [&] { order.push_back(8); }, [&] { ++cancelled; }), rw::Outcome::Completed);
  }));
  EXPECT_FALSE(rw::capturing_frame_tail());
  payload.reset();
  const auto deadline = std::chrono::steady_clock::now() + 10ms;
  size_t polls = 0;
  do {
    EXPECT_EQ(tail.poll(), rw::Outcome::Pending);
    EXPECT_FALSE(witness.expired());
    EXPECT_TRUE(order.empty()); // no rejected graphics job ran inline
    EXPECT_EQ(cancelled, 0);
    ++polls;
  } while (std::chrono::steady_clock::now() < deadline);
  EXPECT_GT(polls, 1u);
  allowReturn.store(true, std::memory_order_release);
  auto result = rw::Outcome::Pending;
  const auto admissionDeadline = std::chrono::steady_clock::now() + 2s;
  while ((result = tail.poll()) == rw::Outcome::Pending && std::chrono::steady_clock::now() < admissionDeadline)
    std::this_thread::yield();
  EXPECT_EQ(result, rw::Outcome::Completed);
  EXPECT_TRUE(rw::shutdown());
  EXPECT_EQ(completed.load(), 256u);
  EXPECT_EQ(order, (std::vector<int>{7, 8}));
  EXPECT_EQ(cancelled, 0);
  EXPECT_TRUE(witness.expired());
  EXPECT_EQ(tail.poll(), rw::Outcome::Completed);
}

TEST_F(RenderWorkerTest, FailedRetainedTailAbortsExactlyOnceWithoutInlineGraphics) {
  namespace rw = aurora::gfx::render_worker;
  rw::initialize();
  bool ran = false;
  int aborted = 0;
  auto payload = std::make_shared<int>(9);
  std::weak_ptr<int> witness = payload;
  rw::FrameTail tail;
  ASSERT_TRUE(tail.capture([&] {
    rw::enqueue_end_frame(1, [&, payload] { ran = true; }, [&] { ++aborted; });
  }));
  payload.reset();
  EXPECT_FALSE(witness.expired());
  rw::notify_failure();
  EXPECT_EQ(tail.poll(), rw::Outcome::Failed);
  EXPECT_EQ(aborted, 1);
  EXPECT_FALSE(ran);
  EXPECT_TRUE(witness.expired());
  EXPECT_EQ(tail.poll(), rw::Outcome::Completed);
  EXPECT_EQ(aborted, 1);
  EXPECT_TRUE(rw::shutdown());
}

TEST(RenderWorkerFrameSlots, RecyclesReleasedSlots) {
  FrameSlotPool slots{2};
  const size_t first = slots.acquire();
  const size_t second = slots.acquire();
  EXPECT_NE(first, second);
  EXPECT_EQ(slots.free_count(), 0u);

  std::atomic_bool acquired = false;
  auto future = std::async(std::launch::async, [&] {
    const size_t recycled = slots.acquire();
    EXPECT_EQ(recycled, first);
    acquired.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(20ms);
  EXPECT_FALSE(acquired.load(std::memory_order_acquire));
  slots.release(first);
  future.wait();
  slots.release(second);
  EXPECT_EQ(slots.free_count(), 1u);
}
TEST_F(RenderWorkerTest, FullQueueProgressRunsWithoutQueueLock) {
  namespace rw = aurora::gfx::render_worker;
  BoundedQueue queue{1};
  ASSERT_TRUE(queue.push({}));
  int progress = 0;
  rw::set_wait_hooks({.main = std::this_thread::get_id(), .healthy = [] { return true; }, .progress = [&] {
    ++progress;
    EXPECT_EQ(queue.size(), 1u); // would deadlock if progress held the queue lock
    bool closed = false;
    EXPECT_TRUE(queue.pop_for(0ms, closed));
  }});
  EXPECT_EQ(queue.push_result({}), rw::Outcome::Completed);
  EXPECT_EQ(progress, 1);
}

TEST_F(RenderWorkerTest, ResourceThreadDoesNotRunMainProgress) {
  namespace rw = aurora::gfx::render_worker;
  BoundedQueue queue{1};
  ASSERT_TRUE(queue.push({}));
  std::atomic_int progress = 0;
  rw::set_wait_hooks({.main = std::this_thread::get_id(), .healthy = [] { return true; }, .progress = [&] { ++progress; }});
  auto producer = std::async(std::launch::async, [&] { return queue.push_result({}); });
  EXPECT_EQ(producer.wait_for(10ms), std::future_status::timeout);
  bool closed = false;
  EXPECT_TRUE(queue.pop_for(0ms, closed));
  EXPECT_EQ(producer.get(), rw::Outcome::Completed);
  EXPECT_EQ(progress.load(), 0);
}

TEST_F(RenderWorkerTest, SyncProgressRunsWithoutSyncLock) {
  namespace rw = aurora::gfx::render_worker;
  std::promise<void> started, release;
  auto released = release.get_future().share();
  bool signalled = false;
  rw::set_wait_hooks({.main = std::this_thread::get_id(), .healthy = [] { return true; }, .progress = [&] {
    if (!signalled) { signalled = true; release.set_value(); }
  }});
  rw::initialize();
  rw::enqueue_work([&started, released] { started.set_value(); released.wait(); });
  started.get_future().wait();
  EXPECT_EQ(rw::synchronize(), rw::Outcome::Completed);
  EXPECT_TRUE(signalled);
}

TEST_F(RenderWorkerTest, FailedWaitDoesNotDestroyExecutingJobCaptures) {
  namespace rw = aurora::gfx::render_worker;
  std::promise<void> started, release;
  auto released = release.get_future().share();
  auto payload = std::make_shared<int>(42);
  std::weak_ptr<int> observed = payload;
  std::atomic_bool healthy = true;
  rw::set_wait_hooks({.main = std::this_thread::get_id(), .healthy = [&] { return healthy.load(); },
                     .progress = [&] { healthy.store(false); }});
  rw::initialize();
  rw::enqueue_work([&started, released, payload] { started.set_value(); released.wait(); EXPECT_EQ(*payload, 42); });
  started.get_future().wait();
  payload.reset();
  EXPECT_EQ(rw::synchronize(), rw::Outcome::Failed);
  EXPECT_FALSE(observed.expired());
  EXPECT_FALSE(rw::is_quiescent());
  release.set_value();
  rw::shutdown();
  EXPECT_TRUE(observed.expired());
  EXPECT_TRUE(rw::is_quiescent());
}

TEST_F(RenderWorkerTest, ClosedQueueRejectsSyncAndGraphicsWithoutInlineFallback) {
  namespace rw = aurora::gfx::render_worker;
  rw::initialize();
  rw::close_admission();
  bool ran = false;
  int cancelled = 0;
  EXPECT_EQ(rw::enqueue_work([&] { ran = true; }, [&] { ++cancelled; }), rw::Outcome::Rejected);
  EXPECT_EQ(rw::synchronize(), rw::Outcome::Rejected);
  EXPECT_FALSE(ran);
  EXPECT_EQ(cancelled, 1);
  rw::shutdown();
  EXPECT_EQ(rw::enqueue_work([&] { ran = true; }, [&] { ++cancelled; }), rw::Outcome::Rejected);
  EXPECT_FALSE(ran);
  EXPECT_EQ(cancelled, 2);
}

TEST_F(RenderWorkerTest, DrainingRootsPreservesAcceptedContinuations) {
  namespace rw = aurora::gfx::render_worker;
  rw::initialize();
  rw::begin_draining();
  EXPECT_FALSE(rw::root_admission_open());
  std::atomic_int completed = 0;
  EXPECT_EQ(rw::enqueue_begin_frame(1, [&] { ++completed; }), rw::Outcome::Completed);
  EXPECT_EQ(rw::enqueue_encode_pass(1, 0, [&] { ++completed; }), rw::Outcome::Completed);
  EXPECT_EQ(rw::enqueue_end_frame(1, [&] { ++completed; }), rw::Outcome::Completed);
  EXPECT_EQ(rw::synchronize(), rw::Outcome::Completed);
  rw::shutdown();
  EXPECT_EQ(completed.load(), 3);
}

TEST_F(RenderWorkerTest, PartialSlotAcquireRollsBackAndProgressOwnsNoPoolLock) {
  namespace rw = aurora::gfx::render_worker;
  FrameSlotPool frames{1}, staging{1};
  const auto busy = *staging.try_acquire();
  std::atomic_bool healthy = true;
  rw::set_wait_hooks({.main = std::this_thread::get_id(), .healthy = [&] { return healthy.load(); }, .progress = [&] {
    EXPECT_EQ(frames.free_count(), 0u);
    EXPECT_EQ(staging.free_count(), 0u);
    healthy.store(false);
  }});
  {
    const auto acquired = frames.acquire_wait();
    ASSERT_TRUE(acquired);
    rw::SlotLease rollback(frames, *acquired);
    EXPECT_FALSE(staging.acquire_wait());
  }
  EXPECT_EQ(frames.free_count(), 1u);
  EXPECT_EQ(staging.free_count(), 0u);
  staging.release(busy);
}
} // namespace
