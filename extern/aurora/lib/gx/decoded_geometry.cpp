#include "decoded_geometry.hpp"
#include "decoded_budget.hpp"
#include "../gfx/recording.hpp"
#include "../gfx/frame.hpp"
#include "../gfx/resources.hpp"
#include "../webgpu/gpu.hpp"

#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace aurora::gx::decoded {
static constexpr Module Log{"aurora::gx::decoded"};

bool enabled() noexcept {
#if defined(AURORA_H700) && !defined(AURORA_FIFO_TESTING)
  static const bool value = requested(std::getenv("MP_H700_RETAIL_GEOMETRY"), std::getenv("MP_H700_BENCH"));
  return value;
#else
  return false;
#endif
}

#if defined(AURORA_H700) && !defined(AURORA_FIFO_TESTING)
namespace {
struct Key {
  immutable::SourceId list = 0;
  uint32_t offset = 0;
  uint32_t count = 0;
  GXPrimitive primitive{};
  uint8_t stride = 0;
  std::array<AttrConfig, MaxVtxAttr> attrs{};
  std::array<immutable::SourceId, MaxVtxAttr> arrays{};
  std::array<uint32_t, MaxVtxAttr> sizes{};
  std::array<uint32_t, MaxVtxAttr> bases{};

  bool operator==(const Key& rhs) const noexcept {
    return list == rhs.list && offset == rhs.offset && count == rhs.count && primitive == rhs.primitive &&
           stride == rhs.stride && std::memcmp(attrs.data(), rhs.attrs.data(), sizeof(attrs)) == 0 &&
           arrays == rhs.arrays && sizes == rhs.sizes && bases == rhs.bases;
  }
  template <typename H> friend H AbslHashValue(H h, const Key& key) {
    h = H::combine(std::move(h), key.list, key.offset, key.count, key.primitive, key.stride);
    h = H::combine_contiguous(std::move(h), reinterpret_cast<const uint8_t*>(key.attrs.data()), sizeof(key.attrs));
    return H::combine(std::move(h), key.arrays, key.sizes, key.bases);
  }
  bool uses(immutable::SourceId id) const noexcept {
    return list == id || std::find(arrays.begin(), arrays.end(), id) != arrays.end();
  }
};

RetainedBudget sBudget{GpuBudgetBytes};
struct Mesh {
  // Declared first, hence destroyed last, after both actual buffer references.
  RetainedBudget::Reservation charge;
  wgpu::Buffer vertex;
  wgpu::Buffer index;
  uint64_t vertexBytes = 0;
  uint64_t indexBytes = 0;
  uint32_t vertexCount = 0;
  uint32_t indexCount = 0;
  uint64_t lastPinFrame = UINT64_MAX; // FIFO only
  MeshRef ref{};
  explicit Mesh(RetainedBudget::Reservation reservation) : charge(std::move(reservation)) {}
};
struct Slot {
  std::weak_ptr<Mesh> mesh;
  uint32_t generation = 0;
};
struct Entry {
  std::shared_ptr<Mesh> mesh; // null: stable decode rejection for this exact immutable tuple
  uint64_t lastUse = 0;
};
std::mutex sMutex;
absl::flat_hash_map<Key, Entry> sCache;
std::vector<Slot> sSlots;
uint64_t sClock = 0;
uint64_t sHits = 0, sMisses = 0, sFallbacks = 0, sUploads = 0, sUploadedBytes = 0;
uint32_t sReportFrame = UINT32_MAX;

void report() {
  const uint32_t frame = gfx::current_frame();
  if (sReportFrame != UINT32_MAX && uint32_t(frame - sReportFrame) < 120) return;
  sReportFrame = frame;
  Log.info("H700 retail geometry: hits={} misses={} fallback={} mesh_uploads={} upload_bytes={} retained_gpu_bytes={} "
           "retained_source_bytes={} entries={}", sHits, sMisses, sFallbacks, sUploads, sUploadedBytes,
           sBudget.used(), immutable::retained_bytes(), sCache.size());
}

void evict_one() {
  auto oldest = sCache.end();
  for (auto it = sCache.begin(); it != sCache.end(); ++it) {
    // A pin owns the allocation after eviction too. Prefer entries which can actually free bytes.
    if (it->second.mesh && it->second.mesh.use_count() != 1) continue;
    if (oldest == sCache.end() || it->second.lastUse < oldest->second.lastUse) oldest = it;
  }
  if (oldest != sCache.end()) sCache.erase(oldest);
}

std::shared_ptr<Mesh> resolve(MeshRef ref) {
  std::lock_guard lock{sMutex};
  if (ref.slot >= sSlots.size() || ref.generation == 0 || sSlots[ref.slot].generation != ref.generation) return {};
  return sSlots[ref.slot].mesh.lock();
}

wgpu::Buffer make_buffer(const void* data, uint64_t size, wgpu::BufferUsage usage, const char* label) {
  const uint64_t aligned = (size + 3) & ~uint64_t{3};
  const wgpu::BufferDescriptor descriptor{.label = label, .usage = usage, .size = aligned, .mappedAtCreation = true};
  auto buffer = webgpu::g_device.CreateBuffer(&descriptor);
  if (!buffer) return {};
  void* const mapped = buffer.GetMappedRange(0, aligned);
  if (!mapped) return {};
  std::memcpy(mapped, data, size);
  if (aligned != size) std::memset(static_cast<uint8_t*>(mapped) + size, 0, aligned - size);
  buffer.Unmap();
  return buffer;
}
} // namespace

std::optional<Admission> acquire(const immutable::SourceView& list, uint32_t vertexOffset,
                                  const DecodeInput& input) noexcept {
  if (!enabled() || !gfx::detail::geometry_pins_supported()) return std::nullopt;
  const auto fail = []() -> std::optional<Admission> { ++sFallbacks; report(); return std::nullopt; };
  if (!list || !immutable::active(list.id()) || !decoded_native_eligible(input.config) ||
      !limits_valid(make_layout(input.config), gfx::detail::resources().limits)) return fail();
  if (vertexOffset > list.size() || input.vertices.size() > list.size() - vertexOffset ||
      input.vertices.data() != list.data() + vertexOffset) return fail();
  Key key;
  key.list = list.id();
  key.offset = vertexOffset;
  key.count = input.vertexCount;
  key.primitive = input.primitive;
  key.stride = input.config.vtxStride;
  key.attrs = input.config.attrs;
  for (uint32_t attr = 0; attr < MaxVtxAttr; ++attr) {
    const auto type = key.attrs[attr].attrType;
    if (type != GX_INDEX8 && type != GX_INDEX16) continue;
    const auto& source = input.arrays[attr];
    if (!source || !immutable::active(source.id())) return fail();
    key.arrays[attr] = source.id();
    key.sizes[attr] = source.size();
    key.bases[attr] = input.baseIndices[attr];
  }
  try {
    std::unique_lock lock{sMutex};
    if (auto it = sCache.find(key); it != sCache.end()) {
      it->second.lastUse = ++sClock;
      auto mesh = it->second.mesh;
      lock.unlock();
      if (!mesh) return fail();
      if (!gfx::detail::retain_geometry(mesh, mesh->lastPinFrame)) return fail();
      ++sHits;
      report();
      return Admission{mesh->ref, mesh->vertexCount, mesh->indexCount};
    }
    ++sMisses;
    if (sCache.size() == MaxCacheEntries) evict_one();
    if (sCache.size() == MaxCacheEntries) { lock.unlock(); return fail(); }
    // Decoding and Dawn allocation never run with the registry mutex held: the worker
    // must be able to resolve an earlier packet while this FIFO thread prepares a miss.
    lock.unlock();
    CpuMesh cpu;
    DecodeFailure failure;
    if (!decode(input, cpu, failure)) {
      if (failure == DecodeFailure::AllocationFailed) return fail();
      lock.lock();
      sCache.emplace(key, Entry{nullptr, ++sClock});
      lock.unlock();
      if (sFallbacks < 8) Log.info("H700 retail geometry decode fallback: reason={} list={} offset={}",
                                  static_cast<uint32_t>(failure), list.id(), vertexOffset);
      return fail();
    }
    const uint64_t vertexBytes = cpu.vertices.size();
    const uint64_t indexBytes = cpu.indices.size() * sizeof(uint16_t);
    const uint64_t bytes = ((vertexBytes + 3) & ~uint64_t{3}) + ((indexBytes + 3) & ~uint64_t{3});
    const auto maxBuffer = gfx::detail::resources().limits.maxBufferSize;
    if (vertexBytes == 0 || indexBytes == 0 || maxBuffer == WGPU_LIMIT_U64_UNDEFINED ||
        vertexBytes > maxBuffer || ((indexBytes + 3) & ~uint64_t{3}) > maxBuffer) return fail();
    auto reservation = sBudget.reserve(bytes);
    if (!reservation) {
      lock.lock();
      for (size_t tries = sCache.size(); tries != 0 && !reservation; --tries) {
        const auto before = sCache.size();
        evict_one();
        if (before == sCache.size()) break;
        reservation = sBudget.reserve(bytes);
      }
      lock.unlock();
    }
    if (!reservation) return fail();
    auto mesh = std::make_shared<Mesh>(std::move(*reservation));
    mesh->vertex = make_buffer(cpu.vertices.data(), vertexBytes, wgpu::BufferUsage::Vertex, "PERF12 retail VB");
    mesh->index = make_buffer(cpu.indices.data(), indexBytes, wgpu::BufferUsage::Index, "PERF12 retail IB");
    if (!mesh->vertex || !mesh->index) return fail();
    mesh->vertexBytes = vertexBytes;
    mesh->indexBytes = indexBytes;
    mesh->vertexCount = cpu.vertexCount;
    mesh->indexCount = static_cast<uint32_t>(cpu.indices.size());
    lock.lock();
    uint32_t slot = 0;
    for (; slot < sSlots.size(); ++slot) {
      if (sSlots[slot].mesh.expired() && sSlots[slot].generation != UINT32_MAX) break;
    }
    if (slot == sSlots.size()) {
      if (sSlots.size() == MaxCacheEntries) { lock.unlock(); return fail(); }
      sSlots.emplace_back();
    }
    mesh->ref = {slot, ++sSlots[slot].generation};
    sSlots[slot].mesh = mesh;
    sCache.emplace(std::move(key), Entry{mesh, ++sClock});
    lock.unlock();
    if (!gfx::detail::retain_geometry(mesh, mesh->lastPinFrame)) return fail();
    ++sUploads;
    sUploadedBytes += bytes;
    report();
    return Admission{mesh->ref, mesh->vertexCount, mesh->indexCount};
  } catch (const std::bad_alloc&) {
    return fail();
  }
}

void invalidate_source(immutable::SourceId id) noexcept {
  std::lock_guard lock{sMutex};
  for (auto it = sCache.begin(); it != sCache.end();) {
    if (it->first.uses(id)) sCache.erase(it++);
    else ++it;
  }
}

bool valid(MeshRef ref) noexcept { return bool(resolve(ref)); }
void bind(MeshRef ref, const wgpu::RenderPassEncoder& pass) {
  const auto mesh = resolve(ref);
  AURORA_ASSERT(mesh && mesh->vertex && mesh->index, "Stale PERF12 recorded geometry {}/{}", ref.slot, ref.generation);
  pass.SetVertexBuffer(0, mesh->vertex, 0, mesh->vertexBytes);
  pass.SetIndexBuffer(mesh->index, wgpu::IndexFormat::Uint16, 0, mesh->indexBytes);
}

void shutdown() noexcept {
  std::lock_guard lock{sMutex};
  sCache.clear();
  // Generations and budget charges are not reset underneath surviving holders.
  for (const auto& slot : sSlots) AURORA_ASSERT(slot.mesh.expired(), "PERF12 holder survived guarded release");
  sSlots.clear();
  AURORA_ASSERT(sBudget.used() == 0, "PERF12 GPU budget survived guarded release");
}
#else
std::optional<Admission> acquire(const immutable::SourceView&, uint32_t, const DecodeInput&) noexcept { return std::nullopt; }
void invalidate_source(immutable::SourceId) noexcept {}
bool valid(MeshRef) noexcept { return false; }
void bind(MeshRef, const wgpu::RenderPassEncoder&) { AURORA_ASSERT(false, "Decoded geometry requires the H700 native path"); }
void shutdown() noexcept {}
#endif

} // namespace aurora::gx::decoded
