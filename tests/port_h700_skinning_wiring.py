"""GM12 stage3 bindings complementary to port_h700_skinning.cpp.

These inspect actual root, workspace publication and SkinPool::Run source order.
They do NOT execute constructors, skin jobs, renderer, GPU or root cleanup. The
C++ executable separately links the actual skin TU/ring and exercises token waits.
"""
import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from wiring_helpers import braced, squash
from port_h700_skin_consumers_wiring import target

SKIN = "src/Kyoto/Animation/DolphinCSkinnedModel.cpp"
RULES = "src/Kyoto/Animation/DolphinCSkinRules.cpp"
AVG = "src/Kyoto/Animation/CSkinnedModelWithAvgNormals.cpp"
HEADER = "include/Kyoto/Animation/CSkinnedModel.hpp"
MAIN = "src/MetroidPrime/main.cpp"


def body(text, signature):
    return braced(text, signature, signature)



class H700SkinningWiring(unittest.TestCase):
    def ordered(self, text, *markers):
        text, cursor = squash(text), 0
        for marker in markers:
            marker = squash(marker)
            found = text.find(marker, cursor)
            self.assertGreaterEqual(found, 0, f"missing/out-of-order {marker!r}")
            cursor = found + len(marker)

    def checked_layout_before(self, method, *consumers, ring=False):
        # Calculate combines health and layout in one short-circuit guard; the
        # other methods use the same layout helper in a standalone guard.
        check = re.search(
            r"if\s*\(\s*(?:!\s*PortSkinningMayContinue\([^;{}]*?\)\s*\|\|\s*)?"
            r"!\s*PortSkinLayout\([^;{}]*?\)\s*\)\s*return(?:\s+nullptr)?\s*;", method)
        self.assertIsNotNone(check, "failed layout must return, not just be computed")
        for consumer in consumers:
            self.ordered(method, check.group(0), consumer)
        if not ring:
            self.assertNotIn("SkinRingSize", method)

    def test_actual_layout_and_health_helpers_bind_production_policy(self):
        skin = target(SKIN)
        health = body(skin, "static bool PortSkinningMayContinue(")
        self.assertEqual(squash(health), squash("""{
            return PortH700::GameWorkMayContinue(aurora::h700::game_work_healthy(), site);
        }"""))
        layout = body(skin, "static bool PortSkinLayout(")
        self.ordered(layout, "if (PortH700::GameFailurePending()) return false;",
                     "if (PortH700::MakeSkinLayout(rules->GetNumPoints(), rules->GetNumNormals(), NormalStride(model), layout))",
                     "return true;", "PortH700::NoteGameFailure(PortH700::GameFailure::SkinSize,",
                     "return false;")

    def test_root_handshake_before_postinitialize_and_renderer(self):
        main = target(MAIN)
        root = body(main, "int CMain::RsMain(")
        self.ordered(root, "terminalMilestones.subsystemsInitialized = true;",
                     "CSkinnedModel::PortInitializeSkinning();",
                     'PortH700RootSafePoint("boot: skinning handshake returned");',
                     "gameGlobalObjects->PostInitialize(x0_osContext, x6d_memorySys);")
        self.assertEqual(root.count("CSkinnedModel::PortInitializeSkinning()"), 1)
        post = body(main, "void CGameGlobalObjects::PostInitialize(")
        self.ordered(post, "AddPaksAndFactories();", "PortH700GameWorkMayContinue(",
                     "Renderer::AllocateRenderer(")
        self.assertIn("static bool PortInitializeSkinning();", target(HEADER))
        self.assertNotIn("PortInitializeSkinning", target(HEADER, h700=False))
        self.assertNotIn("PortInitializeSkinning", target(MAIN, h700=False))

    def test_refcounts_do_not_wait_or_short_circuit_on_failure(self):
        skin = target(SKIN)
        add = body(skin, "void Skinning::AddSkinnedRef()")
        remove = body(skin, "void Skinning::DelSkinnedRef()")
        self.assertRegex(add, r"(?:\+\+\s*sNumSkinnedObjects|sNumSkinnedObjects\s*\+\+)")
        self.assertIn("--sNumSkinnedObjects;", remove)
        for method in (add, remove):
            self.assertNotRegex(method, r"GXReadDrawSync|GXSetDrawSync|main_progress_once|SDL_Delay|while\s*\(")
            self.assertNotIn("GameFailurePending", method)
            self.assertNotIn("return;", method)
        self.ordered(body(skin, "void CSkinnedModel::Construct()"), "Skinning::AddSkinnedRef();", "if (")
        self.assertIn("Skinning::DelSkinnedRef();", body(skin, "CSkinnedModel::~CSkinnedModel()"))

    def test_handshake_commits_only_after_read_and_postread_health(self):
        skin = target(SKIN)
        init = body(skin, "bool CSkinnedModel::PortInitializeSkinning()")
        self.ordered(init, "GXSetDrawSync(0xFFFF);", "GXReadDrawSync()",
                     "Skinning::skCurrentToken = 1;", "Skinning::sSkinningInitialized = true;",
                     "return true;")
        self.assertIn("aurora::h700::main_progress_once()", init)
        self.assertIn("SDL_Delay(", init)
        self.ordered(init, 'if (!PortSkinningMayContinue("skin: root handshake entry")) return false;',
                     "GXSetDrawSync(0xFFFF);",
                     'if (!PortSkinningMayContinue("skin: root handshake submitted")) return false;',
                     "GXReadDrawSync()",
                     'if (!PortSkinningMayContinue("skin: root handshake completion")) return false;',
                     "Skinning::skCurrentToken = 1;", "Skinning::sSkinningInitialized = true;")
        self.assertNotRegex(init, r"throw\b|lock_guard|unique_lock|OSDisableInterrupts")

    def test_ring_checked_before_alloc_metadata_before_pointer_return(self):
        skin = target(SKIN)
        ensure = body(skin, "void* CSkinnedModel::EnsureAllocation(")
        self.ordered(ensure, "PortH700::SkinRingSize(", "sSkinningBuffer->Alloc(",
                     "Skinning::sAllocations.push_back(", "return ptr;")
        self.assertIn("GameFailure::SkinSize", ensure)
        self.assertIn("GameFailure::SkinNotReady", ensure)
        self.assertIn("aurora::h700::main_progress_once()", ensure)
        self.assertIn("SDL_Delay(", ensure)
        self.ordered(ensure, "OSTicksToMilliseconds(", "GXSetDrawSync(Skinning::skCurrentToken);",
                     "++Skinning::skCurrentToken;")
        catch = body(ensure, "catch (const std::bad_alloc&)")
        self.assertIn("GameFailure::SkinAllocation", catch)
        self.assertIn("return nullptr;", catch)
        # A failed metadata append leaves a terminal ring reservation; Free here
        # would free out of FIFO order and cannot safely undo CCircularBuffer::Alloc.
        self.assertNotRegex(catch, r"\bFree\s*\(|pop_front|clear\s*\(")
        for method in (ensure, body(skin, "void CSkinnedModel::TickAllocations()")):
            self.assertNotRegex(method, r"lock_guard|unique_lock|OSDisableInterrupts|throw\b")

    def test_tick_preserves_original_wrap_and_prefix_reclaim(self):
        tick = body(target(SKIN), "void CSkinnedModel::TickAllocations()")
        self.ordered(tick, 'if (!PortSkinningMayContinue("skin: token reclaim entry")) return;',
                     "GXReadDrawSync()",
                     'if (!PortSkinningMayContinue("skin: token reclaim observation")) return;',
                     "syncVal -= 0x10000;",
                     "Skinning::sAllocations.front();", "tokenVal -= 0x10000;",
                     "if (syncVal < tokenVal)", "break;",
                     "Skinning::sSkinningBuffer->Free(front.x0_ptr, front.x4_unk1);",
                     "Skinning::sAllocations.pop_front();")

    def test_timeout_token_read_checks_health_before_marker_submission(self):
        ensure = body(target(SKIN), "void* CSkinnedModel::EnsureAllocation(")
        timeout = body(ensure, "if (OSTicksToMilliseconds(")
        self.ordered(timeout, "GXReadDrawSync()", "PortSkinningMayContinue(",
                     "return nullptr;", "GXSetDrawSync(Skinning::skCurrentToken);")

    def test_construct_storage_and_external_workspace_validate_before_publication(self):
        skin = target(SKIN)
        construct = body(skin, "void CSkinnedModel::Construct()")
        self.checked_layout_before(construct, "rs_new float[", "x28_vertWorkspace =")
        storage = body(skin, "void CSkinnedModel::AllocateStorage()")
        self.checked_layout_before(storage, "EnsureAllocation(", "x28_vertWorkspace =", ring=True)
        self.ordered(storage, "PortH700::SkinRingSize(size_t(layout.total) + 32, capacity, totalSize)",
                     "return;", "TickAllocations();", "EnsureAllocation(")
        self.ordered(storage, "EnsureAllocation(", "if (PortH700::GameFailurePending()) return;", "x28_vertWorkspace =",
                     "x30_normalWorkspace =")
        external = body(skin, "float* CSkinnedModel::AllocateNewWorkspace(")
        self.checked_layout_before(external, "CMemory::Alloc(")
        self.ordered(external, "CMemory::Alloc(", "return nullptr;",
                     "*vertOut = reinterpret_cast<", "return ptr;")
        self.assertNotIn("SkinRingSize", external)

    def test_calculate_and_draw_validate_before_gather_or_size_publication(self):
        skin = target(SKIN)
        calculate = body(skin, "void CSkinnedModel::Calculate(")
        self.checked_layout_before(calculate, "AllocateStorage();", "GXRedirectWriteGatherPipe(verts)")
        self.ordered(calculate, "AllocateStorage();", "GameFailurePending()", "return;",
                     "GXRedirectWriteGatherPipe(verts)")
        legacy = body(calculate, "if (legacy)")
        self.ordered(legacy, "OSDisableInterrupts();", "GXRedirectWriteGatherPipe(verts)",
                     "PortBuildLegacy(pipe);", "GXRestoreWriteGatherPipe();",
                     "OSRestoreInterrupts(interruptState);")
        direct = body(calculate[calculate.index(legacy) + len(legacy):], "else")
        self.ordered(direct, "PortBuildPointsAndNormals(",
                     "if (PortH700::GameFailurePending()) return;", "memset(")
        self.assertNotRegex(direct, r"GXRedirectWriteGatherPipe|OSDisableInterrupts")
        for signature in ("void CSkinnedModel::Draw(const CModelFlags&",
                          "void CSkinnedModel::Draw(const TDrawFunc",
                          "void CSkinnedModel::Draw(const float*"):
            self.checked_layout_before(body(skin, signature), "SetSkinnedArraySizes(")

    def test_averaged_normals_failed_copy_guard_precedes_own_allocations(self):
        avg = target(AVG)
        constructor = avg[avg.index("CSkinnedModelWithAvgNormals::CSkinnedModelWithAvgNormals("):]
        self.ordered(constructor, "x0_skinnedModel(skinnedModel)", "x3c_avgNormals()",
                     "if (PortH700::GameFailurePending()) return;", "try {",
                     "x3c_avgNormals = rstl::auto_ptr< float[] >(",
                     "rs_new float[size_t(skinnedModel.GetNumPoints()) * 12]",
                     "NativeVectors(skinnedModel.GetModel()->GetPositions()",
                     "vertMap.reserve(vertexCount);",
                     "NativeVectors(skinnedModel.GetModel()->GetNormals()",
                     "catch (const std::bad_alloc&)")
        catch = body(constructor, "catch (const std::bad_alloc&)")
        self.assertIn("PortH700::NoteGameFailure(PortH700::GameFailure::SkinAllocation,", catch)
        self.assertNotRegex(constructor,
                            r"\bthrow\b|RootTerminalToken|PortH700Root|PrepareRootUnwind|"
                            r"RootUnwindReady|RootCleanupAllowed|poll_generation_|"
                            r"request_generation_|synchronize_game_work")

    def test_actual_skinpool_run_waits_for_zero_pending_not_failure_shortcut(self):
        rules = target(RULES)
        run = body(rules, "void Run(int jobs,")
        self.ordered(run, "mFn = &fn;", "mPending = jobs;", "mWake.notify_all();",
                     "Work(generation, fn, jobs);", "std::unique_lock< std::mutex > lock(mMutex);",
                     "mDone.wait(lock, [this] { return mPending == 0; });")
        tail = run[run.index("Work(generation, fn, jobs);") :]
        self.assertNotRegex(tail, r"GameFailurePending|game_work_healthy|return\s*;")
        work = body(rules, "void Work(unsigned generation,")
        self.ordered(work, "fn(static_cast< int >(value & 0xFFFFFFFFu));", "++finished;",
                     "mPending -= finished;", "if (mPending == 0)", "mDone.notify_all();")


if __name__ == "__main__":
    unittest.main()
