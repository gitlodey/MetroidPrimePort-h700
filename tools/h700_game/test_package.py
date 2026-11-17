"""Game package content/metadata tests; fixtures are text, never game executables."""
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

import build

package = build.bootstrap.load_staging(build.GAME)


class GamePackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stage = self.root / "stage"
        self.stage.mkdir()
        for name in package.ALLOWED:
            target = self.stage / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("synthetic package fixture\n")
            target.chmod(0o644)
        self.seal()

    def seal(self, **overrides):
        manifest = dict(scenario=package.SCENARIO, device_status="pending", public_distribution_authorized=False,
                        payload=package.common.payload(self.stage, allowed_files=package.ALLOWED))
        manifest.update(overrides)
        build.bootstrap.write_json(self.stage / "manifest.json", manifest)
        (self.stage / "SHA256SUMS").write_text("".join(
            build.bootstrap.sha256(p) + "  " + p.relative_to(self.stage).as_posix() + "\n"
            for p in sorted(self.stage.rglob("*")) if p.is_file() and p.name != "SHA256SUMS"))

    def test_game_uses_existing_exact_payload_audit_and_archive_inventory(self):
        package.validate_package(self.stage)
        archive = self.root / "game.tar.gz"
        package.common.archive_package(self.stage, archive, root_name="H700Game")
        with tarfile.open(archive) as packed:
            self.assertTrue(all(member.name.startswith("H700Game/") and member.isfile()
                                for member in packed))

    def test_no_settings_cards_assets_or_container_libraries(self):
        for name in ("game.iso", "user/port_settings.ini", "user/USA/Card A/save.gci",
                     "cache/pipeline_cache.db", "lib/libEGL.so.1", "lib/libSDL2-2.0.so.0",
                     "lib/libmali.so", "h700_integration_driver"):
            with self.subTest(name=name):
                target = self.stage / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("forbidden fixture")
                with self.assertRaisesRegex(ValueError, "Unexpected"):
                    package.validate_package(self.stage)
                target.unlink()

    def test_tamper_missing_and_symlinks_rejected(self):
        target = self.stage / "metroid_prime_port"
        original = target.read_bytes()
        target.write_bytes(original + b"changed")
        with self.assertRaisesRegex(ValueError, "mismatch"):
            package.validate_package(self.stage)
        target.unlink()
        with self.assertRaisesRegex(ValueError, "mismatch"):
            package.validate_package(self.stage)
        target.symlink_to(self.root / "outside")
        with self.assertRaisesRegex(ValueError, "symlinks"):
            package.validate_package(self.stage)

    def test_even_resealed_payload_requires_both_game_and_local_sdl(self):
        (self.stage / "lib/libSDL3.so.0").unlink()
        self.seal()
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            package.validate_package(self.stage)

    def test_local_rc_cannot_claim_distribution_or_device_clearance(self):
        self.seal(public_distribution_authorized=True)
        with self.assertRaisesRegex(ValueError, "distribution clearance"):
            package.validate_package(self.stage)
        self.seal(device_status="PASS")
        with self.assertRaisesRegex(ValueError, "status changed"):
            package.validate_package(self.stage)

    def test_no_device_or_null_scene_tests_are_selected(self):
        self.assertNotIn("SceneHdrTest", package.CPU_FILTER)
        self.assertNotIn("h700_driver_scene_tests", package.CPU_TARGETS)
        self.assertIn("GfxRecordingTest", package.CPU_FILTER)
        self.assertRegex("port_h700_launcher", package.CPU_FILTER)


if __name__ == "__main__":
    unittest.main()
