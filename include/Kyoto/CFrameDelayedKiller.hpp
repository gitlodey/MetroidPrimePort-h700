#ifndef _CFRAMEDELAYEDKILLER
#define _CFRAMEDELAYEDKILLER

#include "types.h"

class CFrameDelayedKiller {
public:
  struct Stats {
    uint unk1;
    uint unk2;
    uint unk3;
    uint unk4;
    uint unk5;
    uint unk6;
    uint unk7;
    uint unk8;
  };
  enum EWhichFrame {
    kWhichFrame_ThisFrame,
    kWhichFrame_NextFrame,
  };
  static void Initialize();
  static void ShutDown();

  static void FlushAllocationsForFrame();
  static void ScheduleDeletion(const EWhichFrame thisFrame, void* victim);
#ifdef TARGET_PC
  static void ScheduleHostDeletion(const EWhichFrame thisFrame, void* victim);
#endif
  // No graphics commands. H700 owner cleanup must first prove its FIFO/render/callback
  // tail complete via CGraphics::PortGameOwnerBoundary(), with game producers stopped.
  static void FlushAllAllocations();
  // Issues GXDrawDone: a FIFO barrier only, not render/callback or GPU completion.
  static void StallAndFlushAllAllocations();
  const CFrameDelayedKiller::Stats& GetStats() {
    return mUnusedStats;
  }
  static Stats mUnusedStats;
};

#endif // _CFRAMEDELAYEDKILLER
