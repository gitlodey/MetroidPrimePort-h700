#include "decoded_cpu.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>

namespace aurora::gx::decoded {
namespace {

struct TupleEntry {
  uint64_t hash = 0;
  uint32_t rawVertex = std::numeric_limits<uint32_t>::max();
  uint32_t localVertex = 0;
};

struct FieldPlan {
  const AttrConfig* attr = nullptr;
  std::span<const uint8_t> source;
  uint64_t base = 0;
  uint32_t elementBytes = 0;
  float scale = 1.f;
  bool color = false;
};

// Leave room for the fixed-size layout/attribute plan and scalar working state,
// as well as the explicitly accounted map and both worst-case output allocations.
constexpr uint64_t FixedScratchBytes = 4096;
static_assert(sizeof(Layout) + sizeof(std::array<FieldPlan, MaxAttributes>) + sizeof(CpuMesh) +
                  sizeof(std::array<uint16_t, 4>) + sizeof(std::array<float, 4>) + 512 <
              FixedScratchBytes);

uint32_t element_bytes(const AttrConfig& attr, bool color) noexcept {
  if (color) {
    switch (attr.compType) {
    case GX_RGB565:
    case GX_RGBA4: return 2;
    case GX_RGB8:
    case GX_RGBA6: return 3;
    case GX_RGBX8:
    case GX_RGBA8: return 4;
    default: return 0;
    }
  }
  switch (attr.compType) {
  case GX_U8:
  case GX_S8: return attr.cnt;
  case GX_U16:
  case GX_S16: return uint32_t{attr.cnt} * 2;
  case GX_F32: return uint32_t{attr.cnt} * 4;
  default: return 0;
  }
}

// Byte loads also handle unaligned and tightly packed array elements without
// reading padding/a following word, unlike the shader's word transport helpers.
uint32_t load_integer(const uint8_t* bytes, uint32_t size, bool le) noexcept {
  uint32_t value = 0;
  for (uint32_t i = 0; i < size; ++i) {
    value |= uint32_t{bytes[i]} << ((le ? i : size - 1 - i) * 8);
  }
  return value;
}

std::array<float, 4> load_color(const uint8_t* bytes, uint8_t type, bool le) noexcept {
  switch (type) {
  case GX_RGB565: {
    const uint32_t value = load_integer(bytes, 2, le);
    return {float((value >> 11) & 31) / 31.f, float((value >> 5) & 63) / 63.f,
            float(value & 31) / 31.f, 1.f};
  }
  case GX_RGBA4: {
    const uint32_t value = load_integer(bytes, 2, le);
    return {float((value >> 12) & 15) / 15.f, float((value >> 8) & 15) / 15.f,
            float((value >> 4) & 15) / 15.f, float(value & 15) / 15.f};
  }
  case GX_RGBA6: {
    const uint32_t value = load_integer(bytes, 3, le);
    return {float((value >> 18) & 63) / 63.f, float((value >> 12) & 63) / 63.f,
            float((value >> 6) & 63) / 63.f, float(value & 63) / 63.f};
  }
  default: // RGB8/RGBX8/RGBA8 are byte channels: endianness does not reorder them.
    return {float(bytes[0]) / 255.f, float(bytes[1]) / 255.f, float(bytes[2]) / 255.f,
            type == GX_RGBA8 ? float(bytes[3]) / 255.f : 1.f};
  }
}

bool load_numeric(const uint8_t* bytes, const AttrConfig& attr, float scale, bool le,
                  std::array<float, 4>& value) noexcept {
  for (uint32_t i = 0; i < attr.cnt; ++i) {
    switch (attr.compType) {
    case GX_U8: value[i] = float(bytes[i]) * scale; break;
    case GX_S8: {
      const int32_t raw = bytes[i];
      value[i] = float(raw >= 128 ? raw - 256 : raw) * scale;
      break;
    }
    case GX_U16: value[i] = float(load_integer(bytes + i * 2, 2, le)) * scale; break;
    case GX_S16: {
      const int32_t raw = static_cast<int32_t>(load_integer(bytes + i * 2, 2, le));
      value[i] = float(raw >= 32768 ? raw - 65536 : raw) * scale;
      break;
    }
    case GX_F32: {
      const uint32_t bits = load_integer(bytes + i * 4, 4, le);
      // Check the representation, so even fast-math cannot discard this guard.
      if ((bits & 0x7f800000u) == 0x7f800000u) {
        return false;
      }
      value[i] = std::bit_cast<float>(bits);
      break;
    }
    default: return false; // Rejected during planning, before any allocation.
    }
  }
  return true;
}

uint64_t tuple_hash(const uint8_t* bytes, uint32_t size) noexcept {
  uint64_t hash = 14695981039346656037ull;
  for (uint32_t i = 0; i < size; ++i) {
    hash = (hash ^ bytes[i]) * 1099511628211ull;
  }
  return hash;
}

} // namespace

bool decode(const DecodeInput& input, CpuMesh& out, DecodeFailure& failure) noexcept {
  failure = DecodeFailure::None;
  const auto reject = [&failure](DecodeFailure reason) noexcept {
    failure = reason;
    return false;
  };

  uint64_t indexCount = 0;
  switch (input.primitive) {
  case GX_TRIANGLES: indexCount = uint64_t{input.vertexCount / 3} * 3; break;
  case GX_QUADS: indexCount = uint64_t{input.vertexCount / 4} * 6; break;
  case GX_TRIANGLESTRIP:
  case GX_TRIANGLEFAN:
    indexCount = input.vertexCount < 3 ? 0 : uint64_t{input.vertexCount - 2} * 3;
    break;
  default: return reject(DecodeFailure::UnsupportedPrimitive);
  }

  const auto& config = input.config;
  const Layout layout = make_layout(config);
  if (!layout.valid || layout.stride == 0 || layout.stride > MaxStride ||
      layout.attributeCount > MaxAttributes) {
    return reject(DecodeFailure::UnsupportedConfig);
  }
  if (config.vtxStride == 0) {
    return reject(DecodeFailure::InvalidVertexLayout);
  }

  const uint32_t maxUnique = std::min(input.vertexCount, MaxLocalVertices);
  const uint32_t mapCapacity = maxUnique == 0 ? 0 : std::bit_ceil(maxUnique * 2);
  const uint64_t vertexBytes = uint64_t{maxUnique} * layout.stride;
  const uint64_t scratchBytes = FixedScratchBytes + uint64_t{mapCapacity} * sizeof(TupleEntry) +
                               vertexBytes + indexCount * sizeof(uint16_t);
  if (scratchBytes > ScratchBudgetBytes) {
    return reject(DecodeFailure::ScratchBudgetExceeded);
  }
  if (uint64_t{input.vertexCount} * config.vtxStride > input.vertices.size()) {
    return reject(DecodeFailure::VertexDataOutOfBounds);
  }

  std::array<FieldPlan, MaxAttributes> plans{};
  for (uint32_t i = 0; i < layout.attributeCount; ++i) {
    const auto& field = layout.fields[i];
    if (field.attr == GX_VA_NULL) {
      for (const auto& selector : Selectors) {
        if (layout.has_selector(selector.attr) &&
            uint32_t{config.attrs[selector.attr].offset} + 1 > config.vtxStride) {
          return reject(DecodeFailure::InvalidVertexLayout);
        }
      }
      continue;
    }
    auto& plan = plans[i];
    plan.attr = &config.attrs[field.attr];
    plan.color = field.attr == GX_VA_CLR0 || field.attr == GX_VA_CLR1;
    plan.elementBytes = element_bytes(*plan.attr, plan.color);
    if (plan.elementBytes == 0) {
      return reject(DecodeFailure::UnsupportedConfig);
    }
    if (!plan.color) {
      plan.scale = 1.f / float(uint32_t{1} << plan.attr->frac);
    }
    const bool direct = plan.attr->attrType == GX_DIRECT;
    const uint32_t recordBytes = direct ? plan.elementBytes : plan.attr->attrType == GX_INDEX8 ? 1 : 2;
    if (uint32_t{plan.attr->offset} + recordBytes > config.vtxStride) {
      return reject(DecodeFailure::InvalidVertexLayout);
    }
    if (!direct && input.vertexCount != 0) {
      const auto& source = input.arrays[field.attr];
      if (!source || source.kind() != immutable::SourceKind::Array) {
        return reject(DecodeFailure::MissingImmutableSource);
      }
      plan.source = source.bytes();
      plan.base = uint64_t{input.baseIndices[field.attr]} * plan.attr->stride;
    }
  }

  try {
    CpuMesh mesh;
    if (input.vertexCount != 0) {
      // Exact, one-time sizes: no rehash, remap vector, push_back growth or
      // shrink_to_fit allocation can escape the preflight bound.
      auto tuples = std::make_unique<TupleEntry[]>(mapCapacity);
      mesh.vertices.resize(static_cast<size_t>(vertexBytes));
      mesh.indices.resize(static_cast<size_t>(indexCount));
      uint32_t uniqueCount = 0;
      size_t indexCursor = 0;
      std::array<uint16_t, 4> group{};
      uint16_t first = 0, prev2 = 0, prev1 = 0;
      const auto emit = [&](uint16_t a, uint16_t b, uint16_t c) noexcept {
        mesh.indices[indexCursor++] = a;
        mesh.indices[indexCursor++] = b;
        mesh.indices[indexCursor++] = c;
      };
      for (uint32_t v = 0; v < input.vertexCount; ++v) {
        const auto* record = input.vertices.data() + size_t{v} * config.vtxStride;
        const uint64_t hash = tuple_hash(record, config.vtxStride);
        uint32_t slot = static_cast<uint32_t>(hash ^ (hash >> 32)) & (mapCapacity - 1);
        // At most 1/2 full, even when the Uint16 unique-vertex limit is reached.
        while (tuples[slot].rawVertex != std::numeric_limits<uint32_t>::max()) {
          const auto& entry = tuples[slot];
          if (entry.hash == hash &&
              std::memcmp(record, input.vertices.data() + size_t{entry.rawVertex} * config.vtxStride,
                          config.vtxStride) == 0) {
            break;
          }
          slot = (slot + 1) & (mapCapacity - 1);
        }
        auto& entry = tuples[slot];
        if (entry.rawVertex == std::numeric_limits<uint32_t>::max()) {
          if (uniqueCount == MaxLocalVertices) {
            return reject(DecodeFailure::VertexLimitExceeded);
          }
          auto* destination = mesh.vertices.data() + size_t{uniqueCount} * layout.stride;
          for (uint32_t i = 0; i < layout.attributeCount; ++i) {
            const auto& field = layout.fields[i];
            if (field.attr == GX_VA_NULL) {
              std::array<uint32_t, 3> words{};
              for (const auto& selector : Selectors) {
                if (layout.has_selector(selector.attr)) {
                  words[selector.word] |= uint32_t{record[config.attrs[selector.attr].offset]} << selector.shift;
                }
              }
              std::memcpy(destination + field.offset, words.data(), field.size());
              continue;
            }
            const auto& plan = plans[i];
            const auto& attr = *plan.attr;
            const auto* bytes = record + attr.offset;
            const bool direct = attr.attrType == GX_DIRECT;
            if (!direct) {
              // Indices are always BE, independent of the array's endianness.
              const uint32_t index = load_integer(bytes, attr.attrType == GX_INDEX8 ? 1 : 2, false);
              const uint64_t offset = plan.base + uint64_t{index} * attr.stride;
              if (offset > plan.source.size() || plan.elementBytes > plan.source.size() - offset) {
                return reject(DecodeFailure::ArrayDataOutOfBounds);
              }
              bytes = plan.source.data() + static_cast<size_t>(offset);
            }
            std::array<float, 4> value{};
            if (plan.color) {
              value = load_color(bytes, attr.compType, !direct && attr.le);
            } else if (!load_numeric(bytes, attr, plan.scale, !direct && attr.le, value)) {
              return reject(DecodeFailure::NonFiniteValue);
            }
            std::memcpy(destination + field.offset, value.data(), field.size());
          }
          entry = {hash, v, uniqueCount++};
        }
        const auto local = static_cast<uint16_t>(entry.localVertex);
        switch (input.primitive) {
        case GX_TRIANGLES:
          group[v % 3] = local;
          if (v % 3 == 2) {
            emit(group[0], group[1], group[2]);
          }
          break;
        case GX_QUADS:
          group[v & 3] = local;
          if ((v & 3) == 3) {
            emit(group[0], group[1], group[2]);
            emit(group[2], group[3], group[0]);
          }
          break;
        case GX_TRIANGLESTRIP:
          if (v >= 2) {
            emit((v & 1) ? prev1 : prev2, (v & 1) ? prev2 : prev1, local);
          }
          break;
        case GX_TRIANGLEFAN:
          if (v >= 2) {
            emit(first, prev1, local);
          }
          break;
        default: break; // Primitive preflight already rejected every other value.
        }
        if (v == 0) {
          first = local;
        }
        prev2 = prev1;
        prev1 = local;
      }
      mesh.vertexCount = uniqueCount;
      mesh.vertices.resize(size_t{uniqueCount} * layout.stride);
    }
    out.vertices.swap(mesh.vertices);
    out.indices.swap(mesh.indices);
    out.vertexCount = mesh.vertexCount;
    return true;
  } catch (...) {
    return reject(DecodeFailure::AllocationFailed);
  }
}

} // namespace aurora::gx::decoded
