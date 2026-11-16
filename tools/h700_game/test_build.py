"""GM-01 profile tests; existing bootstrap tests cover archive/output safety."""
import argparse
import contextlib
import io
from pathlib import Path
import unittest
from unittest import mock

import build


class GameBuildTests(unittest.TestCase):
    def test_root_source_and_separate_cache_keep_staged_inputs_last(self):
        command = build.bootstrap.configure_command(build.GAME)
        self.assertEqual(command[command.index("-S") + 1], "/src")
        self.assertEqual([command[i + 1] for i, value in enumerate(command) if value == "-C"],
                         ["/src/cmake/h700/GameInitialCache.cmake", "/out/Inputs.cmake"])
        self.assertIn("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", command)

    def test_real_game_target_and_no_unproven_help_execution(self):
        self.assertEqual(build.bootstrap.build_command(2, build.GAME),
                         ["cmake", "--build", "/out/build", "--target",
                          "metroid_prime_port", "--parallel", "2"])
        self.assertFalse(build.GAME.help_check)
        self.assertNotEqual(build.GAME.marker, build.bootstrap.MARKER)

    def test_game_uses_same_exact_image_and_docker_isolation(self):
        command = build.bootstrap.docker_command(
            Path("/project"), Path("/project/build/new"), "game-test",
            argparse.Namespace(jobs=2, configure_only=True), build.GAME)
        self.assertIn(build.bootstrap.IMAGE_ID, command)
        self.assertIn("/src/tools/h700_game/build.py", command)
        self.assertNotIn("/src/tools/h700_integration_driver/build.py", command)
        for flag in ("--pull=never", "--network=none", "--read-only", "--configure-only"):
            self.assertIn(flag, command)
        self.assertEqual([command[i + 1] for i, value in enumerate(command) if value == "--volume"],
                         ["/project:/src:ro", "/project/build/new:/out:rw"])

    def test_entry_reuses_bootstrap_without_a_second_manifest(self):
        with mock.patch.object(build.bootstrap, "main", return_value=0) as main:
            self.assertEqual(build.main(["--check-inputs"]), 0)
        main.assert_called_once_with(["--check-inputs"], profile=build.GAME,
                                     configure_only_required=True)
        self.assertFalse((Path(__file__).parent / "inputs.json").exists())

    def test_compile_and_demo_package_refused_before_side_effects(self):
        for args in ([], ["--package", "--configure-only"]):
            with self.subTest(args=args), \
                 mock.patch.object(build.bootstrap, "check_inputs", side_effect=AssertionError("no staging")), \
                 mock.patch.object(build.bootstrap.subprocess, "run", side_effect=AssertionError("no Docker")), \
                 contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(build.main(args), 1)


if __name__ == "__main__":
    unittest.main()
