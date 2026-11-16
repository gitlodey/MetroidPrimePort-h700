#include "gfx/pipeline_wait.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

namespace {
using namespace std::chrono_literals;
using aurora::gfx::detail::PipelineWaitOutcome;
using aurora::gfx::detail::PipelineWorkControl;
using aurora::gfx::detail::wait_pipeline;
namespace worker = aurora::gfx::render_worker;

class PipelineWaitTest : public testing::Test {
protected:
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic_bool healthy{true};
  unsigned progressCount = 0;
  PipelineWaitOutcome result = PipelineWaitOutcome::Pending;

  void SetUp() override { ASSERT_TRUE(worker::prepare_bootstrap()); }
  void TearDown() override { EXPECT_TRUE(worker::prepare_bootstrap()); }
  void hooks(worker::WorkCallback progress = {}) {
    worker::set_wait_hooks({.main = std::this_thread::get_id(),
                           .healthy = [this] { return healthy.load(std::memory_order_acquire); },
                           .progress = [this, progress = std::move(progress)] {
                             ++progressCount;
                             std::unique_lock lock{mutex, std::try_to_lock};
                             EXPECT_TRUE(lock.owns_lock()) << "main progress holds the cache lock";
                             if (lock.owns_lock()) lock.unlock();
                             if (progress) progress();
                           }});
  }
};

TEST_F(PipelineWaitTest, ReadyDoesNotNeedProgress) {
  hooks();
  std::unique_lock lock{mutex};
  EXPECT_EQ(wait_pipeline(lock, cv, [] { return PipelineWaitOutcome::Ready; }), PipelineWaitOutcome::Ready);
  EXPECT_EQ(progressCount, 0u);
}

TEST_F(PipelineWaitTest, IndependentCompletionAndMainProgressRunOutsideCacheLock) {
  std::atomic_bool compileMayReturn{false};
  PipelineWorkControl control;
  control.started();
  std::thread compiler{[&] {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!compileMayReturn.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    {
      std::lock_guard lock{mutex};
      result = PipelineWaitOutcome::Ready;
    }
    control.returned();
    cv.notify_all();
  }};
  hooks([&] { compileMayReturn.store(true, std::memory_order_release); });
  std::unique_lock lock{mutex};
  const auto outcome = wait_pipeline(lock, cv, [&] { return result; });
  lock.unlock();
  compiler.join();
  EXPECT_EQ(outcome, PipelineWaitOutcome::Ready);
  EXPECT_GT(progressCount, 0u);
  EXPECT_EQ(control.active(), 0u);
}

TEST_F(PipelineWaitTest, FailureCancelsWaitButKeepsActiveCallAndCapturesUntilReturn) {
  PipelineWorkControl control;
  control.started();
  auto capture = std::make_shared<int>(1);
  const std::weak_ptr<int> lifetime = capture;
  std::atomic_bool release{false};
  std::thread compiler{[&, capture = std::move(capture)]() mutable {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!release.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    // Mirrors production: return from the call, release captures, THEN acknowledge ownership.
    capture.reset();
    control.returned();
  }};
  hooks([&] { healthy.store(false, std::memory_order_release); });
  std::unique_lock lock{mutex};
  EXPECT_EQ(wait_pipeline(lock, cv, [&] { return result; }), PipelineWaitOutcome::Failed);
  lock.unlock();
  EXPECT_EQ(control.active(), 1u);
  EXPECT_FALSE(lifetime.expired());
  release.store(true, std::memory_order_release);
  compiler.join();
  EXPECT_EQ(control.active(), 0u);
  EXPECT_TRUE(lifetime.expired());
}

TEST_F(PipelineWaitTest, FailedAndCancelledAreNeverReady) {
  hooks();
  std::unique_lock lock{mutex};
  EXPECT_EQ(wait_pipeline(lock, cv, [] { return PipelineWaitOutcome::Failed; }), PipelineWaitOutcome::Failed);
  EXPECT_EQ(wait_pipeline(lock, cv, [] { return PipelineWaitOutcome::Cancelled; }), PipelineWaitOutcome::Cancelled);
}

TEST_F(PipelineWaitTest, TerminalFailureWinsOverLateReadyPublication) {
  hooks();
  healthy.store(false, std::memory_order_release);
  std::unique_lock lock{mutex};
  EXPECT_EQ(wait_pipeline(lock, cv, [] { return PipelineWaitOutcome::Ready; }), PipelineWaitOutcome::Failed);
}

TEST_F(PipelineWaitTest, DrainingRejectsBackgroundButFinishesAcceptedContinuations) {
  PipelineWorkControl control;
  EXPECT_TRUE(control.background_open());
  EXPECT_TRUE(control.continuation_open());
  worker::begin_draining();
  EXPECT_FALSE(control.background_open());
  EXPECT_TRUE(control.continuation_open());
  control.begin_draining();
  control.started(); // an accepted frame may still publish a required compiler continuation
  control.close_continuations(); // only after upstream producers finish
  EXPECT_FALSE(control.background_open());
  EXPECT_FALSE(control.continuation_open());
  EXPECT_EQ(control.active(), 1u);
  hooks([&] { control.returned(); });
  std::unique_lock lock{mutex};
  EXPECT_EQ(wait_pipeline(lock, cv, [&] {
    return control.active() == 0 ? PipelineWaitOutcome::Ready : PipelineWaitOutcome::Pending;
  }), PipelineWaitOutcome::Ready);
  EXPECT_GT(progressCount, 0u);
}

TEST_F(PipelineWaitTest, NonMainWaitObservesFailureWithoutRunningMainProgress) {
  hooks();
  std::atomic_bool waiting{false};
  PipelineWaitOutcome outcome = PipelineWaitOutcome::Pending;
  std::thread waiter{[&] {
    std::unique_lock lock{mutex};
    waiting.store(true, std::memory_order_release);
    outcome = wait_pipeline(lock, cv, [&] { return result; });
  }};
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!waiting.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  healthy.store(false, std::memory_order_release);
  cv.notify_all();
  waiter.join();
  EXPECT_EQ(outcome, PipelineWaitOutcome::Failed);
  EXPECT_EQ(progressCount, 0u);
}

TEST(PipelineCompilerSource, ActualCompilerKeepsPriorityQueuesIndependentAndJoinsBeforeRelease) {
  std::ifstream file(PIPELINE_CACHE_SOURCE_FILE);
  ASSERT_TRUE(file.good());
  const std::string source{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>()};
  const auto start = source.find("static void pipeline_worker()");
  const auto end = source.find("// Pipelines compile on a pool", start);
  ASSERT_NE(start, std::string::npos);
  ASSERT_NE(end, std::string::npos);
  const auto compiler = source.substr(start, end - start);
  const auto call = compiler.find("auto result = create_pipeline_timed(");
  ASSERT_NE(call, std::string::npos);
  EXPECT_LT(compiler.find("g_pipelineWork.started()"), call);
  EXPECT_LT(call, compiler.find("pending.create = {}"));
  EXPECT_LT(compiler.find("pending.create = {}"), compiler.find("g_pipelineWork.returned()"));
  EXPECT_NE(compiler.find("source.pop_front();\n    }\n    auto result"), std::string::npos);
  EXPECT_EQ(compiler.find("enqueue_work"), std::string::npos);
  EXPECT_EQ(compiler.find("enqueue_encode_pass"), std::string::npos);
  EXPECT_NE(source.find("g_pipelineThreads.emplace_back(pipeline_worker)"), std::string::npos);
  const auto lookup = source.substr(source.find("if (blocking && !pipelineReady)"),
                                   source.find("std::string detail::pipeline_cache_path") -
                                       source.find("if (blocking && !pipelineReady)"));
  EXPECT_NE(lookup.find("detail::wait_pipeline(lock, g_pipelineReadyCv"), std::string::npos);
  EXPECT_NE(lookup.find("outcome != detail::PipelineWaitOutcome::Ready"), std::string::npos);
  const auto shutdown = source.substr(source.find("bool shutdown_pipeline_cache()"),
                                     source.find("void drop_pipelines()") - source.find("bool shutdown_pipeline_cache()"));
  const auto join = source.substr(source.find("PipelineShutdownStatus poll_pipeline_cache_shutdown()"),
                                  source.find("bool join_pipeline_cache_workers()") - source.find("PipelineShutdownStatus poll_pipeline_cache_shutdown()"));
  EXPECT_LT(join.find("!g_pipelineQueue.empty() || g_pipelineWork.active() != 0"), join.find("g_pipelineThreadEnd = true"));
  EXPECT_LT(join.find("g_livePipelineThreads.load"), join.find("thread.join()"));
  EXPECT_LT(join.find("g_pipelineCacheWriterExited.load"), join.find("g_pipelineCacheWriterThread.join()"));
  EXPECT_LT(shutdown.find("join_pipeline_cache_workers()"), shutdown.find("g_pipelines.clear()"));
  EXPECT_LT(shutdown.find("stop_pipeline_cache_writer()"), shutdown.find("pipeline_cache_abort()"));
  const auto count = source.substr(source.find("static size_t pipeline_thread_count()"), 750);
  EXPECT_LT(count.find("wgpu::BackendType::OpenGLES"), count.find("return 1;"));
}
} // namespace
