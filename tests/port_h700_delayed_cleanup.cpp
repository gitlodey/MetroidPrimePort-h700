// Link with the actual src/Kyoto/CFrameDelayedKiller.cpp, not a reimplementation.
// CPU-only: the held-checkpoint query and GX barrier are controlled seams; the real
// CFDk queues, both frame lists, heap checks and game/host deleters execute here.
#include "Kyoto/CFrameDelayedKiller.hpp"
#include "Kyoto/Alloc/CMemory.hpp"
#include "Kyoto/Particles/IElement.hpp"
#include "../extern/aurora/lib/h700/lifecycle.hpp"
#include "port_test.h"

#include <dolphin/gx/GXManage.h>

#include <array>
#include <cstdlib>
#include <new>

#if !defined(TARGET_PC) || !defined(AURORA_H700)
#error This regression must exercise the H700 host CFDk implementation
#endif

namespace {
struct GameVictim {
  void* ptr = nullptr;
  size_t size = 0;
};
std::array<GameVictim, 2> gameVictims;
std::array<void*, 2> hostVictims{};
constexpr char Header[] = "port_h700_delayed_cleanup";
bool checkpointHeld = false;
int queries = 0;
int barriers = 0;
int expectedBarriersAtFree = 0;
int gameFrees = 0;
int hostFrees = 0;
int heapChecks = 0;

void NoteHostFree(void* ptr) {
  for (auto& victim : hostVictims) {
    if (ptr != nullptr && ptr == victim) {
      REQUIRE(barriers == expectedBarriersAtFree);
      victim = nullptr;
      ++hostFrees;
      return;
    }
  }
}

void CheckFlush(bool held, void (*flush)(), bool queriesCheckpoint = true) {
  checkpointHeld = held;
  queries = barriers = gameFrees = hostFrees = heapChecks = 0;
  expectedBarriersAtFree = queriesCheckpoint && !held ? 1 : 0;
  for (size_t i = 0; i < gameVictims.size(); ++i) {
    auto& game = gameVictims[i];
    REQUIRE(game.ptr == nullptr && hostVictims[i] == nullptr);
    game.size = 24 + i;
    game.ptr = std::malloc(game.size);
    REQUIRE(game.ptr != nullptr);
    hostVictims[i] = new uchar[32 + i];
    const auto frame = i == 0 ? CFrameDelayedKiller::kWhichFrame_ThisFrame
                              : CFrameDelayedKiller::kWhichFrame_NextFrame;
    CFrameDelayedKiller::ScheduleDeletion(frame, game.ptr);
    CFrameDelayedKiller::ScheduleHostDeletion(frame, hostVictims[i]);
  }
  CHECK_EQ(gameFrees, 0);
  CHECK_EQ(hostFrees, 0);
  CHECK_EQ(barriers, 0);
  CHECK_EQ(queries, 0); // scheduling is not a shutdown/checkpoint operation
  flush();
  CHECK_EQ(queries, queriesCheckpoint ? 1 : 0);
  CHECK_EQ(barriers, expectedBarriersAtFree);
  CHECK_EQ(gameFrees, 2);
  CHECK_EQ(hostFrees, 2);
  CHECK_EQ(heapChecks, 2);
  for (size_t i = 0; i < gameVictims.size(); ++i) {
    CHECK(gameVictims[i].ptr == nullptr);
    CHECK(hostVictims[i] == nullptr);
  }
  // Both real lists were cleared: a second flush must not free any victim twice.
  flush();
  CHECK_EQ(queries, queriesCheckpoint ? 2 : 0);
  CHECK_EQ(barriers, 2 * expectedBarriersAtFree);
  CHECK_EQ(gameFrees, 2);
  CHECK_EQ(hostFrees, 2);
  CHECK_EQ(heapChecks, 4);
}
} // namespace

namespace aurora::h700 {
bool generation_cpu_quiescence_held() noexcept {
  ++queries;
  return checkpointHeld;
}
} // namespace aurora::h700

extern "C" void GXDrawDone() {
  REQUIRE(!checkpointHeld); // any barrier at the actual held checkpoint is a regression
  if (barriers == 0) REQUIRE(gameFrees == 0 && hostFrees == 0);
  ++barriers;
}

void CMemory::Free(const void* ptr) {
  for (auto& victim : gameVictims) {
    if (ptr != nullptr && ptr == victim.ptr) {
      REQUIRE(barriers == expectedBarriersAtFree);
      victim.ptr = nullptr;
      ++gameFrees;
      std::free(const_cast<void*>(ptr));
      return;
    }
  }
  REQUIRE_MSG(false, "unexpected or duplicate game free");
}

// Pair the test's array allocator/deleter, also used by rstl list nodes. Only the
// explicitly queued host victims count; list-node retirement is not a victim free.
void* operator new[](size_t size) {
  if (void* ptr = std::malloc(size == 0 ? 1 : size)) return ptr;
  throw std::bad_alloc();
}
void operator delete[](void* ptr) noexcept {
  NoteHostFree(ptr);
  std::free(ptr);
}
void operator delete[](void* ptr, size_t) noexcept { ::operator delete[](ptr); }

bool PortHeapCheckEnabled() { return true; }
void PortHeapCheckAll() { ++heapChecks; }
bool PortHeapPeekBlock(const void* ptr, const char** fileAndLine, size_t* len) {
  for (const auto& victim : gameVictims) {
    if (ptr != nullptr && ptr == victim.ptr) {
      *fileAndLine = Header;
      *len = victim.size;
      return true;
    }
  }
  return false;
}

// Unrelated particle allocation members share the production TU; keep the test
// link standalone without pulling in the particle system or disc/resource owners.
void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) {
  return ::operator new(size);
}
void IElement::CElementAllocator::Free(void* ptr, size_t) { ::operator delete(ptr); }

int main() {
  CheckFlush(false, CFrameDelayedKiller::StallAndFlushAllAllocations);
  CheckFlush(false, CFrameDelayedKiller::Initialize);
  CheckFlush(false, CFrameDelayedKiller::ShutDown);
  CheckFlush(true, CFrameDelayedKiller::StallAndFlushAllAllocations);
  // A later destructor may enqueue more victims while the same CPU checkpoint holds.
  CheckFlush(true, CFrameDelayedKiller::StallAndFlushAllAllocations);
  CheckFlush(true, CFrameDelayedKiller::ShutDown);
  CheckFlush(true, CFrameDelayedKiller::FlushAllAllocations, false);
  return PORT_TEST_RESULT();
}
