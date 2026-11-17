#ifndef METROID_PRIME_PORT_PORT_H700_SKINNING_H
#define METROID_PRIME_PORT_PORT_H700_SKINNING_H

#include <climits>
#include <cstddef>
#include <cstdint>

namespace PortH700 {

struct SkinLayout {
  unsigned vertices = 0;
  unsigned normals = 0;
  unsigned alignedVertices = 0;
  unsigned alignedNormals = 0;
  unsigned total = 0;
};

// Byte counts also reach signed legacy skin/cache cursors. Validate before any
// signed multiplication, alignment, narrowing or publication of a workspace.
inline bool MakeSkinLayout(int points, int normals, unsigned normalStride, SkinLayout& out) noexcept {
  if (points < 0 || normals < 0 || normalStride == 0) return false;
  const auto vertices = std::uint64_t(points) * 12;
  const auto normalBytes = std::uint64_t(normals) * normalStride;
  const auto alignedVertices = (vertices + 31) & ~std::uint64_t(31);
  const auto alignedNormals = (normalBytes + 31) & ~std::uint64_t(31);
  const auto total = alignedVertices + alignedNormals;
  if (total > INT_MAX) return false;
  out = {unsigned(vertices), unsigned(normalBytes), unsigned(alignedVertices),
         unsigned(alignedNormals), unsigned(total)};
  return true;
}

// This limit belongs only to the ring; heap/external workspaces use SkinLayout.
inline bool SkinRingSize(std::size_t bytes, std::size_t capacity, int& out) noexcept {
  if (bytes == 0 || bytes > INT_MAX) return false;
  const auto aligned = (std::uint64_t(bytes) + 31) & ~std::uint64_t(31);
  if (aligned > capacity || aligned > INT_MAX) return false;
  out = int(aligned);
  return true;
}

} // namespace PortH700

#endif // METROID_PRIME_PORT_PORT_H700_SKINNING_H
