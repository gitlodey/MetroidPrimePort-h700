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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <thread>
#include <vector>

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

TEST_F(GfxRecordingTest, RetirementAbandonsRecorderWithoutFinishOrExtraWork) {
  const std::array<uint8_t, 4> source{1, 2, 3, 4};
  ASSERT_TRUE(queue_client_texture_upload_data(source.data(), 4, 1, {}, {1, 1, 1}));
  std::weak_ptr<const std::vector<uint8_t>> payload = frame.textureUploads.front().clientData;
  seed(320, 180); // CPU cache metadata; this does not create or exercise GPU readback resources
  begin_offscreen(320, 180);
  const auto passes = frame.renderPasses.size();
  ASSERT_TRUE(recording_active());
  ASSERT_TRUE(retire_frame_resources_after_quiescence());
  recordingActive = false;
  EXPECT_FALSE(recording_active());
  EXPECT_EQ(frame.renderPasses.size(), passes); // no finish/seal/extra frame was manufactured
  EXPECT_EQ(pending_internal_maps(), 0u);
  EXPECT_TRUE(retire_frame_resources_after_quiescence());
  EXPECT_FALSE(payload.expired()); // abandoning the recorder does not release its owner's packet
  EXPECT_EQ(frame.clientTextureUploadBytes, source.size());
  frame = {}; // the CPU fixture owns this packet; reset only after the quiescence guard above
  EXPECT_TRUE(payload.expired());
  EXPECT_EQ(frame.clientTextureUploadBytes, 0u);
}

TEST_F(GfxRecordingTest, ClientUploadOwnsUnpaddedCopyAcrossSourceLifetimeAndPacketReset) {
  const std::array<uint8_t, 24> expected{
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
  std::weak_ptr<const std::vector<uint8_t>> payload;
  {
    auto source = expected;
    ASSERT_TRUE(queue_client_texture_upload_data(source.data(), 12, 2, {}, {3, 2, 1}));
    EXPECT_EQ(source, expected);
    ASSERT_EQ(frame.textureUploads.size(), 1u);
    const auto& upload = frame.textureUploads.front();
    ASSERT_TRUE(upload.clientData);
    EXPECT_NE(upload.clientData->data(), source.data());
    EXPECT_FALSE(upload.buffer);
    EXPECT_EQ(upload.layout.offset, 0u);
    EXPECT_EQ(upload.layout.bytesPerRow, 12u); // not PBO-aligned to 256
    EXPECT_EQ(upload.layout.rowsPerImage, 2u);
    EXPECT_EQ(upload.size.width, 3u);
    EXPECT_EQ(upload.size.height, 2u);
    EXPECT_EQ(upload.size.depthOrArrayLayers, 1u);
    payload = upload.clientData;
    source.fill(0xff);
  }
  EXPECT_EQ(*frame.textureUploads.front().clientData, (std::vector<uint8_t>{expected.begin(), expected.end()}));
  EXPECT_EQ(frame.clientTextureUploadBytes, expected.size());
  EXPECT_EQ(frame.textureUpload.size(), 0u);
  finish();
  detail::end_recording();
  recordingActive = false;
  ASSERT_EQ(frame.ops.size(), 1u);
  EXPECT_EQ(frame.ops.front().highWater.textureUploadCount, 1u);
  ASSERT_EQ(frame.ops.front().textureUploads.size(), 1u);
  EXPECT_EQ(frame.ops.front().textureUploads.front(), &frame.textureUploads.front());
  EXPECT_FALSE(payload.expired());
  frame = {};
  EXPECT_TRUE(payload.expired());
  EXPECT_EQ(frame.clientTextureUploadBytes, 0u);
  EXPECT_TRUE(frame.textureUploads.empty());
}

TEST_F(GfxRecordingTest, ClientUploadsUseExistingImmutableHighWaterSnapshots) {
  const std::array<uint8_t, 4> source{1, 2, 3, 4};
  ASSERT_TRUE(queue_client_texture_upload_data(source.data(), 4, 1, {}, {1, 1, 1}));
  queue_texture_copy({}, {}, {1, 1, 1});
  ASSERT_EQ(frame.ops.size(), 2u);
  const auto& first = frame.ops.front();
  ASSERT_EQ(first.textureUploads.size(), 1u);
  const auto* upload = first.textureUploads.front();
  for (size_t i = 0; i < 128; ++i) {
    ASSERT_TRUE(queue_client_texture_upload_data(source.data(), 4, 1, {}, {1, 1, 1}));
  }
  EXPECT_EQ(first.highWater.textureUploadCount, 1u);
  ASSERT_EQ(first.textureUploads.size(), 1u);
  EXPECT_EQ(first.textureUploads.front(), upload);
  EXPECT_EQ(upload, &frame.textureUploads.front());
  EXPECT_EQ(*upload->clientData, (std::vector<uint8_t>{source.begin(), source.end()}));
  queue_texture_copy({}, {}, {1, 1, 1});
  EXPECT_EQ(frame.ops.back().highWater.textureUploadCount, 129u);
  EXPECT_EQ(frame.ops.back().textureUploads.size(), 129u);
  EXPECT_EQ(frame.clientTextureUploadBytes, 129u * source.size());
  EXPECT_EQ(frame.copied.textureUploadCount, 0u); // recording alone submits no API calls
}

TEST_F(GfxRecordingTest, ClientUploadRejectsInvalidLayoutAndLargeProductsWithoutMutation) {
  const std::array<uint8_t, 4> source{1, 2, 3, 4};
  ASSERT_TRUE(queue_client_texture_upload_data(source.data(), 4, 1, {}, {1, 1, 1}));
  const auto* upload = &frame.textureUploads.front();
  const auto reject = [&](const uint8_t* data, uint32_t row, uint32_t rows, wgpu::Extent3D size) {
    EXPECT_FALSE(queue_client_texture_upload_data(data, row, rows, {}, size));
    EXPECT_EQ(frame.textureUploads.size(), 1u);
    EXPECT_EQ(&frame.textureUploads.front(), upload);
    EXPECT_EQ(frame.clientTextureUploadBytes, source.size());
    EXPECT_EQ(frame.textureUpload.size(), 0u);
    EXPECT_TRUE(frame.ops.empty());
  };
  reject(nullptr, 4, 1, {1, 1, 1});
  reject(source.data(), 4, 1, {0, 1, 1});
  reject(source.data(), 4, 1, {1, 0, 1});
  reject(source.data(), 0, 1, {1, 1, 1});
  reject(source.data(), 4, 0, {1, 1, 1});
  reject(source.data(), 4, 1, {1, 1, 0});
  reject(source.data(), 4, 1, {1, 1, 2});
  reject(source.data(), 4, 1, {2, 1, 1});
  reject(source.data(), 5, 1, {1, 1, 1});
  reject(source.data(), 4, 1, {1, 2, 1});
  reject(source.data(), 4, 1, {std::numeric_limits<uint32_t>::max(), 1, 1});
  reject(source.data(), 0x80000000u, 2, {1, 2, 1}); // 4 GiB, not a wrapped zero-byte payload
  reject(source.data(), 0xfffffffcu, std::numeric_limits<uint32_t>::max(), {1, 1, 1});
  EXPECT_EQ(*upload->clientData, (std::vector<uint8_t>{source.begin(), source.end()}));
}

TEST_F(GfxRecordingTest, ClientUploadBudgetIsAggregateAndRefusalIsTransactional) {
  const std::array<uint8_t, 32> source{};
  const uint64_t limit = frame_buffer_sizes().textureUpload;
  ASSERT_GE(limit, source.size());
  // Seed prior occupancy to test the exact existing budget without allocating another 8/24 MiB.
  frame.clientTextureUploadBytes = limit - source.size();
  ASSERT_TRUE(queue_client_texture_upload_data(source.data(), 16, 2, {}, {4, 2, 1}));
  EXPECT_EQ(frame.clientTextureUploadBytes, limit);
  const auto* upload = &frame.textureUploads.front();
  EXPECT_FALSE(queue_client_texture_upload_data(source.data(), 4, 1, {}, {1, 1, 1}));
  EXPECT_EQ(frame.clientTextureUploadBytes, limit);
  ASSERT_EQ(frame.textureUploads.size(), 1u);
  EXPECT_EQ(&frame.textureUploads.front(), upload);
  EXPECT_EQ(upload->clientData->size(), source.size());
  frame.clientTextureUploadBytes = std::numeric_limits<uint64_t>::max();
  EXPECT_FALSE(queue_client_texture_upload_data(source.data(), 4, 1, {}, {1, 1, 1}));
  EXPECT_EQ(frame.clientTextureUploadBytes, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(frame.textureUploads.size(), 1u);
}

TEST_F(GfxRecordingTest, ClientUploadRejectsOutsideRecordingWithoutMutation) {
  finish();
  detail::end_recording();
  recordingActive = false;
  const auto ops = frame.ops.size();
  const std::array<uint8_t, 4> source{};
  EXPECT_FALSE(queue_client_texture_upload_data(source.data(), 4, 1, {}, {1, 1, 1}));
  EXPECT_TRUE(frame.textureUploads.empty());
  EXPECT_EQ(frame.clientTextureUploadBytes, 0u);
  EXPECT_EQ(frame.ops.size(), ops);
}

TEST_F(GfxRecordingTest, ClientUploadRejectsFailedFrameWithoutMutation) {
  const std::array<uint8_t, 4> source{};
  fail_frame_work(0);
  EXPECT_FALSE(queue_client_texture_upload_data(source.data(), 4, 1, {}, {1, 1, 1}));
  EXPECT_TRUE(frame.textureUploads.empty());
  EXPECT_EQ(frame.clientTextureUploadBytes, 0u);
  EXPECT_TRUE(frame.ops.empty());
}

TEST_F(GfxRecordingTest, ClientUploadCannotAppendToSealedPass) {
  const std::array<uint8_t, 4> source{};
  frame.renderPasses.front().sealed = true;
  EXPECT_DEATH_IF_SUPPORTED(
      (void)queue_client_texture_upload_data(source.data(), 4, 1, {}, {1, 1, 1}), "sealed render pass");
  frame.renderPasses.front().sealed = false;
  EXPECT_TRUE(frame.textureUploads.empty());
  EXPECT_EQ(frame.clientTextureUploadBytes, 0u);
}

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
  const std::array<uint8_t, 4> source{1, 2, 3, 4};
  ASSERT_TRUE(queue_client_texture_upload_data(source.data(), 4, 1, {}, {1, 1, 1}));
  std::weak_ptr<const std::vector<uint8_t>> payload = frame.textureUploads.front().clientData;
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
  EXPECT_FALSE(payload.expired());
  EXPECT_EQ(frame.clientTextureUploadBytes, source.size());
  ASSERT_TRUE(render_worker::is_quiescent());
  frame = {}; // local fixture packet, after cancellation and worker join; no GPU completion claim
  EXPECT_TRUE(payload.expired());
  EXPECT_EQ(frame.clientTextureUploadBytes, 0u);
}

TEST(GfxClientTextureUploadWiring, WorkerWritesOwnedDataWithinExistingSnapshotLoop) {
  // CPU fixture cannot encode a GPU upload; check only the small worker API/lifetime seam in source.
  const auto path = std::filesystem::path(__FILE__).parent_path() / "../lib/gfx/encoding.cpp";
  std::ifstream file(path);
  ASSERT_TRUE(file.is_open()) << path;
  const std::string source{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  const auto loop = source.find("for (size_t i = frame.copied.textureUploadCount; i < op.textureUploads.size(); ++i)");
  ASSERT_NE(loop, std::string::npos);
  const auto branch = source.find("if (item.clientData)", loop);
  ASSERT_NE(branch, std::string::npos);
  const auto worker = source.find("AURORA_ASSERT(render_worker::is_worker_thread()", branch);
  ASSERT_NE(worker, std::string::npos);
  const auto write = source.find(
      "g_queue.WriteTexture(&item.tex, item.clientData->data(), bytes, &item.layout, &item.size)", branch);
  ASSERT_NE(write, std::string::npos);
  const auto next = source.find("continue;", branch);
  ASSERT_NE(next, std::string::npos);
  const auto pbo = source.find("cmd.CopyBufferToTexture(&buf, &item.tex, &item.size)", next);
  ASSERT_NE(pbo, std::string::npos);
  EXPECT_LT(worker, write);
  EXPECT_LT(write, next);
  EXPECT_LT(next, pbo);
  const auto clientBranch = source.substr(branch, next - branch);
  EXPECT_NE(clientBranch.find("const auto bytes = item.clientData->size()"), std::string::npos);
  EXPECT_NE(clientBranch.find("calls == 1 || calls % 60 == 0"), std::string::npos);
  EXPECT_NE(clientBranch.find("H700 movie client upload: calls={} bytes={} width={} height={} row={}"),
            std::string::npos);
  EXPECT_NE(source.find("frame.copied.textureUploadCount = op.textureUploads.size();", pbo), std::string::npos);
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
