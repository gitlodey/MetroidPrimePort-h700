// Production FIFO/processor round trips. Only renderer endpoints are stubbed by
// gx_test_stubs.cpp; immutable tickets, GX encoders, FIFO worker and XF reads are real.
#include "gx_test_common.hpp"

#include "gx/decoded_geometry.hpp"
#include "gx/immutable.hpp"
#include "gx/pipeline.hpp"
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace aurora::gfx {
extern gx::DrawData g_testLastDraw;
extern uint32_t g_testDrawCount;
extern bool g_testStorageFull;
extern Range g_testResidentRegions[3];
extern uint32_t g_testResidentUploads;
} // namespace aurora::gfx

namespace {
namespace fifo = aurora::gx::fifo;
namespace immutable = aurora::gx::immutable;
using immutable::SourceId;
using immutable::SourceKind;
using aurora::gx::g_gxState;

std::atomic<unsigned> sImmutableDrawDoneCalls{0};
void immutable_draw_done() { sImmutableDrawDoneCalls.fetch_add(1, std::memory_order_release); }

std::array<u32, 32> sCallbackMarkers{};
std::array<uint64_t, 32> sCallbackWatermarks{};
std::array<bool, 32> sCallbackUnlocked{};
std::atomic<bool> sBlockingCallbackEntered{false};
std::atomic<bool> sBlockingCallbackReturn{false};

void capture_immutable_draw_done() {
  const auto index = sImmutableDrawDoneCalls.load(std::memory_order_relaxed);
  if (index < sCallbackMarkers.size()) {
    sCallbackMarkers[index] = g_gxState.bpRegCache[0x41];
    sCallbackWatermarks[index] = fifo::processed_bytes();
    sCallbackUnlocked[index] = fifo::testing::try_buffer_lock();
  }
  sImmutableDrawDoneCalls.store(index + 1, std::memory_order_release);
}

void blocking_immutable_draw_done() {
  capture_immutable_draw_done();
  sBlockingCallbackEntered.store(true, std::memory_order_release);
  while (!sBlockingCallbackReturn.load(std::memory_order_acquire)) std::this_thread::yield();
}

bool wait_for_callback() {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (!sBlockingCallbackEntered.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

struct CallbackReturnGuard {
  ~CallbackReturnGuard() { sBlockingCallbackReturn.store(true, std::memory_order_release); }
};

void append_bp(std::vector<u8>& list, u8 reg, u32 value) {
  list.insert(list.end(), {GX_LOAD_BP_REG, reg, u8(value >> 16), u8(value >> 8), u8(value)});
}
void append_draw_done(std::vector<u8>& list) { append_bp(list, 0x45, 2); }
void append_immutable_call(std::vector<u8>& list, SourceId id, u32 size) {
  list.insert(list.end(), {GX_AURORA, u8(GX_AURORA_CALL_IMMUTABLE_DL >> 8), u8(GX_AURORA_CALL_IMMUTABLE_DL)});
  for (unsigned i = 0; i < 8; ++i) list.push_back(u8(id >> (56 - i * 8)));
  for (unsigned i = 0; i < 4; ++i) list.push_back(u8(size >> (24 - i * 8)));
}
void write_bp(u8 reg, u32 value) {
  fifo::write_u8(GX_LOAD_BP_REG);
  fifo::write_u32(u32(reg) << 24 | value);
}

class GXImmutableFifoTest : public GXFifoTest {
protected:
  std::vector<SourceId> m_sources;
  std::vector<std::unique_ptr<std::vector<u8>>> m_listOriginals;
  struct List {
    SourceId id;
    const void* original;
    u32 size;
  };
  fifo::DrawDoneCallback m_previousCallback = nullptr;
  bool m_stopped = false;
  bool m_previousStorageFull = false;
  uint32_t m_residentUploads = 0;

  void SetUp() override {
    GXFifoTest::SetUp();
    ASSERT_TRUE(fifo::drain());
    ASSERT_TRUE(fifo::shutdown());
    m_stopped = true;
    // Match the CPU ownership precondition of guarded side-binding retirement.
    // This fixture does not simulate or manufacture H700 GPU/map completion.
    fifo::clear_immutable_bindings();
    ASSERT_TRUE(immutable::registries_empty());
    ASSERT_EQ(immutable::retained_bytes(), 0u);
    ASSERT_FALSE(aurora::gx::decoded::enabled()); // existing non-H700/FIFO-test stubs
    for (const auto& region : aurora::gfx::g_testResidentRegions) ASSERT_EQ(region.size, 0u);
    m_residentUploads = aurora::gfx::g_testResidentUploads;
    m_previousStorageFull = aurora::gfx::g_testStorageFull;
    aurora::gfx::g_testStorageFull = false;
    aurora::gfx::g_testDrawCount = 0;
    m_previousCallback = fifo::set_draw_done_callback(nullptr);
    ASSERT_TRUE(fifo::init());
    m_stopped = false;
    ASSERT_TRUE(fifo::begin_frame());
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_U8, 0);
  }

  void TearDown() override {
    if (m_stopped) {
      ASSERT_TRUE(fifo::init()); // Stop() already consumed the full prior tail
      m_stopped = false;
    }
    for (const auto id : m_sources) EXPECT_TRUE(immutable::unregister_source(id));
    ASSERT_TRUE(fifo::drain());
    if (fifo::frame_active()) EXPECT_TRUE(fifo::end_frame());
    ASSERT_TRUE(fifo::shutdown());
    m_stopped = true;
    fifo::clear_immutable_bindings();
    EXPECT_TRUE(immutable::registries_empty());
    EXPECT_EQ(immutable::retained_bytes(), 0u);
    EXPECT_EQ(aurora::gfx::g_testResidentUploads, m_residentUploads);
    fifo::set_draw_done_callback(m_previousCallback);
    aurora::gfx::g_testStorageFull = m_previousStorageFull;
    GXFifoTest::TearDown();
  }

  SourceId Register(const void* data, u32 size, SourceKind kind = SourceKind::Array) {
    // The production public wrapper is intentionally OFF outside H700 opt-in.
    // Internal registration still emits the real checked ownership FIFO record.
    const auto id = immutable::register_source(data, size, kind);
    if (id != 0) m_sources.push_back(id);
    return id;
  }

  List MakeList(std::vector<u8> bytes) {
    auto original = std::make_unique<std::vector<u8>>(std::move(bytes));
    const List list{Register(original->data(), original->size(), SourceKind::DisplayList),
                    original->data(), static_cast<u32>(original->size())};
    m_listOriginals.push_back(std::move(original));
    return list;
  }

  void FreeListOriginals() {
    for (auto& bytes : m_listOriginals) std::fill(bytes->begin(), bytes->end(), 0xCD);
    m_listOriginals.clear();
  }

  void CaptureCallbacks() {
    sCallbackMarkers = {};
    sCallbackWatermarks = {};
    sCallbackUnlocked = {};
    sImmutableDrawDoneCalls.store(0, std::memory_order_relaxed);
    fifo::set_draw_done_callback(capture_immutable_draw_done);
  }

  bool Stop() {
    if (!fifo::drain()) return false;
    if (fifo::frame_active() && !fifo::end_frame()) return false;
    if (!fifo::shutdown()) return false;
    m_stopped = true;
    return fifo::is_quiescent();
  }
};

TEST_F(GXImmutableFifoTest, RegisterBindAndReleaseQueuedBeforeDrainKeepFreedOriginalSnapshot) {
  const std::array<u8, 9> expected{1, 2, 3, 4, 5, 6, 7, 8, 9};
  auto original = std::make_unique<std::array<u8, 9>>(expected);
  const void* key = original->data();
  const auto id = Register(key, expected.size());
  ASSERT_NE(id, 0u);
  EXPECT_FALSE(immutable::active(id)); // registration is still in the unpublished tail
  GXSetArray(GX_VA_POS, key, expected.size(), 3, false);
  ASSERT_TRUE(immutable::unregister_source(id));
  EXPECT_EQ(immutable::producer_admission(key, expected.size(), SourceKind::Array), 0u);
  original->fill(0xCD);
  original.reset();

  ASSERT_TRUE(fifo::drain());
  EXPECT_FALSE(immutable::active(id));
  EXPECT_FALSE(immutable::lookup_array(id, expected.size()));
  const auto& bound = g_gxState.arrays[GX_VA_POS];
  ASSERT_NE(bound.data, nullptr);
  EXPECT_NE(bound.data, key);
  EXPECT_EQ(bound.size, expected.size());
  EXPECT_EQ(bound.stride, 3);
  EXPECT_FALSE(bound.le);
  EXPECT_EQ(std::memcmp(bound.data, expected.data(), expected.size()), 0);
  EXPECT_EQ(immutable::retained_bytes(), expected.size()); // side binding alone owns it
  ASSERT_TRUE(Stop());
  EXPECT_TRUE(immutable::registries_empty());
  fifo::clear_immutable_bindings();
  EXPECT_EQ(immutable::retained_bytes(), 0u);
}

TEST_F(GXImmutableFifoTest, ReleaseRevokesAdmissionButHeldViewOutlivesNullRebind) {
  const std::array<u8, 9> expected{19, 18, 17, 16, 15, 14, 13, 12, 11};
  auto original = std::make_unique<std::array<u8, 9>>(expected);
  const void* key = original->data();
  const auto id = Register(key, expected.size());
  ASSERT_NE(id, 0u);
  original->fill(0);
  original.reset();
  GXSetArray(GX_VA_POS, key, expected.size(), 3, false);
  ASSERT_TRUE(fifo::drain());
  auto held = immutable::lookup_array(id, expected.size());
  ASSERT_TRUE(held);
  EXPECT_EQ(held.original_base(), key);
  EXPECT_EQ(held.original_size(), expected.size());
  EXPECT_EQ(g_gxState.arrays[GX_VA_POS].data, held.data());

  ASSERT_TRUE(immutable::unregister_source(id));
  ASSERT_TRUE(fifo::drain());
  EXPECT_FALSE(immutable::active(id));
  EXPECT_FALSE(immutable::lookup_array(id, expected.size()));
  EXPECT_EQ(std::memcmp(held.data(), expected.data(), expected.size()), 0);
  GXSetArray(GX_VA_POS, nullptr, 0, 0, true);
  ASSERT_TRUE(fifo::drain());
  EXPECT_EQ(g_gxState.arrays[GX_VA_POS].data, nullptr);
  EXPECT_EQ(g_gxState.arrays[GX_VA_POS].size, 0u);
  EXPECT_EQ(immutable::retained_bytes(), expected.size()); // external view, not registry
  held = {};
  EXPECT_EQ(immutable::retained_bytes(), 0u);
}

TEST_F(GXImmutableFifoTest, ReusedOriginalAddressGetsNewBindingAndStaleReleaseCannotEraseIt) {
  std::array<u8, 9> original{1, 2, 3, 4, 5, 6, 7, 8, 9};
  const auto oldBytes = original;
  const auto oldId = Register(original.data(), original.size());
  ASSERT_NE(oldId, 0u);
  GXSetArray(GX_VA_POS, original.data(), original.size(), 3, false);
  ASSERT_TRUE(fifo::drain());
  auto oldView = immutable::lookup_array(oldId, original.size());
  ASSERT_TRUE(oldView);
  g_gxState.arrays[GX_VA_POS].cachedRange = {64, 9};
  g_gxState.dirty = 0;

  ASSERT_TRUE(immutable::unregister_source(oldId));
  original.fill(42);
  const auto newId = Register(original.data(), original.size());
  ASSERT_GT(newId, oldId);
  GXSetArray(GX_VA_POS, original.data(), original.size(), 3, false);
  ASSERT_TRUE(fifo::drain());
  EXPECT_FALSE(immutable::active(oldId));
  EXPECT_TRUE(immutable::active(newId));
  const auto current = immutable::lookup_array(newId, original.size());
  ASSERT_TRUE(current);
  EXPECT_EQ(g_gxState.arrays[GX_VA_POS].data, current.data());
  EXPECT_NE(current.data(), oldView.data());
  EXPECT_EQ(current.data()[0], 42);
  EXPECT_EQ(std::memcmp(oldView.data(), oldBytes.data(), oldBytes.size()), 0);
  EXPECT_EQ(g_gxState.arrays[GX_VA_POS].cachedRange.size, 0u);
  EXPECT_NE(g_gxState.dirty & aurora::gx::DirtyImmediates, 0);
  EXPECT_EQ(immutable::lookup_array(original.data(), original.size()).id(), newId);

  ASSERT_TRUE(fifo::enqueue_ownership(GX_AURORA_RELEASE_IMMUTABLE_SOURCE, oldId));
  ASSERT_TRUE(fifo::drain());
  EXPECT_TRUE(immutable::active(newId));
  EXPECT_EQ(g_gxState.arrays[GX_VA_POS].data, current.data());
  EXPECT_EQ(immutable::retained_bytes(), original.size() * 2);
}

TEST_F(GXImmutableFifoTest, LegacyRebindOfSameSnapshotAddressDropsImmutableBinding) {
  std::array<u8, 9> original{1, 2, 3, 4, 5, 6, 7, 8, 9};
  const auto id = Register(original.data(), original.size());
  ASSERT_NE(id, 0u);
  GXSetArray(GX_VA_POS, original.data(), original.size(), 3, false);
  ASSERT_TRUE(fifo::drain());
  auto held = immutable::lookup_array(id, original.size());
  ASSERT_TRUE(held);
  ASSERT_TRUE(immutable::unregister_source(id));
  ASSERT_TRUE(fifo::drain());
  g_gxState.arrays[GX_VA_POS].cachedRange = {32, 9};
  g_gxState.dirty = 0;

  // Same pointer/size/endian as the current processor binding, but now an explicitly
  // caller-owned legacy array. ID -> 0 must still drop the side-array reference.
  GXSetArray(GX_VA_POS, held.data(), held.size(), 3, false);
  ASSERT_TRUE(fifo::drain());
  EXPECT_EQ(g_gxState.arrays[GX_VA_POS].cachedRange.size, 0u);
  EXPECT_NE(g_gxState.dirty & aurora::gx::DirtyImmediates, 0);
  ASSERT_TRUE(Stop());
  fifo::clear_immutable_bindings();
  EXPECT_EQ(g_gxState.arrays[GX_VA_POS].data, held.data()); // legacy binding is not cleared
  EXPECT_EQ(g_gxState.arrays[GX_VA_POS].size, held.size());
  EXPECT_EQ(immutable::retained_bytes(), original.size());
}

TEST_F(GXImmutableFifoTest, SideClearReleasesOnlyImmutableBindingsWithoutConsumingFifo) {
  std::array<u8, 48> original{};
  const std::array<u8, 4> legacy{1, 2, 3, 4};
  const auto id = Register(original.data(), original.size());
  ASSERT_NE(id, 0u);
  for (const auto attr : {GX_VA_POS, GX_VA_NRM, GX_POS_MTX_ARRAY})
    GXSetArray(attr, original.data(), original.size(), 12, false);
  GXSetArray(GX_VA_CLR0, legacy.data(), legacy.size(), 4, true);
  ASSERT_TRUE(fifo::drain());
  auto held = immutable::lookup_array(id, original.size());
  ASSERT_TRUE(held);
  ASSERT_TRUE(immutable::unregister_source(id));
  ASSERT_TRUE(Stop());
  EXPECT_TRUE(immutable::registries_empty()); // held views are not active admission
  EXPECT_EQ(immutable::retained_bytes(), original.size());
  const auto published = fifo::published_bytes();
  const auto processed = fifo::processed_bytes();
  const auto bytes = fifo::get_buffer_size();
  fifo::clear_immutable_bindings();
  fifo::clear_immutable_bindings();
  for (const auto attr : {GX_VA_POS, GX_VA_NRM, GX_POS_MTX_ARRAY}) {
    EXPECT_EQ(g_gxState.arrays[attr].data, nullptr);
    EXPECT_EQ(g_gxState.arrays[attr].size, 0u);
    EXPECT_EQ(g_gxState.arrays[attr].cachedRange.size, 0u);
  }
  EXPECT_EQ(g_gxState.arrays[GX_VA_CLR0].data, legacy.data());
  EXPECT_EQ(g_gxState.arrays[GX_VA_CLR0].size, legacy.size());
  EXPECT_EQ(g_gxState.arrays[GX_VA_CLR0].stride, 4);
  EXPECT_TRUE(g_gxState.arrays[GX_VA_CLR0].le);
  EXPECT_EQ(fifo::published_bytes(), published);
  EXPECT_EQ(fifo::processed_bytes(), processed);
  EXPECT_EQ(fifo::get_buffer_size(), bytes);
  EXPECT_EQ(immutable::retained_bytes(), original.size());
  held = {};
  EXPECT_EQ(immutable::retained_bytes(), 0u);
}

class GXImmutableIndexedXFFifoTest : public GXImmutableFifoTest, public ::testing::WithParamInterface<bool> {};

TEST_P(GXImmutableIndexedXFFifoTest, FullIndexUsesSnapshotAfterOriginalFreeAndOrderedRelease) {
  constexpr size_t index = 0x100;
  constexpr size_t stride = 12 * sizeof(f32);
  constexpr size_t bytes = (index + 1) * stride;
  const std::array<f32, 12> expected{1, 2, 3, 10, 4, 5, 6, 20, 7, 8, 9, 30};
  auto original = std::make_unique<std::array<u8, bytes>>();
  for (size_t word = 0; word < expected.size(); ++word) {
    const auto bits = std::bit_cast<u32>(expected[word]);
    for (unsigned byte = 0; byte < sizeof(u32); ++byte) {
      const unsigned shift = GetParam() ? byte * 8 : (3 - byte) * 8;
      (*original)[index * stride + word * sizeof(u32) + byte] = u8(bits >> shift);
    }
  }
  const void* key = original->data();
  const auto id = Register(key, bytes);
  ASSERT_NE(id, 0u);
  original->fill(0xCD);
  original.reset();
  GXSetArray(GX_POS_MTX_ARRAY, key, bytes, stride, GetParam());
  GXLoadPosMtxIndx(index, GX_PNMTX3);
  GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_NOOP);
  ASSERT_TRUE(fifo::drain());
  const auto* first = reinterpret_cast<const f32*>(&g_gxState.pnMtx[3].pos);
  for (size_t word = 0; word < expected.size(); ++word) EXPECT_FLOAT_EQ(first[word], expected[word]);
  EXPECT_EQ(g_gxState.blendMode, GX_BM_SUBTRACT); // following command alignment
  EXPECT_NE(g_gxState.arrays[GX_POS_MTX_ARRAY].data, key);
  EXPECT_EQ(g_gxState.arrays[GX_POS_MTX_ARRAY].le, GetParam());

  ASSERT_TRUE(immutable::unregister_source(id));
  ASSERT_TRUE(fifo::drain());
  EXPECT_FALSE(immutable::active(id));
  GXLoadPosMtxIndx(index, GX_PNMTX6); // still-bound owning view, not active lookup
  ASSERT_TRUE(fifo::drain());
  const auto* second = reinterpret_cast<const f32*>(&g_gxState.pnMtx[6].pos);
  for (size_t word = 0; word < expected.size(); ++word) EXPECT_FLOAT_EQ(second[word], expected[word]);
  EXPECT_EQ(immutable::retained_bytes(), bytes);
}

INSTANTIATE_TEST_SUITE_P(Endian, GXImmutableIndexedXFFifoTest, ::testing::Bool(),
                        [](const ::testing::TestParamInfo<bool>& info) {
                          return info.param ? "LittleEndian" : "BigEndian";
                        });

TEST_F(GXImmutableFifoTest, ImmutableDisplayListExecutesLegacyPrimitivesFromFreedOriginal) {
  std::array<u8, 32> expected{
      GX_DRAW_LINES | GX_VTXFMT0, 0, 2, 1, 2, 3, 4, 5, 6,
      GX_DRAW_TRIANGLES | GX_VTXFMT0, 0, 3, 11, 12, 13, 14, 15, 16, 17, 18, 19};
  auto original = std::make_unique<std::array<u8, 32>>(expected);
  const void* key = original->data();
  const auto id = Register(key, expected.size(), SourceKind::DisplayList);
  ASSERT_NE(id, 0u);
  original->fill(0xCD);
  original.reset();
  GXCallDisplayList(key, expected.size());
  GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_NOOP);
  ASSERT_TRUE(fifo::drain());
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 2u);
  EXPECT_EQ(aurora::gfx::g_testLastDraw.vtxCount, 3u);
  EXPECT_EQ(g_gxState.blendMode, GX_BM_SUBTRACT);
  {
    const auto held = immutable::lookup_display_list(id, expected.size());
    ASSERT_TRUE(held);
    EXPECT_EQ(std::memcmp(held.data(), expected.data(), expected.size()), 0);
  }
  ASSERT_TRUE(immutable::unregister_source(id));
  ASSERT_TRUE(fifo::drain());
  EXPECT_EQ(immutable::retained_bytes(), 0u); // sImmutableList restored after nested process
}

TEST_F(GXImmutableFifoTest, DrawDoneInsideImmutableListMustNotDiscardFollowingCommands) {
  // Compare the same valid FIFO list via legacy inline transport and immutable call.
  // Regression: ignoring nested ProcessResult loses both the draw-done callback and
  // every list byte following it. Retail draw-only MREA lists do not hit this case.
  const std::array<u8, 32> list{GX_LOAD_BP_REG, 0x45, 0, 0, 2,
                                GX_LOAD_BP_REG, 0x41, 0, 0, 1};
  sImmutableDrawDoneCalls.store(0, std::memory_order_relaxed);
  fifo::set_draw_done_callback(immutable_draw_done);
  GXCallDisplayList(list.data(), list.size());
  ASSERT_TRUE(fifo::drain());
  ASSERT_EQ(g_gxState.bpRegCache[0x41], 0x41000001u);
  ASSERT_EQ(sImmutableDrawDoneCalls.load(std::memory_order_acquire), 1u);

  GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_NOOP);
  ASSERT_TRUE(fifo::drain());
  ASSERT_EQ(g_gxState.blendMode, GX_BM_SUBTRACT);
  sImmutableDrawDoneCalls.store(0, std::memory_order_relaxed);
  const auto id = Register(list.data(), list.size(), SourceKind::DisplayList);
  ASSERT_NE(id, 0u);
  GXCallDisplayList(list.data(), list.size());
  ASSERT_TRUE(fifo::drain());
  EXPECT_EQ(g_gxState.bpRegCache[0x41], 0x41000001u);
  EXPECT_EQ(sImmutableDrawDoneCalls.load(std::memory_order_acquire), 1u);
}

TEST_F(GXImmutableFifoTest, MultipleDrawDoneIncludingEofResumeBeforeOuterTailAndKeepShortcallUnretired) {
  std::vector<u8> bytes;
  for (const u32 marker : {1u, 8u, 2u}) {
    append_bp(bytes, 0x41, marker);
    append_draw_done(bytes);
  }
  const auto list = MakeList(std::move(bytes));
  ASSERT_NE(list.id, 0u);
  __GXSetDirtyState();
  ASSERT_TRUE(fifo::drain());
  ASSERT_TRUE(fifo::end_frame()); // queue prefix/call/tail atomically before explicit drain
  const auto baseline = fifo::processed_bytes();
  CaptureCallbacks();
  FreeListOriginals();
  write_bp(0x41, 0x20);
  GXCallDisplayList(list.original, list.size);
  write_bp(0x41, 0x40);
  ASSERT_TRUE(immutable::unregister_source(list.id));
  ASSERT_TRUE(fifo::drain());
  ASSERT_EQ(sImmutableDrawDoneCalls.load(std::memory_order_acquire), 3u);
  const std::array<u32, 3> markers{1, 8, 2};
  for (unsigned i = 0; i < markers.size(); ++i) {
    EXPECT_EQ(sCallbackMarkers[i], 0x41000000u | markers[i]);
    EXPECT_EQ(sCallbackWatermarks[i], baseline + (i == 0 ? 0 : 5));
    EXPECT_TRUE(sCallbackUnlocked[i]);
  }
  EXPECT_EQ(g_gxState.bpRegCache[0x41], 0x41000040u);
  EXPECT_EQ(fifo::processed_bytes(), fifo::published_bytes());
  EXPECT_EQ(immutable::retained_bytes(), 0u);
}

TEST_F(GXImmutableFifoTest, SuspendedLastShortcallSurvivesBufferGrowthAndProducerUnregisterDuringCallback) {
  std::vector<u8> bytes;
  append_bp(bytes, 0x41, 1);
  append_draw_done(bytes);
  append_bp(bytes, 0x41, 8);
  const auto list = MakeList(std::move(bytes));
  ASSERT_NE(list.id, 0u);
  __GXSetDirtyState();
  ASSERT_TRUE(fifo::drain());
  const auto baseline = fifo::processed_bytes();
  CaptureCallbacks();
  sBlockingCallbackEntered.store(false, std::memory_order_relaxed);
  sBlockingCallbackReturn.store(false, std::memory_order_relaxed);
  fifo::set_draw_done_callback(blocking_immutable_draw_done);
  CallbackReturnGuard releaseCallback;
  FreeListOriginals();
  GXCallDisplayList(list.original, list.size);
  ASSERT_TRUE(wait_for_callback());
  EXPECT_TRUE(sCallbackUnlocked[0]);
  EXPECT_EQ(sCallbackMarkers[0], 0x41000001u);
  EXPECT_EQ(sCallbackWatermarks[0], baseline);
  EXPECT_EQ(fifo::processed_bytes(), baseline);
  EXPECT_EQ(fifo::published_bytes(), baseline + 15);
  EXPECT_EQ(immutable::retained_bytes(), list.size);
  ASSERT_TRUE(immutable::unregister_source(list.id));
  EXPECT_EQ(immutable::producer_admission(list.original, list.size, SourceKind::DisplayList), 0u);
  // Reallocate the outer FIFO while the callback runs without sBufferMutex.
  const std::vector<u8> padding(128 * 1024, GX_NOP);
  fifo::write_data(padding.data(), padding.size());
  fifo::publish();
  EXPECT_EQ(fifo::processed_bytes(), baseline);
  sBlockingCallbackReturn.store(true, std::memory_order_release);
  ASSERT_TRUE(fifo::drain());
  EXPECT_EQ(g_gxState.bpRegCache[0x41], 0x41000008u);
  EXPECT_EQ(sImmutableDrawDoneCalls.load(std::memory_order_acquire), 1u);
  EXPECT_FALSE(immutable::active(list.id));
  EXPECT_EQ(immutable::retained_bytes(), 0u);
  EXPECT_EQ(fifo::processed_bytes(), fifo::published_bytes());
}

TEST_F(GXImmutableFifoTest, NestedListsResumeChildThenParentUsingOrderedSnapshots) {
  std::vector<u8> childBytes;
  append_bp(childBytes, 0x41, 1);
  append_draw_done(childBytes);
  append_bp(childBytes, 0x41, 8);
  append_draw_done(childBytes);
  const auto child = MakeList(std::move(childBytes));
  ASSERT_NE(child.id, 0u);
  std::vector<u8> siblingBytes;
  append_bp(siblingBytes, 0x41, 4);
  append_draw_done(siblingBytes);
  const auto sibling = MakeList(std::move(siblingBytes));
  ASSERT_NE(sibling.id, 0u);
  std::vector<u8> parentBytes;
  append_immutable_call(parentBytes, child.id, child.size);
  append_bp(parentBytes, 0x41, 2);
  append_draw_done(parentBytes);
  append_immutable_call(parentBytes, sibling.id, sibling.size);
  append_bp(parentBytes, 0x41, 0x20);
  const auto parent = MakeList(std::move(parentBytes));
  ASSERT_NE(parent.id, 0u);
  __GXSetDirtyState();
  ASSERT_TRUE(fifo::drain());
  ASSERT_TRUE(fifo::end_frame());
  const auto baseline = fifo::processed_bytes();
  CaptureCallbacks();
  FreeListOriginals();
  GXCallDisplayList(parent.original, parent.size);
  // Revoked producer admission must not bypass earlier nested processor calls.
  for (const auto id : {child.id, sibling.id, parent.id}) ASSERT_TRUE(immutable::unregister_source(id));
  ASSERT_TRUE(fifo::drain());
  ASSERT_EQ(sImmutableDrawDoneCalls.load(std::memory_order_acquire), 4u);
  const std::array<u32, 4> markers{1, 8, 2, 4};
  for (unsigned i = 0; i < markers.size(); ++i) {
    EXPECT_EQ(sCallbackMarkers[i], 0x41000000u | markers[i]);
    EXPECT_EQ(sCallbackWatermarks[i], baseline);
    EXPECT_TRUE(sCallbackUnlocked[i]);
  }
  EXPECT_EQ(g_gxState.bpRegCache[0x41], 0x41000020u);
  EXPECT_EQ(immutable::retained_bytes(), 0u);
}

TEST_F(GXImmutableFifoTest, MaximumNestedDepthResumesAllParentTails) {
  std::vector<u8> leafBytes;
  append_draw_done(leafBytes);
  auto root = MakeList(std::move(leafBytes));
  ASSERT_NE(root.id, 0u);
  for (u32 depth = 1; depth < fifo::MaxImmutableListDepth; ++depth) {
    std::vector<u8> parent;
    append_immutable_call(parent, root.id, root.size);
    append_bp(parent, 0x41, depth);
    root = MakeList(std::move(parent));
    ASSERT_NE(root.id, 0u);
  }
  __GXSetDirtyState();
  ASSERT_TRUE(fifo::drain());
  ASSERT_TRUE(fifo::end_frame());
  CaptureCallbacks();
  FreeListOriginals();
  GXCallDisplayList(root.original, root.size);
  for (const auto id : m_sources) ASSERT_TRUE(immutable::unregister_source(id));
  ASSERT_TRUE(fifo::drain());
  EXPECT_EQ(sImmutableDrawDoneCalls.load(std::memory_order_acquire), 1u);
  EXPECT_EQ(g_gxState.bpRegCache[0x41], 0x41000000u | (fifo::MaxImmutableListDepth - 1));
  EXPECT_EQ(immutable::retained_bytes(), 0u);
}

TEST_F(GXImmutableFifoTest, ExcessiveImmutableNestingFailsBeforeStackOverflow) {
  auto root = MakeList(std::vector<u8>{GX_NOP});
  ASSERT_NE(root.id, 0u);
  for (u32 depth = 1; depth <= fifo::MaxImmutableListDepth; ++depth) {
    std::vector<u8> parent;
    append_immutable_call(parent, root.id, root.size);
    root = MakeList(std::move(parent));
    ASSERT_NE(root.id, 0u);
  }
  ASSERT_TRUE(Stop()); // no FIFO worker survives into the death-test child
  FreeListOriginals();
  std::vector<u8> call;
  append_immutable_call(call, root.id, root.size);
  EXPECT_DEATH_IF_SUPPORTED((void)fifo::process(call.data(), call.size()), "Immutable display list nesting exceeds");
}

TEST_F(GXImmutableFifoTest, QuiescentSideClearReleasesPendingNestedViewsWithoutResumingCommands) {
  std::vector<u8> childBytes;
  append_draw_done(childBytes);
  const auto child = MakeList(std::move(childBytes));
  ASSERT_NE(child.id, 0u);
  std::vector<u8> parentBytes;
  append_immutable_call(parentBytes, child.id, child.size);
  append_bp(parentBytes, 0x41, 1);
  const auto parent = MakeList(std::move(parentBytes));
  ASSERT_NE(parent.id, 0u);
  ASSERT_TRUE(Stop());
  FreeListOriginals();
  std::vector<u8> call;
  append_immutable_call(call, parent.id, parent.size);
  // Direct production parser at the joined CPU checkpoint leaves the same owned
  // continuation that a failed FIFO/callback would retain for root cleanup.
  const auto result = fifo::process(call.data(), call.size());
  EXPECT_EQ(result.bytesProcessed, 0u);
  EXPECT_TRUE(result.drawDone);
  EXPECT_EQ(result.pendingCallBytes, call.size());
  ASSERT_TRUE(immutable::consume_release(child.id));
  ASSERT_TRUE(immutable::consume_release(parent.id));
  EXPECT_EQ(immutable::retained_bytes(), child.size + parent.size); // pending views alone
  const auto published = fifo::published_bytes();
  const auto processed = fifo::processed_bytes();
  fifo::clear_immutable_bindings();
  EXPECT_EQ(immutable::retained_bytes(), 0u);
  EXPECT_EQ(fifo::published_bytes(), published);
  EXPECT_EQ(fifo::processed_bytes(), processed);
}

} // namespace
