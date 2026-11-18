#include "gx.hpp"
#include "__gx.h"

#include "../../gx/fifo.hpp"
#include "../../gx/resident.hpp"
#include "../../gx/immutable.hpp"
#include "dolphin/gx/GXAurora.h"
#include "dolphin/gx/GXExtra.h"

#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

static __GXData_struct sSavedGXData;

namespace {
// What the game has retained (GXPortRetainResident), on the game thread: the processor holds
// the copies, by the same pointers.
struct Retained {
  u32 size;
  u32 count;
};
std::unordered_map<const void*, Retained> sRetained;
} // namespace

bool aurora::gx::resident::can_destroy_without_graphics_commands() noexcept {
  return sRetained.empty() && processor_registry_empty();
}

extern "C" {
void GXBeginDisplayList(void* list, u32 size) {
  CHECK(!aurora::gx::fifo::in_display_list(), "Display list began twice!");

  // Flush any pending dirty state before recording
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // Save current shadow register state if requested
  if (__gx->dlSaveContext != 0) {
    std::memcpy(&sSavedGXData, __gx, sizeof(sSavedGXData));
  }

  __gx->inDispList = 1;

  // Redirect FIFO writes to the user-provided buffer
  aurora::gx::fifo::begin_display_list(static_cast<u8*>(list), size);
}

u32 GXEndDisplayList() {
  // Flush any pending dirty state into the display list
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // End FIFO redirection and get the byte count (ROUNDUP32)
  u32 bytesWritten = aurora::gx::fifo::end_display_list();

  // Restore saved shadow register state
  if (__gx->dlSaveContext != 0) {
    std::memcpy(__gx, &sSavedGXData, sizeof(*__gx));
  }

  __gx->inDispList = 0;

  return bytesWritten;
}

void GXCallDisplayList(const void* data, u32 nbytes) {
  // Flush any pending dirty state before calling
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // Flush pending primitives
  if (*reinterpret_cast<u32*>(&__gx->vNum) != 0) {
    __GXSendFlushPrim();
  }

  if (!aurora::gx::fifo::in_display_list()) {
    const auto id = aurora::gx::immutable::producer_admission(
        data, nbytes, aurora::gx::immutable::SourceKind::DisplayList);
    if (id != 0) {
      u8 record[15] = {GX_AURORA, u8(GX_AURORA_CALL_IMMUTABLE_DL >> 8), u8(GX_AURORA_CALL_IMMUTABLE_DL)};
      for (unsigned i = 0; i < 8; ++i) record[3 + i] = u8(id >> (56 - i * 8));
      for (unsigned i = 0; i < 4; ++i) record[11 + i] = u8(nbytes >> (24 - i * 8));
      if (aurora::gx::fifo::write_checked(record, sizeof(record))) aurora::gx::fifo::publish();
      return; // rejected work is not replaced by an unsafe raw-pointer command
    }
  }

  if (!sRetained.empty() && !aurora::gx::fifo::in_display_list()) {
    const auto found = sRetained.find(data);
    if (found != sRetained.end() && found->second.size == nbytes) {
      // The processor draws its copy.
      GX_WRITE_AURORA(GX_AURORA_RESIDENT_CALL_DL);
      GX_WRITE_U64(reinterpret_cast<u64>(data));
      GX_WRITE_U32(nbytes);
      aurora::gx::fifo::publish();
      return;
    }
  }

  // Write display list contents to the FIFO
  aurora::gx::fifo::write_data(data, nbytes);
  aurora::gx::fifo::publish();
}

u64 GXPortRegisterImmutableSource(const void* data, u32 size, GXPortImmutableKind kind) {
#ifdef AURORA_H700
  static const bool enabled = [] {
    const char* optIn = std::getenv("MP_H700_RETAIL_GEOMETRY");
    const char* bench = std::getenv("MP_H700_BENCH");
    return optIn != nullptr && std::strcmp(optIn, "1") == 0 && bench != nullptr &&
           (std::strcmp(bench, "1") == 0 || std::strcmp(bench, "menu") == 0);
  }();
  if (!enabled || (kind != GX_PORT_IMMUTABLE_ARRAY && kind != GX_PORT_IMMUTABLE_DISPLAY_LIST)) return 0;
  return aurora::gx::immutable::register_source(
      data, size, kind == GX_PORT_IMMUTABLE_ARRAY ? aurora::gx::immutable::SourceKind::Array
                                               : aurora::gx::immutable::SourceKind::DisplayList);
#else
  (void)data;
  (void)size;
  (void)kind;
  return 0;
#endif
}

GXBool GXPortUnregisterImmutableSource(u64 sourceId) {
  return aurora::gx::immutable::unregister_source(sourceId);
}

u64 GXPortImmutableSourceEpoch() { return aurora::gx::immutable::producer_epoch(); }

void GXPortRetainResident(const void* data, u32 size) {
  if (data == nullptr || size == 0) {
    return;
  }
  auto [it, added] = sRetained.try_emplace(data, Retained{size, 0});
  if (!added && it->second.size != size) {
    // The same pointer with a different size is different data: the memory was reused
    // without a release. Retain the new data in its place.
    it->second = Retained{size, 0};
    added = true;
  }
  ++it->second.count;
  if (!added) {
    return;
  }
  // The processor takes the copy.
  auto* copy = new std::vector<u8>(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
  GX_WRITE_AURORA(GX_AURORA_RESIDENT_RETAIN);
  GX_WRITE_U64(reinterpret_cast<u64>(data));
  GX_WRITE_U64(reinterpret_cast<u64>(copy));
}

void GXPortReleaseResident(const void* data) {
  const auto found = sRetained.find(data);
  if (found == sRetained.end() || --found->second.count != 0) {
    return;
  }
  sRetained.erase(found);
  GX_WRITE_AURORA(GX_AURORA_RESIDENT_RELEASE);
  GX_WRITE_U64(reinterpret_cast<u64>(data));
}

}
