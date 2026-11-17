"""Bind GM-11b CPU policy tests to actual root and retail pause cleanup paths."""
import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from wiring_helpers import braced as extract_braced, squash


def braced(text, signature):
    return extract_braced(text, signature, signature)


def source(path):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", (ROOT / path).read_text(), flags=re.S)


class H700TerminalWiring(unittest.TestCase):
    def test_actual_guards_before_throw_and_again_in_catch(self):
        main = source("src/MetroidPrime/main.cpp")
        guard = braced(main, "static bool PortH700CleanupObligationsEmpty()")
        self.assertEqual(squash(guard), squash("""{
          return aurora::h700::resident_resources_empty() &&
            PortRoomEnv::CanDestroyWithoutGraphicsCommands() &&
            PortRoomGeo::CanDestroyWithoutGraphicsCommands() &&
            PortRoomLiquid::CanDestroyWithoutGraphicsCommands();
        }"""))
        prepare = braced(main, "static void PortH700PrepareRootUnwind(const char* safePoint)")
        self.assertLess(prepare.index("RootCleanupAllowed(checkpoint, PortH700CleanupObligationsEmpty)"),
                        prepare.index("throw PortH700::RootTerminalToken"))
        self.assertIn("PortH700RootFailStop", prepare)
        catch = braced(main, "catch (const PortH700::RootTerminalToken& terminal)")
        self.assertLess(catch.index("RootCleanupAllowed(terminal.checkpoint, PortH700CleanupObligationsEmpty)"),
                        catch.index("CleanupTerminalOwners("))
        self.assertIn("return result;", catch)
        for forbidden in ("PortGameOwnerBoundary", "EndScene", "GXDrawDone", "poll_generation_", "aurora_shutdown"):
            self.assertNotIn(forbidden, catch)

    def test_live_owners_and_completed_initialization_milestones(self):
        main = source("src/MetroidPrime/main.cpp")
        root = braced(main, "int CMain::RsMain(")
        first_try = root.index("try {")
        for declaration in ("gameGlobalObjects;", "archSupport;", "TerminalMilestones terminalMilestones;"):
            self.assertLess(root.index(declaration), first_try)
        self.assertLess(root.index("InitializeSubsystems();"), root.index("terminalMilestones.subsystemsInitialized = true;"))
        self.assertLess(root.index("terminalMilestones.subsystemsInitialized = true;"),
                        root.index('PortH700RootSafePoint("boot: subsystems initialized")'))
        self.assertLess(root.index("ShutdownSubsystems();"), root.index("terminalMilestones.subsystemsRetired = true;"))
        self.assertLess(root.index("CARAMManager::Shutdown();"), root.index("terminalMilestones.aramRetired = true;"))
        entry = braced(main, 'extern "C" int metroid_main(')
        self.assertIn("result = gpMain->RsMain(argc, argv);", entry)
        self.assertIn("return result;", entry)

    def test_pause_dtors_reach_checkpoint_aware_delayed_flush(self):
        pause = source("src/MetroidPrime/CPauseScreen.cpp")
        for signature in ("CPauseScreen::~CPauseScreen()", "CPauseScreenBase::~CPauseScreenBase()"):
            self.assertEqual(squash(braced(pause, signature)),
                             "{CFrameDelayedKiller::StallAndFlushAllAllocations();}")
        delayed = source("src/Kyoto/CFrameDelayedKiller.cpp")
        flush = braced(delayed, "void CFrameDelayedKiller::StallAndFlushAllAllocations()")
        terminal = braced(flush, "if (aurora::h700::generation_cpu_quiescence_held())")
        self.assertEqual(squash(terminal), "{FlushAllAllocations();return;}")
        self.assertLess(flush.index(terminal), flush.index("GXDrawDone();"))
        self.assertEqual(squash(flush[flush.index("#endif") + len("#endif"):]),
                         "GXDrawDone();FlushAllAllocations();}")


if __name__ == "__main__":
    unittest.main()
