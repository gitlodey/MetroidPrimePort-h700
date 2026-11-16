#include "gfx/map_completion.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

namespace {
using aurora::gfx::detail::MapCompletion;

TEST(MapCompletionTest, PendingAndExecutingCallbacksCannotBeRetired) {
  MapCompletion record;
  EXPECT_TRUE(record.pending());
  EXPECT_FALSE(record.retire());
  ASSERT_TRUE(record.begin_completion());
  EXPECT_TRUE(record.pending());
  EXPECT_FALSE(record.retire());
  ASSERT_TRUE(record.finish_completion(true));
  EXPECT_FALSE(record.pending());
  EXPECT_TRUE(record.succeeded());
  EXPECT_TRUE(record.retire());
  EXPECT_FALSE(record.retire());
}

TEST(MapCompletionTest, FailedCompletionIsSettledButNeverSuccess) {
  MapCompletion record;
  ASSERT_TRUE(record.begin_completion());
  ASSERT_TRUE(record.finish_completion(false));
  EXPECT_FALSE(record.pending());
  EXPECT_FALSE(record.succeeded());
  EXPECT_FALSE(record.finish_completion(true));
  EXPECT_TRUE(record.retire());
}

TEST(MapCompletionTest, DuplicateAndOldRequestCannotPublishIntoReplacement) {
  auto old = std::make_shared<MapCompletion>();
  unsigned writes = 0;
  const auto callback = [old, &writes] {
    if (!old->begin_completion()) return;
    ++writes;
    EXPECT_TRUE(old->finish_completion(true));
  };
  callback();
  callback();
  ASSERT_TRUE(old->retire());
  auto replacement = std::make_shared<MapCompletion>();
  callback(); // a late duplicate owns only the old request record, not the reusable slot
  EXPECT_EQ(writes, 1u);
  EXPECT_TRUE(replacement->pending());
  EXPECT_FALSE(replacement->succeeded());
}

TEST(MapCompletionTest, ResetKeepsCreatedCpuHolderUntilCallbackActuallyFinishes) {
  auto record = std::make_shared<MapCompletion>();
  auto holder = std::make_shared<int>(42); // CPU ownership witness, not a GPU buffer/shim
  const std::weak_ptr<int> lifetime = holder;
  auto reset = [&] {
    if (record->pending()) return false;
    if (!record->retire()) return false;
    holder.reset();
    return true;
  };
  EXPECT_FALSE(reset());
  ASSERT_TRUE(record->begin_completion());
  EXPECT_FALSE(reset());
  EXPECT_FALSE(lifetime.expired());
  ASSERT_TRUE(record->finish_completion(true));
  EXPECT_TRUE(reset());
  EXPECT_TRUE(lifetime.expired());
  EXPECT_FALSE(record->begin_completion());
}

TEST(MapCompletionTest, ConcurrentDuplicateDoesNotDoubleCompleteOrRelease) {
  for (unsigned i = 0; i < 30; ++i) {
    MapCompletion record;
    std::atomic_uint writes{0};
    const auto callback = [&] {
      if (!record.begin_completion()) return;
      writes.fetch_add(1, std::memory_order_relaxed);
      EXPECT_TRUE(record.finish_completion(true));
    };
    std::thread first{callback};
    std::thread duplicate{callback};
    first.join();
    duplicate.join();
    EXPECT_EQ(writes.load(), 1u);
    EXPECT_FALSE(record.pending());
    EXPECT_TRUE(record.retire());
    EXPECT_FALSE(record.retire());
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
  if (begin == std::string::npos || end == std::string::npos) {
    ADD_FAILURE() << "Missing source section " << first << " .. " << last;
    return {};
  }
  return text.substr(begin, end - begin);
}

TEST(MapRetirementSource, PollAndRetirementDoNotRequireAnotherSubmit) {
  const auto frame = source("gfx/frame.cpp");
  const auto retire = section(frame, "bool retire_frame_resources_after_quiescence()", "size_t pending_frame_packets()");
  EXPECT_NE(retire.find("gx::fifo::is_quiescent()"), std::string::npos);
  EXPECT_NE(retire.find("render_worker::is_quiescent()"), std::string::npos);
  EXPECT_LT(retire.find("g_instance.ProcessEvents()"), retire.find("pending_internal_maps() != 0"));
  EXPECT_LT(retire.find("pending_internal_maps() != 0"), retire.find("shutdown_recording()"));
  EXPECT_NE(retire.find("webgpu::gpu_prof::shutdown()"), std::string::npos);
  EXPECT_NE(retire.find("vis_query::detail::shutdown()"), std::string::npos);
  EXPECT_EQ(retire.find(".Submit("), std::string::npos);
  EXPECT_EQ(retire.find("after_submit("), std::string::npos);
  EXPECT_EQ(retire.find("enqueue_"), std::string::npos);
  const auto profiler = source("webgpu/gpu_prof.cpp");
  const auto profPoll = section(profiler, "void poll_maps()", "const wgpu::PassTimestampWrites*");
  EXPECT_EQ(profPoll.find("MapAsync("), std::string::npos);
  EXPECT_EQ(profPoll.find(".Submit("), std::string::npos);
  const auto visibility = source("gfx/vis_query.cpp");
  const auto visPoll = section(visibility, "void poll_maps()", "size_t pending_maps()");
  EXPECT_EQ(visPoll.find("MapAsync("), std::string::npos);
  EXPECT_EQ(visPoll.find(".Submit("), std::string::npos);
}

TEST(MapRetirementSource, ActualCallbacksOwnRecordsNotReusableReadbackSlots) {
  for (const auto path : {"webgpu/gpu_prof.cpp", "gfx/vis_query.cpp"}) {
    const auto text = source(path);
    EXPECT_NE(text.find("[completion](wgpu::MapAsyncStatus status"), std::string::npos) << path;
    EXPECT_NE(text.find("completion->begin_completion()"), std::string::npos) << path;
    EXPECT_EQ(text.find("[&slot](wgpu::MapAsyncStatus"), std::string::npos) << path;
    EXPECT_EQ(text.find("[target](wgpu::MapAsyncStatus"), std::string::npos) << path;
  }
  const auto frame = source("gfx/frame.cpp");
  EXPECT_NE(frame.find("g_mapCompletions[slot].load(std::memory_order_acquire) != completion"), std::string::npos);
  EXPECT_NE(frame.find("ownership->complete();\n        (void)completion->finish_completion(true)"), std::string::npos);
}

TEST(MapRetirementSource, QueryAndStaticDescriptorHoldersHaveGuardedResetPoints) {
  const auto profiler = source("webgpu/gpu_prof.cpp");
  const auto profReset = section(profiler, "bool shutdown()", "void frame_begin(");
  EXPECT_LT(profReset.find("pending_maps() != 0"), profReset.find("g_passWrites = {}"));
  for (const auto reset : {"g_querySet = {}", "g_resolveBuffer = {}", "slot.readback = {}", "slot.completion.reset()"})
    EXPECT_NE(profReset.find(reset), std::string::npos) << reset;
  EXPECT_EQ(profiler.find("static std::array<wgpu::PassTimestampWrites"), std::string::npos);
  const auto visibility = source("gfx/vis_query.cpp");
  const auto visReset = section(visibility, "bool shutdown()", "} // namespace detail");
  EXPECT_LT(visReset.find("pending_maps() != 0"), visReset.find("g_pipelines.clear()"));
  for (const auto reset : {"slot.querySet = {}", "slot.resolve = {}", "slot.readback = {}", "g_drawType = InvalidDrawType"})
    EXPECT_NE(visReset.find(reset), std::string::npos) << reset;
  const auto recording = source("gfx/recording.cpp");
  const auto recordReset = section(recording, "void shutdown_recording()", "namespace testing");
  EXPECT_NE(recordReset.find("g_recorder = FrameRecorder{}"), std::string::npos);
  EXPECT_NE(recordReset.find("pool = {}"), std::string::npos);
  EXPECT_NE(recordReset.find("g_offscreenCache.clear()"), std::string::npos);
}

TEST(MapRetirementSource, CreatedBootstrapConversionsRemainInCleanupInventory) {
  const auto copy = source("gfx/tex_copy_conv.cpp");
  const auto copyReset = section(copy, "void shutdown()", "static void execute(");
  for (const auto reset : {"g_pipelines.clear()", "g_depthPipelinesMS.clear()", "g_domainPipelines.clear()",
                           "g_blitPipeline = {}", "g_depthSnapshotPipeline = {}", "g_nearestSampler = {}",
                           "g_depthCompareSampler = {}", "g_linearSampler = {}", "g_bindGroupLayout = {}"})
    EXPECT_NE(copyReset.find(reset), std::string::npos) << reset;
  const auto palette = source("gfx/tex_palette_conv.cpp");
  const auto paletteReset = section(palette, "void shutdown()", "void run(");
  for (const auto reset : {"g_directPipeline = {}", "g_fromFloat8Pipeline = {}", "g_fromFloat4Pipeline = {}", "g_sampler = {}"})
    EXPECT_NE(paletteReset.find(reset), std::string::npos) << reset;
  const auto depth = source("gfx/depth_peek.cpp");
  EXPECT_NE(depth.find("if (!webgpu::g_hasCoreFeatures)"), std::string::npos);
  const auto depthReset = section(depth, "void shutdown()", "bool enabled()");
  EXPECT_NE(depthReset.find("g_pipeline = {}"), std::string::npos);
  EXPECT_NE(depthReset.find("g_bindGroupLayout = {}"), std::string::npos);
}
} // namespace
