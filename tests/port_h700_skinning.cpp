// Link the actual DolphinCSkinnedModel.cpp and CCircularBuffer.cpp, not shims.cpp.
// Use the game include paths/compat.h, TARGET_PC, AURORA_H700, VERSION=0,
// -ffunction-sections -fdata-sections and ELF -Wl,--gc-sections. No GPU/SDL library.
// Each argv case MUST run in a fresh process: production's failure latch has no reset.
// Cases: layout refs init-pending init-fail-progress init-fail-read not-ready
//        oversized ring wrap metadata wait-fail-progress wait-fail-read
#include "Kyoto/Alloc/CCircularBuffer.hpp"
#include "Kyoto/Alloc/CMemory.hpp"
#include "Kyoto/Animation/CSkinnedModel.hpp"
#include "port_h700_game_failure.h"
#include "port_h700_skinning.h"
#include "port_test.h"
#include "../extern/aurora/lib/h700/lifecycle.hpp"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

#ifndef AURORA_H700
#error This test must compile the production H700 skinning branches
#endif
#if defined(VERSION) && VERSION != 0
#error Exercise the H700 static 32 MiB ring (VERSION=0, also the native default)
#endif

namespace {
using Failure = PortH700::GameFailure;
constexpr int RingBytes = 32 * 1024 * 1024;
struct Marker { u16 token; unsigned ready; };
Marker markers[256];
unsigned submitted = 0, consumed = 0, progresses = 0, delays = 0, reads = 0;
unsigned tokenWrites = 0, markerWrites = 0, pendingReads = 0;
unsigned healthQueries = 0, failAtProgress = 0;
bool healthy = true, failAtRead = false, completeMarkers = true;
bool sawFFFF = false, sawZero = false, sawOneAfterZero = false;
u16 completedToken = 0, lastMarker = 0;
u32 ticks = 0;
unsigned arrayAllocations = 0, arrayFrees = 0, failedArrays = 0;
unsigned heapAllocations = 0, heapFrees = 0;
bool failNextArray = false;

void Healthy() {
  CHECK(!PortH700::GameFailurePending());
  CHECK_EQ(PortH700::GetGameFailure(), Failure::None);
}
void Failed(Failure failure) {
  CHECK(PortH700::GameFailurePending());
  CHECK_EQ(PortH700::GetGameFailure(), failure);
  CHECK(PortH700::GameFailureSite() != nullptr);
}
struct Ref {
  Ref() { CSkinnedModel::AddDummySkinnedModelRef(); }
  ~Ref() { CSkinnedModel::RemoveDummySkinnedModelRef(); }
  Ref(const Ref&) = delete;
  Ref& operator=(const Ref&) = delete;
};
void Initialize() {
  REQUIRE(CSkinnedModel::PortInitializeSkinning());
  Healthy();
  CHECK_EQ(completedToken, 0xFFFF);
  CHECK_EQ(tokenWrites, 1u);
}
void CheckLayout() {
  PortH700::SkinLayout out;
  CHECK(PortH700::MakeSkinLayout(0, 0, 12, out));
  CHECK_EQ(out.total, 0u);
  CHECK(PortH700::MakeSkinLayout(1, 1, 12, out));
  CHECK_EQ(out.vertices, 12u);
  CHECK_EQ(out.normals, 12u);
  CHECK_EQ(out.alignedVertices, 32u);
  CHECK_EQ(out.alignedNormals, 32u);
  CHECK_EQ(out.total, 64u);
  for (const unsigned stride : {36u, 60u}) {
    REQUIRE(PortH700::MakeSkinLayout(3, 2, stride, out));
    CHECK_EQ(out.vertices, 36u);
    CHECK_EQ(out.normals, 2 * stride);
    CHECK_EQ(out.alignedVertices, 64u);
    CHECK_EQ(out.alignedNormals, stride == 36 ? 96u : 128u);
    CHECK_EQ(out.total, stride == 36 ? 160u : 192u);
  }
  const auto reject = [&](int points, int normals, unsigned stride) {
    out = {1, 2, 3, 4, 5};
    CHECK(!PortH700::MakeSkinLayout(points, normals, stride, out));
    CHECK_EQ(out.vertices, 1u);
    CHECK_EQ(out.normals, 2u);
    CHECK_EQ(out.alignedVertices, 3u);
    CHECK_EQ(out.alignedNormals, 4u);
    CHECK_EQ(out.total, 5u);
  };
  reject(-1, 0, 12);
  reject(0, INT_MIN, 12);
  reject(1, 1, 0);
  reject(INT_MAX, 0, 12);
  reject(0, INT_MAX, 60);
  reject(0, 1, UINT_MAX);
  // Raw bytes can fit int while their aligned byte cursor does not.
  reject(INT_MAX / 12, 0, 12);
  reject(0, INT_MAX / 12, 12);
  const int largestAligned = INT_MAX & ~31;
  REQUIRE(PortH700::MakeSkinLayout(largestAligned / 12, 0, 12, out));
  CHECK_EQ(out.total, static_cast<unsigned>(largestAligned));
  reject(largestAligned / 12, 1, 12); // sum, not just each product
  REQUIRE(PortH700::MakeSkinLayout(3000000, 1, 60, out));
  CHECK(out.total > RingBytes); // external/heap workspaces are NOT ring-capped
  CHECK(out.total <= INT_MAX);

  int bytes = -7;
  CHECK(PortH700::SkinRingSize(1, RingBytes, bytes));
  CHECK_EQ(bytes, 32);
  CHECK(PortH700::SkinRingSize(RingBytes - 1, RingBytes, bytes));
  CHECK_EQ(bytes, RingBytes);
  CHECK(PortH700::SkinRingSize(RingBytes, RingBytes, bytes));
  CHECK_EQ(bytes, RingBytes);
  for (const std::size_t invalid : {std::size_t(0), std::size_t(RingBytes + 1),
       std::size_t(INT_MAX), std::size_t(INT_MAX) + 1,
       std::numeric_limits<std::size_t>::max()}) {
    bytes = -7;
    CHECK(!PortH700::SkinRingSize(invalid, RingBytes, bytes));
    CHECK_EQ(bytes, -7);
  }
  CHECK(!PortH700::SkinRingSize(33, 63, bytes));
  CHECK(!PortH700::SkinRingSize(INT_MAX, std::numeric_limits<std::size_t>::max(), bytes));
  CHECK_EQ(arrayAllocations, 0u);
  CHECK_EQ(heapAllocations, 0u);
}
void CheckCircularPrefix() {
  alignas(32) char storage[128];
  CCircularBuffer ring(storage, sizeof(storage));
  CHECK(ring.Alloc(64) == storage);
  CHECK(ring.Alloc(32) == storage + 64);
  CHECK(ring.Alloc(32) == storage + 96);
  CHECK(ring.Alloc(32) == nullptr);
  ring.Free(storage, 64);
  CHECK_EQ(ring.GetAllocatedAmount(), 64);
  CHECK(ring.Alloc(64) == storage); // wrap only into the reclaimed prefix
  CHECK(ring.Alloc(32) == nullptr);
  ring.Free(storage + 64, 32);
  CHECK(ring.Alloc(32) == storage + 64);
  ring.Free(storage + 96, 32);
  ring.Free(storage, 64);
  ring.Free(storage + 64, 32);
  CHECK_EQ(ring.GetAllocatedAmount(), 0);
  CHECK(ring.Alloc(sizeof(storage)) == storage);
}
void CheckInitPending() {
  Initialize();
  CHECK(progresses >= 3);
  CHECK(delays > 0);
  CHECK(pendingReads > 0);
  CHECK(healthQueries >= 2);
  const auto oldProgress = progresses, oldReads = reads;
  REQUIRE(CSkinnedModel::PortInitializeSkinning());
  CHECK_EQ(tokenWrites, 1u);
  CHECK_EQ(progresses, oldProgress);
  // An already-initialized query need not read the GPU token again.
  CHECK(reads >= oldReads);
  Ref ref;
  REQUIRE(CSkinnedModel::EnsureAllocation(32) != nullptr);
  CHECK_EQ(markerWrites, 0u); // first reservation does not emit fake completion
}
void CheckRefs() {
  Initialize();
  const auto initialReads = reads, initialWrites = tokenWrites;
  const auto initialProgress = progresses, initialDelays = delays;
  {
    Ref first;
    { Ref second; }
  }
  CHECK_EQ(reads, initialReads);
  CHECK_EQ(tokenWrites, initialWrites);
  CHECK_EQ(progresses, initialProgress);
  CHECK_EQ(delays, initialDelays); // refs cannot repeat the root handshake
  Healthy();
  void* base;
  {
    Ref first;
    {
      Ref second;
      base = CSkinnedModel::EnsureAllocation(RingBytes);
      REQUIRE(base != nullptr);
    }
    const auto before = progresses;
    REQUIRE(CSkinnedModel::EnsureAllocation(32) == base);
    CHECK(progresses > before); // removing only one ref did NOT reset a live ring
  }
  const auto before = progresses;
  {
    Ref next;
    CHECK(CSkinnedModel::EnsureAllocation(RingBytes) == base);
    CHECK_EQ(progresses, before); // balanced final removal resets ring/metadata
  }
  Healthy();
}
void CheckInitFailure(bool atRead) {
  if (atRead) {
    completedToken = 0xFFFF;
    failAtRead = true;
  } else {
    completeMarkers = false;
    failAtProgress = 2;
  }
  CHECK(!CSkinnedModel::PortInitializeSkinning());
  Failed(Failure::Backend);
  if (!atRead) CHECK_EQ(completedToken, 0); // never substitute the submitted token
  const auto writes = tokenWrites;
  CHECK(!CSkinnedModel::PortInitializeSkinning());
  CHECK_EQ(tokenWrites, writes);
  Ref ref; // still balanced even when init failed
  CHECK(CSkinnedModel::EnsureAllocation(32) == nullptr);
  CHECK_EQ(arrayAllocations, 0u);
}
void CheckNotReady() {
  Ref ref;
  Failed(Failure::SkinNotReady); // AddRef counts the inert owner, but never waits
  { Ref another; } // failure does not bypass either side of reference accounting
  CHECK(CSkinnedModel::EnsureAllocation(32) == nullptr);
  Failed(Failure::SkinNotReady);
  CHECK_EQ(arrayAllocations, 0u);
  CHECK_EQ(tokenWrites, 0u);
  CHECK_EQ(progresses, 0u);
}
void CheckOversized() {
  Initialize();
  Ref ref;
  const auto beforeReads = reads, beforeProgress = progresses;
  CHECK(CSkinnedModel::EnsureAllocation(RingBytes + 1) == nullptr);
  Failed(Failure::SkinSize);
  CHECK_EQ(arrayAllocations, 0u);
  CHECK_EQ(heapAllocations, 0u);
  CHECK_EQ(reads, beforeReads);
  CHECK_EQ(progresses, beforeProgress);
  CHECK_EQ(markerWrites, 0u);
}
void CheckRing() {
  CheckCircularPrefix();
  Initialize();
  Ref ref;
  auto* base = static_cast<char*>(CSkinnedModel::EnsureAllocation(RingBytes / 2));
  REQUIRE(base != nullptr);
  CHECK(CSkinnedModel::EnsureAllocation(RingBytes / 2) == base + RingBytes / 2);
  const auto beforeFrees = arrayFrees, beforeProgress = progresses, beforeDelay = delays;
  CSkinnedModel::TickAllocations(); // init sentinel is older than token 1
  CHECK_EQ(arrayFrees, beforeFrees);
  REQUIRE(CSkinnedModel::EnsureAllocation(RingBytes / 2) == base);
  CHECK(progresses > beforeProgress);
  CHECK(delays > beforeDelay);
  CHECK(markerWrites > 0);
  CHECK_EQ(arrayFrees, beforeFrees + 2); // real token reclaims the FIFO batch
  CHECK_EQ(arrayAllocations - arrayFrees, 1u);
  const auto afterFrees = arrayFrees;
  CSkinnedModel::TickAllocations();
  CHECK_EQ(arrayFrees, afterFrees); // old completion cannot retire the new workspace
  CHECK(CSkinnedModel::EnsureAllocation(RingBytes / 2) == base + RingBytes / 2);
  Healthy();
}
void CheckWrap() {
  Initialize();
  Ref ref;
  void* base = CSkinnedModel::EnsureAllocation(RingBytes);
  REQUIRE(base != nullptr);
  // Advance actual production ushort tokens via its existing >60ms marker path.
  // Only one FIFO reservation survives each iteration; no test-only state mutation.
  for (unsigned i = 0; i < 65538; ++i) {
    const auto before = progresses, frees = arrayFrees;
    REQUIRE(CSkinnedModel::EnsureAllocation(RingBytes) == base);
    REQUIRE(progresses > before);
    REQUIRE(arrayFrees == frees + 1);
    REQUIRE(!PortH700::GameFailurePending());
  }
  CHECK(sawFFFF);
  CHECK(sawZero);
  CHECK(sawOneAfterZero);
  CHECK_EQ(arrayAllocations - arrayFrees, 1u);
}
void CheckMetadataFailure() {
  Initialize();
  Ref ref;
  REQUIRE(CSkinnedModel::EnsureAllocation(32) != nullptr);
  const auto beforeAlloc = arrayAllocations, beforeFree = arrayFrees;
  failNextArray = true; // rstl::list node allocation, NOT a replacement ring allocator
  void* published = CSkinnedModel::EnsureAllocation(32);
  CHECK(published == nullptr);
  CHECK(!failNextArray);
  CHECK_EQ(failedArrays, 1u);
  Failed(Failure::SkinAllocation);
  CHECK_EQ(arrayAllocations, beforeAlloc);
  CHECK_EQ(arrayFrees, beforeFree); // no earlier FIFO metadata destroyed on failure
  const auto beforeProgress = progresses;
  CHECK(CSkinnedModel::EnsureAllocation(32) == nullptr);
  CSkinnedModel::TickAllocations();
  CHECK_EQ(arrayAllocations, beforeAlloc);
  CHECK_EQ(arrayFrees, beforeFree);
  CHECK_EQ(progresses, beforeProgress); // terminal reservation cannot be reused
}
void CheckWaitFailure(bool atRead) {
  Initialize();
  Ref ref;
  REQUIRE(CSkinnedModel::EnsureAllocation(RingBytes) != nullptr);
  const auto beforeAlloc = arrayAllocations, beforeFree = arrayFrees;
  completeMarkers = false;
  if (atRead) {
    // The read reports the allocation's actual token, but simultaneously loses health.
    completedToken = 1;
    failAtRead = true;
  } else {
    failAtProgress = progresses + 2;
  }
  CHECK(CSkinnedModel::EnsureAllocation(32) == nullptr);
  Failed(Failure::Backend);
  CHECK_EQ(arrayAllocations, beforeAlloc);
  CHECK_EQ(arrayFrees, beforeFree); // health after read must precede reclaim/publication
}
} // namespace

// Only external services are stubs. All sizing/ring/init/reclaim code is production.
void* operator new[](std::size_t bytes) {
  if (failNextArray) {
    failNextArray = false;
    ++failedArrays;
    throw std::bad_alloc();
  }
  void* p = std::malloc(bytes == 0 ? 1 : bytes);
  if (!p) throw std::bad_alloc();
  ++arrayAllocations;
  return p;
}
void operator delete[](void* p) noexcept {
  if (p) ++arrayFrees;
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept { ::operator delete[](p); }

const char CCallStack::kUnknownType[] = "test";
CCallStack::CCallStack(uint, const char* line, const char* type) : x0_line(line), x4_type(type) {}
void* CMemory::Alloc(std::size_t bytes, IAllocator::EHint, IAllocator::EScope,
                     IAllocator::EType, const CCallStack&) {
  ++heapAllocations;
  void* ptr = nullptr;
  if (posix_memalign(&ptr, 32, bytes == 0 ? 32 : bytes) != 0) throw std::bad_alloc();
  return ptr;
}
void CMemory::Free(const void* p) {
  if (p) ++heapFrees;
  std::free(const_cast<void*>(p));
}
extern "C" void GXSetDrawSync(u16 token) {
  REQUIRE(healthy);
  REQUIRE(!PortH700::GameFailurePending());
  ++tokenWrites;
  if (tokenWrites == 1) {
    REQUIRE(token == 0xFFFF);
  } else {
    ++markerWrites;
    REQUIRE(token == static_cast<u16>(lastMarker + 1));
    lastMarker = token;
    if (token == 0xFFFF) sawFFFF = true;
    if (token == 0 && sawFFFF) sawZero = true;
    if (token == 1 && sawZero) sawOneAfterZero = true;
  }
  REQUIRE(submitted - consumed < 256);
  markers[submitted++ % 256] = {token, progresses + 3};
  // Deliberately no completedToken assignment: submission is NOT completion.
}
extern "C" u16 GXReadDrawSync() {
  REQUIRE(healthy);
  REQUIRE(!PortH700::GameFailurePending());
  REQUIRE(++reads < 2000000); // bounded regression failure, not an infinite spin
  if (consumed != submitted) ++pendingReads;
  if (failAtRead) healthy = false;
  return completedToken;
}
extern "C" OSTick OSGetTick() {
  ticks += static_cast<u32>(OSMillisecondsToTicks(61));
  return ticks;
}
extern "C" void SDLCALL SDL_Delay(Uint32) { ++delays; }
extern "C" bool SDLCALL SDL_IsMainThread() { return true; }
namespace aurora::h700 {
bool game_work_healthy() noexcept { ++healthQueries; return healthy; }
bool main_progress_once() {
  REQUIRE(++progresses < 1000000);
  if (progresses == failAtProgress) healthy = false;
  if (completeMarkers && consumed != submitted && markers[consumed % 256].ready <= progresses) {
    completedToken = markers[consumed++ % 256].token;
  }
  // Production reports whether the progress pass succeeded, not whether a
  // token completed. A healthy pass with a still-pending token returns true.
  return healthy;
}
} // namespace aurora::h700

int main(int argc, char** argv) {
  REQUIRE_MSG(argc == 2, "one case per fresh process; see case list at top of file");
  const char* name = argv[1];
  if (!std::strcmp(name, "layout")) CheckLayout();
  else if (!std::strcmp(name, "refs")) CheckRefs();
  else if (!std::strcmp(name, "init-pending")) CheckInitPending();
  else if (!std::strcmp(name, "init-fail-progress")) CheckInitFailure(false);
  else if (!std::strcmp(name, "init-fail-read")) CheckInitFailure(true);
  else if (!std::strcmp(name, "not-ready")) CheckNotReady();
  else if (!std::strcmp(name, "oversized")) CheckOversized();
  else if (!std::strcmp(name, "ring")) CheckRing();
  else if (!std::strcmp(name, "wrap")) CheckWrap();
  else if (!std::strcmp(name, "metadata")) CheckMetadataFailure();
  else if (!std::strcmp(name, "wait-fail-progress")) CheckWaitFailure(false);
  else if (!std::strcmp(name, "wait-fail-read")) CheckWaitFailure(true);
  else REQUIRE_MSG(false, "unknown skinning test case");
  CHECK_EQ(heapAllocations, heapFrees);
  return PORT_TEST_RESULT();
}
