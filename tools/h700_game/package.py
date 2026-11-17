"""Game-specific staging atop the existing offline H700 package/audit helpers."""

import importlib.util
import json
from pathlib import Path
import shutil

import build

common = build.load_staging(build.INTEGRATION)
ROOT = build.ROOT
CONFIGURE_OPTIONS = ["-DMP_BUILD_TESTS=ON", "-DBUILD_TESTING=ON"]
CPU_TARGETS = [
    "port_h700_profile_tests", "port_h700_memory_tests", "port_h700_init_tests",
    "port_h700_game_boundary_tests", "port_h700_terminal_tests", "port_h700_game_failure_tests",
    "port_h700_skinning_tests", "port_h700_delayed_cleanup_tests", "port_h700_log_copy_tests",
    "port_h700_paths_probe", "port_card_tests", "port_gci_tests", "io_tests",
    "h700_dvd_lifetime_tests", "gx_fifo_tests", "render_worker_tests", "gfx_recording_tests",
    "bloom_readback_slots_tests",
]
CPU_FILTER = (r"^(port_h700_|port_version$|port_settings_roundtrip$|port_card_tests$|port_gci_tests$|"
              r"IoTest[.]|H700DvdLifetime[.]|GenerationLifecycle|MapCompletionTest|MapRetirementSource|"
              r"RenderWorker|Thread|PipelineWaitTest|PipelineCompilerSource|GfxRecordingTest[.]|"
              r"GfxFrameWaitOwnership[.]|ReadbackSlots)")
SCENARIO = dict(profile="h700-mixed-v1", game="Metroid Prime Unbound v0.46.0",
                target="ANBERNIC RG40XX H / muOS 2601.0 Jacaranda",
                backend="hardware OpenGLES Compatibility; no fallback", native_size=[640, 480],
                validation=True, robustness=True, assets=False, user_data=False,
                quick_resume=False, shared_card=False, device_validated=False)
ALLOWED = frozenset(("metroid_prime_port", "lib/libSDL3.so.0", "run-game.sh",
                     "Metroid Prime Unbound.sh.in", "README.md", "audit.json", "source.json"))


def source_snapshot(root):
    return common.source_snapshot(root, extra_prefixes=(
        "src/", "include/", "platform/", "tests/", "tools/h700_game/", "textures/", "assets/",
        "extern/musyx/", "extern/astcenc/", "extern/GamepadMotionHelpers/"),
        extra_names=("CMakeLists.txt", "files.cmake"))


def verify_notices(root):
    return common.verify_notices(root)


def run_checks(out, commands):
    commands.run("cpu-tests", ["ctest", "--test-dir", out / "build", "-R", CPU_FILTER,
                               "--output-on-failure", "--timeout", "45"], 180)
    commands.run("fifo-tests", [out / "build/extern/aurora/tests/gx_fifo_tests"], 120)
    for name in ("h700_integration_driver", "h700_game"):
        commands.run(name + "-helper-tests", ["python3", "-B", "-m", "unittest", "discover",
                     "-s", ROOT / "tools" / name, "-p", "test_*.py", "-v"], 180)


def validate_package(stage):
    manifest = common.validate_package(stage, scenario=SCENARIO, allowed_files=ALLOWED)
    build.require(manifest.get("public_distribution_authorized") is False, "Local RC is not distribution clearance")
    build.require(all((stage / name).is_file() for name in ALLOWED), "Incomplete game payload")
    return manifest


def collect_game_notices(out, stage, commands):
    common.collect_notices(out, stage, commands)
    spec = importlib.util.spec_from_file_location("h700_game_notices", ROOT / "tools/h700_game/notices.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.verify_game_notices(ROOT, out, stage / "licenses")


def stage_package(out, commands):
    stage = out / "package"
    build.require(not stage.exists(), "Fresh game package staging required")
    artifacts = json.loads((out / "build/artifacts.json").read_text())
    stage.mkdir()
    (stage / "lib").mkdir()
    for key, rel in (("executable", "metroid_prime_port"), ("sdl", "lib/libSDL3.so.0")):
        path = Path(artifacts[key]).resolve()
        build.require(path.is_relative_to(out / "build") and path.is_file(), "Artifact outside fresh game build: " + key)
        shutil.copyfile(path, stage / rel)
        (stage / rel).chmod(0o755 if key == "executable" else 0o644)
    for name in ("run-game.sh", "Metroid Prime Unbound.sh.in", "README.md"):
        shutil.copyfile(ROOT / "packaging/h700" / name, stage / name)
        (stage / name).chmod(0o755 if name.endswith((".sh", ".sh.in")) else 0o644)
    shutil.copyfile(out / "source.json", stage / "source.json")
    notices = collect_game_notices(out, stage, commands)
    common.audit(out, stage, commands, executable="metroid_prime_port")
    manifest = dict(schema=1, source=json.loads((out / "source.json").read_text()), image_id=build.IMAGE_ID,
                    inputs=json.loads((out / "input-manifest.json").read_text()), scenario=SCENARIO,
                    device_status="pending", public_distribution_authorized=False,
                    unresolved_primary_grants=notices["unresolved_primary_grants"],
                    payload=common.payload(stage, allowed_files=ALLOWED))
    build.write_json(stage / "manifest.json", manifest)
    (stage / "SHA256SUMS").write_text("".join(build.sha256(p) + "  " + p.relative_to(stage).as_posix() + "\n"
        for p in sorted(stage.rglob("*")) if p.is_file() and p.name != "SHA256SUMS"))
    validate_package(stage)
    for argument, label in (("--version", "version"), ("--h700-profile-selftest", "profile"),
                            ("--settings-selftest", "settings")):
        commands.run("staged-" + label, [stage / "metroid_prime_port", argument], 30)
    commands.run("launcher-syntax", ["sh", "-n", stage / "run-game.sh"], 10)
    commands.run("entry-syntax", ["sh", "-n", stage / "Metroid Prime Unbound.sh.in"], 10)
    result = common.archive_package(stage, out / "h700-game-rc.tar.gz", root_name="H700Game")
    result.update(executable_sha256=build.sha256(stage / "metroid_prime_port"),
                  source_sha256=manifest["source"]["source_sha256"],
                  public_distribution_authorized=False, device_validated=False)
    build.write_json(out / "package-result.json", result)
    return result
