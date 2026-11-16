"""Narrow IG-01 bootstrap tests; no Docker, compiler, network or device calls."""
import argparse
import contextlib
import copy
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile

import build


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / build.DRIVER).mkdir(parents=True)
        self.out = self.root / "build/run"
        self.out.mkdir(parents=True)

    def tar(self, name="source.tar.gz", members=None):
        path = self.root / name
        with tarfile.open(path, "w:gz") as archive:
            for member_name, kind, value in members or [("source/CMakeLists.txt", "file", b"project(test)\n")]:
                member = tarfile.TarInfo(member_name)
                if kind == "file":
                    member.size = len(value)
                    archive.addfile(member, io.BytesIO(value))
                else:
                    member.type = {"symlink": tarfile.SYMTYPE, "hardlink": tarfile.LNKTYPE}[kind]
                    member.linkname = value
                    archive.addfile(member)
        return path

    def manifest(self, paths):
        manifest = {"schema": 1, "image_id": build.IMAGE_ID, "platform": "linux/arm64",
                    "archives": [{"name": f"input-{i}", "path": path.name,
                                  "sha256": build.sha256(path), "strip_prefix": "source",
                                  "fetchcontent": f"INPUT_{i}"} for i, path in enumerate(paths)]}
        self.save_manifest(manifest)
        return manifest

    def save_manifest(self, manifest):
        (self.root / build.DRIVER / "inputs.json").write_text(json.dumps(manifest))

    def test_missing_and_late_hash_failure_precede_all_staging(self):
        paths = [self.tar("first.tar.gz"), self.tar("second.tar.gz")]
        manifest = self.manifest(paths)
        for failure in ("missing", "hash"):
            with self.subTest(failure=failure):
                broken = copy.deepcopy(manifest)
                if failure == "missing":
                    broken["archives"][1]["path"] = "absent.tar.gz"
                else:
                    broken["archives"][1]["sha256"] = "0" * 64
                self.save_manifest(broken)
                with self.assertRaisesRegex(ValueError, "Missing input|SHA256 mismatch"):
                    build.stage_inputs(self.root, self.out)
                self.assertFalse((self.out / "Inputs.cmake").exists())
                self.assertFalse((self.out / "inputs").exists())

    def test_staging_generates_only_verified_source_and_package_overrides(self):
        archive = self.tar()
        manifest = self.manifest([archive])
        manifest["archives"][0].update(package="DAWN", fetchcontent="DAWN_PREBUILT")
        self.save_manifest(manifest)
        self.assertEqual(build.stage_inputs(self.root, self.out), manifest)
        cache = (self.out / "Inputs.cmake").read_text()
        self.assertIn('FETCHCONTENT_SOURCE_DIR_DAWN_PREBUILT "/out/inputs/input-0"', cache)
        self.assertIn('AURORA_DAWN_PACKAGE_URL "file:///out/inputs/input-0/source.tar.gz"', cache)
        self.assertIn(manifest["archives"][0]["sha256"], cache)
        self.assertIn("H700_INPUTS_STAGED ON", cache)
        self.assertIn("FETCHCONTENT_FULLY_DISCONNECTED ON", cache)
        self.assertEqual((self.out / "inputs/input-0/CMakeLists.txt").read_bytes(), b"project(test)\n")
        self.assertEqual(build.sha256(self.out / "inputs/input-0/source.tar.gz"), build.sha256(archive))

    def test_zip_strips_root_and_preserves_files(self):
        archive = self.root / "source.zip"
        with zipfile.ZipFile(archive, "w") as source:
            source.writestr("source/", "")
            source.writestr("source/sqlite3.c", "/* pinned */")
        build.extract_archive(archive, self.out / "zip", "source")
        self.assertEqual((self.out / "zip/sqlite3.c").read_text(), "/* pinned */")

    def test_unsafe_tar_paths_links_and_special_members_rejected_before_writes(self):
        cases = [
            [("source/../../escape", "file", b"bad")],
            [("/escape", "file", b"bad")],
            [("wrong-root/file", "file", b"bad")],
            [("source/link", "symlink", "../../escape")],
            [("source/link", "hardlink", "source/file")],
            [("source/link", "symlink", "target"), ("source/link/file", "file", b"bad")],
        ]
        for members in cases:
            with self.subTest(members=members):
                archive = self.tar(members=members)
                target = self.out / "unsafe"
                with self.assertRaises(ValueError):
                    build.extract_archive(archive, target, "source")
                self.assertFalse(target.exists())
        self.assertFalse((self.root / "escape").exists())

    def test_zip_traversal_and_symlink_rejected(self):
        for symlink in (False, True):
            with self.subTest(symlink=symlink):
                archive = self.root / "unsafe.zip"
                with zipfile.ZipFile(archive, "w") as source:
                    member = zipfile.ZipInfo("source/link" if symlink else "source/../../escape")
                    if symlink:
                        member.create_system = 3
                        member.external_attr = 0o120777 << 16
                    source.writestr(member, "../../escape")
                with self.assertRaises(ValueError):
                    build.extract_archive(archive, self.out / "unsafe", "source")
                self.assertFalse((self.out / "unsafe").exists())

    def test_zstd_style_internal_alias_and_dot_package_root(self):
        archive = self.tar(members=[("./bin/zstd", "file", b"tool"),
                                    ("./bin/unzstd", "symlink", "zstd")])
        build.extract_archive(archive, self.out / "package", "")
        self.assertEqual((self.out / "package/bin/unzstd").read_bytes(), b"tool")

    def test_existing_output_and_escape_refused(self):
        for path in ("build/run", "build", "../outside", "/tmp/h700-outside"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                build.output_path(self.root, path)
        (self.root / "build/link").symlink_to(self.out, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            build.output_path(self.root, "build/link/new")
        self.assertEqual(build.output_path(self.root, "build/fresh/nested"), self.root / "build/fresh/nested")

    def test_check_inputs_cli_is_read_only_without_docker(self):
        self.manifest([self.tar()])
        before = sorted(p.relative_to(self.root).as_posix() for p in self.root.rglob("*"))
        checker = build.check_inputs
        with mock.patch.object(build, "ROOT", self.root), \
             mock.patch.object(build, "check_inputs", wraps=lambda: checker(self.root)), \
             mock.patch.object(build.subprocess, "run", side_effect=AssertionError("No subprocess allowed")), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(build.main(["--check-inputs"]), 0)
        self.assertEqual(before, sorted(p.relative_to(self.root).as_posix() for p in self.root.rglob("*")))

    def test_subprocess_timeout_retains_command_and_log(self):
        commands = build.Commands(self.out)
        with mock.patch.object(build.subprocess, "run", side_effect=build.subprocess.TimeoutExpired("cmake", 3)) as run:
            with self.assertRaises(build.subprocess.TimeoutExpired):
                commands.run("configure", ["cmake", "--version"], 3)
        self.assertEqual(run.call_args.kwargs["timeout"], 3)
        self.assertEqual(run.call_args.kwargs["stderr"], build.subprocess.STDOUT)
        self.assertTrue((self.out / "configure.log").exists())
        record = json.loads((self.out / "commands.json").read_text())[0]
        self.assertEqual(record["command"], ["cmake", "--version"])
        self.assertEqual(record["status"], "timeout")
        self.assertEqual(record["exit_code"], 124)

    def test_commands_use_exact_image_isolation_cache_order_and_real_target(self):
        args = argparse.Namespace(jobs=2, configure_only=True)
        command = build.docker_command(self.root, self.out, "unit-test", args)
        self.assertIn(build.IMAGE_ID, command)
        for flag in ("--pull=never", "--network=none", "--read-only", "--configure-only"):
            self.assertIn(flag, command)
        mounts = [command[i + 1] for i, item in enumerate(command) if item == "--volume"]
        self.assertEqual(mounts, [f"{self.root}:/src:ro", f"{self.out}:/out:rw"])
        self.assertNotIn("--tmpfs", command)
        configure = build.configure_command()
        caches = [configure[i + 1] for i, item in enumerate(configure) if item == "-C"]
        self.assertEqual(caches, ["/src/cmake/h700/InitialCache.cmake", "/out/Inputs.cmake"])
        self.assertIn("/src/tools/h700_integration_driver", configure)
        self.assertIn("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", configure)
        self.assertEqual(build.build_command(2), ["cmake", "--build", "/out/build", "--target",
                                                "h700_integration_driver", "--parallel", "2"])

if __name__ == "__main__":
    unittest.main()
