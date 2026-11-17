# H700 game installation contract

Target: Metroid Prime Unbound v0.46.0, ANBERNIC RG40XX H / muOS 2601.0 Jacaranda. This layout is separate from every old port and from `.h700-integration-test`. No device installation/run is authorized by this document. The launcher and game package profile reuse the approved offline build/audit helpers. Actual build results and selected RC hashes are recorded in `docs/H700_GAME_REMAINING_PLAN.md` in the source checkout; no device PASS is implied.

## Physical layout

The future Ports entry and payload sit together in the selected card's `ROMS/Ports` directory:

```text
ROMS/Ports/
  Metroid Prime Unbound.sh
  .metroid-prime-unbound-h700/
    metroid_prime_port
    run-game.sh
    lib/libSDL3.so.0
    user/
      port_settings.ini
      USA/Card A/                 # selected release determines region
    cache/
```

The launcher resolves its own physical directory and uses only the fixed `.metroid-prime-unbound-h700` payload beside it. The executable is a real copy, not a symlink/hardlink into an old install. `MP_USER_PATH` is that payload's absolute `user/`; `MP_CACHE_PATH` is its absolute `cache/`. Both directories must be owned and writable by this installation. `CardFolder()` independently chooses the physical executable's writable `user/`, so setting just `MP_USER_PATH` cannot isolate a binary kept in an old directory.

Start with a new directory. Do not put old loose INI, raw cards, region folders, mods, importers or caches beside this executable: the upstream path code deliberately migrates loose legacy data. Do not link this user/cache to old directories. The launcher takes one explicitly supplied readable absolute disc path; it never searches another port's settings for a disc. Do not include the disc, extracted assets, cards, INI or cache in the package.

Initial H700 defaults keep quick resume and shared emulator cards OFF; original assets/effects, modern/twin-stick controls and the agreed retail profile remain in effect. Own settings and bindings persist. There is no arbitrary-settings/UI sandbox; conflicting installation/profile settings must be explained before a run, not silently overwritten. No raw-card conversion is required for a clean GCI-folder card.

Update only payload files after the actual game child has exited. Preserve `user/` and `cache/`; do not replace the directory wholesale or rotate/remove a log still being drained. Changing executable/package bytes creates a new RC/hash. Installation, updates, device access and game runs require the separately agreed window.

## Local validation (GM-14)

`port_h700_installation` copies a small executable physically into disposable old/new fixtures, runs the actual `PortPaths` functions with SDL's actual base path in fresh processes, and verifies no migration or changes to old-install/home sentinels. Cache configuration is bound to the actual startup source without starting graphics.

The real game's early `--h700-settings-persistence-selftest write|read <absolute-fixture-directory>` requires an existing explicit directory matching `MP_USER_PATH`. Write refuses an existing INI or temporary INI; read never saves. Separate processes use actual settings/PortInput parsing and `SaveSettings` atomic rename to preserve `hud_scale=90`, modern preset and `shift_pad=9`. These are disposable fixture choices, not the release profile.

ARM64 game/probe/card/IO targets build;48/48 scoped CPU/no-init/DVD/CARD/GCI/atomic CTest pass. CARD checks include nonzero stable serial after remount and rejection of duplicate identities. The existing GCI suite (including raw export) also passes on clean container-native fixtures. No personal save fixtures or `../wii-save` are read.

Docker Desktop bind mounts initially produced `copy_file: Permission denied` in CARD/raw backup fixtures. The same unchanged card code/tests pass with temporary container-native storage. For that combined test run use the approved image with `--tmpfs /tmp:rw,exec,nosuid,size=96m`, `TMPDIR=/tmp`, and `--tmpfs /out/build/card-test-data:rw,noexec,nosuid,size=16m`. `/tmp` needs execution permission only for the physically copied path-probe binary. No capabilities, dependencies or firmware were changed. Logs/commands remain under ignored `gm14-*` outputs.

These results do not prove physical save-station/load behavior: that is window B, using the real game in a new process.

## Launcher and logs (GM-15)

`run-game.sh` lives inside the fixed payload and accepts exactly one readable regular absolute disc path. The adjacent `Metroid Prime Unbound.sh.in` Ports-entry template starts with `DISC=''` and deliberately refuses launch until that line contains the user's safely quoted literal path. Do not execute the disc value with `eval`, derive it from old settings or pass it unquoted. Copy that template to the adjacent Ports entry and configure it privately before window A; no private disc path is committed.

The launcher keeps the two required SDL video variables, the five known muOS frontend SDL values and explicitly supplied audio-routing values. PipeWire `/run` is a default only when `PIPEWIRE_RUNTIME_DIR` is unset and `/run/pipewire-0` is an actual socket. No audio gain/backend is forced. Conflicting renderer/presence pins are rejected before sanitation; arbitrary MP/AURORA/debug/path and loader-injection overrides are not inherited. Libraries use the package RUNPATH, never an injected `LD_LIBRARY_PATH`.

`user/`, `cache/` and `user/tmp/` are persistent own directories. `run.lock` allows only one actual game child. A shell HUP/INT/TERM defers until the foreground child has really exited/reaped, then preserves its exit code and removes the lock. SIGKILL or unknown state can leave a lock: do not remove it without independently establishing that the game has exited. There is no timeout, kill, automatic retry or frontend restart. The game lock is not retained just for an already orphaned log copier.

The game uses its existing logger/redaction and RAM diagnostics. Its initial terminal output goes to `/dev/null`, avoiding a second unlimited frontend file; the existing copier writes `user/metroid_prime_port.log` and retains the previous run as `.old.log`. H700 caps each file at **8 MiB including header and an explicit truncation marker**, while continuing to drain every pipe byte to EOF. Missing/truncated logs cannot prove absence of errors. There is no new ready/health/final messaging protocol. Before copying, inspecting a final log, or cleaning it, account for a copier which may still be draining after game exit; never kill it or delete active logs to manufacture a clean run.

Validated: shell syntax;13/13 bounded mock-child launcher cases on macOS and approved ARM64 Linux (literal metacharacters, invalid inputs, profile/env/audio, actual socket fixture, live-child signals/lock/exit7, repeat launches, copier-descendant independence). Actual production copier flood/split-redaction/unterminated-tail tests pass, including writer completion with default SIGPIPE and actual copier exit/reap. Full game+copier target build,50/50 scoped CTest, non-H700 log TU compile pass. No real game/GPU/device launch.

## Fresh game package recipe (GM-16/17)

From the H700 checkout, with the existing approved native ARM64 Docker image and14 pinned local inputs:

```sh
python3 -B tools/h700_game/build.py --package --output build/h700-game-rc1 --jobs 2
python3 -B tools/h700_game/build.py --package --output build/h700-game-rc2 --jobs 2
```

Each output must be new. No pull, network, package installs or GPU run occurs. Both commands compile the actual game and relevant CPU targets, run no-init/game/lifecycle/DVD/skinning/recording/readback/CARD/atomic/launcher/helper checks, and stage game+matched SDL with exact local `$ORIGIN/lib`. The package-profile build-tree binary also has that SDL in its own `lib/`; it does not need a build-path RPATH or `LD_LIBRARY_PATH`.

Outputs include `h700-game-rc.tar.gz` (top-level `H700Game/`), `package-result.json`, source/input/environment/provider/ELF records, audit and all notices/checksums. The source inventory includes actual game/CMake/version/embedded inputs and shared/vendored code used by this recipe, not only the diagnostic slice. Archives contain no user/cache/disc/save data and no container EGL/GLES/SDL2/libmali. The audit checks known providers and version nodes against the existing firmware snapshot; incomplete exact-symbol evidence and all untested runtime/kernel/driver paths remain explicitly reported.

Keep the same build-relevant source/config/input bytes for the second build. Compare source/input/provider manifests and executable/archive hashes, not just successful commands. Preserve completed packages, manifests and logs; if space is limited, only that newly completed build's disposable `build/` and extracted `inputs/` may be removed before the next fresh output. Do not remove shared input archives, previous outputs or user data. A source/package edit creates new bytes and requires a new RC.

### Install/update/rollback after separate device permission

Extract the chosen verified archive into a new private staging directory. Inspect `manifest.json` and check `SHA256SUMS` before using it. Place the physical `H700Game` payload as the fixed `.metroid-prime-unbound-h700` beside the Ports entry. Copy its unconfigured `Metroid Prime Unbound.sh.in` to `Metroid Prime Unbound.sh` beside the payload, and replace only `DISC=''` with one safely quoted absolute legal disc path. It is intentionally not runnable before that private choice; do not infer it from old settings.

On first installation there is no `user/` or cache to import. The launcher creates its own directories. On update, first establish actual child exit; replace only payload files from the new verified archive and keep `user/`/`cache/`. Keep a copy/hash of the old executable+SDL+launcher for rollback, without mixing versions. Never roll a live process back or delete cards/settings/cache to force a cold-launch PASS. Firmware libraries and the old port remain untouched.

### Notices and local-only scope

The existing full collector is retained, with game-specific actual-source coverage for MusyX, G.721, astcenc, SDL embedded codecs, ImGui/STB/fonts, SQLite, GamepadMotionHelpers, nod and pinned Dawn/transitives. Compiled Remastered codecs are included even with runtime mods OFF. `licenses/game-coverage.json` records exact source evidence and missing primary grants separately.

**This is a private local RC, not permission for public distribution.** Six documented unresolved groups include primary texts/revision evidence for tegra_swizzle, bcdec_rs/bcdec, astc-decode/UNC-derived portions, retrotool/LZSS, Kenney/Xelu packs, and game/SDK/source-derived code. Existing attribution is preserved; no missing grant is invented or replaced by the port's MIT. Metadata/provider audit PASS likewise does not authorize game assets or certify every firmware symbol.
