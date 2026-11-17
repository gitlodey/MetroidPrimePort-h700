// Resident vertex arrays and display lists (GXPortRetainResident): what the processor draws
// from them, against what it draws from the same data sent every frame.

#include "gx_test_common.hpp"
#include "__gx.h"

#include "dolphin/gx/GXExtra.h"
#include "gx/pipeline.hpp"
#include "gx/resident.hpp"

#include <array>
#include <memory>
#include <vector>

using aurora::gx::g_gxState;

namespace aurora::gfx {
extern gx::DrawData g_testLastDraw;
extern uint32_t g_testDrawCount;
extern Range g_testResidentRegions[3];
extern uint32_t g_testResidentUploads;
} // namespace aurora::gfx

namespace {

constexpr uint32_t kVertexBase = 5 << 20;
constexpr uint32_t kIndexBase = 2 << 20;
constexpr uint32_t kStorageBase = 8 << 20;

class GXResidentTest : public GXFifoTest {
protected:
  void SetUp() override {
    GXFifoTest::SetUp();
    aurora::gfx::g_testResidentRegions[0] = {kVertexBase, 1 << 20};
    aurora::gfx::g_testResidentRegions[1] = {kIndexBase, 1 << 20};
    aurora::gfx::g_testResidentRegions[2] = {kStorageBase, 1 << 20};
    aurora::gfx::g_testResidentUploads = 0;
    aurora::gx::resident::reset();
    aurora::gx::fifo::init();
    aurora::gx::fifo::begin_frame();
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_U8, 0);
  }

  void TearDown() override {
    aurora::gx::fifo::end_frame();
    aurora::gx::resident::reset();
    aurora::gfx::g_testResidentRegions[0] = {};
    aurora::gfx::g_testResidentRegions[1] = {};
    aurora::gfx::g_testResidentRegions[2] = {};
    GXFifoTest::TearDown();
  }

  // Draws a display list and returns how many draws the processor made of it.
  uint32_t call(const std::vector<u8>& dl) {
    aurora::gfx::g_testDrawCount = 0;
    GXCallDisplayList(dl.data(), static_cast<u32>(dl.size()));
    aurora::gx::fifo::drain();
    return aurora::gfx::g_testDrawCount;
  }
};

// These CPU-only tests join the real FIFO worker before reading either registry. There
// are no renderer/compiler jobs or GPU allocations; production requires the full H700
// CpuQuiescent/Maps checkpoint through the lifecycle bridge, not merely FIFO quiescence.
class GXResidentCleanupTest : public GXFifoTest {
protected:
  std::array<u8, 4> data{1, 2, 3, 4};
  std::array<u8, 4> otherData{5, 6, 7, 8};
  u32 producerRetains = 0;

  void retain_producer() {
    GXPortRetainResident(data.data(), static_cast<u32>(data.size()));
    ++producerRetains;
  }

  void release_producer() {
    GXPortReleaseResident(data.data());
    --producerRetains;
  }

  bool stop_processor() {
    return aurora::gx::fifo::shutdown() && aurora::gx::fifo::is_quiescent();
  }

  void expect_guard(bool expected) {
    ASSERT_TRUE(aurora::gx::fifo::is_quiescent());
    const auto* buffer = aurora::gx::fifo::get_buffer_data();
    const auto size = aurora::gx::fifo::get_buffer_size();
    const std::vector<u8> bytes(buffer, buffer + size);
    const auto published = aurora::gx::fifo::published_bytes();
    const auto processed = aurora::gx::fifo::processed_bytes();
    const auto used = aurora::gx::resident::used_bytes();
    const auto uploads = aurora::gfx::g_testResidentUploads;
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(aurora::gx::resident::can_destroy_without_graphics_commands(), expected);
      EXPECT_EQ(aurora::gx::fifo::get_buffer_data(), buffer);
      EXPECT_EQ(aurora::gx::fifo::get_buffer_size(), size);
      EXPECT_EQ(std::vector<u8>(buffer, buffer + size), bytes);
      EXPECT_EQ(aurora::gx::fifo::published_bytes(), published);
      EXPECT_EQ(aurora::gx::fifo::processed_bytes(), processed);
      EXPECT_EQ(aurora::gx::resident::used_bytes(), used);
      EXPECT_EQ(aurora::gfx::g_testResidentUploads, uploads);
    }
  }

  void TearDown() override {
    ASSERT_TRUE(stop_processor());
    // Only fixture cleanup consumes the unpublished retain copies, after all guard
    // assertions. The query itself must leave these commands and their ownership intact.
    decode_fifo(capture_fifo());
    ASSERT_TRUE(aurora::gx::fifo::init());
    while (producerRetains != 0) release_producer();
    ASSERT_TRUE(aurora::gx::fifo::drain());
    ASSERT_TRUE(stop_processor());
    aurora::gx::resident::reset();
    GXFifoTest::TearDown();
  }
};

// A 4-vertex strip and a triangle (two triangles and one), three bytes a vertex, padded to 32.
std::vector<u8> strip_and_triangle() {
  std::vector<u8> dl{GX_DRAW_TRIANGLE_STRIP | GX_VTXFMT0, 0, 4};
  for (u8 i = 0; i < 12; ++i) {
    dl.push_back(i);
  }
  dl.insert(dl.end(), {GX_DRAW_TRIANGLES | GX_VTXFMT0, 0, 3});
  for (u8 i = 0; i < 9; ++i) {
    dl.push_back(100 + i);
  }
  dl.resize(64, GX_NOP);
  return dl;
}

} // namespace

TEST(GXResidentAllocator, FirstFitAndJoin) {
  aurora::gx::resident::Allocator allocator;
  allocator.reset({1000, 4096}); // the start rounds up to 1024
  uint32_t a = 0, b = 0, c = 0, d = 0;
  ASSERT_TRUE(allocator.alloc(100, a));
  ASSERT_TRUE(allocator.alloc(300, b));
  ASSERT_TRUE(allocator.alloc(256, c));
  EXPECT_EQ(a, 1024u);
  EXPECT_EQ(b, 1280u);
  EXPECT_EQ(c, 1792u);
  EXPECT_EQ(allocator.used(), 1024u);
  EXPECT_FALSE(allocator.alloc(4096, d));
  allocator.free(b, 300);
  ASSERT_TRUE(allocator.alloc(200, d));
  EXPECT_EQ(d, 1280u); // the first hole that fits
  allocator.free(d, 200);
  allocator.free(a, 100);
  allocator.free(c, 256);
  EXPECT_EQ(allocator.used(), 0u);
  // Everything joined back into one block.
  ASSERT_TRUE(allocator.alloc(3072, d));
  EXPECT_EQ(d, 1024u);
}

TEST_F(GXResidentCleanupTest, EmptyRegistriesAllowNoCommandCleanup) {
  ASSERT_TRUE(stop_processor());
  EXPECT_TRUE(aurora::gx::resident::processor_registry_empty());
  expect_guard(true);
}

TEST_F(GXResidentCleanupTest, InertRetainsLeaveRegistriesEmpty) {
  GXPortRetainResident(nullptr, static_cast<u32>(data.size()));
  GXPortRetainResident(data.data(), 0);
  ASSERT_TRUE(stop_processor());
  EXPECT_TRUE(aurora::gx::resident::processor_registry_empty());
  EXPECT_EQ(aurora::gx::fifo::get_buffer_size(), 0u);
  expect_guard(true);
}

TEST_F(GXResidentCleanupTest, ProducerOnlyRetainBlocksCleanupWithoutConsumingItsCommand) {
  retain_producer(); // no publish: the processor has never seen this entry
  ASSERT_TRUE(stop_processor());
  ASSERT_GT(aurora::gx::fifo::get_buffer_size(), 0u);
  EXPECT_TRUE(aurora::gx::resident::processor_registry_empty());
  EXPECT_EQ(aurora::gx::resident::find(data.data()), nullptr);
  EXPECT_EQ(aurora::gx::resident::used_bytes(), 0u);
  expect_guard(false);
}

TEST_F(GXResidentCleanupTest, ProcessorOnlyEntryBlocksCleanupWithZeroGpuBytes) {
  ASSERT_TRUE(stop_processor());
  aurora::gx::resident::retain(data.data(), std::make_unique<std::vector<u8>>(data.begin(), data.end()));
  ASSERT_NE(aurora::gx::resident::find(data.data()), nullptr);
  EXPECT_FALSE(aurora::gx::resident::processor_registry_empty());
  EXPECT_EQ(aurora::gx::resident::used_bytes(), 0u);
  expect_guard(false);
  aurora::gx::resident::release(data.data());
  expect_guard(true);
}

TEST_F(GXResidentCleanupTest, MismatchedProducerAndProcessorKeysBlockCleanup) {
  retain_producer();
  ASSERT_TRUE(stop_processor());
  aurora::gx::resident::retain(otherData.data(),
                             std::make_unique<std::vector<u8>>(otherData.begin(), otherData.end()));
  EXPECT_EQ(aurora::gx::resident::find(data.data()), nullptr);
  ASSERT_NE(aurora::gx::resident::find(otherData.data()), nullptr);
  EXPECT_EQ(aurora::gx::resident::used_bytes(), 0u);
  expect_guard(false);
  aurora::gx::resident::release(otherData.data());
  EXPECT_TRUE(aurora::gx::resident::processor_registry_empty());
  expect_guard(false); // the producer obligation still exists
}

TEST_F(GXResidentCleanupTest, MatchingEntryBeforeGpuAllocationStillBlocksCleanup) {
  retain_producer();
  ASSERT_TRUE(aurora::gx::fifo::drain());
  ASSERT_TRUE(stop_processor());
  const auto* entry = aurora::gx::resident::find(data.data());
  ASSERT_NE(entry, nullptr);
  const auto* copy = entry->bytes.get();
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(*copy, std::vector<u8>(data.begin(), data.end()));
  EXPECT_EQ(aurora::gx::resident::used_bytes(), 0u);
  expect_guard(false);
  EXPECT_EQ(aurora::gx::resident::find(data.data()), entry);
  EXPECT_EQ(entry->bytes.get(), copy);
  EXPECT_EQ(*copy, std::vector<u8>(data.begin(), data.end()));
  EXPECT_EQ(entry->storage.size, 0u);
  EXPECT_EQ(entry->vert.size, 0u);
  EXPECT_EQ(entry->idx.size, 0u);
  EXPECT_FALSE(entry->arrayTried);
  EXPECT_FALSE(entry->dlTried);
  EXPECT_TRUE(entry->chunks.empty());
}

TEST_F(GXResidentCleanupTest, RepeatedQueriesPreserveCountedRetainsAndProcessorOwnership) {
  retain_producer();
  retain_producer();
  ASSERT_TRUE(aurora::gx::fifo::drain());
  ASSERT_TRUE(stop_processor());
  const auto* entry = aurora::gx::resident::find(data.data());
  ASSERT_NE(entry, nullptr);
  expect_guard(false);

  ASSERT_TRUE(aurora::gx::fifo::init());
  release_producer();
  ASSERT_TRUE(aurora::gx::fifo::drain());
  ASSERT_TRUE(stop_processor());
  EXPECT_EQ(aurora::gx::resident::find(data.data()), entry);
  expect_guard(false); // queries must not decrement the producer's retain count

  ASSERT_TRUE(aurora::gx::fifo::init());
  release_producer();
  ASSERT_TRUE(aurora::gx::fifo::drain());
  ASSERT_TRUE(stop_processor());
  EXPECT_EQ(aurora::gx::resident::find(data.data()), nullptr);
  expect_guard(true);
}

TEST_F(GXResidentTest, DisplayListDrawsFromItsResidentCopy) {
  const std::vector<u8> dl = strip_and_triangle();
  // Sent every frame: a draw a primitive (the test stubs never merge).
  EXPECT_EQ(call(dl), 2u);

  GXPortRetainResident(dl.data(), static_cast<u32>(dl.size()));
  EXPECT_EQ(call(dl), 1u);
  const auto first = aurora::gfx::g_testLastDraw;
  EXPECT_EQ(first.vtxCount, 7u);
  EXPECT_EQ(first.indexCount, 6u + 3u);
  EXPECT_EQ(first.vertRange.offset, kVertexBase);
  EXPECT_EQ(first.vertRange.size, 21u);
  EXPECT_EQ(first.idxRange.offset, kIndexBase);
  EXPECT_EQ(first.idxRange.size, 18u);
  EXPECT_EQ(aurora::gfx::g_testResidentUploads, 2u); // vertices and indices

  // Again: the same draw, nothing uploaded.
  EXPECT_EQ(call(dl), 1u);
  EXPECT_EQ(aurora::gfx::g_testLastDraw.vertRange, first.vertRange);
  EXPECT_EQ(aurora::gfx::g_testResidentUploads, 2u);

  // Counted: one release of two retains keeps it.
  GXPortRetainResident(dl.data(), static_cast<u32>(dl.size()));
  GXPortReleaseResident(dl.data());
  EXPECT_EQ(call(dl), 1u);
  GXPortReleaseResident(dl.data());
  EXPECT_EQ(call(dl), 2u);
  aurora::gx::fifo::drain();
  EXPECT_EQ(aurora::gx::resident::used_bytes(), 0u);
}

TEST_F(GXResidentTest, DisplayListWithLinesIsSentAsBefore) {
  std::vector<u8> dl{GX_DRAW_LINES | GX_VTXFMT0, 0, 2, 1, 2, 3, 4, 5, 6};
  dl.resize(32, GX_NOP);
  GXPortRetainResident(dl.data(), static_cast<u32>(dl.size()));
  EXPECT_EQ(call(dl), 1u);
  EXPECT_EQ(call(dl), 1u);
  EXPECT_EQ(aurora::gfx::g_testResidentUploads, 0u);
  GXPortReleaseResident(dl.data());
  aurora::gx::fifo::drain();
}

TEST_F(GXResidentTest, DisplayListIsRemadeForNewVertexSizes) {
  // Whole draws at three bytes a vertex and at two: the three NOPs after six bytes of
  // vertices are the third vertex's last bytes at three.
  std::vector<u8> dl{GX_DRAW_TRIANGLES | GX_VTXFMT0, 0, 3, 1, 2, 3, 4, 5, 6, GX_NOP, GX_NOP, GX_NOP};
  dl.resize(32, GX_NOP);
  GXPortRetainResident(dl.data(), static_cast<u32>(dl.size()));
  EXPECT_EQ(call(dl), 1u);
  EXPECT_EQ(aurora::gfx::g_testLastDraw.vertRange.size, 9u);
  EXPECT_EQ(aurora::gfx::g_testResidentUploads, 2u);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_U8, 0); // two bytes a vertex
  EXPECT_EQ(call(dl), 1u);
  EXPECT_EQ(aurora::gfx::g_testLastDraw.vertRange.size, 6u);
  EXPECT_EQ(aurora::gfx::g_testResidentUploads, 4u);
  GXPortReleaseResident(dl.data());
  aurora::gx::fifo::drain();
  EXPECT_EQ(aurora::gx::resident::used_bytes(), 0u);
}

TEST_F(GXResidentTest, ArrayReadsItsResidentCopy) {
  std::array<u8, 64> positions{};
  GXPortRetainResident(positions.data(), static_cast<u32>(positions.size()));
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
  GXSetArray(GX_VA_POS, positions.data(), static_cast<u32>(positions.size()), 3, false);
  aurora::gfx::g_testDrawCount = 0;
  GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
  GXPosition1x8(0);
  GXPosition1x8(1);
  GXPosition1x8(2);
  GXEnd();
  aurora::gx::fifo::drain();
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 1u);
  EXPECT_EQ(aurora::gfx::g_testLastDraw.immediateData.arrayStart[0], kStorageBase);
  EXPECT_EQ(aurora::gfx::g_testResidentUploads, 1u);
  GXPortReleaseResident(positions.data());
  aurora::gx::fifo::drain();
  EXPECT_EQ(g_gxState.arrays[GX_VA_POS].cachedRange.size, 0u);
}
