// Standalone: link the real decoded_cpu.cpp and immutable.cpp; no renderer/device.
#include "../extern/aurora/lib/gx/decoded_cpu.hpp"
#undef CHECK
#include "port_test.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <new>
#include <utility>
#include <vector>

// Count every decoder allocation, and inject failure at each allocation site.
// Tracking is enabled only around decode(), never around fixture/source setup.
namespace allocation_probe {
bool active = false;
uint64_t bytes = 0;
unsigned calls = 0;
int failAt = -1;
void start(int fail = -1) {
  bytes = 0;
  calls = 0;
  failAt = fail;
  active = true;
}
void stop() { active = false; }
} // namespace allocation_probe

void* operator new(std::size_t size) {
  if (allocation_probe::active) {
    const unsigned call = allocation_probe::calls++;
    if (int(call) == allocation_probe::failAt) {
      throw std::bad_alloc{};
    }
    allocation_probe::bytes += size;
  }
  if (void* pointer = std::malloc(size == 0 ? 1 : size)) {
    return pointer;
  }
  throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

// Only immutable ownership transport is stubbed. Decoding and snapshot bytes are real.
namespace aurora::gx::fifo {
bool enqueue_ownership(uint16_t, uint64_t) noexcept { return true; }
bool in_display_list() { return false; }
} // namespace aurora::gx::fifo
namespace aurora::h700 {
bool generation_cpu_quiescence_held() noexcept { return false; }
} // namespace aurora::h700

namespace {
using namespace aurora::gx;
using namespace aurora::gx::decoded;

static_assert(ScratchBudgetBytes == 16ull * 1024 * 1024);
static_assert(MaxLocalVertices == 65536);
static_assert(noexcept(decode(std::declval<const DecodeInput&>(), std::declval<CpuMesh&>(),
                              std::declval<DecodeFailure&>())));

void put(std::vector<uint8_t>& bytes, size_t offset, uint32_t value, unsigned size, bool le = false) {
  REQUIRE(offset + size <= bytes.size());
  for (unsigned i = 0; i < size; ++i) {
    bytes[offset + i] = uint8_t(value >> ((le ? i : size - 1 - i) * 8));
  }
}

immutable::SourceView snapshot(std::span<const uint8_t> bytes,
                               immutable::SourceKind kind = immutable::SourceKind::Array,
                               uint32_t viewSize = 0) {
  const auto id = immutable::register_source(bytes.data(), uint32_t(bytes.size()), kind);
  REQUIRE(id != 0);
  REQUIRE(immutable::consume_register(id));
  if (viewSize == 0) {
    viewSize = uint32_t(bytes.size());
  }
  auto view = kind == immutable::SourceKind::Array ? immutable::lookup_array(id, viewSize)
                                                 : immutable::lookup_display_list(id, viewSize);
  REQUIRE(view);
  REQUIRE(immutable::unregister_source(id));
  REQUIRE(immutable::consume_release(id));
  REQUIRE(!immutable::active(id));
  return view; // Tests decode after ordered release AND after original storage is freed.
}

struct Fixture {
  ShaderConfig config{};
  std::vector<uint8_t> raw;
  std::array<immutable::SourceView, MaxVtxAttr> arrays{};
  std::array<uint32_t, MaxVtxAttr> bases{};

  Fixture() {
    config.vtxStride = 3;
    config.attrs[GX_VA_POS] = {.attrType = GX_DIRECT, .cnt = 3, .compType = GX_U8};
  }
  DecodeInput input(GXPrimitive primitive, uint32_t count) const {
    return {config, primitive, count, raw, arrays, bases};
  }
  void repeat(const std::vector<uint8_t>& record, uint32_t count) {
    REQUIRE(record.size() == config.vtxStride);
    raw.resize(record.size() * count);
    for (uint32_t i = 0; i < count; ++i) {
      std::memcpy(raw.data() + size_t{i} * record.size(), record.data(), record.size());
    }
  }
  CpuMesh run(GXPrimitive primitive = GX_TRIANGLES) const {
    CpuMesh mesh;
    DecodeFailure failure = DecodeFailure::AllocationFailed;
    REQUIRE(decode(input(primitive, uint32_t(raw.size() / config.vtxStride)), mesh, failure));
    CHECK_EQ(failure, DecodeFailure::None);
    CHECK_EQ(mesh.vertices.size(), size_t{mesh.vertexCount} * make_layout(config).stride);
    for (auto index : mesh.indices) {
      REQUIRE(index < mesh.vertexCount);
    }
    return mesh;
  }
};

CpuMesh sentinel() { return {{91, 92, 93, 94}, {17, 18, 19}, 20}; }
void expect_unchanged(const CpuMesh& mesh) {
  CHECK((mesh.vertices == std::vector<uint8_t>{91, 92, 93, 94}));
  CHECK((mesh.indices == std::vector<uint16_t>{17, 18, 19}));
  CHECK_EQ(mesh.vertexCount, 20u);
}
void expect_failure(const Fixture& fixture, DecodeFailure expected, GXPrimitive primitive = GX_TRIANGLES,
                    uint32_t count = 3) {
  CpuMesh mesh = sentinel();
  DecodeFailure failure = DecodeFailure::None;
  CHECK(!decode(fixture.input(primitive, count), mesh, failure));
  CHECK_EQ(failure, expected);
  expect_unchanged(mesh);
}

std::array<float, 4> values(const CpuMesh& mesh, const ShaderConfig& config, GXAttr attr,
                          uint32_t vertex = 0) {
  const auto layout = make_layout(config);
  const auto* field = layout.find(attr);
  REQUIRE(field != nullptr);
  REQUIRE(vertex < mesh.vertexCount);
  std::array<float, 4> result{};
  std::memcpy(result.data(), mesh.vertices.data() + size_t{vertex} * layout.stride + field->offset,
              field->size());
  return result;
}
void expect_values(const CpuMesh& mesh, const ShaderConfig& config, GXAttr attr,
                   std::initializer_list<float> expected, uint32_t vertex = 0) {
  const auto actual = values(mesh, config, attr, vertex);
  size_t i = 0;
  for (float value : expected) {
    CHECK_EQ(actual[i++], value);
  }
}
std::array<uint32_t, 3> selector_words(const CpuMesh& mesh, const ShaderConfig& config, uint32_t vertex = 0) {
  const auto layout = make_layout(config);
  const auto* field = layout.find(GX_VA_NULL);
  REQUIRE(field != nullptr && field->format == wgpu::VertexFormat::Uint32x3);
  std::array<uint32_t, 3> result{};
  std::memcpy(result.data(), mesh.vertices.data() + size_t{vertex} * layout.stride + field->offset,
              sizeof(result));
  return result;
}

void direct_offsets_and_zero_extension() {
  Fixture f;
  f.config.vtxStride = 45;
  f.config.attrs[GX_VA_POS] = {.attrType = GX_DIRECT, .cnt = 2, .compType = GX_F32,
                             .offset = 3, .frac = 31, .le = true};
  f.config.attrs[GX_VA_NRM] = {.attrType = GX_DIRECT, .cnt = 3, .compType = GX_S8,
                             .offset = 15, .frac = 6, .le = true};
  f.config.attrs[GX_VA_TEX0] = {.attrType = GX_DIRECT, .cnt = 2, .compType = GX_S16,
                              .offset = 20, .frac = 4, .le = true};
  f.config.attrs[GX_VA_TEX1] = {.attrType = GX_DIRECT, .cnt = 1, .compType = GX_U16,
                              .offset = 25, .frac = 15, .le = true};
  f.config.attrs[GX_VA_TEX2] = {.attrType = GX_DIRECT, .cnt = 2, .compType = GX_U8,
                              .offset = 28, .frac = 1};
  f.config.attrs[GX_VA_TEX3] = {.attrType = GX_DIRECT, .cnt = 1, .compType = GX_S8,
                              .offset = 31, .frac = 31};
  f.config.attrs[GX_VA_TEX4] = {.attrType = GX_DIRECT, .cnt = 2, .compType = GX_F32,
                              .offset = 33, .frac = 31, .le = true};
  std::vector<uint8_t> record(45, 0xcd);
  put(record, 3, std::bit_cast<uint32_t>(12.5f), 4);
  put(record, 7, 0x80000000u, 4);
  record[15] = 0x80;
  record[16] = 0xff;
  record[17] = 0x7f;
  put(record, 20, 0x8000, 2);
  put(record, 22, 0x7fff, 2);
  put(record, 25, 0xffff, 2);
  record[28] = 255;
  record[29] = 0;
  record[31] = 0x80;
  put(record, 33, std::bit_cast<uint32_t>(-.25f), 4);
  put(record, 37, std::bit_cast<uint32_t>(1.25f), 4);
  f.repeat(record, 3);
  const auto before = f.config;
  const auto mesh = f.run();
  CHECK_EQ(mesh.vertexCount, 1u);
  CHECK((mesh.indices == std::vector<uint16_t>{0, 0, 0}));
  expect_values(mesh, f.config, GX_VA_POS, {12.5f, -0.f, 0.f});
  CHECK_EQ(std::bit_cast<uint32_t>(values(mesh, f.config, GX_VA_POS)[1]), 0x80000000u);
  expect_values(mesh, f.config, GX_VA_NRM, {-2.f, -1.f / 64.f, 127.f / 64.f});
  expect_values(mesh, f.config, GX_VA_TEX0, {-2048.f, 32767.f / 16.f});
  expect_values(mesh, f.config, GX_VA_TEX1, {65535.f / 32768.f, 0.f});
  expect_values(mesh, f.config, GX_VA_TEX2, {127.5f, 0.f});
  expect_values(mesh, f.config, GX_VA_TEX3, {-128.f / 2147483648.f, 0.f});
  expect_values(mesh, f.config, GX_VA_TEX4, {-.25f, 1.25f});
  CHECK(std::memcmp(&before, &f.config, sizeof(before)) == 0);
}

void numeric_endian_matrix() {
  struct NumericCase {
    uint8_t type, size, frac;
    std::array<uint32_t, 3> bits;
    std::array<float, 3> expected;
  };
  const std::array cases{
      NumericCase{GX_U8, 1, 3, {255, 1, 127}, {255.f / 8.f, 1.f / 8.f, 127.f / 8.f}},
      NumericCase{GX_S8, 1, 6, {0x80, 0xff, 0x7f}, {-2.f, -1.f / 64.f, 127.f / 64.f}},
      NumericCase{GX_U16, 2, 15, {0xffff, 0x1234, 1}, {65535.f / 32768.f, 4660.f / 32768.f, 1.f / 32768.f}},
      NumericCase{GX_S16, 2, 14, {0x8000, 0xffff, 0x7fff}, {-2.f, -1.f / 16384.f, 32767.f / 16384.f}},
      NumericCase{GX_F32, 4, 31, {0x3fa00000, 0xc0200000, 0x40700000}, {1.25f, -2.5f, 3.75f}},
  };
  for (const auto& test : cases) {
    for (auto mode : {GX_DIRECT, GX_INDEX8, GX_INDEX16}) {
      for (bool le : {false, true}) {
        Fixture f;
        const uint8_t element = test.size * 3;
        f.config.vtxStride = mode == GX_DIRECT ? element + 3 : 5;
        f.config.attrs[GX_VA_POS] = {.attrType = uint8_t(mode), .cnt = 3, .compType = test.type,
                                   .offset = 2, .stride = uint8_t(element + 3), .frac = test.frac, .le = le};
        std::vector<uint8_t> record(f.config.vtxStride, 0xab);
        if (mode == GX_DIRECT) {
          for (unsigned i = 0; i < 3; ++i) {
            put(record, 2 + i * test.size, test.bits[i], test.size); // DIRECT ignores le=true.
          }
        } else {
          const uint32_t index = mode == GX_INDEX8 ? 2 : 258;
          f.bases[GX_VA_POS] = 3;
          const size_t offset = size_t{index + 3} * (element + 3);
          std::vector<uint8_t> source(offset + element, 0xcd); // Exact final element, no tail word.
          for (unsigned i = 0; i < 3; ++i) {
            put(source, offset + i * test.size, test.bits[i], test.size, le);
          }
          f.arrays[GX_VA_POS] = snapshot(source);
          source.assign(source.size(), 0); // The immutable snapshot, not the original, must be read.
          put(record, 2, index, mode == GX_INDEX8 ? 1 : 2); // INDEX16 stays BE for LE arrays.
        }
        f.repeat(record, 3);
        const auto mesh = f.run();
        CHECK_EQ(mesh.vertexCount, 1u);
        const auto actual = values(mesh, f.config, GX_VA_POS);
        for (unsigned i = 0; i < 3; ++i) {
          CHECK_EQ(actual[i], test.expected[i]);
        }
      }
    }
  }
  Fixture broadcast;
  broadcast.config.vtxStride = 2;
  broadcast.config.attrs[GX_VA_POS] = {.attrType = GX_INDEX16, .cnt = 3, .compType = GX_U16,
                                     .stride = 0, .le = true};
  broadcast.bases[GX_VA_POS] = 0xffffffffu;
  broadcast.arrays[GX_VA_POS] = snapshot(std::array<uint8_t, 6>{3, 0, 4, 0, 5, 0});
  broadcast.repeat({255, 255}, 3);
  expect_values(broadcast.run(), broadcast.config, GX_VA_POS, {3.f, 4.f, 5.f});
}

void retail_mixed_arrays() {
  Fixture f;
  f.config.vtxStride = 10;
  f.config.attrs[GX_VA_PNMTXIDX] = {.attrType = GX_DIRECT, .cnt = 1};
  f.config.attrs[GX_VA_POS] = {.attrType = GX_INDEX16, .cnt = 3, .compType = GX_F32,
                             .offset = 2, .stride = 13, .le = false};
  f.config.attrs[GX_VA_NRM] = {.attrType = GX_INDEX16, .cnt = 3, .compType = GX_S16,
                             .offset = 4, .stride = 7, .frac = 14, .le = true};
  f.config.attrs[GX_VA_CLR0] = {.attrType = GX_INDEX16, .cnt = 1, .compType = GX_RGB565,
                              .offset = 6, .stride = 3, .le = false};
  f.config.attrs[GX_VA_TEX0] = {.attrType = GX_INDEX16, .cnt = 2, .compType = GX_S16,
                              .offset = 8, .stride = 5, .frac = 8, .le = false};
  f.config.tcgs[0].src = GX_TG_TEX0;
  f.config.tcgs[0].type = GX_TG_MTX2x4;
  f.config.colorChannels[GX_COLOR0].lightingEnabled = true;
  f.bases[GX_VA_POS] = 1;
  f.bases[GX_VA_NRM] = 2;
  f.bases[GX_VA_CLR0] = 1;
  f.bases[GX_VA_TEX0] = 3;
  std::vector<uint8_t> positions(38, 0xcd), normals(27, 0xcd), colors(8, 0xcd), uvs(24, 0xcd);
  const std::array<float, 6> xyz{-1.5f, 2.25f, -3.75f, 8.f, 9.f, 10.f};
  const std::array<uint32_t, 6> nrm{0x8000, 0x4000, 0x7fff, 0xc000, 0, 0x2000};
  for (unsigned i = 0; i < 6; ++i) {
    put(positions, (1 + i / 3) * 13 + (i % 3) * 4, std::bit_cast<uint32_t>(xyz[i]), 4);
    put(normals, (2 + i / 3) * 7 + (i % 3) * 2, nrm[i], 2, true);
  }
  put(colors, 3, 0xf81f, 2);
  put(colors, 6, 0x07e0, 2);
  put(uvs, 15, 0xff80, 2);
  put(uvs, 17, 0x0180, 2);
  put(uvs, 20, 0x0080, 2);
  put(uvs, 22, 0xfe80, 2);
  f.arrays[GX_VA_POS] = snapshot(positions);
  f.arrays[GX_VA_NRM] = snapshot(normals);
  f.arrays[GX_VA_CLR0] = snapshot(colors);
  f.arrays[GX_VA_TEX0] = snapshot(uvs);
  const std::vector<uint8_t> base{6, 0xcd, 0, 0, 0, 0, 0, 0, 0, 0};
  for (unsigned changed : {0u, 4u, 6u, 8u, 0u, 2u}) {
    auto record = base;
    if (changed != 0) {
      put(record, changed, 1, 2);
    }
    f.raw.insert(f.raw.end(), record.begin(), record.end());
  }
  const auto mesh = f.run();
  CHECK_EQ(mesh.vertexCount, 5u);
  CHECK((mesh.indices == std::vector<uint16_t>{0, 1, 2, 3, 0, 4}));
  expect_values(mesh, f.config, GX_VA_POS, {-1.5f, 2.25f, -3.75f});
  expect_values(mesh, f.config, GX_VA_POS, {8.f, 9.f, 10.f}, 4);
  expect_values(mesh, f.config, GX_VA_NRM, {-2.f, 1.f, 32767.f / 16384.f});
  expect_values(mesh, f.config, GX_VA_NRM, {-1.f, 0.f, .5f}, 1);
  expect_values(mesh, f.config, GX_VA_CLR0, {1.f, 0.f, 1.f, 1.f});
  expect_values(mesh, f.config, GX_VA_CLR0, {0.f, 1.f, 0.f, 1.f}, 2);
  expect_values(mesh, f.config, GX_VA_TEX0, {-.5f, 1.5f});
  expect_values(mesh, f.config, GX_VA_TEX0, {.5f, -1.5f}, 3);
  const std::array<uint32_t, 3> words{6, 0, 0};
  CHECK(selector_words(mesh, f.config) == words);
}

void packed_color_matrix() {
  struct ColorCase {
    uint8_t type, size;
    uint32_t packed;
    std::array<float, 4> expected;
  };
  const std::array cases{
      ColorCase{GX_RGB565, 2, (17u << 11) | (9u << 5) | 3u, {17.f / 31.f, 9.f / 63.f, 3.f / 31.f, 1.f}},
      ColorCase{GX_RGB8, 3, 0x1181ee, {17.f / 255.f, 129.f / 255.f, 238.f / 255.f, 1.f}},
      ColorCase{GX_RGBX8, 4, 0x1181ee00, {17.f / 255.f, 129.f / 255.f, 238.f / 255.f, 1.f}},
      ColorCase{GX_RGBA4, 2, 0x17bd, {1.f / 15.f, 7.f / 15.f, 11.f / 15.f, 13.f / 15.f}},
      ColorCase{GX_RGBA6, 3, (1u << 18) | (15u << 12) | (37u << 6) | 62u,
                {1.f / 63.f, 15.f / 63.f, 37.f / 63.f, 62.f / 63.f}},
      ColorCase{GX_RGBA8, 4, 0x1181ee40, {17.f / 255.f, 129.f / 255.f, 238.f / 255.f, 64.f / 255.f}},
  };
  for (const auto& test : cases) {
    for (auto mode : {GX_DIRECT, GX_INDEX8, GX_INDEX16}) {
      for (bool le : {false, true}) {
        Fixture f;
        f.config.vtxStride = mode == GX_DIRECT ? 5 + test.size : 7;
        const AttrConfig color{.attrType = uint8_t(mode), .cnt = 1, .compType = test.type,
                               .offset = 5, .stride = uint8_t(test.size + 3), .le = le};
        f.config.attrs[GX_VA_CLR0] = color;
        f.config.attrs[GX_VA_CLR1] = color;
        std::vector<uint8_t> record(f.config.vtxStride, 0xcd);
        const bool packed = test.type == GX_RGB565 || test.type == GX_RGBA4 || test.type == GX_RGBA6;
        if (mode == GX_DIRECT) {
          put(record, 5, test.packed, test.size);
        } else {
          const uint32_t index = mode == GX_INDEX8 ? 1 : 257;
          f.bases[GX_VA_CLR0] = f.bases[GX_VA_CLR1] = 3;
          const size_t offset = size_t{index + 3} * color.stride;
          std::vector<uint8_t> source(offset + test.size, 0xab);
          put(source, offset, test.packed, test.size, packed && le);
          f.arrays[GX_VA_CLR0] = snapshot(source);
          f.arrays[GX_VA_CLR1] = f.arrays[GX_VA_CLR0];
          put(record, 5, index, mode == GX_INDEX8 ? 1 : 2);
        }
        f.repeat(record, 3);
        const auto mesh = f.run();
        for (auto attr : {GX_VA_CLR0, GX_VA_CLR1}) {
          const auto actual = values(mesh, f.config, attr);
          for (unsigned i = 0; i < 4; ++i) {
            CHECK_EQ(actual[i], test.expected[i]);
          }
        }
      }
    }
  }
}

Fixture full_fixture() {
  Fixture f;
  uint8_t offset = 0;
  for (uint32_t attr = GX_VA_POS; attr <= GX_VA_TEX7; ++attr) {
    const bool color = attr == GX_VA_CLR0 || attr == GX_VA_CLR1;
    const uint8_t count = color ? 1 : attr == GX_VA_POS || attr == GX_VA_NRM ? 3 : 2;
    const uint8_t size = color ? 4 : count;
    f.config.attrs[attr] = {.attrType = GX_INDEX8, .cnt = count,
                           .compType = uint8_t(color ? GX_RGBA8 : GX_U8),
                           .offset = offset++, .stride = size, .le = true};
    std::vector<uint8_t> source(size * 2);
    for (uint8_t i = 0; i < size; ++i) {
      source[i] = 10 + i;
      source[size + i] = 20 + i;
    }
    f.arrays[attr] = snapshot(source);
  }
  for (const auto& selector : Selectors) {
    // The raw shader uses just a byte, not compType/frac/le, for selectors.
    f.config.attrs[selector.attr] = {.attrType = GX_DIRECT, .cnt = 1, .compType = 0xff,
                                    .offset = offset++, .frac = 0xff, .le = true};
  }
  f.config.vtxStride = offset;
  return f;
}

void full_tuple_and_selectors() {
  auto f = full_fixture();
  const auto layout = make_layout(f.config);
  REQUIRE(layout.valid);
  CHECK_EQ(layout.stride, MaxStride);
  CHECK_EQ(layout.attributeCount, MaxAttributes);
  std::vector<uint8_t> base(f.config.vtxStride, 0);
  const std::array<uint8_t, 9> selectors{3, 0x80, 255, 1, 0x11, 0x22, 0x33, 0x44, 0x55};
  for (size_t i = 0; i < Selectors.size(); ++i) {
    base[f.config.attrs[Selectors[i].attr].offset] = selectors[i];
  }
  f.raw = base;
  f.raw.insert(f.raw.end(), base.begin(), base.end());
  std::vector<uint16_t> expectedIndices{0, 0};
  for (uint32_t attr = GX_VA_POS; attr <= GX_VA_TEX7; ++attr) {
    auto changed = base;
    changed[f.config.attrs[attr].offset] = 1;
    f.raw.insert(f.raw.end(), changed.begin(), changed.end());
    expectedIndices.push_back(uint16_t(expectedIndices.size() - 1));
  }
  for (const auto& selector : Selectors) {
    auto changed = base;
    changed[f.config.attrs[selector.attr].offset] ^= 0x40;
    f.raw.insert(f.raw.end(), changed.begin(), changed.end());
    expectedIndices.push_back(uint16_t(expectedIndices.size() - 1));
  }
  f.raw.insert(f.raw.end(), base.begin(), base.end());
  expectedIndices.push_back(0);
  const auto before = f.config;
  auto mesh = f.run();
  CHECK_EQ(mesh.vertexCount, 22u);
  CHECK(mesh.indices == expectedIndices);
  const std::array<uint32_t, 3> expectedWords{0x01ff8003, 0x44332211, 0x55};
  CHECK(selector_words(mesh, f.config) == expectedWords); // PNMTXIDX remains 3, not 1.
  for (uint32_t attr = GX_VA_POS; attr <= GX_VA_TEX7; ++attr) {
    const bool color = attr == GX_VA_CLR0 || attr == GX_VA_CLR1;
    const auto actual = values(mesh, f.config, static_cast<GXAttr>(attr), attr - GX_VA_POS + 1);
    const uint32_t count = color ? 4 : f.config.attrs[attr].cnt;
    for (uint32_t i = 0; i < count; ++i) {
      CHECK_EQ(actual[i], color ? float(20 + i) / 255.f : float(20 + i));
    }
  }
  for (size_t i = 0; i < Selectors.size(); ++i) {
    auto changedWords = expectedWords;
    changedWords[Selectors[i].word] ^= 0x40u << Selectors[i].shift;
    CHECK(selector_words(mesh, f.config, uint32_t(13 + i)) == changedWords);
  }
  CHECK(std::memcmp(&before, &f.config, sizeof(before)) == 0);
  f.config.decodedNative = true;
  const auto flagged = f.run();
  CHECK(flagged.vertices == mesh.vertices && flagged.indices == mesh.indices);

  Fixture sparse;
  sparse.config.vtxStride = 5;
  sparse.config.attrs[GX_VA_PNMTXIDX] = {.attrType = GX_DIRECT, .cnt = 1, .offset = 3};
  sparse.config.attrs[GX_VA_TEX7MTXIDX] = {.attrType = GX_DIRECT, .cnt = 1, .offset = 4};
  sparse.repeat({1, 2, 3, 33, 254}, 3);
  const std::array<uint32_t, 3> sparseWords{33, 0, 254};
  CHECK(selector_words(sparse.run(), sparse.config) == sparseWords);
}

void raw_tuple_collision_and_identity() {
  Fixture f;
  // Deliberately collide in the first bucket of the 8-entry table. Equality must
  // still compare the full record, not bucket/hash or only the position index.
  const auto bucket = [](uint8_t value) {
    uint64_t hash = 14695981039346656037ull;
    for (auto byte : {value, uint8_t{0}, uint8_t{0}}) {
      hash = (hash ^ byte) * 1099511628211ull;
    }
    return uint32_t(hash ^ (hash >> 32)) & 7u;
  };
  std::vector<uint8_t> colliding;
  for (unsigned value = 0; value < 256 && colliding.size() < 3; ++value) {
    if (bucket(uint8_t(value)) == bucket(0)) {
      colliding.push_back(uint8_t(value));
    }
  }
  REQUIRE(colliding.size() == 3);
  for (auto value : {colliding[0], colliding[1], colliding[2], colliding[0]}) {
    f.raw.insert(f.raw.end(), {value, 0, 0});
  }
  auto mesh = f.run(GX_TRIANGLEFAN);
  CHECK_EQ(mesh.vertexCount, 3u);
  CHECK((mesh.indices == std::vector<uint16_t>{0, 1, 2, 0, 2, 0}));
  for (unsigned i = 0; i < 3; ++i) {
    expect_values(mesh, f.config, GX_VA_POS, {float(colliding[i]), 0.f, 0.f}, i);
  }

  // Raw tuples are permitted to remain distinct even if decoded values match.
  f.config.vtxStride = 4;
  f.repeat({1, 2, 3, 0}, 3);
  f.raw[7] = 1;
  mesh = f.run();
  CHECK_EQ(mesh.vertexCount, 2u);
  CHECK((mesh.indices == std::vector<uint16_t>{0, 1, 0}));
  expect_values(mesh, f.config, GX_VA_POS, {1.f, 2.f, 3.f}, 1);
}

void primitive_orientation() {
  struct Case {
    GXPrimitive primitive;
    uint32_t count;
    std::vector<uint16_t> expected;
  };
  const std::array cases{
      Case{GX_TRIANGLES, 8, {0, 1, 2, 3, 4, 5}},
      Case{GX_QUADS, 9, {0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4}},
      Case{GX_TRIANGLESTRIP, 6, {0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5}},
      Case{GX_TRIANGLEFAN, 6, {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5}},
  };
  for (const auto& test : cases) {
    Fixture f;
    for (uint32_t i = 0; i < test.count; ++i) {
      f.raw.insert(f.raw.end(), {uint8_t(i), 0, 0});
    }
    const auto mesh = f.run(test.primitive);
    CHECK_EQ(mesh.vertexCount, test.count);
    CHECK(mesh.indices == test.expected);
  }
  for (auto primitive : {GX_TRIANGLES, GX_QUADS, GX_TRIANGLESTRIP, GX_TRIANGLEFAN}) {
    for (uint32_t count = 0; count < (primitive == GX_QUADS ? 4u : 3u); ++count) {
      Fixture f;
      for (uint32_t i = 0; i < count; ++i) {
        f.raw.insert(f.raw.end(), {uint8_t(i), 0, 0});
      }
      auto mesh = sentinel();
      DecodeFailure failure = DecodeFailure::AllocationFailed;
      REQUIRE(decode(f.input(primitive, count), mesh, failure));
      CHECK_EQ(failure, DecodeFailure::None);
      CHECK_EQ(mesh.vertexCount, count);
      CHECK(mesh.indices.empty());
      CHECK_EQ(mesh.vertices.size(), size_t{count} * 12);
    }
  }
  Fixture degenerate;
  degenerate.raw = {0, 0, 0, 1, 0, 0, 2, 0, 0, 1, 0, 0, 0, 0, 0, 2, 0, 0};
  const auto mesh = degenerate.run(GX_TRIANGLESTRIP);
  CHECK((mesh.indices == std::vector<uint16_t>{0, 1, 2, 2, 1, 1, 2, 1, 0, 0, 1, 2}));
}

void unsupported_and_record_bounds() {
  const auto reject_config = [](auto change) {
    Fixture f;
    f.repeat({1, 2, 3}, 3);
    change(f.config);
    expect_failure(f, DecodeFailure::UnsupportedConfig);
  };
  reject_config([](auto& sc) { sc.nativeVertex = true; });
  reject_config([](auto& sc) { sc.lineMode = 1; });
  reject_config([](auto& sc) { sc.pbrBindPos = true; });
  reject_config([](auto& sc) { sc.attrs[GX_VA_POS].attrType = GX_NONE; });
  reject_config([](auto& sc) { sc.attrs[GX_VA_POS].attrType = 0xff; });
  reject_config([](auto& sc) { sc.attrs[GX_VA_POS].compType = 0xff; });
  reject_config([](auto& sc) { sc.attrs[GX_VA_POS].cnt = 1; });
  reject_config([](auto& sc) { sc.attrs[GX_VA_POS].frac = 32; });
  reject_config([](auto& sc) { sc.attrs[GX_VA_NBT] = sc.attrs[GX_VA_POS]; });
  reject_config([](auto& sc) { sc.attrs[GX_POS_MTX_ARRAY] = sc.attrs[GX_VA_POS]; });
  reject_config([](auto& sc) { sc.attrs[GX_VA_NRM] = sc.attrs[GX_VA_POS]; sc.attrs[GX_VA_NRM].nbt3 = true; });
  for (uint8_t count : {uint8_t{2}, uint8_t{9}, uint8_t{15}}) {
    reject_config([count](auto& sc) { sc.attrs[GX_VA_NRM] = sc.attrs[GX_VA_POS]; sc.attrs[GX_VA_NRM].cnt = count; });
  }
  reject_config([](auto& sc) { sc.attrs[GX_VA_TEX7] = sc.attrs[GX_VA_POS]; }); // UV cnt=3.
  reject_config([](auto& sc) { sc.attrs[GX_VA_CLR0] = {.attrType = GX_DIRECT, .cnt = 1, .compType = 0xff}; });
  reject_config([](auto& sc) { sc.attrs[GX_VA_CLR1] = {.attrType = GX_DIRECT, .cnt = 4, .compType = GX_RGBA8}; });
  reject_config([](auto& sc) { sc.attrs[GX_VA_TEX7MTXIDX] = {.attrType = GX_INDEX8, .cnt = 1}; });
  reject_config([](auto& sc) { sc.attrs[GX_VA_PNMTXIDX] = {.attrType = GX_DIRECT, .cnt = 2}; });
  reject_config([](auto& sc) { sc.tcgs[0].type = GX_TG_BUMP0; });
  reject_config([](auto& sc) { sc.tcgs[0].src = GX_TG_BINRM; });

  Fixture f;
  f.repeat({1, 2, 3}, 3);
  for (auto primitive : {GX_LINES, GX_LINESTRIP, GX_POINTS, static_cast<GXPrimitive>(0x88)}) {
    expect_failure(f, DecodeFailure::UnsupportedPrimitive, primitive);
  }
  f.raw.pop_back();
  expect_failure(f, DecodeFailure::VertexDataOutOfBounds);
  f.raw.resize(30, 0); // An offset crossing a record is invalid even if the full span contains it.
  f.config.attrs[GX_VA_POS].offset = 1;
  expect_failure(f, DecodeFailure::InvalidVertexLayout);
  f.config.attrs[GX_VA_POS].offset = 0;
  f.config.vtxStride = 0;
  expect_failure(f, DecodeFailure::InvalidVertexLayout);
  f.config.vtxStride = 3;
  f.config.attrs[GX_VA_PNMTXIDX] = {.attrType = GX_DIRECT, .cnt = 1, .offset = 3};
  expect_failure(f, DecodeFailure::InvalidVertexLayout);
  f.config.attrs[GX_VA_PNMTXIDX] = {};
  f.config.attrs[GX_VA_POS] = {.attrType = GX_INDEX16, .cnt = 3, .compType = GX_U8, .offset = 2};
  expect_failure(f, DecodeFailure::InvalidVertexLayout);
  expect_failure(f, DecodeFailure::ScratchBudgetExceeded, GX_TRIANGLESTRIP, 0xffffffffu);
}

void indexed_bounds_and_unused_uv() {
  Fixture f;
  f.config.vtxStride = 2;
  f.config.attrs[GX_VA_POS] = {.attrType = GX_INDEX16, .cnt = 3, .compType = GX_F32,
                             .stride = 13, .le = false};
  f.repeat({0, 3}, 3);
  expect_failure(f, DecodeFailure::MissingImmutableSource);
  std::vector<uint8_t> source(100, 0);
  f.arrays[GX_VA_POS] = snapshot(source, immutable::SourceKind::DisplayList);
  expect_failure(f, DecodeFailure::MissingImmutableSource);
  f.bases[GX_VA_POS] = 2;
  f.arrays[GX_VA_POS] = snapshot(source, immutable::SourceKind::Array, 76);
  expect_failure(f, DecodeFailure::ArrayDataOutOfBounds); // (2+3)*13 + 12 = 77, not original_size=100.
  f.arrays[GX_VA_POS] = snapshot(source, immutable::SourceKind::Array, 77);
  expect_values(f.run(), f.config, GX_VA_POS, {0.f, 0.f, 0.f});
  f.bases[GX_VA_POS] = 0xffffffffu;
  expect_failure(f, DecodeFailure::ArrayDataOutOfBounds); // No base/index/stride wraparound.
  f.bases[GX_VA_POS] = 0;
  f.raw[1] = 255;
  expect_failure(f, DecodeFailure::ArrayDataOutOfBounds);
  f.arrays[GX_VA_POS] = snapshot(std::array<uint8_t, 11>{});
  f.repeat({0, 0}, 3);
  expect_failure(f, DecodeFailure::ArrayDataOutOfBounds);

  Fixture uv;
  uv.config.vtxStride = 5;
  uv.config.attrs[GX_VA_TEX7] = {.attrType = GX_INDEX16, .cnt = 2, .compType = GX_S16,
                              .offset = 3, .stride = 4, .frac = 8, .le = true};
  uv.arrays[GX_VA_TEX7] = snapshot(std::array<uint8_t, 4>{0, 1, 0, 2});
  uv.repeat({1, 2, 3, 0, 0}, 3);
  expect_values(uv.run(), uv.config, GX_VA_TEX7, {1.f, 2.f});
  uv.raw.back() = 1; // Retail can have an unknown/unused OOB UV. Reject the whole draw.
  expect_failure(uv, DecodeFailure::ArrayDataOutOfBounds);
  uv.arrays[GX_VA_TEX7] = {};
  expect_failure(uv, DecodeFailure::MissingImmutableSource);
}

void nonfinite_and_float_bits() {
  for (uint32_t bits : {0x7fc12345u, 0x7f800001u, 0xffc00001u, 0x7f800000u, 0xff800000u}) {
    for (auto mode : {GX_DIRECT, GX_INDEX8, GX_INDEX16}) {
      for (bool le : {false, true}) {
        Fixture f;
        f.config.vtxStride = mode == GX_DIRECT ? 12 : 2;
        f.config.attrs[GX_VA_POS] = {.attrType = uint8_t(mode), .cnt = 3, .compType = GX_F32,
                                   .stride = 12, .le = le};
        f.repeat(std::vector<uint8_t>(f.config.vtxStride, 0), 3);
        if (mode == GX_DIRECT) {
          put(f.raw, 2 * 12 + 8, bits, 4);
        } else {
          std::vector<uint8_t> source(24, 0);
          put(source, 12 + 8, bits, 4, le);
          f.arrays[GX_VA_POS] = snapshot(source);
          put(f.raw, 4, 1, mode == GX_INDEX8 ? 1 : 2);
        }
        expect_failure(f, DecodeFailure::NonFiniteValue);
      }
    }
  }
  Fixture f;
  f.config.vtxStride = 12;
  f.config.attrs[GX_VA_POS].compType = GX_F32;
  const std::array<uint32_t, 3> bits{1, 0x7f7fffff, 0x80000000};
  std::vector<uint8_t> record(12);
  for (unsigned i = 0; i < 3; ++i) {
    put(record, i * 4, bits[i], 4);
  }
  f.repeat(record, 3);
  const auto actual = values(f.run(), f.config, GX_VA_POS);
  for (unsigned i = 0; i < 3; ++i) {
    CHECK_EQ(std::bit_cast<uint32_t>(actual[i]), bits[i]);
  }
  f.config.vtxStride = 16;
  f.config.attrs[GX_VA_TEX0] = {.attrType = GX_DIRECT, .cnt = 1, .compType = GX_F32, .offset = 12};
  record.resize(16);
  put(record, 12, 0x7f800000, 4);
  f.repeat(record, 3);
  expect_failure(f, DecodeFailure::NonFiniteValue); // Even a currently unreferenced attribute.
}

void uint16_and_scratch_limits() {
  Fixture f;
  for (uint32_t i = 0; i < MaxLocalVertices; ++i) {
    f.raw.insert(f.raw.end(), {uint8_t(i >> 16), uint8_t(i >> 8), uint8_t(i)});
  }
  auto mesh = f.run(GX_TRIANGLESTRIP);
  CHECK_EQ(mesh.vertexCount, MaxLocalVertices);
  CHECK_EQ(mesh.indices.size(), size_t{MaxLocalVertices - 2} * 3); // No u16 index-count truncation.
  CHECK_EQ(mesh.indices.back(), 65535u);
  expect_values(mesh, f.config, GX_VA_POS, {0.f, 255.f, 255.f}, 65535);
  f.raw.insert(f.raw.end(), {0, 0, 0, 0, 0, 1, 0, 0, 2});
  mesh = f.run(GX_TRIANGLEFAN); // More input vertices are legal when tuples reuse local Uint16 IDs.
  CHECK_EQ(mesh.vertexCount, MaxLocalVertices);
  CHECK_EQ(mesh.indices.back(), 2u);
  f.raw.resize(size_t{MaxLocalVertices} * 3);
  f.raw.insert(f.raw.end(), {1, 0, 0});
  expect_failure(f, DecodeFailure::VertexLimitExceeded, GX_TRIANGLESTRIP, MaxLocalVertices + 1);

  auto full = full_fixture();
  constexpr uint32_t acceptedCount = 1000000;
  std::vector<uint8_t> record(full.config.vtxStride, 0);
  full.repeat(record, acceptedCount);
  CpuMesh large;
  DecodeFailure failure{};
  allocation_probe::start();
  const bool accepted = decode(full.input(GX_TRIANGLESTRIP, acceptedCount), large, failure);
  allocation_probe::stop();
  REQUIRE(accepted);
  CHECK_EQ(failure, DecodeFailure::None);
  CHECK_EQ(large.vertexCount, 1u);
  CHECK_EQ(large.vertices.size(), MaxStride);
  CHECK_EQ(large.indices.size(), size_t{acceptedCount - 2} * 3);
  CHECK_EQ(large.indices.front(), 0u);
  CHECK_EQ(large.indices.back(), 0u);
  CHECK_EQ(allocation_probe::calls, 3u); // Fixed table + worst-case vertex output + index output.
  CHECK(allocation_probe::bytes <= ScratchBudgetBytes);
  CHECK(allocation_probe::bytes > ScratchBudgetBytes - 65536);
  CHECK_EQ(large.vertices.capacity(), size_t{MaxLocalVertices} * MaxStride);

  // Output alone fits 16 MiB, but output + tuple table does not. Reject before
  // allocating or touching the deliberately shorter input, not after dedup.
  auto unchanged = sentinel();
  allocation_probe::start();
  const bool overBudget = decode(full.input(GX_TRIANGLESTRIP, 1005000), unchanged, failure);
  allocation_probe::stop();
  CHECK(!overBudget);
  CHECK_EQ(failure, DecodeFailure::ScratchBudgetExceeded);
  CHECK_EQ(allocation_probe::calls, 0u);
  expect_unchanged(unchanged);

  Fixture small;
  small.repeat({1, 2, 3}, 3);
  for (int site = 0; site < 3; ++site) {
    auto out = sentinel();
    allocation_probe::start(site);
    const bool okay = decode(small.input(GX_TRIANGLES, 3), out, failure);
    allocation_probe::stop();
    CHECK(!okay);
    CHECK_EQ(failure, DecodeFailure::AllocationFailed);
    expect_unchanged(out);
  }
}

} // namespace

int main() {
  direct_offsets_and_zero_extension();
  numeric_endian_matrix();
  retail_mixed_arrays();
  packed_color_matrix();
  full_tuple_and_selectors();
  raw_tuple_collision_and_identity();
  primitive_orientation();
  unsupported_and_record_bounds();
  indexed_bounds_and_unused_uv();
  nonfinite_and_float_bits();
  uint16_and_scratch_limits();
  CHECK(immutable::registries_empty());
  CHECK_EQ(immutable::retained_bytes(), 0u);
  if (port_test::Failures() == 0) {
    std::puts("h700_decoded_cpu_test: all decoder value/bounds/limit checks passed");
  }
  return PORT_TEST_RESULT();
}
