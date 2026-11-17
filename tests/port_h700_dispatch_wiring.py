"""GM12 dispatch source bindings: ordinary return to the existing root.

No game/GPU execution or synthetic rendering harness. Check the real callback,
pass and message boundaries, including restores which must precede the return.
"""
import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from wiring_helpers import braced, squash

GUARD = "if (PortH700::GameFailurePending()) return;"
BOOL_GUARD = "if (PortH700::GameFailurePending()) return false;"


def source(path):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", (ROOT / path).read_text(), flags=re.S)


def function(text, signature):
    # These definitions end at column zero. DrawWorld has mutually exclusive
    # preprocessor branches with opening braces, so raw brace counting is wrong.
    start = text.index(signature)
    return text[start:text.index("\n}", start) + 2]


class H700DispatchWiring(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.renderer = source("src/MetaRender/CCubeRenderer.cpp")
        cls.state = source("src/MetroidPrime/CStateManager.cpp")
        cls.game = source("src/MetroidPrime/CMFGame.cpp")
        cls.actor = source("src/MetroidPrime/CActor.cpp")
        cls.io = source("src/MetroidPrime/CIOWinManager.cpp")
        cls.main = source("src/MetroidPrime/main.cpp")

    def ordered(self, text, *markers):
        text = squash(text)
        cursor = 0
        for marker in markers:
            marker = squash(marker)
            cursor = text.index(marker, cursor) + len(marker)

    def guarded_call(self, text, call, guard=GUARD):
        self.assertIn(squash(call + "\n#ifdef AURORA_H700\n" + guard + "\n#endif"), squash(text))

    def test_renderer_entry_and_bucket_callback_stop_before_next_item(self):
        add = function(self.renderer, "void CCubeRenderer::AddDrawable(")
        self.ordered(add, GUARD, "if (sorting ==", "xa8_drawableCallback(", "Buckets::Insert(")
        buckets = function(self.renderer, "void CCubeRenderer::RenderBucketItems(")
        self.ordered(buckets, GUARD, "for (AUTO(bucketIdx,", "for (AUTO(drawableIt,",
                     "switch (drawType)", "CParticleGen*", "xa8_drawableCallback(",
                     GUARD, "lastType = drawType;")
        # The callback's callers must still leave the existing renderer scopes.
        sorted_draw = function(self.renderer, "void CCubeRenderer::DrawSortedGeometry(")
        self.ordered(sorted_draw, "RenderBucketItems(areaListItem);", "SetupCGraphicsStates();",
                     "DrawRenderBucketsDebug();", "Buckets::Clear();")
        drawables = function(self.renderer, "void CCubeRenderer::PortDrawSortedDrawables(")
        self.ordered(drawables, "sPortDrawablesOnly = true;", "DrawSortedGeometry(",
                     "sPortDrawablesOnly = false;")

    def test_recursive_parent_failure_skips_child_and_draw_token_publication(self):
        recursive = function(self.state, "void CStateManager::RecursiveDrawTree(")
        self.ordered(recursive, GUARD, "RecursiveDrawTree(nextNode);", GUARD,
                     "actor->Render(*this);", GUARD, "actor->SetDrawToken(")
        callback = function(self.state, "void CStateManager::RendererDrawCallback(")
        self.ordered(callback, GUARD, "mgr.RecursiveDrawTree(nextNode);", GUARD,
                     "CPortActorRenderScope presented(actor);", "actor.Render(mgr);", GUARD,
                     "actor.SetDrawToken(")

    def test_prerender_stops_before_next_actor_reflection_and_lights(self):
        pre = function(self.state, "void CStateManager::PreRender()")
        self.ordered(pre, GUARD, "x850_world->PreRender();", "BuildDynamicLightListForWorld();",
                     "actor->CalculateRenderBounds();", GUARD, "actor->PreRender(*this, frustum);",
                     GUARD, "CacheReflection();", GUARD, "gpRender->PrepareDynamicLights(")
        self.guarded_call(pre, "#endif")  # after either world PreRender branch
        self.guarded_call(pre, "actor->CalculateRenderBounds();")
        self.guarded_call(pre, "actor->PreRender(*this, frustum);")

    def test_world_actor_and_pass_calls_are_guarded_before_next_consumer(self):
        world = function(self.state, "void CStateManager::DrawWorld()")
        self.ordered(world, GUARD, "const auto drawPasses = [&]()", "renderFirst.begin()",
                     "actor->Render(*this);", GUARD, "PortDrawVolumetricFog(",
                     "actor->AddToRenderer(frustum, *this);", GUARD,
                     "xf7c_projectedShadow->Render(*this);", GUARD,
                     "gpRender->PortDrawSortedDrawables(", GUARD, "PortSceneCopyPoint();")
        for pattern in (r"actor->Render\(\*this\);", r"actor->AddToRenderer\(frustum, \*this\);",
                        r"x84c_player->AddToRenderer\(frustum, \*this\);",
                        r"portShadowOffscreen\(\*actor, i\);"):
            calls = list(re.finditer(pattern, world))
            self.assertTrue(calls, pattern)
            for match in calls:
                self.assertTrue(squash(world[match.end():]).startswith(
                    squash("#ifdef AURORA_H700\n" + GUARD + "\n#endif")), match.group())
        for call in ("shadow->Render(*this);", "x84c_player->Render(*this);",
                     "actor->PortAddThermalColdNode(frustum, *this);",
                     "gpRender->DrawUnsortedGeometry(id.Value(), mask, 0x20);",
                     "gpRender->DrawAreaGeometry(id.Value(), mask, 0x10);",
                     "gpRender->DrawSortedGeometry(id.Value(), mask, 0x10);",
                     "PortRoomGeo::DrawThermalHot(*this, *areas[i], frustum);"):
            self.guarded_call(world, call)
        self.ordered(world, "gpRender->DrawSortedGeometry(area.GetId().Value(), mask, targetMask);",
                     "PortRoomGeo::sReplacingArea = false;", GUARD, "PortShell::Flush();")

    def test_world_failure_restores_state_without_success_filters(self):
        world = function(self.state, "void CStateManager::DrawWorld()")
        tail = world[world.index("  drawPasses();"):]
        failure = braced(tail, "if (PortH700::GameFailurePending())", "world failure restore")
        self.ordered(failure, "PortRoomGeo::sReplacingArea = false;", "GXPortSetShadowCaster(false);",
                     "GXPortVolumetricFogEnd();", "sPortPBRThermal = CCubeMaterial::kPT_None;",
                     "PortSetThermalPass(PortVisorMesh::kTP_None);", "sPortThermalColdNodes = 0;",
                     "sPortVisorEffectsDeferred = false;", "x87c_fluidPlaneManager->EndFrame();",
                     "gpRender->SetThermal(false, 0.f, CColor::Black());", "xf34_thermalFlag = kTD_Bypass;",
                     "CGraphics::SetDepthRange(0.125f, 1.f);",
                     "ResetViewAfterDraw(backupViewport, backupViewMatrix);", "return;")
        self.assertNotIn("DrawE3DeathEffect", tail)
        self.assertNotIn("DrawAdditionalFilters", tail)
        # Keep the healthy final pipeline scope alive through the filters, just
        # as before splitting out the failure-return body.
        self.ordered(world, "const auto drawPasses = [&]()", "DrawDebugStuff();",
                     "CPortPipelineContextScope globalPipelines(AURORA_PIPELINE_CONTEXT_GLOBAL);",
                     "ResetViewAfterDraw(backupViewport, backupViewMatrix);",
                     "DrawE3DeathEffect();", "DrawAdditionalFilters();", "};", "drawPasses();", failure)
        self.ordered(world, "gpRender->DrawSortedGeometry(area.GetId().Value(), mask, targetMask);",
                     "PortRoomGeo::sReplacingArea = false;", GUARD)

    def test_gun_projection_is_restored_before_failure_return(self):
        world = function(self.state, "void CStateManager::DrawWorld()")
        self.ordered(world, "PortRenderDepth(", "CGraphics::SetProjectionState(gunProj);", GUARD)
        gun = world[world.index("x84c_player->RenderGun("):]
        self.ordered(gun, "if (gunFov) {", "CGraphics::SetProjectionState(gunProj);", GUARD,
                     "PortViewModel::Draw(*this);", GUARD, "renderLast.begin()",
                     "actor->Render(*this);", GUARD)
        self.ordered(gun, "arm.RenderGrappleBeam(", "if (PortH700::GameFailurePending()) {",
                     "if (gunFov) CGraphics::SetProjectionState(gunProj);", "return;", "if (muzzle)")

    def test_deferred_visor_loop_breaks_to_existing_restores(self):
        effects = function(self.state, "void CStateManager::PortDrawVisorEffects()")
        self.ordered(effects, GUARD, "sPortVisorEffectsDeferred = false;", "begin_visor_effects()",
                     "SetupViewForDraw(backupViewport);", "SetDepthRange(0.015625f, 0.03125f);",
                     "if (PortH700::GameFailurePending()) break;", "actor->Render(*this);",
                     "SetDepthRange(0.125f, 1.f);", "end_visor_effects();",
                     "ResetViewAfterDraw(backupViewport, backupViewMatrix);")
        after_begin = effects[effects.index("const CViewport backupViewport"):]
        self.assertNotIn("return;", after_begin)
        # Reflection callback itself already has an unconditional restore tail.
        reflected = function(self.state, "void CStateManager::DrawReflection(")
        self.ordered(reflected, "player->RenderReflectedPlayer(*this);",
                     "CGraphics::SetViewPointMatrix(backupView);",
                     "CGraphics::SetProjectionState(backupProj);")

    def test_think_failure_stops_update_before_relay_and_graveyard(self):
        think = function(self.state, "void CStateManager::Think(")
        self.assertEqual(think.count("ent->Think(dt, *this);"), 1)
        self.guarded_call(think, "ent->Think(dt, *this);")
        update = function(self.state, "void CStateManager::Update(")
        self.guarded_call(update, "Think(dt);")
        self.assertEqual(update.count("ent->Think(dt, *this);"), 1)
        self.guarded_call(update, "ent->Think(dt, *this);")
        self.ordered(update, "Think(dt);", GUARD, "PortRoomGeo::Think(*this, dt);",
                     "while (xf76_lastRelay != kInvalidUniqueId)", "ent->Think(dt, *this);",
                     GUARD, "PostUpdatePlayer(dt);", "x850_world->TravelToArea(",
                     "ClearGraveyard();")

    def test_update_message_failure_restores_random_before_nonremoving_exit(self):
        message = function(self.game, "CIOWin::EMessageReturn CMFGame::OnMessage(")
        self.assertEqual(message.count("mStateManager->Update(dt);"), 2)
        ingame = message[message.index("case kSMT_InGame:"):message.index("case kSMT_MapScreen:")]
        restore_exit = ("if (PortH700::GameFailurePending()) { "
                        "mStateManager->SetRandomAvailable(false); return kMR_Exit; }")
        self.guarded_call(ingame, "mStateManager->Update(dt);", restore_exit)
        self.ordered(ingame, "mStateManager->Update(dt);", restore_exit,
                     "mStateManager->GetWantsToQuit()")
        died = message[message.index("case kGFS_PlayerDied:"):]
        self.guarded_call(died, "mStateManager->Update(dt);\n"
                          "mStateManager->SetRandomAvailable(false);",
                          "if (PortH700::GameFailurePending()) return kMR_Exit;")
        self.ordered(died, "mStateManager->Update(dt);", "SetRandomAvailable(false);",
                     "if (PortH700::GameFailurePending()) return kMR_Exit;",
                     "mGuiManager->Update(")

    def test_actor_failure_leaves_time_scope_and_clears_hook_before_particles(self):
        render = function(self.actor, "void CActor::Render(")
        self.ordered(render, "mgr.SetupParticleHook(*this);", "if (xe5_29_globalTimeProvider)",
                     "RenderInternal(mgr);", "CTimeProvider tp(localTime);",
                     "RenderInternal(mgr);", "CSkinnedModel::ClearPointGeneratorFunc();",
                     GUARD, "mgr.GetActorModelParticles()->Render(mgr, *this);",
                     "RenderSystemsToBeDrawnLast();", "DrawTouchBounds();")
        # Both time-provider branches join here; its scope and the hook cleanup
        # must finish even on failure, before either particle-rendering tail.
        self.guarded_call(render, "RenderInternal(mgr);\n}\n"
                          "if (xe5_31_pointGeneratorParticles) {\n"
                          "CSkinnedModel::ClearPointGeneratorFunc();\n}")

    def test_failed_world_skips_postprocess_next_world_and_gui(self):
        game = function(self.game, "void CMFGame::Draw()")
        self.ordered(game, GUARD, "mStateManager->PreRender();", GUARD,
                     "mStateManager->DrawWorld();", "if (PortH700::GameFailurePending()) {",
                     "CCubeMaterial::sPortSceneHdr = false;", "return false;", "return requestSceneHdr;")
        scan = game[game.index("CStateManager::sPortViewZoom = scanZoom;"):]
        restore_return = "if (PortH700::GameFailurePending()) { CStateManager::sPortViewZoom = 1.f; return; }"
        self.ordered(scan, "const bool sceneHdr = drawWorld();", restore_return,
                     "postProcess(false, sceneHdr);", restore_return,
                     "mGuiManager->PortDrawScanZoomReticles(*mStateManager);",
                     "CStateManager::sPortViewZoom = 1.f;", GUARD, "CPlayerVisor::PortCopyScanZoom(",
                     "const bool sceneHdr = drawWorld();", GUARD, "postProcess(true, sceneHdr);", GUARD,
                     "mGuiManager->PreDraw(*mStateManager, IsCameraActiveFlow());", GUARD,
                     "mGuiManager->Draw(*mStateManager);", "CPlayerVisor::PortDropScanZoom();", GUARD)

    def test_iowin_status_precedes_action_dispatch_and_owner_removal(self):
        dispatch = function(self.io, "bool CIOWinManager::DistributeOneMessage(")
        self.ordered(dispatch, BOOL_GUARD, "iow = node->GetIOWin();", "iow->OnMessage(", BOOL_GUARD,
                     "while (!x8_localGatherQueue.IsEmpty())", "OnIOWinMessage(msg)",
                     "x8_localGatherQueue.Clear();", "queue.Clear();", "RemoveIOWin(iow);")
        pump = function(self.io, "void CIOWinManager::PumpMessages(")
        self.ordered(pump, "while (!queue.IsEmpty())", GUARD, "queue.Pop();", "DistributeOneMessage(msg, queue);")
        draw = function(self.io, "void CIOWinManager::Draw()")
        self.ordered(draw, GUARD, "node->GetIOWin()->PreDraw();", GUARD,
                     "node->GetIOWin()->GetIsContinueDraw()", "node->GetIOWin()->Draw();", GUARD,
                     "node->GetIOWin()->GetIsContinueDraw()")
        # This checkout has no manager pumping/message-safe flags. Do not invent
        # fields/ABI or move root event-pump restoration into nested dispatch.
        header = source("include/MetroidPrime/CIOWinManager.hpp")
        self.assertNotIn("mIsPumping", header + self.io)
        self.assertNotIn("SetEventPumpSafe", dispatch + pump)
        root = function(self.main, "int CMain::RsMain(")
        self.ordered(root, "SetEventPumpSafe(true);", "aurora_update();", "SetEventPumpSafe(false);",
                     'PortH700RootSafePoint("loop: event pump returned, snapshot safety cleared");')

    def test_tick_message_failure_skips_next_tick_and_success_accounting(self):
        ticks = function(self.main, "bool CGameArchitectureSupport::UpdateTicks()")
        self.ordered(ticks, BOOL_GUARD, "OSDisableInterrupts();", "OSRestoreInterrupts(interrupts);",
                     "CreateTimerTick(", "x58_ioWinMgr.PumpMessages(x4_archQueue);", BOOL_GUARD,
                     "++sTicksAdvanced;", "x58_ioWinMgr.PumpMessages(x4_archQueue);", BOOL_GUARD,
                     "return !terminate;")
        update = function(self.main, "void CGameArchitectureSupport::Update()")
        self.ordered(update, GUARD, "TouchModels();", GUARD, "CreateFrameEnd(", "PumpMessages(")
        root = function(self.main, "int CMain::RsMain(")
        self.ordered(root, "archSupport->UpdateTicks();",
                     'PortH700RootSafePoint("loop: game tick returned, before draw");')

    def test_new_guards_are_h700_only_and_do_not_produce_or_checkpoint(self):
        for text in (self.renderer, self.state, self.game, self.actor, self.io, self.main):
            stack = []
            for line in text.splitlines():
                directive = line.strip()
                if directive.startswith(("#if ", "#ifdef ", "#ifndef ")):
                    stack.append(True if directive == "#ifdef AURORA_H700" else
                                 False if directive == "#ifndef AURORA_H700" else None)
                elif directive.startswith("#else"):
                    if stack[-1] is not None:
                        stack[-1] = not stack[-1]
                elif directive.startswith("#endif"):
                    stack.pop()
                elif "GameFailurePending" in line or "port_h700_game_failure" in line:
                    self.assertIn(True, stack, line)
        bodies = [function(self.renderer, "void CCubeRenderer::AddDrawable("),
                  function(self.renderer, "void CCubeRenderer::RenderBucketItems("),
                  function(self.state, "void CStateManager::Think("),
                  function(self.state, "void CStateManager::Update("),
                  function(self.state, "void CStateManager::RecursiveDrawTree("),
                  function(self.state, "void CStateManager::RendererDrawCallback("),
                  function(self.state, "void CStateManager::PreRender()"),
                  function(self.state, "void CStateManager::DrawWorld()"),
                  function(self.state, "void CStateManager::PortDrawVisorEffects()"),
                  function(self.game, "void CMFGame::Draw()"),
                  function(self.game, "CIOWin::EMessageReturn CMFGame::OnMessage("),
                  function(self.actor, "void CActor::Render("),
                  function(self.io, "void CIOWinManager::Draw()"),
                  function(self.io, "void CIOWinManager::PumpMessages("),
                  function(self.io, "bool CIOWinManager::DistributeOneMessage("),
                  function(self.main, "bool CGameArchitectureSupport::UpdateTicks()"),
                  function(self.main, "void CGameArchitectureSupport::Update()")]
        for body in bodies:
            for forbidden in ("NoteGameFailure", "throw ", "poll_generation_", "synchronize_game_work",
                              "PortH700PrepareRootUnwind", "PortH700RootSafePoint"):
                self.assertNotIn(forbidden, body)


if __name__ == "__main__":
    unittest.main()
