// F1 overlay: The settings file: one table of keys (kSettingKeys) that reads and writes
// port_settings.ini, saving it, and the settings self-test. State and helpers shared with the other
// parts are in debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_gyro_motion.h"
#include "port_photo.h"
#include "port_third_person.h"
#include "port_pointer.h"
#include "port_room_geo.h"
#include "port_savestate.h"
#include "port_log.h"
#include "port_paths.h"
#include "port_input_devices.h"
#include "port_gci.h"
#include "port_discord.h"
#include "port_beam_colours.h"
#include "port_suit_colours.h"
#include "port_dread_samus_mod.h"
#include "port_echoes_suits_mod.h"
#include "port_textures.h"
#include "port_utf8_path.h"

#include <dolphin/gx/GXExtra.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace {

// One key of port_settings.ini: how its value is read and how it is written.
// The table is the file's order. ApplySetting reads through it, SettingsText
// writes through it, and SettingsSelfTest checks that each key's write reads
// back as the same value.
struct SettingKey {
  // nullptr: a block of keys another module writes (PortInput), read by it too.
  const char* key;
  void (*read)(const std::string& value);
  // The value only, after "key="; nullptr for an old name that is only read.
  void (*write)(std::ostream& out);
  // Write no line while the value is empty.
  bool omitEmpty;
  // Two different values as written, which the self-test applies in turn and
  // expects back; nullptr when the written form is not the one read (a path
  // that is normalised, a value with its own text format).
  const char* sampleA;
  const char* sampleB;
};

void WriteBool(std::ostream& out, bool value) { out << (value ? 1 : 0); }

// dynamic_res, the old key, was saved as 1 with auto render scale too, where it did
// nothing; with auto it now runs. Read apart, and settled in ApplySettingsText once the
// whole text (and its render_scale) is in: only on a fixed scale does it stay on.
int sLegacyDynamicRes = -1; // -1: not in the text
bool sDynamicResRead = false; // dynamic_resolution, the new key, was

// crosshair_color, crosshair_target_color: RRGGBB, or empty for none.
void ReadColour(const std::string& value, bool& set, float rgb[3]) {
  unsigned packed = 0;
  set = value.size() == 6 && std::sscanf(value.c_str(), "%6x", &packed) == 1;
  if (set) {
    for (int i = 0; i < 3; ++i) {
      rgb[i] = static_cast< float >((packed >> (16 - 8 * i)) & 0xff) / 255.f;
    }
  }
}

void WriteColour(std::ostream& out, bool set, const float rgb[3]) {
  char hex[8] = "";
  if (set) {
    unsigned packed = 0;
    for (int i = 0; i < 3; ++i) {
      packed = packed << 8 | static_cast< unsigned >(std::lround(std::clamp(rgb[i], 0.f, 1.f) * 255.f));
    }
    std::snprintf(hex, sizeof(hex), "%06x", packed);
  }
  out << hex;
}

// hud_opacity_*: a percent, or "visor" to follow the game's Visor Opacity.
void ReadHudOpacity(PortDebug::HudOpacityPart part, const std::string& value) {
  if (value == "visor") {
    sHudOpacity[part] = PortDebug::kHudOpacityVisor;
    return;
  }
  const int s = std::atoi(value.c_str());
  if (s >= PortDebug::kHudOpacityMin && s <= PortDebug::kHudOpacityMax) {
    sHudOpacity[part] = s;
  }
}

void WriteHudOpacity(std::ostream& out, PortDebug::HudOpacityPart part) {
  if (sHudOpacity[part] == PortDebug::kHudOpacityVisor) {
    out << "visor";
  } else {
    out << sHudOpacity[part];
  }
}

// suit_colour_<suit>: PortSuitColours::PicksKey, a slot each ("FFD000,-").
template < int Suit >
void ReadSuitColours(const std::string& value) {
  PortSuitColours::SetPicksKey(Suit, value);
}

template < int Suit >
void WriteSuitColours(std::ostream& out) {
  out << PortSuitColours::PicksKey(Suit);
}

// beam_colour_<beam>: "RRGGBB", or empty for the beam's own.
template < int Beam >
void ReadBeamColour(const std::string& value) {
  PortBeamColours::SetColour(Beam, PortSuitColours::ParseColour(value));
}

template < int Beam >
void WriteBeamColour(std::ostream& out) {
  out << PortSuitColours::FormatColour(PortBeamColours::Colour(Beam));
}

// A key that is one bool: read with ParseBool, written as 1 or 0, sampled with both.
template < bool* Flag >
constexpr SettingKey BoolKey(const char* key) {
  return {key, [](const std::string& value) { *Flag = ParseBool(value); },
          [](std::ostream& out) { WriteBool(out, *Flag); }, false, "1", "0"};
}

const SettingKey kSettingKeys[] = {
    {"aspect",
     [](const std::string& value) {
       if (value == "16:9") {
         sAspectMode = PortDebug::kAspect_16_9;
       } else if (value == "window") {
         sAspectMode = PortDebug::kAspect_Window;
       } else if (value == "4:3") {
         sAspectMode = PortDebug::kAspect_4_3;
       }
     },
     [](std::ostream& out) {
       out << (sAspectMode == PortDebug::kAspect_16_9    ? "16:9"
               : sAspectMode == PortDebug::kAspect_Window ? "window"
                                                          : "4:3");
     },
     false, "16:9", "window"},
    BoolKey< &sHudWide >("hud_wide"),
    BoolKey< &sCinemaBars >("cinema_bars"),
    BoolKey< &sBallBars >("ball_bars"),
    BoolKey< &sPhazonShell >("phazon_shell"),
    BoolKey< &sSharpScanWindow >("sharp_scan_window"),
    BoolKey< &sGeneratedBallLooks >("power_spider_ball"),
    {"suit_colour_power", &ReadSuitColours< 0 >, &WriteSuitColours< 0 >, true, "FF4000,-", "-,2050FF"},
    {"suit_colour_gravity", &ReadSuitColours< 1 >, &WriteSuitColours< 1 >, true, "FF4000,-,-", "-,-,2050FF"},
    {"suit_colour_varia", &ReadSuitColours< 2 >, &WriteSuitColours< 2 >, true, "FF4000,-", "-,2050FF"},
    {"suit_colour_phazon", &ReadSuitColours< 3 >, &WriteSuitColours< 3 >, true, "FF4000,-", "-,2050FF"},
    {"suit_colour_fusion", &ReadSuitColours< 4 >, &WriteSuitColours< 4 >, true, "FF4000,-,-", "-,-,2050FF"},
    {"suit_colour_fusion_gravity", &ReadSuitColours< 5 >, &WriteSuitColours< 5 >, true, "FF4000,-,-", "-,-,2050FF"},
    {"suit_colour_fusion_varia", &ReadSuitColours< 6 >, &WriteSuitColours< 6 >, true, "FF4000,-,-", "-,-,2050FF"},
    {"suit_colour_fusion_phazon", &ReadSuitColours< 7 >, &WriteSuitColours< 7 >, true, "FF4000,-,-", "-,-,2050FF"},
    {"suit_colour_dread_power", &ReadSuitColours< 8 >, &WriteSuitColours< 8 >, true, "FF4000,-,-", "-,2050FF,FFE0C0"},
    {"suit_colour_dread_gravity", &ReadSuitColours< 9 >, &WriteSuitColours< 9 >, true, "FF4000,-,-,-",
     "-,-,2050FF,FFE0C0"},
    {"suit_colour_dread_varia", &ReadSuitColours< 10 >, &WriteSuitColours< 10 >, true, "FF4000,-,-", "-,2050FF,FFE0C0"},
    {"suit_colour_dread_phazon", &ReadSuitColours< 11 >, &WriteSuitColours< 11 >, true, "FF4000,-", "-,2050FF"},
    {"beam_colour_power", &ReadBeamColour< 0 >, &WriteBeamColour< 0 >, true, "33FF66", "FF00FF"},
    {"beam_colour_ice", &ReadBeamColour< 1 >, &WriteBeamColour< 1 >, true, "33FF66", "FF00FF"},
    {"beam_colour_wave", &ReadBeamColour< 2 >, &WriteBeamColour< 2 >, true, "33FF66", "FF00FF"},
    {"beam_colour_plasma", &ReadBeamColour< 3 >, &WriteBeamColour< 3 >, true, "33FF66", "FF00FF"},
    {"beam_colour_phazon", &ReadBeamColour< 4 >, &WriteBeamColour< 4 >, true, "33FF66", "FF00FF"},
    BoolKey< &sAutoAim >("auto_aim"),
    {"scan_speed",
     [](const std::string& value) {
       const int s = std::atoi(value.c_str());
       if (s >= PortDebug::kScanSpeedMin && s <= PortDebug::kScanSpeedMax) {
         sScanSpeed = s;
       }
     },
     [](std::ostream& out) { out << sScanSpeed; }, false, "100", "500"},
    // The F1 Game > Audio rows' part above 100 percent; EnsureInitialized hands it to the mixers.
    {"music_boost",
     [](const std::string& value) {
       const int b = std::atoi(value.c_str());
       if (b >= PortDebug::kVolumeBoostMin && b <= PortDebug::kVolumeBoostMax) {
         sMusicBoost = b;
       }
     },
     [](std::ostream& out) { out << sMusicBoost; }, false, "100", "200"},
    {"sfx_boost",
     [](const std::string& value) {
       const int b = std::atoi(value.c_str());
       if (b >= PortDebug::kVolumeBoostMin && b <= PortDebug::kVolumeBoostMax) {
         sSfxBoost = b;
       }
     },
     [](std::ostream& out) { out << sSfxBoost; }, false, "100", "200"},
    {"hint_delay",
     [](const std::string& value) {
       const int m = std::atoi(value.c_str());
       if (m >= PortDebug::kHintDelayMin && m <= PortDebug::kHintDelayMax) {
         sHintDelay = m;
       }
     },
     [](std::ostream& out) { out << sHintDelay; }, false, "0", "30"},
    BoolKey< &sSecondaryCrosshairs >("secondary_crosshairs"),
    BoolKey< &sCrosshairXRay >("crosshair_xray"),
    BoolKey< &sCrosshairThermal >("crosshair_thermal"),
    BoolKey< &sMenuStandardButtons >("menu_standard_buttons"),
    BoolKey< &sMenuExit >("menu_exit"),
    {"pointer_menus", [](const std::string& value) { PortPointer::SetEnabled(ParseBool(value)); },
     [](std::ostream& out) { WriteBool(out, PortPointer::Enabled()); }, false, "1", "0"},
    {"map_swap_fingers", [](const std::string& value) { PortPointer::SetMapSwapFingers(ParseBool(value)); },
     [](std::ostream& out) { WriteBool(out, PortPointer::MapSwapFingers()); }, false, "1", "0"},
    {"prompt_glyphs",
     [](const std::string& value) {
       for (int i = 0; i < int(std::size(kPromptGlyphs)); ++i) {
         if (value == kPromptGlyphs[i]) {
           sPromptGlyphs.store(i, std::memory_order_relaxed);
         }
       }
     },
     [](std::ostream& out) { out << kPromptGlyphs[sPromptGlyphs.load(std::memory_order_relaxed)]; }, false,
     "playstation", "keyboard"},
    BoolKey< &sShowShaderCompilation >("show_shader_compilation"),
    BoolKey< &sShowMenuHint >("show_menu_hint"),
    BoolKey< &sShowFpsCounter >("show_fps_counter"),
    {"hud_scale",
     [](const std::string& value) {
       const int s = std::atoi(value.c_str());
       if (s >= PortDebug::kHudScaleMin && s <= PortDebug::kHudScaleMax) {
         sHudScale = s;
       }
     },
     [](std::ostream& out) { out << sHudScale; }, false, "50", "100"},
    {"scan_spread",
     [](const std::string& value) {
       const int s = std::atoi(value.c_str());
       if (s >= PortDebug::kScanSpreadMin && s <= PortDebug::kScanSpreadMax) {
         sScanSpread = s;
       }
     },
     [](std::ostream& out) { out << sScanSpread; }, false, "0", "100"},
    {"crosshair_opacity",
     [](const std::string& value) {
       const int s = std::atoi(value.c_str());
       if (s >= PortDebug::kCrosshairOpacityMin && s <= PortDebug::kCrosshairOpacityMax) {
         sCrosshairOpacity = s;
       }
     },
     [](std::ostream& out) { out << sCrosshairOpacity; }, false, "0", "100"},
    {"crosshair_style",
     [](const std::string& value) {
       if (value == "classic") {
         sCrosshairStyle = PortDebug::kCrosshairStyle_Classic;
       } else if (value == "remastered") {
         sCrosshairStyle = PortDebug::kCrosshairStyle_Remastered;
       } else if (value == "gamecube") {
         sCrosshairStyle = PortDebug::kCrosshairStyle_GameCube;
       }
     },
     [](std::ostream& out) {
       out << (sCrosshairStyle == PortDebug::kCrosshairStyle_Remastered ? "remastered"
               : sCrosshairStyle == PortDebug::kCrosshairStyle_GameCube ? "gamecube"
                                                                         : "classic");
     },
     false, "remastered", "classic"},
    {"hud_opacity_xray", [](const std::string& value) { ReadHudOpacity(PortDebug::kHudOpacityXRay, value); },
     [](std::ostream& out) { WriteHudOpacity(out, PortDebug::kHudOpacityXRay); }, false, "visor", "40"},
    {"hud_opacity_thermal", [](const std::string& value) { ReadHudOpacity(PortDebug::kHudOpacityThermal, value); },
     [](std::ostream& out) { WriteHudOpacity(out, PortDebug::kHudOpacityThermal); }, false, "visor", "40"},
    {"hud_opacity_scan", [](const std::string& value) { ReadHudOpacity(PortDebug::kHudOpacityScan, value); },
     [](std::ostream& out) { WriteHudOpacity(out, PortDebug::kHudOpacityScan); }, false, "visor", "40"},
    {"hud_opacity_energy", [](const std::string& value) { ReadHudOpacity(PortDebug::kHudOpacityEnergy, value); },
     [](std::ostream& out) { WriteHudOpacity(out, PortDebug::kHudOpacityEnergy); }, false, "visor", "40"},
    {"crosshair_color", [](const std::string& value) { ReadColour(value, sCrosshairCustomColor, sCrosshairColor); },
     [](std::ostream& out) { WriteColour(out, sCrosshairCustomColor, sCrosshairColor); }, false, "ff8000", ""},
    {"crosshair_target_color",
     [](const std::string& value) { ReadColour(value, sCrosshairTargetCustomColor, sCrosshairTargetColor); },
     [](std::ostream& out) { WriteColour(out, sCrosshairTargetCustomColor, sCrosshairTargetColor); }, false,
     "ff0000", ""},
    BoolKey< &sHideHelmet >("hide_helmet"),
    {"third_person", [](const std::string& value) { PortThirdPerson::SetEnabled(ParseBool(value)); },
     [](std::ostream& out) { WriteBool(out, PortThirdPerson::Enabled()); }, false, "1", "0"},
    {"third_person_distance",
     [](const std::string& value) { PortThirdPerson::SetDistance(std::strtof(value.c_str(), nullptr)); },
     [](std::ostream& out) { out << PortThirdPerson::GetParams().distance; }, false, "2.5", "4"},
    {"third_person_shoulder",
     // Every save writes every key, so files written before 69b52d4a3 hold the old defaults
     // (shoulder 0.6, height 0.1), which put the crosshair on her pad: those move to the new ones.
     [](const std::string& value) {
       const float shoulder = std::strtof(value.c_str(), nullptr);
       PortThirdPerson::SetShoulder(shoulder == 0.6f ? PortThirdPerson::Params{}.shoulder : shoulder);
     },
     [](std::ostream& out) { out << PortThirdPerson::GetParams().shoulder; }, false, "0.7", "-0.5"},
    {"third_person_height",
     [](const std::string& value) {
       const float height = std::strtof(value.c_str(), nullptr);
       PortThirdPerson::SetHeight(height == 0.1f ? PortThirdPerson::Params{}.height : height);
     },
     [](std::ostream& out) { out << PortThirdPerson::GetParams().height; }, false, "0.5", "0"},
    BoolKey< &sHideVisorEffects >("hide_visor_effects"),
    BoolKey< &sRevealMap >("reveal_map"),
    {"map_pickups",
     [](const std::string& value) {
       // Old hand-edited on/true/yes meant the all-pickups dots.
       const int mode = ParseBool(value) ? PortDebug::kMapPickups_All : std::atoi(value.c_str());
       sMapPickups = mode >= PortDebug::kMapPickups_Off && mode <= PortDebug::kMapPickups_Rooms
                         ? mode
                         : PortDebug::kMapPickups_Off;
     },
     [](std::ostream& out) { out << sMapPickups; }, false, "2", "3"},
    BoolKey< &sTrackerProgress >("tracker_progress"),
    BoolKey< &sMapLogicColors >("map_logic_colors"),
    BoolKey< &sMapPickupKinds >("map_pickup_kinds"),
    BoolKey< &sMapStationPickups >("map_station_pickups"),
    BoolKey< &sMapExtraUi >("map_extra_ui"),
    {"ap_suit_damage",
     [](const std::string& value) {
       const int mode = std::atoi(value.c_str());
       sApSuitDamage = mode >= 0 && mode <= 2 ? mode : 1;
     },
     [](std::ostream& out) { out << sApSuitDamage; }, false, "0", "2"},
    BoolKey< &sCheats >("cheats"),
    BoolKey< &sSkippableCutscenes >("skippable_cutscenes"),
    BoolKey< &sBeamDoorsStayUnlocked >("beam_doors_stay_unlocked"),
    BoolKey< &sStackBeams >("stack_beams"),
    {"beam_switch_speed",
     [](const std::string& value) {
       const int s = std::atoi(value.c_str());
       if (s >= PortDebug::kBeamSwitchSpeedMin && s <= PortDebug::kBeamSwitchSpeedMax) {
         sBeamSwitchSpeed = s;
       }
     },
     [](std::ostream& out) { out << sBeamSwitchSpeed; }, false, "100", "400"},
    BoolKey< &sSeamlessPickups >("seamless_pickups"),
    BoolKey< &sWalkSpeedEnabled >("walk_speed_enabled"),
    {"walk_speed",
     [](const std::string& value) {
       const int s = std::atoi(value.c_str());
       if (s >= PortDebug::kWalkSpeedMin && s <= PortDebug::kWalkSpeedMax) {
         sWalkSpeed = s;
       }
     },
     [](std::ostream& out) { out << sWalkSpeed; }, false, "50", "200"},
    BoolKey< &sRevision100Compatibility >("revision_100_compatibility"),
    // Normalised on the way in (NormalizeEmulatorFolder), so not sampled.
    {"emulator_folder", [](const std::string& value) { PortGci::SetEmulatorFolder(value); },
     [](std::ostream& out) { out << PortGci::EmulatorFolder(); }, false, nullptr, nullptr},
    {"share_emulator_card", [](const std::string& value) { PortGci::SetShareEmulatorCard(ParseBool(value)); },
     [](std::ostream& out) { WriteBool(out, PortGci::ShareEmulatorCard()); }, false, "1", "0"},
    {"texture_pack", [](const std::string& value) { PortTextures::SetUserPackEnabled(ParseBool(value)); },
     [](std::ostream& out) { WriteBool(out, PortTextures::UserPackEnabled()); }, false, "0", "1"},
    {"text_language", [](const std::string& value) { sTextLanguage = value; },
     [](std::ostream& out) { out << sTextLanguage; }, false, "fr", "de"},
    {"elevator_ride",
     [](const std::string& value) {
       const int mode = std::atoi(value.c_str());
       sElevatorRide = mode >= PortDebug::kElevatorRide_Original && mode <= PortDebug::kElevatorRide_Skip
                           ? mode
                           : PortDebug::kElevatorRide_Original;
     },
     [](std::ostream& out) { out << sElevatorRide; }, false, "1", "2"},
    {"boot_logos",
     [](const std::string& value) {
       const int mode = std::atoi(value.c_str());
       sBootLogos = mode >= PortDebug::kBootLogos_Show && mode <= PortDebug::kBootLogos_Off
                        ? mode
                        : PortDebug::kBootLogos_Skippable;
     },
     [](std::ostream& out) { out << sBootLogos; }, false, "0", "2"},
    BoolKey< &sSaveStateHotkeys >("savestate_hotkeys"),
    BoolKey< &sQuickResume >("quick_resume"),
    // The save state slot F5 / F9 act on (PortSaveState::SelectedSlot), kept between launches.
    {"state_slot",
     [](const std::string& value) {
       PortSaveState::SetSelectedSlot(std::atoi(value.c_str())); // out of range is ignored there
     },
     [](std::ostream& out) { out << PortSaveState::SelectedSlot(); }, false, "3", "7"},
    BoolKey< &sSaveStateRoomState >("savestate_room_state"),
    {"fov",
     [](const std::string& value) {
       const float f = static_cast< float >(std::atof(value.c_str()));
       if (std::isfinite(f) && f >= PortDebug::kFovMin && f <= PortDebug::kFovMax) {
         sFirstPersonFov = f;
       }
     },
     [](std::ostream& out) { out << sFirstPersonFov; }, false, "45", "72.5"},
    BoolKey< &sGunFollowsFov >("gun_follows_fov"),
    BoolKey< &sBallFollowsFov >("ball_follows_fov"),
    {"gun_fov",
     [](const std::string& value) {
       const float f = static_cast< float >(std::atof(value.c_str()));
       if (std::isfinite(f) && f >= PortDebug::kFovMin && f <= PortDebug::kFovMax) {
         sGunFov = f;
       }
     },
     [](std::ostream& out) { out << sGunFov; }, false, "45", "72.5"},
    {"gun_offset",
     [](const std::string& value) {
       float v[3] = {};
       if (std::sscanf(value.c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]) == 3) {
         for (int i = 0; i < 3; ++i) {
           sGunOffset[i] =
               std::isfinite(v[i]) ? std::clamp(v[i], -PortDebug::kGunOffsetMax, PortDebug::kGunOffsetMax) : 0.f;
         }
       }
     },
     [](std::ostream& out) { out << sGunOffset[0] << ',' << sGunOffset[1] << ',' << sGunOffset[2]; }, false,
     "0.5,-0.25,1", "0,0,0"},
    {"msaa", [](const std::string& value) { sMsaa = std::atoi(value.c_str()) >= 4 ? 4 : 1; },
     [](std::ostream& out) { out << sMsaa; }, false, "4", "1"},
    {"opengles",
     [](const std::string& value) {
       sOpenGles = ParseBool(value);
       sOpenGlesAtStart = sOpenGles;
     },
     [](std::ostream& out) { WriteBool(out, sOpenGles); }, false, "1", "0"},
    {"graphics_backend",
     [](const std::string& value) {
       // An unknown value keeps what is already there (the default, Auto, at
       // startup), so a hand-edited or older file never breaks the start.
       PortDebug::EGraphicsBackend parsed = sGraphicsBackend;
       if (PortDebug::ParseGraphicsBackend(value, parsed)) {
         sGraphicsBackend = parsed;
         sGraphicsBackendAtStart = parsed;
       }
     },
     [](std::ostream& out) { out << PortDebug::GraphicsBackendConfigName(sGraphicsBackend); }, false, "d3d12",
     "vulkan"},
    {"gpu_driver",
     [](const std::string& value) {
       sGpuDriver = value;
       sGpuDriverAtStart = value;
     },
     [](std::ostream& out) { out << sGpuDriver; }, false, "turnip", ""},
    {"anisotropy",
     [](const std::string& value) {
       const int a = std::atoi(value.c_str());
       if (a >= 1 && a <= 16) {
         sAnisotropy = a;
       }
     },
     [](std::ostream& out) { out << sAnisotropy; }, false, "1", "16"},
    BoolKey< &sPerPixelLighting >("per_pixel_lighting"),
    BoolKey< &sSunShadows >("sun_shadows"),
    BoolKey< &sProjectedShadows >("projected_shadows"),
    BoolKey< &sUnlockHardMode >("unlock_hard_mode"),
    {"damage_taken",
     [](const std::string& value) {
       const float f = std::strtof(value.c_str(), nullptr);
       if (std::isfinite(f) && f >= PortDebug::kDamageMultMin && f <= PortDebug::kDamageMultMax) {
         sDamageTakenMult = f;
       }
     },
     [](std::ostream& out) { out << sDamageTakenMult; }, false, "0.5", "3"},
    {"damage_dealt",
     [](const std::string& value) {
       const float f = std::strtof(value.c_str(), nullptr);
       if (std::isfinite(f) && f >= PortDebug::kDamageMultMin && f <= PortDebug::kDamageMultMax) {
         sDamageDealtMult = f;
       }
     },
     [](std::ostream& out) { out << sDamageDealtMult; }, false, "0.5", "3"},
    BoolKey< &sUnlockFusionSuit >("unlock_fusion_suit"),
    BoolKey< &sUnlockGalleries >("unlock_galleries"),
    {"storage_clamp",
     [](const std::string& value) {
       const int v = std::atoi(value.c_str());
       sStorageClamp = v < 0 ? -1 : (v > 0 ? 1 : 0);
       sStorageClampAtStart = sStorageClamp;
     },
     [](std::ostream& out) { out << sStorageClamp; }, false, "-1", "1"},
    {"driver_workarounds",
     [](const std::string& value) {
       sDriverWorkarounds = ParseDriverWorkarounds(value);
       sDriverWorkaroundsAtStart = sDriverWorkarounds;
     },
     [](std::ostream& out) {
       const char* sep = "";
       for (size_t i = 0; i < kDriverWorkaroundCount; ++i) {
         if (sDriverWorkarounds & (1u << i)) {
           out << sep << kDriverWorkarounds[i].key;
           sep = ",";
         }
       }
     },
     true, "keep_depth", "unmonitored_fence,validation"},
    BoolKey< &sSpeedrunTimer >("speedrun_timer"),
    BoolKey< &sLiveSplit >("livesplit"),
    {"livesplit_address",
     [](const std::string& value) {
       if (!value.empty()) {
         sLiveSplitAddress = value;
       }
     },
     [](std::ostream& out) { out << sLiveSplitAddress; }, false, "127.0.0.1:16834", "localhost:1"},
    BoolKey< &sLiveSplitSplitUpgrades >("livesplit_split_upgrades"),
    // Not "discord": 0.16.0 saved discord=0 for everyone while it was opt-in.
    BoolKey< &sDiscord >("discord_presence"),
    BoolKey< &sUpdateCheck >("update_check"),
    BoolKey< &sModsEnabled >("mods"),
    BoolKey< &sOriginalExperience >("original_experience"),
    {"mods_disabled", [](const std::string& value) { sModsDisabled = value; },
     [](std::ostream& out) { out << sModsDisabled; }, false, "a,b", ""},
    {"dread_first_person_cannon",
     [](const std::string& value) { PortDreadSamus::SetFirstPersonCannonShown(ParseBool(value)); },
     [](std::ostream& out) { WriteBool(out, PortDreadSamus::FirstPersonCannonShown()); }, false, "0", "1"},
    {"echoes_first_person_cannon",
     [](const std::string& value) { PortEchoesSuits::SetFirstPersonCannonShown(ParseBool(value)); },
     [](std::ostream& out) { WriteBool(out, PortEchoesSuits::FirstPersonCannonShown()); }, false, "0", "1"},
    {"echoes_dark_samus", [](const std::string& value) { PortEchoesSuits::SetPlayAsDarkSamus(ParseBool(value)); },
     [](std::ostream& out) { WriteBool(out, PortEchoesSuits::PlayAsDarkSamus()); }, false, "0", "1"},
    BoolKey< &sVsyncEnabled >("vsync"),
    BoolKey< &sFullscreen >("fullscreen"),
    BoolKey< &sOverlayWindowed >("overlay_windowed"),
    {"ui_theme",
     [](const std::string& value) {
       sUiTheme = SDL_strcasecmp(value.c_str(), "prime") == 0        ? 1
                  : SDL_strcasecmp(value.c_str(), "remastered") == 0 ? 2
                  : SDL_strcasecmp(value.c_str(), "plain") == 0      ? 3
                                                                     : 0;
     },
     [](std::ostream& out) {
       out << (sUiTheme == 1 ? "prime" : sUiTheme == 2 ? "remastered" : sUiTheme == 3 ? "plain" : "auto");
     },
     false, "remastered", "auto"},
    BoolKey< &sUiSounds >("ui_sounds"),
    {"photo_supersample", [](const std::string& value) { PortPhoto::SetSupersample(std::atoi(value.c_str())); },
     [](std::ostream& out) { out << PortPhoto::Supersample(); }, false, "4", "1"},
    {"photo_speed",
     [](const std::string& value) {
       const float f = static_cast< float >(std::atof(value.c_str()));
       if (std::isfinite(f)) {
         PortPhoto::SetSpeed(f);
       }
     },
     [](std::ostream& out) { out << PortPhoto::Speed(); }, false, "0.5", "20"},
    {"render_scale",
     [](const std::string& value) {
       const float f = static_cast< float >(std::atof(value.c_str()));
       if (std::isfinite(f) && f >= 0.f && f <= PortDebug::kRenderScaleMax) {
         sRenderScale = f;
       }
     },
     [](std::ostream& out) { out << sRenderScale; }, false, "0.5", "2"},
    {"dynamic_resolution",
     [](const std::string& value) {
       sDynamicRes = ParseBool(value);
       sDynamicResRead = true;
     },
     [](std::ostream& out) { WriteBool(out, sDynamicRes); }, false, "1", "0"},
    {"dynamic_res", [](const std::string& value) { sLegacyDynamicRes = ParseBool(value) ? 1 : 0; }, nullptr, false,
     nullptr, nullptr},
    {"dynamic_res_target",
     [](const std::string& value) { sDynamicResTarget = std::clamp(std::atoi(value.c_str()), 0, kFpsCapMax); },
     [](std::ostream& out) { out << sDynamicResTarget; }, false, "0", "60"},
    {"dynamic_res_min",
     [](const std::string& value) {
       const float f = static_cast< float >(std::atof(value.c_str()));
       if (std::isfinite(f)) {
         sDynamicResMin = std::clamp(f, PortDebug::kDynamicResMinLowest, 4.f);
       }
     },
     [](std::ostream& out) { out << sDynamicResMin; }, false, "0.5", "0.75"},
    {"upscale_sharpen",
     [](const std::string& value) {
       sSharpenMode = value == "off" ? 0 : value == "always" ? 2 : 1;
     },
     [](std::ostream& out) { out << (sSharpenMode == 0 ? "off" : sSharpenMode == 2 ? "always" : "dynamic"); },
     false, "off", "always"},
    {"upscale_sharpen_amount",
     [](const std::string& value) { sSharpenAmount = std::clamp(std::atoi(value.c_str()), 0, 100); },
     [](std::ostream& out) { out << sSharpenAmount; }, false, "0", "80"},
    BoolKey< &sFrameLimitEnabled >("frame_limit"),
    {"fps_cap", [](const std::string& value) { sFpsCap = ClampFpsCap(std::atoi(value.c_str())); },
     [](std::ostream& out) { out << sFpsCap; }, false, "0", "144"},
    {"sim_rate",
     [](const std::string& value) {
       const long rate = std::strtol(value.c_str(), nullptr, 10);
       if (rate >= 30 && rate <= 480) {
         sSimRate = static_cast< unsigned >(rate);
       }
     },
     [](std::ostream& out) { out << sSimRate; }, false, "30", "120"},
    BoolKey< &sSimAdaptive >("sim_adaptive"),
    {"smooth_frames",
     [](const std::string& value) {
       sSmoothFrames = ParseBool(value);
       sFrameInterpolation = sActorInterpolation = sPoseInterpolation = sParticleInterpolation = sSmoothFrames;
     },
     [](std::ostream& out) { WriteBool(out, sSmoothFrames); }, false, "1", "0"},
    BoolKey< &sRoomGeoResident >("room_geo_gpu"),
    // The old key, saved as 0 by everyone while it was off by default: only an explicit 1 counts.
    {"room_geo_resident", [](const std::string& value) { sRoomGeoResident = sRoomGeoResident || ParseBool(value); },
     nullptr, false, nullptr, nullptr},
    {"room_geo_min_px",
     [](const std::string& value) { PortRoomGeo::SetMinPixels(std::strtof(value.c_str(), nullptr)); },
     [](std::ostream& out) { out << PortRoomGeo::MinPixels(); }, false, "0", "3.5"},
    {"room_geo_lod",
     [](const std::string& value) { PortRoomGeo::SetLodDistance(std::strtof(value.c_str(), nullptr)); },
     [](std::ostream& out) { out << PortRoomGeo::LodDistance(); }, false, "0.5", "2"},
    {nullptr, nullptr, [](std::ostream& out) { PortInput::WriteControlSettings(out); }, false, nullptr, nullptr},
    BoolKey< &sInvulnerable >("invulnerable"),
    BoolKey< &sPhazonBeamCheat >("cheat_phazon_beam"),
    // Not "log_file": builds from before the log was on by default wrote
    // log_file=0 into every settings file, which kept it off after an update.
    BoolKey< &sLogFile >("logging"),
    {nullptr, nullptr, [](std::ostream& out) { PortInput::WriteGameplaySettings(out); }, false, nullptr, nullptr},
    // Its own JSON text (PortRandoGen::SettingsText), so not sampled here.
    {"rando_settings",
     [](const std::string& value) {
       PortRandoGen::Settings parsed;
       if (!PortRandoGen::ParseSettings(value, parsed)) {
         parsed = PortRandoGen::Settings();
       }
       std::lock_guard< std::mutex > lock(sRandoMutex);
       sRandoSettings = parsed;
     },
     [](std::ostream& out) {
       std::lock_guard< std::mutex > lock(sRandoMutex);
       out << PortRandoGen::SettingsText(sRandoSettings);
     },
     false, nullptr, nullptr},
    {nullptr, nullptr, [](std::ostream& out) { PortInput::WriteAimSettings(out); }, false, nullptr, nullptr},
    {"game_options", [](const std::string& value) { sGameOptions = value; },
     [](std::ostream& out) { out << sGameOptions; }, true, "0a1b", "ff"},
    {"disc_path", [](const std::string& value) { sDiscPath = value; }, [](std::ostream& out) { out << sDiscPath; },
     true, "/games/Metroid Prime #1 (v1.00).iso", "C:\\Games\\mp.rvz"},
    {"remastered_nsp", [](const std::string& value) { sRemasteredImagePath = value; },
     [](std::ostream& out) { out << sRemasteredImagePath; }, true, "/dumps/mpr.nsp", "/dumps/mpr.xci"},
    {"remastered_keys", [](const std::string& value) { sRemasteredKeysPath = value; },
     [](std::ostream& out) { out << sRemasteredKeysPath; }, true, "/keys/prod.keys", "/k/p.keys"},
    {"remastered_skip", [](const std::string& value) { sRemasteredSkip = value; },
     [](std::ostream& out) { out << sRemasteredSkip; }, true, "geometry,movies", "hud"},
    {"remastered_hide", [](const std::string& value) { sRemasteredHide = value; },
     [](std::ostream& out) { out << sRemasteredHide; }, true, "geometry,lighting", "hud"},
    {"remastered_performance",
     [](const std::string& value) { GXPortSetPerformance(ParseBool(value) ? GX_PORT_PERF_ALL : 0); },
     [](std::ostream& out) { out << (GXPortGetPerformance() != 0 ? 1 : 0); }, true, "1", "0"},
};

// False for a key nothing reads.
bool ApplySetting(const std::string& key, const std::string& value) {
  if (PortInput::ApplySetting(key, value)) {
    return true;
  }
  for (const SettingKey& entry : kSettingKeys) {
    if (entry.key != nullptr && key == entry.key) {
      entry.read(value);
      return true;
    }
  }
  return false;
}

// Applies the key=value lines of a settings file. Only whole-line comments:
// values are written raw, and a path may contain '#'.
void ApplySettingsText(std::istream& text) {
  sLegacyDynamicRes = -1;
  sDynamicResRead = false;
  std::string line;
  while (std::getline(text, line)) {
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#') {
      continue;
    }
    const size_t separator = line.find('=');
    if (separator == std::string::npos) {
      continue;
    }
    const std::string key = Trim(line.substr(0, separator));
    const std::string value = Trim(line.substr(separator + 1));
    if (!key.empty()) {
      ApplySetting(key, value);
    }
  }
  if (sLegacyDynamicRes >= 0 && !sDynamicResRead) {
    sDynamicRes = sLegacyDynamicRes == 1 && sRenderScale > 0.f;
  }
}

// key=value lines of a settings text, comments skipped.
std::map< std::string, std::string > SettingsValues(const std::string& text) {
  std::map< std::string, std::string > values;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const size_t separator = line.find('=');
    if (!line.empty() && line[0] != '#' && separator != std::string::npos) {
      values[line.substr(0, separator)] = line.substr(separator + 1);
    }
  }
  return values;
}

#if defined(AURORA_H700)
// Captured once by the same initialization path used before LoadSettings.
std::string sH700DefaultSettings;
bool sH700DefaultInterpolationOff = false;
bool sH700DefaultStockAudio = false;
#endif

} // namespace

namespace DebugUi {

// The settings file's text, as written by SaveSettings and copied into the log.
std::string SettingsText() {
  std::ostringstream file;
  file << "# Metroid Prime native port settings. Written by the F1 debug overlay.\n";
  file << "# Environment variables (MP_*) override these for a single run.\n";
  for (const SettingKey& entry : kSettingKeys) {
    if (entry.write == nullptr) {
      continue;
    }
    if (entry.key == nullptr) {
      entry.write(file);
      continue;
    }
    std::ostringstream value;
    entry.write(value);
    if (entry.omitEmpty && value.str().empty()) {
      continue;
    }
    file << entry.key << '=' << value.str() << '\n';
  }
  return file.str();
}

#if defined(AURORA_H700)
void ApplyH700Defaults() {
  if (!sH700DefaultSettings.empty()) {
    return;
  }
  // Initial choices only: the INI and per-run environment still load afterwards.
  sAspectMode = PortDebug::kAspect_4_3;
  sRenderScale = 1.f;
  sDynamicRes = false;
  sMsaa = 1;
  sAnisotropy = 1;
  sModsEnabled = false;
  PortTextures::SetUserPackEnabled(false);
  sPerPixelLighting = false;
  sSmoothFrames = false;
  sFrameInterpolation = sActorInterpolation = sPoseInterpolation = sParticleInterpolation = false;
  sRoomGeoResident = false;
  sSunShadows = false;
  sSharpScanWindow = false;
  // Keep retail projected shadows, visor effects and the stock audio balance.
  PortInput::ApplySetting("pad_preset", "modern");
  sInput.twinStick = true;
  sFrameLimitEnabled = false;
  sFpsCap = 30;
  sSimRate = 60;
  sSimAdaptive = false;
  sQuickResume = false;
  PortGci::SetShareEmulatorCard(false);
  sFullscreen = true;

  sH700DefaultSettings = SettingsText();
  sH700DefaultInterpolationOff =
      !sFrameInterpolation && !sActorInterpolation && !sPoseInterpolation && !sParticleInterpolation;
  sH700DefaultStockAudio = sAiAudioEnabled && sMusyxAudioEnabled &&
                          sMusicBoost == PortDebug::kVolumeBoostDefault &&
                          sSfxBoost == PortDebug::kVolumeBoostDefault;
  PortLog::Write("port: H700 settings baseline (defaults before INI/environment): "
                 "aspect=4:3 render_scale=1 dynamic_resolution=0 msaa=1 anisotropy=1 "
                 "mods=0 texture_pack=0 per_pixel_lighting=0 smooth_frames=0 room_geo_gpu=0 "
                 "sun_shadows=0 sharp_scan_window=0 projected_shadows=1 "
                 "pad_preset=modern twin_stick=1 frame_limit=0 fps_cap=30 sim_rate=60 sim_adaptive=0 "
                 "quick_resume=0 share_emulator_card=0 fullscreen=1; retail effects/audio unchanged\n");
}
#endif

void LoadSettings() {
  const std::string path = SettingsFilePath();
  std::ifstream file(PortUtf8::FromUtf8(path));
  if (!file.is_open()) {
    return;
  }
  PortLog::Write("metroid_prime_port: loaded settings from %s\n", path.c_str());
  ApplySettingsText(file);
  PortInput::PostLoad();
}

// Paths in the log keep only their file name: the folders say nothing about a bug.
std::string LoggedSettingValue(const std::string& key, const std::string& value) {
  if (key == "disc_path" || key == "remastered_nsp" || key == "remastered_keys") {
    const size_t slash = value.find_last_of("/\\");
    return slash == std::string::npos ? value : value.substr(slash + 1);
  }
  return value;
}

// The settings last copied into the log; SaveSettings logs what changed since.
std::map< std::string, std::string > sLoggedSettings;
bool sSettingsLogged = false;

void LogSettingChanges(const std::string& text) {
  std::map< std::string, std::string > values = SettingsValues(text);
  if (sSettingsLogged) {
    for (const auto& [key, value] : values) {
      const auto old = sLoggedSettings.find(key);
      const std::string before = old == sLoggedSettings.end() ? "(unset)" : LoggedSettingValue(key, old->second);
      if (old == sLoggedSettings.end() || old->second != value) {
        PortLog::Write("port: setting %s: %s -> %s\n", key.c_str(), before.c_str(),
                       LoggedSettingValue(key, value).c_str());
      }
    }
    for (const auto& [key, value] : sLoggedSettings) {
      if (values.find(key) == values.end()) {
        PortLog::Write("port: setting %s: %s -> (unset)\n", key.c_str(), LoggedSettingValue(key, value).c_str());
      }
    }
  }
  sLoggedSettings = std::move(values);
}

// Settings an MP_* variable changed this run: key -> (file value, override value).
std::map< std::string, std::pair< std::string, std::string > > sEnvOverrides;

void NoteEnvOverrides(const std::string& fileText) {
  const auto before = SettingsValues(fileText);
  for (const auto& [key, value] : SettingsValues(SettingsText())) {
    const auto old = before.find(key);
    if (old != before.end() && old->second != value) {
      sEnvOverrides[key] = {old->second, value};
    }
  }
}

// The text to store: a setting still at its MP_* override keeps the file's
// value, so a per-run variable (MP_ORIGINAL=1 in a launcher) never becomes a
// saved setting when something else is saved. One changed since is stored.
std::string StoredSettingsText() {
  std::string text = SettingsText();
  if (sEnvOverrides.empty()) {
    return text;
  }
  std::string stored;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const size_t separator = line.find('=');
    if (!line.empty() && line[0] != '#' && separator != std::string::npos) {
      const auto found = sEnvOverrides.find(line.substr(0, separator));
      if (found != sEnvOverrides.end() && line.compare(separator + 1, std::string::npos, found->second.second) == 0) {
        line.resize(separator + 1);
        line += found->second.first;
      }
    }
    stored += line;
    stored += '\n';
  }
  return stored;
}

void SaveSettings() {
  if (!sInitialized || !sSettingsDirty || sConfigReset) {
    return;
  }
  LogSettingChanges(SettingsText());
  const std::string text = StoredSettingsText();
  const std::string path = SettingsFilePath();
  // Write a temporary file and rename it over the settings, so a crash or kill
  // mid-write (this runs on every slider release and at exit) cannot leave a
  // truncated file that resets everything on the next start.
  const std::filesystem::path target = PortUtf8::FromUtf8(path);
  std::filesystem::path partial = target;
  partial += ".tmp";
  std::ofstream file(partial, std::ios::trunc);
  if (!file.is_open()) {
    PortLog::Write("metroid_prime_port: could not write settings to %s\n", path.c_str());
    return;
  }
  file << text;
  file.flush();
  if (!file) {
    PortLog::Write("metroid_prime_port: could not write settings to %s (write failed)\n", path.c_str());
    return;
  }
  file.close();
  if (file.fail()) {
    PortLog::Write("metroid_prime_port: could not write settings to %s (close failed)\n", path.c_str());
    return;
  }
  std::error_code ec;
  std::filesystem::rename(partial, target, ec);
  if (ec) {
    PortLog::Write("metroid_prime_port: could not replace settings at %s: %s\n", path.c_str(), ec.message().c_str());
    std::filesystem::remove(partial, ec);
    return;
  }
  PortLog::Write("metroid_prime_port: saved settings to %s\n", path.c_str());
  sSettingsDirty = false;
}

} // namespace DebugUi

namespace PortDebug {

#if defined(AURORA_H700)
int H700SettingsPersistenceSelfTest(const char* mode, const char* fixtureDirectory) {
  namespace fs = std::filesystem;
  const auto fail = [](int code, const char* detail) {
    std::fprintf(stderr, "H700 settings persistence self-test: %s\n", detail);
    return code;
  };
  const bool write = mode != nullptr && std::string(mode) == "write";
  if ((!write && (mode == nullptr || std::string(mode) != "read")) || fixtureDirectory == nullptr) {
    return fail(2, "expected write|read and an explicit absolute fixture directory");
  }
  // Validate before SettingsFilePath can resolve/cache any default or personal folder.
  const char* user = std::getenv("MP_USER_PATH");
  const fs::path directory = PortUtf8::FromUtf8(fixtureDirectory);
  std::error_code ec;
  if (!directory.is_absolute() || user == nullptr || std::string(fixtureDirectory) != user ||
      !fs::is_directory(directory, ec) || ec) {
    return fail(2, "fixture must be an existing absolute directory exactly matching explicit MP_USER_PATH");
  }
  const fs::path target = PortUtf8::FromUtf8(SettingsFilePath());
  if (target.lexically_normal() != (directory / "port_settings.ini").lexically_normal()) {
    return fail(2, "SettingsFilePath did not resolve inside the explicit fixture");
  }
  const fs::file_status status = fs::symlink_status(target, ec);
  if (ec && ec != std::errc::no_such_file_or_directory) {
    return fail(2, "could not inspect fixture INI");
  }
  if (write) {
    if (fs::exists(status)) {
      return fail(2, "refusing to overwrite an existing INI");
    }
    fs::path partial = target;
    partial += ".tmp";
    ec.clear();
    const fs::file_status partialStatus = fs::symlink_status(partial, ec);
    if ((ec && ec != std::errc::no_such_file_or_directory) || fs::exists(partialStatus)) {
      return fail(2, "refusing to overwrite an existing or inaccessible INI temporary file");
    }
  } else if (!fs::is_regular_file(status)) {
    return fail(2, "read requires an existing regular fixture INI");
  }
  if (sInitialized) {
    return fail(2, "fixture must run before normal settings initialization");
  }

  // Only this early-exit fixture enables SaveSettings locally. EnsureInitialized
  // would register exit persistence and start unrelated network/audio services.
  sInitialized = true;
  sSettingsDirty = false;
  ApplyH700Defaults();
  if (sHudScale == 90 || PortInput::Get().shiftBindings[2] == 9 || !sShowFpsCounter) {
    return fail(1, "fixture values must differ from startup defaults");
  }
  if (write) {
    // Real table reader and PortInput parser/writer, not a synthetic INI serializer.
    // SDL gamepad button 9 is the left shoulder; the default shift_pad is unbound.
    if (!ApplySetting("hud_scale", "90") || !ApplySetting("show_fps_counter", "0") ||
        !PortInput::ApplySetting("pad_preset", "modern") ||
        !PortInput::ApplySetting("shift_pad", "9")) {
      return fail(1, "fixture setting keys were not accepted");
    }
    sSettingsDirty = true;
    SaveSettings(); // Actual .tmp write and atomic rename; only success clears dirty.
    if (sSettingsDirty) {
      return fail(1, "SaveSettings did not complete");
    }
    std::ifstream file(target);
    if (!file.is_open()) {
      return fail(1, "saved fixture INI is missing");
    }
    const auto values = SettingsValues(std::string(std::istreambuf_iterator<char>(file), {}));
    const auto matches = [&](const char* key, const char* expected) {
      const auto found = values.find(key);
      return found != values.end() && found->second == expected;
    };
    if (file.bad() || !matches("hud_scale", "90") || !matches("pad_preset", "modern") ||
        !matches("shift_pad", "9") || !matches("show_fps_counter", "0")) {
      return fail(1, "saved fixture values do not match");
    }
  } else {
    // A separate process reaches here with defaults, never the write-mode values.
    LoadSettings();
  }
  if (sHudScale != 90 || PortInput::Get().padPreset != 2 || PortInput::Get().shiftBindings[2] != 9 ||
      sShowFpsCounter || sSettingsDirty) {
    return fail(1, "expected hud_scale=90 pad_preset=modern shift_pad=9 show_fps_counter=0 without pending save");
  }
  std::fprintf(stderr, "H700 settings persistence self-test: %s PASS hud_scale=90 pad_preset=modern shift_pad=9 show_fps_counter=0\n",
               mode);
  return 0;
}

int H700SettingsSelfTest() {
  // Use the actual one-time startup initializer, not a separate test preset.
  // Do not call EnsureInitialized: it reads files and registers persistence.
  ApplyH700Defaults();
  const struct {
    const char* key;
    const char* baseline;
    const char* overrideValue;
  } cases[] = {
      {"aspect", "4:3", "16:9"},
      {"render_scale", "1", "2"},
      {"dynamic_resolution", "0", "1"},
      {"msaa", "1", "4"},
      {"anisotropy", "1", "16"},
      {"mods", "0", "1"},
      {"texture_pack", "0", "1"},
      {"per_pixel_lighting", "0", "1"},
      {"smooth_frames", "0", "1"},
      {"room_geo_gpu", "0", "1"},
      {"sun_shadows", "0", "1"},
      {"sharp_scan_window", "0", "1"},
      {"projected_shadows", "1", "0"},
      {"hide_visor_effects", "0", "1"},
      {"scan_speed", "100", "500"},
      {"original_experience", "0", "1"},
      {"pad_preset", "modern", "gamecube"},
      {"twin_stick", "1", "0"},
      {"frame_limit", "0", "1"},
      {"fps_cap", "30", "45"},
      {"sim_rate", "60", "120"},
      {"sim_adaptive", "0", "1"},
      {"quick_resume", "0", "1"},
      {"share_emulator_card", "0", "1"},
      {"fullscreen", "1", "0"},
      {"music_boost", "100", "200"},
      {"sfx_boost", "100", "200"},
      {"show_fps_counter", "1", "0"},
  };
  int failures = 0;
  const auto check = [&](bool passed, const char* detail) {
    if (!passed) {
      PortLog::Write("H700 settings self-test: %s\n", detail);
      ++failures;
    }
  };
  const auto checkValues = [&](const std::string& text, bool overrides, const char* phase) {
    const auto values = SettingsValues(text);
    for (const auto& entry : cases) {
      const auto found = values.find(entry.key);
      const std::string got = found == values.end() ? "(missing)" : found->second;
      const char* expected = overrides ? entry.overrideValue : entry.baseline;
      if (got != expected) {
        PortLog::Write("H700 settings self-test: %s %s: expected %s, got %s\n", phase, entry.key,
                       expected, got.c_str());
        ++failures;
      }
    }
  };
  checkValues(sH700DefaultSettings, false, "startup defaults");
  check(sH700DefaultInterpolationOff, "startup interpolation is not fully off");
  check(sH700DefaultStockAudio, "startup stock audio changed");

  // Save/restore only the tested in-memory keys. Reading all of SettingsText
  // would also run unrelated graphics setters (e.g. remastered_performance).
  const auto before = SettingsValues(SettingsText());
  const bool frameInterpolation = sFrameInterpolation;
  const bool actorInterpolation = sActorInterpolation;
  const bool poseInterpolation = sPoseInterpolation;
  const bool particleInterpolation = sParticleInterpolation;
  const int padPreset = sInput.padPreset; // -1 has no written pad_preset line
  const int legacyDynamicRes = sLegacyDynamicRes;
  const bool dynamicResRead = sDynamicResRead;
  const bool dirty = sSettingsDirty;
  std::string restoreText;
  std::string baselineText;
  std::string overrideText;
  const auto defaults = SettingsValues(sH700DefaultSettings);
  for (const auto& entry : cases) {
    if (const auto found = before.find(entry.key); found != before.end()) {
      restoreText += std::string(entry.key) + "=" + found->second + "\n";
    }
    if (const auto found = defaults.find(entry.key); found != defaults.end()) {
      baselineText += std::string(entry.key) + "=" + found->second + "\n";
    }
    overrideText += std::string(entry.key) + "=" + entry.overrideValue + "\n";
  }
  std::istringstream baseline(baselineText);
  ApplySettingsText(baseline);
  std::istringstream empty("# No saved choices yet.\n");
  ApplySettingsText(empty);
  checkValues(SettingsText(), false, "absent settings");
  check(!sFrameInterpolation && !sActorInterpolation && !sPoseInterpolation && !sParticleInterpolation,
        "default smooth_frames does not disable every interpolation component");

  std::istringstream existing(overrideText);
  ApplySettingsText(existing);
  // Re-entering the initializer must never reset already loaded choices.
  ApplyH700Defaults();
  checkValues(SettingsText(), true, "existing settings override");
  check(sFrameInterpolation && sActorInterpolation && sPoseInterpolation && sParticleInterpolation,
        "saved smooth_frames does not restore every interpolation component");
  check(sSettingsDirty == dirty, "defaults/INI parsing requested a save");

  std::istringstream restore(restoreText);
  ApplySettingsText(restore);
  sFrameInterpolation = frameInterpolation;
  sActorInterpolation = actorInterpolation;
  sPoseInterpolation = poseInterpolation;
  sParticleInterpolation = particleInterpolation;
  sInput.padPreset = padPreset;
  sLegacyDynamicRes = legacyDynamicRes;
  sDynamicResRead = dynamicResRead;
  sSettingsDirty = dirty;
  PortLog::Write("H700 settings self-test: %zu keys checked (defaults and existing overrides), %d failure(s)\n",
                 std::size(cases), failures);
  return failures == 0 ? 0 : 1;
}
#endif

int SettingsSelfTest() {
  int failures = 0;
  int sampled = 0;
  // Each sampled key, through the file reader: both values must come back as written.
  for (const SettingKey& entry : kSettingKeys) {
    if (entry.key == nullptr || entry.write == nullptr || entry.sampleA == nullptr) {
      continue;
    }
    ++sampled;
    for (const char* sample : {entry.sampleA, entry.sampleB}) {
      std::istringstream line(std::string(entry.key) + "=" + sample + "\n");
      ApplySettingsText(line);
      const auto written = SettingsValues(SettingsText());
      const auto found = written.find(entry.key);
      const std::string got = found == written.end() ? std::string() : found->second;
      if (got != sample) {
        PortLog::Write("settings self-test: %s=%s reads back as %s=%s\n", entry.key, sample, entry.key,
                       got.c_str());
        ++failures;
      }
    }
  }
  // Every line written is a key something reads, and reading the whole file
  // back writes it unchanged.
  const std::string first = SettingsText();
  for (const auto& [key, value] : SettingsValues(first)) {
    if (!ApplySetting(key, value)) {
      PortLog::Write("settings self-test: %s is written but nothing reads it\n", key.c_str());
      ++failures;
    }
  }
  std::istringstream again(first);
  ApplySettingsText(again);
  const std::string second = SettingsText();
  if (second != first) {
    // Both whole files: past PortLog's 2048-byte line, so straight to stderr.
    std::fprintf(stderr, "settings self-test: the file changes when read back\n%s\n---\n%s\n", first.c_str(),
                 second.c_str());
    ++failures;
  }
  PortLog::Write("settings self-test: %d keys sampled, %zu lines round-tripped, %d failure(s)\n", sampled,
                 SettingsValues(first).size(), failures);
  return failures == 0 ? 0 : 1;
}

} // namespace PortDebug
