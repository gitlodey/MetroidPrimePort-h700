"""Bind first GM12 status/continuation policy to actual boot/frame/root sources.

These are source checks, not execution of the game, GPU, shims or actual joins.
The standalone C++ test exercises the shared predicate and existing GM11b gates.
"""
import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from wiring_helpers import braced as extract_braced, squash


def source(path):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", (ROOT / path).read_text(), flags=re.S)


def braced(text, signature):
    return extract_braced(text, signature, signature)


class H700GameFailureWiring(unittest.TestCase):
    def ordered(self, text, *markers):
        text = squash(text)
        cursor = 0
        for marker in markers:
            cursor = text.index(squash(marker), cursor) + len(squash(marker))

    def test_actual_root_uses_shared_policy_then_existing_checkpoint_guards(self):
        main = source("src/MetroidPrime/main.cpp")
        policy = braced(main, "static bool PortH700GameWorkMayContinue(")
        self.assertEqual(squash(policy), squash("""{
          return PortH700::GameWorkMayContinue(aurora::h700::game_work_healthy(), staticSite);
        }"""))
        root = braced(main, "static void PortH700RootSafePoint(")
        self.ordered(root, "if (!SDL_IsMainThread())", "PortH700RootFailStop(",
                     "if (!PortH700GameWorkMayContinue(safePoint)) PortH700PrepareRootUnwind(safePoint);")
        prepare = braced(main, "static void PortH700PrepareRootUnwind(")
        self.ordered(prepare, "PortH700::PrepareRootUnwind(",
                     "aurora::h700::poll_generation_cpu_quiescence()",
                     "if (!PortH700::RootUnwindReady(checkpoint))",
                     "PortH700RootFailStop(",
                     "if (!PortH700::RootCleanupAllowed(checkpoint, PortH700CleanupObligationsEmpty))",
                     "PortH700RootFailStop(", "throw PortH700::RootTerminalToken")
        self.assertIn("PortH700::GameFailureSite()", prepare)
        catch = braced(main, "catch (const PortH700::RootTerminalToken& terminal)")
        self.ordered(catch, "RootCleanupAllowed(terminal.checkpoint, PortH700CleanupObligationsEmpty)",
                     "CleanupTerminalOwners(", "return result;")
        guards = braced(main, "static bool PortH700CleanupObligationsEmpty()")
        for predicate in ("aurora::h700::resident_resources_empty()",
                          "PortRoomEnv::CanDestroyWithoutGraphicsCommands()",
                          "PortRoomGeo::CanDestroyWithoutGraphicsCommands()",
                          "PortRoomLiquid::CanDestroyWithoutGraphicsCommands()"):
            self.assertIn(predicate, guards)

    def test_actual_pak_loop_returns_without_further_boot_consumers(self):
        main = source("src/MetroidPrime/main.cpp")
        # VERSION=0 is the H700 signature; do not parse the mutually exclusive
        # VERSION!=0 opening brace as a second nested C++ body.
        boot = braced(main, "void CGameGlobalObjects::AddPaksAndFactories()")
        loop = braced(boot, "while (!factory.GetResLoader().AreAllPaksLoaded())")
        self.ordered(loop,
                     'if (!PortH700GameWorkMayContinue("boot: before PAK event pump")) break;',
                     "aurora_update();",
                     'if (!PortH700GameWorkMayContinue("boot: PAK event pump returned")) break;',
                     "ARQPoll();", "AsyncIdlePakLoading();", "errorWindow.Update();",
                     'if (!PortH700GameWorkMayContinue("boot: PAK loading update returned")) break;',
                     "if (CGraphics::BeginScene())",
                     'if (!PortH700GameWorkMayContinue("boot: PAK BeginScene returned")) break;',
                     "errorWindow.ShowMessage();",
                     'if (!PortH700GameWorkMayContinue("boot: PAK splash draw returned")) break;',
                     "CGraphics::EndScene();",
                     'if (!PortH700GameWorkMayContinue("boot: PAK frame returned")) break;',
                     "controller->Poll();", "gpMain->CheckReset();")
        self.ordered(boot, loop, "gpController = nullptr;",
                     'if (!PortH700GameWorkMayContinue("boot: PAK loading returned")) return;',
                     'AddPakFileAsync(rstl::string_l("aram:SamusGun")', "GetFactoryMgr().AddFactory(")
        for forbidden in ("throw ", "PortH700PrepareRootUnwind", "poll_generation_", "synchronize_game_work"):
            self.assertNotIn(forbidden, boot)

    def test_actual_postinitialize_stops_before_string_renderer_and_later_init(self):
        main = source("src/MetroidPrime/main.cpp")
        post = braced(main, "void CGameGlobalObjects::PostInitialize(")
        self.ordered(post, "AddPaksAndFactories();",
                     'if (!PortH700GameWorkMayContinue("boot: AddPaksAndFactories returned")) return;',
                     "LoadStringTable();",
                     'if (!PortH700GameWorkMayContinue("boot: string table loaded")) return;',
                     "Renderer::AllocateRenderer(", "gpRender =",
                     'if (!PortH700GameWorkMayContinue("boot: renderer constructed")) return;',
                     "CEnvFxManager::Initialize();",
                     'if (!PortH700GameWorkMayContinue("boot: environment effects initialized")) return;',
                     "CScriptMazeNode::LoadMazeSeeds();")
        root = braced(main, "int CMain::RsMain(")
        self.ordered(root, "gameGlobalObjects;", "archSupport;", "try {",
                     "gameGlobalObjects->PostInitialize(x0_osContext, x6d_memorySys);",
                     'PortH700RootSafePoint("boot: globals PostInitialize returned");',
                     "x70_tweaks.RegisterTweaks();")

    def test_actual_draw_and_loader_return_to_root_before_normal_continuation(self):
        root = braced(source("src/MetroidPrime/main.cpp"), "int CMain::RsMain(")
        self.ordered(root, "archSupport->GetIOWinManager().Draw();",
                     "CCameraManager::SetPresentationInterpolation(-1.f);",
                     "CGraphics::SetSimTimePresentLag(0.f);",
                     "CGraphics::SetRenderTimePresentLag(0.f);",
                     'PortH700RootSafePoint("loop: game draw returned, presentation restored");',
                     "PortSpeedrunTimer::Draw();", "DrawDebugMetrics(",
                     "AddFrameTime(x104_drawTimes,", "AsyncIdle(idleMicros);",
                     'PortH700RootSafePoint("loop: AsyncIdle returned, before EndScene");',
                     "gpRender->EndScene();",
                     'PortH700RootSafePoint("loop: draw and EndScene returned");',
                     "presented = true;", "++archSupport->GetFramesDrawn();")

    def test_actual_endscene_submission_and_sync_pulse_precede_failure_gate_and_tail(self):
        graphics = source("src/Kyoto/Graphics/DolphinCGraphics.cpp")
        frame_end = braced(graphics, "void AuroraFrameEnd()")
        self.ordered(frame_end, "aurora_end_frame();", "s_auroraFrameOpen = false;",
                     "#ifndef AURORA_H700", "s_framePresented.store(", "#endif")
        end = braced(graphics, "void CGraphics::EndScene()")
        self.ordered(end,
                     'if (!PortH700::GameWorkMayContinue(aurora::h700::game_work_healthy(), "CGraphics::EndScene entry")) return;',
                     "if (!s_auroraFrameOpen)", "CGX::SetZMode(", "GXCopyDisp(",
                     "PortDebug::DrawUI();", "AuroraFrameEnd();", "GXGetFifoPtrs(",
                     "GXEnableBreakPt(writePtr);",
                     'if (!PortH700::GameWorkMayContinue(aurora::h700::game_work_healthy(), "CGraphics::EndScene frame returned")) return;',
                     "s_framePresented.store(", "mLastFrameUsedAbove =", "++mFrameCounter;",
                     "CFrameDelayedKiller::FlushAllocationsForFrame();")
        self.assertEqual(end.count("FlushAllocationsForFrame()"), 1)
        # Existing shims are synchronous; no new wait/ack protocol is needed.
        shim = braced(source("platform/shims.cpp"), 'extern "C" void GXEnableBreakPt(')
        self.ordered(shim, "s_breakPtCallback();", "AuroraRetracePulse();")
        for forbidden in ("throw ", "poll_generation_", "synchronize_game_work", "PortH700PrepareRootUnwind"):
            self.assertNotIn(forbidden, end)

    def test_actual_upper_renderer_does_not_retire_or_age_failed_frame(self):
        end = braced(source("src/MetaRender/CCubeRenderer.cpp"), "void CCubeRenderer::EndScene()")
        self.ordered(end,
                     'if (!PortH700::GameWorkMayContinue(aurora::h700::game_work_healthy(), "CCubeRenderer::EndScene entry")) return;',
                     "x318_31_persistRGBA6 =", "CGraphics::EndScene();",
                     'if (!PortH700::GameWorkMayContinue(aurora::h700::game_work_healthy(), "CCubeRenderer::EndScene frame returned")) return;',
                     "if (x2dc_reflectionAge >= 2)", "x14c_reflectionTex = nullptr;",
                     "++x2dc_reflectionAge;")

    def test_new_production_usage_is_h700_only(self):
        for path in ("src/MetroidPrime/main.cpp", "src/Kyoto/Graphics/DolphinCGraphics.cpp",
                     "src/MetaRender/CCubeRenderer.cpp"):
            stack = []
            for line in source(path).splitlines():
                directive = line.strip()
                if directive.startswith(("#if ", "#ifdef ", "#ifndef ")):
                    stack.append(True if directive == "#ifdef AURORA_H700" else
                                 False if directive == "#ifndef AURORA_H700" else None)
                elif directive.startswith("#else"):
                    if stack[-1] is not None:
                        stack[-1] = not stack[-1]
                elif directive.startswith("#endif"):
                    stack.pop()
                elif re.search(r"GameWorkMayContinue|GameFailure|port_h700_game_failure", line):
                    self.assertIn(True, stack, (path, line))


if __name__ == "__main__":
    unittest.main()
