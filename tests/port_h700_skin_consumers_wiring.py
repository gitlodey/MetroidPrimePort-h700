"""Second GM12 iteration: narrow bindings to actual skin consumers/workspace branches.

Source-order checks only, not game execution, allocation-failure injection, GPU
validation or proof of quiescence. Leaf failure producers remain a later step.
"""
import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from wiring_helpers import braced as extract_braced, squash

SKIN = "src/Kyoto/Animation/DolphinCSkinnedModel.cpp"
ANIM = "src/MetroidPrime/CAnimData.cpp"
MODEL = "src/MetroidPrime/CModelData.cpp"
SHADOW = "src/MetroidPrime/CProjectedShadow.cpp"
SWARMS = (
    ("CFishCloud", "src/MetroidPrime/ScriptObjects/CFishCloud.cpp", "InitAnimBoids",
     "x178_posWorkspaces", "x19c_nrmWorkspaces", "x230_whichModel", 4, "drawMask"),
    ("CSnakeWeedSwarm", "src/MetroidPrime/Enemies/CSnakeWeedSwarm.cpp", "InitAnimBoids",
     "x178_posWorkspaces", "x19c_nrmWorkspaces", "x1c4_which", 4, "posesToBuild"),
    ("CWallCrawlerSwarm", "src/MetroidPrime/Enemies/CWallCrawlerSwarm.cpp", "AllocateSkinnedModels",
     "x430_posWorkspaces", "x484_nrmWorkspaces", "x4dc_whichModel", 10, "drawMask"),
)
HEADERS = ("include/Kyoto/Animation/CSkinnedModel.hpp", "include/MetroidPrime/CAnimData.hpp")
GUARD = "if (PortH700::GameFailurePending()) return;"
BREAK = "if (PortH700::GameFailurePending()) break;"


def source(path):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", (ROOT / path).read_text(), flags=re.S)


def target(path, h700=True, later_version=False):
    # Select only the platform/version alternatives used by these bindings.
    # Unrelated conditionals remain source text; this is not a C++ preprocessor.
    choices = {
        "#ifdef AURORA_H700": h700,
        "#ifndef AURORA_H700": not h700,
        "#ifdef TARGET_PC": True,
        "#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02": later_version,
    }
    out, stack, active = [], [], True
    for line in source(path).splitlines():
        directive = line.strip()
        if directive.startswith(("#if ", "#ifdef ", "#ifndef ")):
            value = choices.get(directive)
            stack.append((active, value))
            if value is None and active:
                out.append(line)
            active = active and value is not False
        elif directive.startswith("#else"):
            parent, value = stack[-1]
            active = parent and value is not True
            if value is None and parent:
                out.append(line)
        elif directive.startswith("#elif"):
            parent, value = stack[-1]
            assert value is None, directive
            active = parent
            if active:
                out.append(line)
        elif directive.startswith("#endif"):
            parent, value = stack.pop()
            if value is None and parent:
                out.append(line)
            active = parent
        elif active:
            out.append(line)
    assert not stack, path
    return "\n".join(out)


def body(text, signature):
    return extract_braced(text, signature, signature)


class H700SkinConsumersWiring(unittest.TestCase):
    def ordered(self, text, *markers):
        text, cursor = squash(text), 0
        for marker in markers:
            marker = squash(marker)
            cursor = text.index(marker, cursor) + len(marker)

    def entry_guard(self, text):
        self.assertTrue(squash(text).startswith(squash("{ " + GUARD)), text)

    def test_platform_only_apis_and_unchanged_void_signatures(self):
        paths = (SKIN, ANIM, MODEL, SHADOW, *(row[1] for row in SWARMS), *HEADERS)
        for path in paths:
            with self.subTest(path=path):
                stack = []
                for line in source(path).splitlines():
                    directive = line.strip()
                    if directive.startswith(("#if ", "#ifdef ", "#ifndef ")):
                        stack.append(True if directive == "#ifdef AURORA_H700" else
                                     False if directive == "#ifndef AURORA_H700" else None)
                    elif directive.startswith("#else") and stack[-1] is not None:
                        stack[-1] = not stack[-1]
                    elif directive.startswith("#endif"):
                        stack.pop()
                    elif re.search(r"PortTry|PortH700::|port_h700_game_failure", line):
                        self.assertIn(True, stack, (path, line))
                self.assertNotRegex(target(path, h700=False), r"PortTry|PortH700::")
        skin, anim = (target(path) for path in HEADERS)
        self.assertIn("void Calculate(", skin)
        self.assertIn("bool PortTryCalculate(", skin)
        self.assertIn("void SetupRender(", anim)
        self.assertIn("bool PortTrySetupRender(", anim)

    def test_bool_wrappers_check_entry_then_actual_void_call_then_latch(self):
        for path, signature, call in (
            (SKIN, "bool CSkinnedModel::PortTryCalculate(",
             "Calculate(pose, morphEffect, averagedNormals, workVerts);"),
            (ANIM, "bool CAnimData::PortTrySetupRender(",
             "SetupRender(model, morphEffect, avgNormals);"),
        ):
            self.assertEqual(squash(body(target(path), signature)), squash("""{
                if (PortH700::GameFailurePending()) return false;
            """ + call + " return !PortH700::GameFailurePending(); }"))

    def test_all_four_skin_draw_entries_guard_before_fallback_or_callback(self):
        for signature in ("void CSkinnedModel::Draw(const CModelFlags&",
                          "void CSkinnedModel::Draw(const TDrawFunc",
                          "void CSkinnedModel::Draw(const float*"):
            self.entry_guard(body(target(SKIN), signature))
        self.entry_guard(body(target(HEADERS[0]), "static void Draw(const TDrawFunc"))

    def test_anim_consumers_do_not_draw_failed_setup(self):
        anim = target(ANIM)
        self.entry_guard(body(anim, "void CAnimData::DrawSkinnedModel("))
        self.entry_guard(body(anim, "void CAnimData::SetupRender("))
        self.ordered(body(anim, "void CAnimData::Render("),
                     "if (!PortTrySetupRender(model, morphEffect, avgNormals)) return;",
                     "DrawSkinnedModel(model, flags);")
        self.assertIn("PortTryCalculate(pose, morphEffect, avgNormals, nullptr)",
                      body(anim, "void CAnimData::PoseSkinnedModel("))

    def test_model_normal_frozen_and_five_explicit_setup_branches(self):
        model = target(MODEL)
        render = body(model, "void CModelData::Render(EWhichModel")
        self.entry_guard(render)
        for selected in ("*frozen", "model"):
            gate = "if (xc_animData->PortTrySetupRender(" + selected
            guarded_draw = body(render, gate)
            self.assertIn("DrawSkinnedModel(" + selected + ", flags);", guarded_draw)
        self.ordered(render, "SetAmbientColor(CColor::White());", "DisableAllLights();",
                     GUARD, "x14_24_renderSorted = false;")
        for method, owner, consumer in (
            ("RenderThermal", "xc_animData", "ThermalDraw(model,"),
            ("DisintegrateDraw(EWhichModel", "xc_animData", "SOneTextureDrawContext ctx("),
            ("FlatDraw(EWhichModel", "xc_animData", "SFlatDrawContext ctx("),
            ("MultiLightingDraw(EWhichModel", "GetAnimationData()", "SMultiLightingDrawContext ctx("),
            ("MultipassDraw(EWhichModel", "GetAnimationData()", "SMultipassDrawContext ctx("),
        ):
            branch = body(model, "void CModelData::" + method)
            self.entry_guard(branch)
            self.ordered(branch, "if (!" + owner + "->PortTrySetupRender(model,",
                         "nullptr)) return;", consumer)
            self.assertNotIn("->SetupRender(", branch)
        self.entry_guard(body(model, "void CModelData::ThermalDraw(CSkinnedModel& model, const float*"))

    def test_shadow_failure_skips_copy_and_publication_but_restores_state(self):
        for later_version in (False, True):
            shadow = target(SHADOW, later_version=later_version)
            signature = ("void CProjectedShadow::RenderShadowBuffer(CStateManager& mgr, int count,"
                         if later_version else
                         "void CProjectedShadow::RenderShadowBuffer(CStateManager& mgr, const CModelData&")
            render = body(shadow, signature)
            self.entry_guard(render)
            if later_version:
                self.ordered(render, "if (!animData->PortTrySetupRender(model,",
                             "nullptr)) break;", "model.Draw(", BREAK)
            else:
                draw = body(render, "if (animData->PortTrySetupRender(")
                self.ordered(draw, "SShadowDrawContext context(", "model.Draw(")
            copy = body(render, "if (!PortH700::GameFailurePending())")
            self.ordered(copy, "GetUseVideoFilter()", "GXCopyTex(", "SetUseVideoFilter(useVideoFilter)")
            self.ordered(render, copy, "SetViewPointMatrix(oldView)",
                         "SetProjectionState(oldProjection)", "SetViewport(oldViewport.mLeft,",
                         "SetDepthRange(oldNear, oldFar)",
                         "if (PortH700::GameFailurePending()) { x80_enabled = false; return; }",
                         "mgr.SetProjectedShadow(this);")
        self.entry_guard(body(target(SHADOW), "void CProjectedShadow::Render(const CStateManager&"))

    def test_swarm_partial_buffers_owned_locally_before_pair_and_model_commit(self):
        for cls, path, init, positions, normals, which, count, _ in SWARMS:
            with self.subTest(swarm=cls):
                init_body = body(target(path), "void " + cls + "::" + init + "(")
                self.entry_guard(init_body)
                self.ordered(init_body, positions + ".clear();", normals + ".clear();",
                             "rstl::reserved_vector< rstl::auto_ptr< rstl::game_memory< float > >, "
                             + str(count) + " > positions;",
                             "for (int i = 0; i < " + str(count) + "; ++i)",
                             "rstl::auto_ptr< rstl::game_memory< float > > workspace(",
                             "AllocateNewWorkspace(&normals)", GUARD,
                             "positions.push_back(workspace);", "normalWorkspaces.push_back(normals);",
                             "AdvanceAnimation(", GUARD,
                             positions + " = positions;", normals + " = normalWorkspaces;",
                             which + " = which;")
                self.assertNotIn(positions + ".push_back(", init_body)
                self.assertNotIn(normals + ".push_back(", init_body)
                self.assertNotIn("NoteGameFailure", init_body)
                self.assertNotIn("catch", init_body)

    def test_swarm_calculation_precedes_mask_change_draw_and_mask_commit(self):
        for cls, path, _, positions, normals, _, _, mask in SWARMS:
            with self.subTest(swarm=cls):
                render = body(target(path), "void " + cls + "::RenderBoid(")
                self.entry_guard(render)
                self.ordered(render, "animData.BuildPose();", "if (!model.PortTryCalculate(",
                             ".get())) return;", "mask &= ~", "model.Draw(", GUARD,
                             mask + " = mask;")
                self.assertNotIn("model.Calculate(", render)
                self.ordered(render, GUARD, positions + "[", normals + "[")

    def test_swarm_enclosing_loops_stop_and_keep_restore_tails(self):
        fish = body(target(SWARMS[0][1]), "void CFishCloud::Render(")
        snake = body(target(SWARMS[1][1]), "void CSnakeWeedSwarm::AddToRenderer(")
        wall = body(target(SWARMS[2][1]), "void CWallCrawlerSwarm::Render(")
        for render, count in ((fish, 2), (snake, 1), (wall, 5)):
            self.entry_guard(render)
            self.assertEqual(render.count(BREAK), count)
            self.ordered(render, BREAK, "RenderBoid(", "CCubeModel::sPortUnbaked = false;")
        self.ordered(wall, "CCubeModel::sPortUnbaked = false;", "DisableAllLights();", GUARD,
                     "DrawTouchBounds();")
        for cls, path, init, *_ in SWARMS[1:]:
            think = body(target(path), "void " + cls + "::Think(")
            self.ordered(think, init + "(mgr, which);", GUARD)
            registered = body(target(path), "void " + cls + "::AcceptScriptMsg(")
            self.ordered(registered, init + "(mgr, CModelData::kWM_Normal);", GUARD)

    def test_leaf_allocation_sizing_and_waits_have_no_new_failure_producers(self):
        skin = target(SKIN)
        for signature in ("void CSkinnedModel::Construct()", "void CSkinnedModel::Calculate(",
                          "void* CSkinnedModel::EnsureAllocation(", "void CSkinnedModel::AllocateStorage()",
                          "float* CSkinnedModel::AllocateNewWorkspace(", "void Skinning::AddSkinnedRef()",
                          "void CSkinnedModel::TickAllocations()"):
            self.assertNotRegex(body(skin, signature), r"PortH700::|PortTry|bad_alloc|catch\s*\(")


if __name__ == "__main__":
    unittest.main()
