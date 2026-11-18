"""Source checks for the H700 chord's actual SDL -> normal quit wiring.

The companion C++ test executes the CPU state machine, not SDL or GPU shutdown.
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


class H700QuitWiring(unittest.TestCase):
    def ordered(self, text, *markers):
        text = squash(text)
        cursor = 0
        for marker in markers:
            cursor = text.index(squash(marker), cursor) + len(squash(marker))

    def test_both_game_event_loops_dispatch_before_ui_visibility(self):
        main = source("src/MetroidPrime/main.cpp")
        hook = """#ifdef AURORA_H700
          if (event->type == AURORA_SDL_EVENT) PortInputDevices::HandleH700QuitEvent(event->sdl);
          #endif"""
        self.assertEqual(squash(main).count(squash(hook)), 2)
        boot = braced(main, "void CGameGlobalObjects::AddPaksAndFactories()")
        self.ordered(boot, "aurora_update();", hook,
                     "if (event->type == AURORA_EXIT)", "gpMain->SetFinished();")
        root = braced(main, "int CMain::RsMain(")
        self.ordered(root, "aurora_update();", hook,
                     "if (event->type == AURORA_SDL_EVENT && !PortDebug::Visible())",
                     "if (event->type == AURORA_EXIT)", "PortSaveState::ResumeSave();",
                     "PortSaveState::ResumeQuitCleanly();", "x160_24_finished = true;")

    def test_adapter_uses_same_pad_events_and_existing_request(self):
        devices = source("platform/input/port_input_devices.cpp")
        self.assertIn("#ifdef AURORA_H700\nvoid HandleH700QuitEvent(", devices)
        handler = braced(devices, "void HandleH700QuitEvent(")
        for marker in ("SDL_EVENT_GAMEPAD_REMOVED", "sH700QuitChord.Remove(event.gdevice.which)",
                       "SDL_EVENT_GAMEPAD_BUTTON_DOWN", "SDL_EVENT_GAMEPAD_BUTTON_UP",
                       "SDL_GAMEPAD_BUTTON_BACK", "SDL_GAMEPAD_BUTTON_START"):
            self.assertIn(marker, handler)
        self.ordered(handler,
                     "if (sH700QuitChord.Update(event.gbutton.which, button, "
                     "event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN))",
                     "PortDebug::RequestQuit();")
        for forbidden in ("Visible(", "Capturing(", "RequestToggle(", "exit(", "_exit(", "kill("):
            self.assertNotIn(forbidden, handler)
        request = braced(source("platform/debug_ui/tabs.cpp"), "void RequestQuit()")
        self.ordered(request, "SaveSettings();", "quit.type = SDL_EVENT_QUIT;", "SDL_PushEvent(&quit);")

    def test_h700_reserves_chord_but_other_platforms_keep_overlay_binding(self):
        controls = braced(source("platform/input/port_input_devices.cpp"), "void BindPortControls(")
        start = controls.index("#ifndef AURORA_H700")
        end = controls.index("#endif", start)
        chord = controls[start:end]
        self.assertIn("SDL_GAMEPAD_BUTTON_BACK", chord)
        self.assertIn("SDL_GAMEPAD_BUTTON_START", chord)
        self.assertIn("b.action = Action::PortMenu;", chord)
        self.assertNotIn("SDL_GAMEPAD_BUTTON_BACK", controls[:start] + controls[end:])
        self.assertIn("SDL_SCANCODE_F1", controls[:start])

    def test_latched_quit_precedes_bindings_and_remap_capture(self):
        poll = braced(source("platform/input/port_input_devices.cpp"), "void PollPortActions()")
        self.ordered(poll, "#ifdef AURORA_H700", "if (sH700QuitChord.Requested()) return;",
                     "#endif", "BuildProfile(next);", "PortInputRemap::Capturing()",
                     "PortDebug::RequestToggle();")


if __name__ == "__main__":
    unittest.main()
