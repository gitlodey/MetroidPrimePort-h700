#include <gtest/gtest.h>

#include "gfx/frame_packet.hpp"
#include "gfx/frame.hpp"
#include "gfx/recording.hpp"
#include "gfx/texture.hpp"
#include "webgpu/gpu.hpp"
#include "gfx/render_worker.hpp"
#include "h700/wait.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <atomic>
#include <thread>

namespace aurora::gfx {
namespace {

constexpr auto ColorFormat = wgpu::TextureFormat::RGBA8Unorm;
constexpr auto DepthFormat = wgpu::TextureFormat::Depth24Plus;

class GfxRecordingTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(render_worker::prepare_bootstrap());
    ASSERT_TRUE(retire_cancelled_frames_after_quiescence());
    webgpu::g_graphicsConfig.surfaceConfiguration.format = ColorFormat;
    webgpu::g_graphicsConfig.depthFormat = DepthFormat;
    webgpu::g_graphicsConfig.msaaSamples = 1;
    webgpu::g_frameBuffer.size = {640, 480, 1};
    webgpu::g_frameBuffer.format = ColorFormat;
    webgpu::g_depthBuffer.size = {640, 480, 1};
    webgpu::g_depthBuffer.format = DepthFormat;
    detail::testing::suppress_render_worker(true);
    detail::begin_recording(frame, 0);
  }

  void TearDown() override {
    if (recordingActive) {
      if (is_offscreen()) {
        end_offscreen();
      }
      finish();
      detail::end_recording();
    }
    detail::shutdown_recording();
    render_worker::set_wait_hooks({});
    render_worker::shutdown();
    EXPECT_TRUE(retire_cancelled_frames_after_quiescence());
    EXPECT_TRUE(render_worker::prepare_bootstrap());
  }

  void seed(uint32_t width, uint32_t height) {
    detail::testing::seed_offscreen_cache(width, height, ColorFormat, DepthFormat);
  }

  void copy_current_offscreen() {
    const auto& pass = frame.renderPasses.back();
    const auto& size = pass.colorAttachments[SceneColorAttachmentIndex].size;
    auto target = std::make_shared<TextureRef>(wgpu::Texture{}, wgpu::TextureView{}, wgpu::TextureView{}, size,
                                               ColorFormat, 1, GX_TF_RGBA8);
    resolve_pass_into(std::move(target), {0, 0, static_cast<int32_t>(size.width), static_cast<int32_t>(size.height)},
                      false, false, false, {}, 1.f);
  }

  size_t count_efb_passes() const {
    return static_cast<size_t>(
        std::ranges::count_if(frame.renderPasses, [](const auto& pass) { return pass.label.starts_with("EFB"); }));
  }

  detail::FramePacket frame;
  bool recordingActive = true;
};

TEST_F(GfxRecordingTest, CreateRestoreReturnsToEfb) {
  seed(320, 180);
  begin_offscreen(320, 180);
  ASSERT_TRUE(is_offscreen());

  end_offscreen();

  EXPECT_FALSE(is_offscreen());
  ASSERT_EQ(frame.renderPasses.size(), 2u);
  EXPECT_TRUE(frame.renderPasses[0].sealed);
  EXPECT_TRUE(frame.renderPasses[0].discardable);
  EXPECT_EQ(count_efb_passes(), 1u);
}

TEST_F(GfxRecordingTest, PushPastMappedBufferReturnsOverflowRange) {
  // The frame's buffers wrap fixed-size mapped staging memory, which used to abort when full.
  std::array<uint8_t, 64> mapped{};
  frame.storage = ByteBuffer{mapped.data(), mapped.size()};
  const std::array<uint8_t, 48> data{};

  const auto first = push_storage(data.data(), data.size());
  EXPECT_FALSE(overflowed(first));
  EXPECT_EQ(first.size, data.size());
  EXPECT_TRUE(overflowed(push_storage(data.data(), data.size())));
  EXPECT_EQ(frame.storage.size(), data.size());

  frame.storage = ByteBuffer{};
}

TEST_F(GfxRecordingTest, EfbPassUsesDiscoveredSceneLayout) {
  ASSERT_FALSE(frame.renderPasses.empty());
  const auto discovered = scene_render_target_layout();
  const auto targetLayout = frame.renderPasses.front().target_layout();

  EXPECT_EQ(targetLayout.key, discovered.key);
  EXPECT_EQ(targetLayout.colorAttachmentCount, discovered.colorAttachmentCount);
  EXPECT_EQ(targetLayout.colorAttachments[SceneColorAttachmentIndex].semantic, ColorAttachmentSemantic::SceneColor);
}

TEST_F(GfxRecordingTest, CopiedOffscreenPassIsRetainedOnRestore) {
  seed(320, 180);
  begin_offscreen(320, 180);
  copy_current_offscreen();

  end_offscreen();

  ASSERT_EQ(frame.renderPasses.size(), 3u);
  EXPECT_TRUE(frame.renderPasses[0].sealed);
  EXPECT_FALSE(frame.renderPasses[0].discardable);
  EXPECT_TRUE(frame.renderPasses[0].has_consumer());
  EXPECT_TRUE(frame.renderPasses[1].sealed);
  EXPECT_TRUE(frame.renderPasses[1].discardable);
}

TEST_F(GfxRecordingTest, ReplacementRetainsCopiedPassesAndDiscardsContinuations) {
  seed(320, 180);
  seed(160, 90);
  begin_offscreen(320, 180);
  copy_current_offscreen();
  begin_offscreen(160, 90);
  copy_current_offscreen();

  end_offscreen();

  ASSERT_EQ(frame.renderPasses.size(), 5u);
  EXPECT_TRUE(frame.renderPasses[0].has_consumer());
  EXPECT_FALSE(frame.renderPasses[0].discardable);
  EXPECT_TRUE(frame.renderPasses[1].discardable);
  EXPECT_TRUE(frame.renderPasses[2].has_consumer());
  EXPECT_FALSE(frame.renderPasses[2].discardable);
  EXPECT_TRUE(frame.renderPasses[3].discardable);
  EXPECT_EQ(count_efb_passes(), 1u);
}

TEST_F(GfxRecordingTest, RepeatedUncopiedCreatesDiscardEarlierPasses) {
  seed(320, 180);
  seed(160, 90);
  begin_offscreen(320, 180);
  begin_offscreen(160, 90);
  end_offscreen();

  ASSERT_EQ(frame.renderPasses.size(), 3u);
  EXPECT_TRUE(frame.renderPasses[0].sealed);
  EXPECT_TRUE(frame.renderPasses[0].discardable);
  EXPECT_TRUE(frame.renderPasses[1].sealed);
  EXPECT_TRUE(frame.renderPasses[1].discardable);
  EXPECT_EQ(count_efb_passes(), 1u);
}

TEST_F(GfxRecordingTest, PublicCreatePassRejectsExistingOffscreenPass) {
  seed(320, 180);
  seed(160, 90);
  ASSERT_TRUE(create_pass(320, 180));

  EXPECT_FALSE(create_pass(160, 90));

  ResolvedTargets ignored;
  EXPECT_TRUE(resolve_pass({.color = false, .depth = false}, ignored));
}

TEST_F(GfxRecordingTest, FinalizedPassesAreSealedOrDeliberatelyDiscarded) {
  seed(320, 180);
  seed(160, 90);
  begin_offscreen(320, 180);
  copy_current_offscreen();
  begin_offscreen(160, 90);
  end_offscreen();
  finish();

  ASSERT_FALSE(frame.renderPasses.empty());
  for (const auto& pass : frame.renderPasses) {
    EXPECT_TRUE(pass.sealed);
    if (!pass.has_consumer() && pass.label.starts_with("Offscreen")) {
      EXPECT_TRUE(pass.discardable);
    }
  }
  detail::end_recording();
  recordingActive = false;
}

TEST_F(GfxRecordingTest, RejectedEndFrameCallsAbortButNeverSuccessCallback) {
  // CPU recording fixture only: stopping the worker must reject before any Dawn call.
  finish();
  render_worker::initialize();
  render_worker::shutdown();
  int aborted = 0;
  const bool accepted = end_frame([](wgpu::CommandEncoder&, std::vector<AfterSubmitCallback>) {
    ADD_FAILURE() << "Rejected frame ran its graphics/success callback";
  }, [&] { ++aborted; });
  recordingActive = false;
  EXPECT_FALSE(accepted);
  EXPECT_EQ(aborted, 1);
  EXPECT_FALSE(recording_active());
}

TEST(GfxFrameWaitOwnership, CancelledMapWaitKeepsSlotUntilInternalCompletion) {
  render_worker::FrameSlotPool staging{1};
  const auto slot = *staging.try_acquire();
  int releases = 0;
  auto ownership = std::make_shared<h700::MapSlotOwnership>(false, [&] { ++releases; staging.release(slot); });
  auto callback = [ownership] { ownership->complete(); };
  ownership->cancel_wait();
  ownership.reset(); // completion, not the cancelled stack, owns the release obligation
  EXPECT_EQ(staging.free_count(), 0u);
  callback();
  callback();
  EXPECT_EQ(staging.free_count(), 1u);
  EXPECT_EQ(releases, 1);
}

TEST(GfxFrameWaitOwnership, CompletionBeforeCancellationStillReleasesExactlyOnce) {
  int releases = 0;
  h700::MapSlotOwnership ownership(false, [&] { ++releases; });
  ownership.complete();
  EXPECT_EQ(releases, 0);
  ownership.cancel_wait();
  ownership.cancel_wait();
  ownership.complete();
  EXPECT_EQ(releases, 1);
}

TEST(GfxFrameWaitOwnership, ConcurrentMapCompletionAndCancellationDoNotLoseRelease) {
  for (int i = 0; i < 50; ++i) {
    std::atomic_int releases = 0;
    h700::MapSlotOwnership ownership(false, [&] { ++releases; });
    std::thread callback([&] { ownership.complete(); });
    ownership.cancel_wait();
    callback.join();
    EXPECT_EQ(releases.load(), 1);
  }
}
} // namespace
} // namespace aurora::gfx
