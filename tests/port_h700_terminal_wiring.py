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

    def test_native_audio_retirement_is_h700_only_and_does_not_quit_video(self):
        main = source("platform/main.cpp")
        signature = "static bool ShutdownH700NativeAudio()"
        self.assertIn("#ifdef AURORA_H700\n" + signature, main)
        native = braced(main, signature)
        owners = braced(native, "if (SDL_WasInit(SDL_INIT_AUDIO) != 0)")
        self.assertIn("return false;", owners)
        self.assertLess(native.index(owners), native.index("dlopen("))
        self.assertIn('dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD)', native)
        self.assertIn('dlsym(library, "SDL_WasInit")', native)
        self.assertIn('dlsym(library, "SDL_QuitSubSystem")', native)
        self.assertIn("constexpr Uint32 nativeAudio = 0x00000010;", native)
        self.assertIn("if (!wasInit || !quitSubSystem)", native)
        # Repeated SDL3 audio initialization must not leave accumulated SDL2 refs.
        self.assertIn("releasedRefs < 256", native)
        self.assertIn("wasInit(nativeAudio) != 0", native)
        self.assertIn("quitSubSystem(nativeAudio);", native)
        self.assertLess(native.rindex("wasInit(nativeAudio)"), native.index("dlclose(library)"))
        self.assertIn("return false;", braced(native, "if (wasInit(nativeAudio) != 0)"))
        for forbidden in ('"SDL_Quit"', "SDL_INIT_VIDEO", "aurora_dvd_close", "SDL_Delay"):
            self.assertNotIn(forbidden, native)

    def test_native_audio_success_log_follows_verified_zero_and_handle_release(self):
        native = braced(source("platform/main.cpp"), "static bool ShutdownH700NativeAudio()")
        self.assertIn("unsigned releasedRefs = 0;", native)
        self.assertIn("for (; releasedRefs < 256 && wasInit(nativeAudio) != 0; ++releasedRefs)", native)
        zero = native.index("if (wasInit(nativeAudio) != 0)")
        close = native.index("if (dlclose(library) != 0) return false;")
        log = native.index('PortLog::Write("port: H700 native audio released: refs=%u\\n", releasedRefs);')
        self.assertLess(zero, close)
        self.assertLess(close, log)
        self.assertLess(log, native.index("return true;"))
        self.assertEqual(native.count("H700 native audio released:"), 1)

    def test_audio_owners_balance_their_sdl3_audio_refs_before_native_cleanup(self):
        architecture = source("include/MetroidPrime/CGameArchitectureSupport.hpp")
        self.assertIn("CAudioSys x0_audioSys;", architecture)
        audio = braced(source("src/Kyoto/Audio/DolphinCAudioSys.cpp"), "CAudioSys::~CAudioSys()")
        self.assertIn("sndQuit();", audio)
        snd = braced(source("extern/musyx/src/musyx/runtime/snd_init.c"), "void sndQuit()")
        self.assertIn("hwExit();", snd)
        hw = braced(source("extern/musyx/src/musyx/runtime/hardware.c"), "void hwExit()")
        self.assertRegex(hw, r"#if MUSY_TARGET == MUSY_TARGET_PC\s+salExitAi\(\);\s+#endif")
        musyx = source("extern/musyx/src/musyx/runtime/hw_pc.c")
        init = braced(musyx, "bool salInitAi(")
        self.assertIn("salAudioSubsystemInitialized = SDL_InitSubSystem(SDL_INIT_AUDIO);", init)
        exit = braced(musyx, "bool salExitAi()")
        quit = braced(exit, "if (salAudioSubsystemInitialized)")
        self.assertEqual(squash(quit),
                         "{SDL_QuitSubSystem(SDL_INIT_AUDIO);salAudioSubsystemInitialized=false;}")
        self.assertLess(exit.index("SDL_DestroyAudioStream("), exit.index(quit))
        self.assertLess(exit.index("SDL_WaitThread("), exit.index(quit))
        ai = source("platform/ai_dma.cpp")
        self.assertIn("sAudioInitialized = SDL_InitSubSystem(SDL_INIT_AUDIO);", braced(ai, "void EnsureStream()"))
        shutdown = braced(ai, 'extern "C" void AIPortShutdown(')
        quit = braced(shutdown, "if (sAudioInitialized)")
        self.assertEqual(squash(quit),
                         "{SDL_QuitSubSystem(SDL_INIT_AUDIO);sAudioInitialized=false;}")
        self.assertLess(shutdown.index("sCallback = nullptr;"), shutdown.index("SDL_DestroyAudioStream("))
        self.assertLess(shutdown.index("SDL_DestroyAudioStream("), shutdown.index(quit))

    def test_native_audio_join_follows_owners_and_services_before_sdl_teardown(self):
        main = source("platform/main.cpp")
        entry = braced(main, "int main(")
        order = ("result = metroid_main(argc, argv);", "AIPortShutdown();",
                 "PortRemastered::StopImport();", "PortDiscVerify::Shutdown();",
                 "PortDiscReplace::Shutdown();", "if (!ShutdownH700NativeAudio())",
                 "PortH700::PollGenerationShutdown(", '"port: H700 normal exit released:',
                 "aurora_dvd_close();")
        # Earlier init/disc-error paths also poll/close; inspect the game-run tail only.
        tail = entry[entry.index(order[0]):]
        positions = [tail.index(item) for item in order]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(entry.count("ShutdownH700NativeAudio()"), 1)
        guarded = tail[tail.index("#ifdef AURORA_H700"):tail.index("#else")]
        self.assertIn("if (!ShutdownH700NativeAudio())", guarded)
        held = braced(guarded, "if (!ShutdownH700NativeAudio())")
        self.assertIn("fail-stop", held)
        self.assertIn("for (;;) std::this_thread::sleep_for", held)
        for forbidden in ("SDL_Delay", "aurora_shutdown", "aurora_dvd_close", "return ", "throw "):
            self.assertNotIn(forbidden, held)
        self.assertIn("shutdown.status != aurora::h700::ShutdownStatus::Released &&", guarded)
        self.assertIn("shutdown.status != aurora::h700::ShutdownStatus::FailedReleased", guarded)
        # Native audio cleanup must not escape an init/root Unknown fail-stop.
        self.assertNotIn("ShutdownH700NativeAudio()", entry[:entry.index(order[0])])
        game = source("src/MetroidPrime/main.cpp")
        root_stop = braced(game, "static void PortH700RootFailStop(")
        self.assertNotIn("ShutdownH700NativeAudio", root_stop)
        self.assertNotIn("aurora_dvd_close", root_stop)

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
