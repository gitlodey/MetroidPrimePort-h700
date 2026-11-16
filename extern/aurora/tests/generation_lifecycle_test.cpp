#include "h700/lifecycle.hpp"

#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <string>

namespace {
using aurora::h700::GenerationLifecycle;
using aurora::h700::ShutdownStage;
using aurora::h700::ShutdownStatus;
constexpr std::array Stages{ShutdownStage::Producers, ShutdownStage::Fifo, ShutdownStage::Render,
                            ShutdownStage::Compiler, ShutdownStage::Maps, ShutdownStage::Native,
                            ShutdownStage::Holders, ShutdownStage::Device, ShutdownStage::Exports, ShutdownStage::Window};

TEST(GenerationLifecycleTest, OrderedReleaseAndSecondCleanGeneration) {
  GenerationLifecycle lifecycle;
  ASSERT_TRUE(lifecycle.begin());
  EXPECT_FALSE(lifecycle.advance(ShutdownStage::Device));
  lifecycle.request_stop();
  for (const auto stage : Stages) {
    EXPECT_EQ(lifecycle.stage(), stage);
    EXPECT_EQ(lifecycle.result().status, ShutdownStatus::Pending);
    EXPECT_FALSE(lifecycle.begin());
    ASSERT_TRUE(lifecycle.advance(stage));
  }
  EXPECT_EQ(lifecycle.result().status, ShutdownStatus::Released);
  ASSERT_TRUE(lifecycle.begin());
  EXPECT_EQ(lifecycle.stage(), ShutdownStage::Running);
}

TEST(GenerationLifecycleTest, PendingOrFailedWaitDoesNotAdvanceOwnershipEvidence) {
  GenerationLifecycle lifecycle;
  ASSERT_TRUE(lifecycle.begin());
  lifecycle.request_stop();
  ASSERT_TRUE(lifecycle.advance(ShutdownStage::Producers));
  lifecycle.fail();
  for (int i = 0; i < 5; ++i) {
    lifecycle.request_stop();
    EXPECT_EQ(lifecycle.stage(), ShutdownStage::Fifo);
    EXPECT_EQ(lifecycle.result().status, ShutdownStatus::Pending);
    EXPECT_FALSE(lifecycle.advance(ShutdownStage::Holders));
  }
}

TEST(GenerationLifecycleTest, FailedButActuallyReleasedIsNotLiveRecovery) {
  GenerationLifecycle lifecycle;
  ASSERT_TRUE(lifecycle.begin());
  lifecycle.fail();
  lifecycle.request_stop();
  for (const auto stage : Stages) ASSERT_TRUE(lifecycle.advance(stage));
  EXPECT_EQ(lifecycle.result().status, ShutdownStatus::FailedReleased);
  EXPECT_FALSE(lifecycle.begin());
}

TEST(GenerationLifecycleTest, UnknownIsStickyAndCannotBecomeReleaseOrRestart) {
  GenerationLifecycle lifecycle;
  ASSERT_TRUE(lifecycle.begin());
  lifecycle.request_stop();
  lifecycle.unknown();
  lifecycle.request_stop();
  EXPECT_FALSE(lifecycle.advance(ShutdownStage::Unknown));
  EXPECT_FALSE(lifecycle.advance(ShutdownStage::Exports));
  EXPECT_EQ(lifecycle.result().status, ShutdownStatus::Unknown);
  EXPECT_FALSE(lifecycle.begin());
}

TEST(GenerationLifecycleTest, RepeatedShutdownDoesNotReopenOrRepeatStages) {
  GenerationLifecycle lifecycle;
  EXPECT_EQ(lifecycle.result().status, ShutdownStatus::Released);
  ASSERT_TRUE(lifecycle.begin());
  lifecycle.request_stop();
  for (const auto stage : Stages) ASSERT_TRUE(lifecycle.advance(stage));
  for (int i = 0; i < 4; ++i) {
    lifecycle.request_stop();
    EXPECT_EQ(lifecycle.result().status, ShutdownStatus::Released);
    EXPECT_FALSE(lifecycle.advance(ShutdownStage::Window));
  }
}

std::string source(const char* path) {
  std::ifstream file(std::string(MAP_RETIREMENT_SOURCE_DIR) + "/" + path);
  EXPECT_TRUE(file.good()) << path;
  return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>()};
}
std::string section(const std::string& text, const char* first, const char* last) {
  const auto begin = text.find(first);
  const auto end = text.find(last, begin);
  if (begin == std::string::npos || end == std::string::npos) { ADD_FAILURE() << first << " .. " << last; return {}; }
  return text.substr(begin, end - begin);
}

TEST(GenerationLifecycleSource, ProducersAndTailFinishBeforeDownstreamCloseAndHolders) {
  const auto aurora = source("aurora.cpp");
  const auto poll = section(aurora, "h700::ShutdownResult poll_h700_shutdown()", "void shutdown() noexcept");
  EXPECT_LT(poll.find("case Stage::Producers"), poll.find("gx::fifo::poll_drain("));
  EXPECT_LT(poll.find("gx::fifo::poll_drain("), poll.find("g_shutdownFrameTail.capture("));
  EXPECT_LT(poll.find("g_shutdownFrameTail.poll()"), poll.find("case Stage::Fifo"));
  EXPECT_LT(poll.find("gx::fifo::poll_shutdown()"), poll.find("gfx::render_worker::request_shutdown()"));
  EXPECT_LT(poll.find("gfx::render_worker::poll_shutdown()"), poll.find("gfx::request_pipeline_cache_shutdown()"));
  EXPECT_LT(poll.find("gfx::retire_frame_resources_after_quiescence()"), poll.find("case Stage::Holders"));
  EXPECT_LT(poll.find("h700::native_frames_drained()"), poll.find("case Stage::Holders"));
  EXPECT_LT(poll.find("imgui::shutdown()"), poll.find("gfx::release_frame_holders_after_quiescence()"));
  EXPECT_LT(poll.find("gfx::release_frame_holders_after_quiescence()"), poll.find("webgpu::poll_h700_device_release()"));
  EXPECT_LT(poll.find("webgpu::poll_h700_device_release()"), poll.find("h700::release_exports()"));
  EXPECT_LT(poll.find("h700::release_exports()"), poll.find("window::shutdown()"));
  EXPECT_EQ(poll.find("take_notifications("), std::string::npos);
}

TEST(GenerationLifecycleSource, CursorReturnsToDeadlineRatherThanBlockingOnWorkersOrTailAdmission) {
  const auto aurora = source("aurora.cpp");
  const auto poll = section(aurora, "h700::ShutdownResult poll_h700_shutdown()", "void shutdown() noexcept");
  for (const auto forbidden : {"gx::fifo::drain()", "gx::fifo::shutdown()", "gfx::render_worker::shutdown()",
                               "join_pipeline_cache_workers()", "gfx::shutdown()", "end_frame()", "sleep_for(", ".join("})
    EXPECT_EQ(poll.find(forbidden), std::string::npos) << forbidden;
  EXPECT_NE(poll.find("g_shutdownFrameTail.capture([] { end_frame(true); })"), std::string::npos);
  EXPECT_NE(poll.find("tail == gfx::render_worker::Outcome::Pending) return g_generationLifecycle.result()"), std::string::npos);
  const auto end = section(aurora, "void end_frame(bool fifoDrained = false)", "void h700::request_generation_shutdown()");
  EXPECT_NE(end.find("!fifoDrained && !gx::fifo::drain()"), std::string::npos);
  const auto fifo = section(source("gx/fifo.cpp"), "PollStatus poll_drain(", "bool is_quiescent()");
  EXPECT_NE(fifo.find("std::try_to_lock"), std::string::npos);
  EXPECT_LT(fifo.find("sProcessed.load"), fifo.find("request.complete = true"));
  EXPECT_LT(fifo.find("sWorkerExited.load"), fifo.find("sWorkerThread.join()"));
  EXPECT_EQ(fifo.find("while ("), std::string::npos);
  const auto worker = section(source("gfx/render_worker.cpp"), "Outcome poll_shutdown()", "bool shutdown()");
  EXPECT_LT(worker.find("g_exited.load"), worker.find("g_thread.join()"));
  EXPECT_EQ(worker.find("while ("), std::string::npos);
  const auto compiler = section(source("gfx/pipeline_cache.cpp"), "PipelineShutdownStatus poll_pipeline_cache_shutdown()", "bool join_pipeline_cache_workers()");
  EXPECT_NE(compiler.find("std::try_to_lock"), std::string::npos);
  EXPECT_LT(compiler.find("!g_pipelineQueue.empty() || g_pipelineWork.active() != 0"), compiler.find("g_pipelineThreadEnd = true"));
  EXPECT_LT(compiler.find("g_livePipelineThreads.load"), compiler.find("thread.join()"));
  EXPECT_LT(compiler.find("g_pipelineCacheWriterExited.load"), compiler.find("g_pipelineCacheWriterThread.join()"));
  EXPECT_EQ(compiler.find("while ("), std::string::npos);
  EXPECT_EQ(compiler.find("stop_pipeline_cache_writer()"), std::string::npos);
  const auto frame = section(source("gfx/frame.cpp"), "bool release_frame_holders_after_quiescence()", "bool wait_for_staging_buffer(");
  EXPECT_NE(frame.find("pipeline_cache_threads_joined()"), std::string::npos);
  EXPECT_NE(frame.find("release_pipeline_cache_holders()"), std::string::npos);
  EXPECT_EQ(frame.find("while ("), std::string::npos);
  const auto growth = section(source("gfx/frame.cpp"), "bool grow_frame_buffer(", "bool grow_staging(");
  EXPECT_NE(growth.find("render_worker::capturing_frame_tail() || try_gpu_synchronize()"), std::string::npos);
}

TEST(GenerationLifecycleSource, EveryWorkerUnbindsOnItsOwnThreadBeforeExitAcknowledgement) {
  const auto fifo = section(source("gx/fifo.cpp"), "void worker_main(", "void start_worker(");
  EXPECT_LT(fifo.find("h700::unbind_current_dawn()"), fifo.find("sWorkerExited.store(true"));
  const auto render = section(source("gfx/render_worker.cpp"), "void worker_main(", "Outcome enqueue(");
  EXPECT_LT(render.find("item.reset()"), render.find("h700::unbind_current_dawn()"));
  EXPECT_LT(render.find("h700::unbind_current_dawn()"), render.find("g_exited.store(true"));
  const auto compiler = section(source("gfx/pipeline_cache.cpp"), "static void pipeline_worker()", "// Pipelines compile on a pool");
  EXPECT_LT(compiler.find("h700::unbind_current_dawn()"), compiler.find("g_livePipelineThreads.fetch_sub"));
  EXPECT_NE(compiler.find("pending.create = {}"), std::string::npos);
}

TEST(GenerationLifecycleSource, FinalQueueFutureCoversBootstrapAndSurvivesFailureUntilCallbackReturn) {
  const auto aurora = section(source("aurora.cpp"), "case Stage::Native:", "case Stage::Holders:");
  EXPECT_LT(aurora.find("webgpu::poll_h700_queue_drain()"), aurora.find("h700::native_frames_drained()"));
  EXPECT_NE(aurora.find("queue == webgpu::QueueDrainStatus::Failed) g_generationLifecycle.fail()"), std::string::npos);
  EXPECT_NE(aurora.find("queue == webgpu::QueueDrainStatus::Pending || !h700::native_frames_drained()"), std::string::npos);
  const auto gpu = source("webgpu/gpu.cpp");
  const auto drain = section(gpu, "QueueDrainStatus poll_h700_queue_drain()", "DeviceReleaseStatus poll_h700_device_release()");
  EXPECT_EQ(drain.find(".Submit("), std::string::npos);
  EXPECT_LT(drain.find("if (!g_queueDrain)"), drain.find("g_queue.OnSubmittedWorkDone("));
  EXPECT_NE(drain.find("wgpu::CallbackMode::WaitAnyOnly"), std::string::npos);
  EXPECT_NE(drain.find("[completion = g_queueDrain]"), std::string::npos);
  EXPECT_LT(drain.find("callback_called.load"), drain.find("g_queueDrain->settled = true"));
  EXPECT_NE(drain.find("wgpu::WaitStatus::TimedOut) return QueueDrainStatus::Pending"), std::string::npos);
  EXPECT_NE(drain.find("!= wgpu::QueueWorkDoneStatus::Success"), std::string::npos);
  EXPECT_EQ(drain.find("g_queueDrain.reset()"), std::string::npos);
  const auto release = section(gpu, "DeviceReleaseStatus poll_h700_device_release()", "bool release_h700_device()");
  EXPECT_LT(release.find("g_device.Destroy()"), release.find("poll_h700_queue_drain()"));
  EXPECT_LT(release.find("if (!g_queueDrain->settled) return DeviceReleaseStatus::Pending"), release.find("g_device = {}"));
  EXPECT_LT(release.find("g_instance = {}"), release.find("g_queueDrain.reset()"));
}

TEST(GenerationLifecycleSource, DestroyIsNotWholeGroupReleaseAndLifecycleMayRebindMain) {
  const auto gpu = section(source("webgpu/gpu.cpp"), "DeviceReleaseStatus poll_h700_device_release()", "void release_surface()");
  EXPECT_LT(gpu.find("g_device.Destroy()"), gpu.find("g_deviceLostObserved->load"));
  EXPECT_LT(gpu.find("h700::settle_native_callbacks()"), gpu.find("g_device = {}"));
  EXPECT_LT(gpu.find("g_device = {}"), gpu.find("g_adapter = {}"));
  EXPECT_LT(gpu.find("g_adapter = {}"), gpu.find("g_instance = {}"));
  EXPECT_LT(gpu.find("g_instance = {}"), gpu.find("h700::acknowledge_dawn_release()"));
  const auto afterFinalRelease = gpu.substr(gpu.find("g_instance = {}"));
  EXPECT_LT(afterFinalRelease.find("h700::unbind_current_dawn()"), afterFinalRelease.find("h700::acknowledge_dawn_release()"));
  const auto presentation = source("h700/presentation.cpp");
  const auto proof = section(presentation, "bool contexts_retired()", "} // namespace");
  EXPECT_NE(proof.find("std::all_of(backend->contexts.begin()"), std::string::npos);
  EXPECT_NE(proof.find("backend->trustworthy"), std::string::npos);
  EXPECT_NE(proof.find("std::none_of(backend->bindings.begin()"), std::string::npos);
  const auto delayed = section(presentation, "void retire_pending()", "void record_binding(");
  EXPECT_NE(delayed.find("context.destroy_requested"), std::string::npos);
  EXPECT_NE(delayed.find("binding.context == context.context"), std::string::npos);
}

TEST(GenerationLifecycleSource, FailedNativeRecordsNeedRealEvidenceAndNeverFakeRecycle) {
  const auto presentation = source("h700/presentation.cpp");
  const auto retire = section(presentation, "bool retire_native_after_dawn_release()", "bool release_dawn_imports()");
  EXPECT_LT(retire.find("contexts_retired()"), retire.find("glFinish()"));
  EXPECT_LT(retire.find("callback_called.load"), retire.find("glFinish()"));
  EXPECT_LT(retire.find("glFinish()"), retire.find("backend->flights[flight->descriptor.slot].reset()"));
  EXPECT_NE(retire.find("retired_flights[flight->descriptor.slot].emplace"), std::string::npos);
  EXPECT_EQ(retire.find("->recycle("), std::string::npos);
  EXPECT_EQ(retire.find("->complete_swap("), std::string::npos);
  const auto exports = section(presentation, "bool release_exports()", "bool prepare_window_destruction()");
  EXPECT_LT(exports.find("contexts_retired()"), exports.find("glDeleteFramebuffers"));
  EXPECT_LT(exports.find("glDeleteFramebuffers"), exports.find("backend->destroy_image"));
  EXPECT_LT(exports.find("backend->destroy_image"), exports.find("glDeleteTextures"));
  const auto finish = presentation.substr(presentation.find("bool finish_shutdown()"));
  EXPECT_LT(finish.find("SDL_WasInit(SDL_INIT_VIDEO)"), finish.find("dlclose("));
  EXPECT_LT(finish.find("dlclose("), finish.find("backend.reset()"));
}
} // namespace
