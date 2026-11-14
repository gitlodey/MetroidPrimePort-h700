"""Host-only unit tests: python3 -B tools/h700_graphics_probe/test_build.py."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
import verify_environment as environment
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("probe_build", Path(__file__).with_name("build.py"))
build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build)

ELF = """Class:                             ELF64
Machine:                           AArch64
[Requesting program interpreter: /lib/ld-linux-aarch64.so.1]
0x0000000000000001 (NEEDED) Shared library: [libSDL3.so.0]
0x0000000000000001 (NEEDED) Shared library: [libc.so.6]
Name: GLIBC_2.17 Name: GLIBC_2.38
Name: GLIBCXX_3.4.9 Name: GLIBCXX_3.4.32 Name: CXXABI_1.3.14
"""


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        (self.root / "build").mkdir()

    def archive(self, member="SDL-pin/source.c", content=b"int x;\n"):
        archive = self.root / "source.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            info = tarfile.TarInfo(member)
            info.size = len(content)
            tar.addfile(info, io.BytesIO(content))
        tree = self.root / "source"
        tree.mkdir()
        (tree / "source.c").write_bytes(content)
        return archive, tree

    def test_archive_matches_and_reports_content_hashes(self):
        archive, tree = self.archive()
        info = build.verify_archive(archive, tree, build.sha256(archive), "SDL-pin")
        self.assertEqual(info["archive_sha256"], build.sha256(archive))
        self.assertEqual(len(info["tree_sha256"]), 64)
        self.assertNotIn(str(self.root), json.dumps(info))

    def test_archive_checksum_mismatch(self):
        archive, tree = self.archive()
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            build.verify_archive(archive, tree, "0" * 64, "SDL-pin")

    def test_modified_source_rejected(self):
        archive, tree = self.archive()
        (tree / "source.c").write_text("modified")
        with self.assertRaisesRegex(ValueError, "differs from archive"):
            build.verify_archive(archive, tree, build.sha256(archive), "SDL-pin")

    def test_extra_source_rejected(self):
        archive, tree = self.archive()
        (tree / "extra.c").write_text("extra")
        with self.assertRaisesRegex(ValueError, "Extra or missing"):
            build.verify_archive(archive, tree, build.sha256(archive), "SDL-pin")

    def test_archive_traversal_rejected(self):
        archive, tree = self.archive("SDL-pin/../escape.c")
        with self.assertRaisesRegex(ValueError, "Unsafe archive"):
            build.verify_archive(archive, tree, build.sha256(archive), "SDL-pin")

    def test_cached_symlink_rejected(self):
        archive, tree = self.archive()
        (tree / "source.c").unlink()
        other = self.root / "outside.c"
        other.write_text("int x;\n")
        (tree / "source.c").symlink_to(other)
        with self.assertRaisesRegex(ValueError, "link"):
            build.verify_archive(archive, tree, build.sha256(archive), "SDL-pin")

    def test_missing_inputs_actionable_and_no_output(self):
        with self.assertRaisesRegex(ValueError, "Missing input: tools/h700_graphics_probe/CMakeLists.txt"):
            build.check_inputs(self.root)
        self.assertEqual(list((self.root / "build").iterdir()), [])

    def test_missing_archive_is_not_downloaded(self):
        for rel in (build.PROBE / "CMakeLists.txt", build.PROBE / "build.py", build.PROBE / "probe.cpp",
                    build.PROBE / "pattern.hpp", Path("tools/collect_licenses.sh"), Path("LICENSE"),
                    Path("NOTICE"), Path("packaging/licenses/dawn.txt"), build.SNAPSHOT,
                    *(build.PROBE / name for name in build.ENVIRONMENT_INPUTS)):
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture")
        with patch.object(build, "run") as run:
            with self.assertRaisesRegex(ValueError, "Missing input:.*tar.gz"):
                build.check_inputs(self.root)
            run.assert_not_called()

    def test_fresh_output_allowed(self):
        self.assertEqual(build.output_path(self.root, "build/h700-graphics-probe-check"),
                         self.root / "build/h700-graphics-probe-check")

    def test_publish_requires_completed_manifest(self):
        stage = self.root / "package.partial"
        stage.mkdir()
        with self.assertRaisesRegex(ValueError, "final manifest"):
            build.publish_package(stage)
        self.assertFalse((self.root / "package").exists())
        self.assertTrue(stage.is_dir())

    def test_publish_atomically_renames_completed_stage(self):
        stage = self.root / "package.partial"
        stage.mkdir()
        (stage / "manifest.json").write_text("{}")
        build.publish_package(stage)
        self.assertFalse(stage.exists())
        self.assertEqual((self.root / "package/manifest.json").read_text(), "{}")

    def test_publish_never_replaces_existing_package(self):
        stage = self.root / "package.partial"
        stage.mkdir()
        (stage / "manifest.json").write_text("{}")
        (self.root / "package").mkdir()
        with self.assertRaisesRegex(ValueError, "already exists"):
            build.publish_package(stage)
        self.assertTrue(stage.is_dir())
        self.assertEqual(list((self.root / "package").iterdir()), [])

    def test_unsafe_outputs_rejected(self):
        for value in (".", "build", "build/muos-linux-arm64", "build/muos-linux-arm64/_deps",
                      "tools/h700_graphics_probe", "../h700-graphics-probe", "build/h700-graphics-probe/child"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                build.output_path(self.root, value)

    def test_existing_output_refused_even_if_empty(self):
        path = self.root / "build/h700-graphics-probe"
        path.mkdir()
        with self.assertRaisesRegex(ValueError, "already exists"):
            build.output_path(self.root, str(path))
        self.assertEqual(build.output_path(self.root, str(path), fresh=False), path)

    def test_output_symlink_refused(self):
        path = self.root / "build/h700-graphics-probe"
        path.symlink_to(self.root / "elsewhere", target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            build.output_path(self.root, str(path))

    def test_symlink_build_parent_refused(self):
        (self.root / "build").rmdir()
        (self.root / "build").symlink_to(self.root / "elsewhere", target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            build.output_path(self.root, "build/h700-graphics-probe")

    def test_elf_versions_sorted_numerically(self):
        info = build.elf_info(ELF, executable=True)
        self.assertEqual(info["max_versions"], build.LIMITS)
        self.assertEqual(info["needed"], ["libSDL3.so.0", "libc.so.6"])

    def test_elf_wrong_arch_or_interpreter(self):
        for text in (ELF.replace("AArch64", "X86-64"), ELF.replace("ELF64", "ELF32"),
                     ELF.replace("/lib/ld-linux-aarch64.so.1", "/lib64/ld-linux-x86-64.so.2")):
            with self.assertRaises(ValueError):
                build.elf_info(text, executable=True)

    def test_each_abi_limit_enforced(self):
        for token in ("GLIBC_2.39", "GLIBCXX_3.4.33", "CXXABI_1.3.15", "GLIBC_PRIVATE"):
            with self.subTest(token=token), self.assertRaises(ValueError):
                build.elf_info(ELF + token, executable=True)

    def test_dso_needs_no_interpreter_or_cpp_runtime(self):
        text = "Class: ELF64\nMachine: AArch64\nName: GLIBC_2.17\n"
        info = build.elf_info(text)
        self.assertIsNone(info["interpreter"])
        self.assertIsNone(info["max_versions"]["GLIBCXX"])

    def test_ldd_missing_library_or_version(self):
        build.check_ldd("libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6")
        for text in ("libfoo.so => not found", "version `GLIBC_2.39' not found"):
            with self.assertRaisesRegex(ValueError, "unresolved"):
                build.check_ldd(text)

    def test_check_inputs_mode_never_runs_docker_or_creates_output(self):
        output = self.root / "build/h700-graphics-probe"
        with patch.object(build, "check_inputs", return_value={"sdl": {"claimed_ref": build.SDL_PIN}}), \
             patch.object(build, "output_path", return_value=output), \
             patch.object(build, "run") as run, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(build.main(["--check-inputs"]), 0)
            run.assert_not_called()
        self.assertFalse(output.exists())

    def test_help_never_checks_inputs_or_runs_commands(self):
        with patch.object(build, "check_inputs") as inputs, patch.object(build, "run") as run, \
             contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as result:
            build.main(["--help"])
        self.assertEqual(result.exception.code, 0)
        inputs.assert_not_called()
        run.assert_not_called()

    def test_source_hashes_are_relative_and_detect_changes(self):
        for rel in (build.PROBE / "probe.cpp", build.PROBE / "pattern.hpp", build.PROBE / "CMakeLists.txt",
                    build.PROBE / "build.py", Path("tools/collect_licenses.sh"), build.SNAPSHOT,
                    *(build.PROBE / name for name in build.ENVIRONMENT_INPUTS)):
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("first")
        first = build.source_hashes(self.root)
        (self.root / build.PROBE / "pattern.hpp").write_text("second")
        self.assertNotEqual(first, build.source_hashes(self.root))
        self.assertNotIn(str(self.root), json.dumps(first))

    def test_command_runner_uses_timeout_and_checks_exit_status(self):
        result = build.subprocess.CompletedProcess(["fake"], 2, stdout="failure")
        with patch.object(build.subprocess, "run", return_value=result) as run:
            with self.assertRaisesRegex(ValueError, "fake failed"):
                build.run(["fake"], timeout=7)
        self.assertEqual(run.call_args.kwargs["timeout"], 7)
        self.assertNotIn("shell", run.call_args.kwargs)

    def test_wrong_docker_arch_refused_before_creating_output(self):
        with patch.object(build, "__file__", str(self.root / build.PROBE / "build.py")), \
             patch.object(build, "check_inputs", return_value={}), \
             patch.object(build.shutil, "which", return_value="docker"), \
             patch.object(build, "run", return_value="sha256:" + "a" * 64 + " linux amd64") as run, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(build.main([]), 1)
        self.assertEqual(run.call_count, 1)
        self.assertFalse((self.root / "build/h700-graphics-probe").exists())

    def test_docker_build_uses_immutable_offline_readonly_mounts(self):
        image = "sha256:" + "a" * 64
        fake_script = self.root / build.PROBE / "build.py"
        with patch.object(build, "__file__", str(fake_script)), \
             patch.object(build, "check_inputs", return_value={}), \
             patch.object(build.shutil, "which", return_value="docker"), \
             patch.object(build, "run", side_effect=[image + " linux arm64", "built"]) as run, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(build.main([]), 0)
        command = run.call_args_list[1].args[0]
        for flag in ("--network=none", "--read-only", "--pull=never", image, f"{self.root}:/src:ro"):
            self.assertIn(flag, command)
        self.assertIn(f"{self.root / 'build/h700-graphics-probe'}:/out:rw", command)


def elf_fixture(definitions=(), needs=None, exports=(), imports=(), needed=()):
    rows = ["Class: ELF64", "Machine: AArch64"]
    rows += [f"(NEEDED) Shared library: [{name}]" for name in needed]
    for index, (token, section) in enumerate([(s, "12") for s in exports] + [(s, "UND") for s in imports]):
        rows.append(f" {index + 1}: 0000000000000000 8 FUNC GLOBAL DEFAULT {section} {token}")
    rows += ["Version definition section '.gnu.version_d' contains entries:"]
    rows += [f"  0x0000: Rev: 1 Flags: none Index: 2 Cnt: 1 Name: {name}" for name in definitions]
    rows += ["Version needs section '.gnu.version_r' contains entries:"]
    for library, nodes in (needs or {}).items():
        rows.append(f"  0x0000: Version: 1 File: {library} Cnt: {len(nodes)}")
        rows += [f"  0x0010: Name: {name} Flags: none Version: 2" for name in nodes]
    return "\n".join(rows)


class AbiGateTests(unittest.TestCase):
    def setUp(self):
        self.snapshot = {"schema": 1, "architecture": "aarch64", "libraries": {
            "libc.so.6": {"status": "found", "machine": "AArch64", "defined_version_nodes": ["GLIBC_2.17", "GLIBC_2.38"],
                          "selected_exports": {"__isoc23_strtol": "__isoc23_strtol@@GLIBC_2.38"}},
            "libsndio.so.7": {"status": "not_found_in_searched_system_directories"}}}
        self.sdl = build.elf_info(elf_fixture(definitions=["SDL3_0"], needed=["libc.so.6"],
                                            needs={"libc.so.6": ["GLIBC_2.17"]}))
        self.info = build.elf_info(elf_fixture(needed=["libSDL3.so.0", "libc.so.6"],
            needs={"libSDL3.so.0": ["SDL3_0"], "libc.so.6": ["GLIBC_2.38"]},
            imports=["__isoc23_strtol@GLIBC_2.38 (2)"]))

    def test_accepts_firmware_and_own_sdl_without_optional_candidates(self):
        result = build.firmware_gate(self.info, self.snapshot, {"libSDL3.so.0": self.sdl})
        self.assertEqual(result["selected_imports_checked"], {"__isoc23_strtol": ["__isoc23_strtol@GLIBC_2.38"]})
        self.assertIn("not every imported symbol", result["limitations"][0])

    def test_missing_needed_fails(self):
        self.info["needed"].append("libunmeasured.so.1")
        with self.assertRaisesRegex(ValueError, "absent.*libunmeasured"):
            build.firmware_gate(self.info, self.snapshot, {"libSDL3.so.0": self.sdl})

    def test_optional_absence_fails_only_when_actually_needed(self):
        self.info["needed"].append("libsndio.so.7")
        with self.assertRaisesRegex(ValueError, "absent.*libsndio"):
            build.firmware_gate(self.info, self.snapshot, {"libSDL3.so.0": self.sdl})

    def test_missing_exact_version_node_fails_below_maximum(self):
        self.info["required_version_nodes"]["libc.so.6"] = ["GLIBC_2.35"]
        with self.assertRaisesRegex(ValueError, "lacks version nodes"):
            build.firmware_gate(self.info, self.snapshot, {"libSDL3.so.0": self.sdl})

    def test_missing_sdl_version_node_fails(self):
        with self.assertRaisesRegex(ValueError, "lacks version nodes"):
            build.firmware_gate(self.info, self.snapshot, {"libSDL3.so.0": {"defined_version_nodes": []}})

    def test_critical_export_must_match_import_version(self):
        self.snapshot["libraries"]["libc.so.6"]["selected_exports"]["__isoc23_strtol"] = "__isoc23_strtol@@GLIBC_2.17"
        with self.assertRaisesRegex(ValueError, "matching selected export"):
            build.firmware_gate(self.info, self.snapshot, {"libSDL3.so.0": self.sdl})

    def test_missing_critical_export_fails(self):
        self.snapshot["libraries"]["libc.so.6"]["selected_exports"] = {}
        with self.assertRaisesRegex(ValueError, "matching selected export"):
            build.firmware_gate(self.info, self.snapshot, {"libSDL3.so.0": self.sdl})

    def test_definitions_are_not_confused_with_needs(self):
        definitions, needs = environment.version_nodes(elf_fixture(
            definitions=["SDL3_0"], needs={"libc.so.6": ["GLIBC_2.38"], "libstdc++.so.6": ["GLIBCXX_3.4.31"]}))
        self.assertEqual(definitions, ["SDL3_0"])
        self.assertEqual(needs, {"libc.so.6": ["GLIBC_2.38"], "libstdc++.so.6": ["GLIBCXX_3.4.31"]})

    def test_defined_symbols_do_not_become_imports(self):
        text = elf_fixture(exports=["__isoc23_strtol@@GLIBC_2.38"], imports=["__isoc23_strtoul@GLIBC_2.38 (4)"])
        self.assertEqual(build.elf_info(text)["critical_imports"], {"__isoc23_strtoul": ["__isoc23_strtoul@GLIBC_2.38"]})

    def test_private_glibc_import_fails(self):
        with self.assertRaisesRegex(ValueError, "private glibc"):
            build.elf_info(elf_fixture(needs={"libc.so.6": ["GLIBC_PRIVATE"]}))


class EnvironmentTests(unittest.TestCase):
    def setUp(self):
        self.reports = {}
        for library in ("libc.so.6", "libstdc++.so.6"):
            exports = [symbol + "@@" + version for symbol, (owner, version) in environment.CRITICAL.items() if owner == library]
            nodes = ["GLIBC_2.38"] if library == "libc.so.6" else ["GLIBCXX_3.4.31", "GLIBCXX_3.4.32", "CXXABI_1.3.14"]
            self.reports[library] = elf_fixture(definitions=nodes, exports=exports)

    def validate(self, machine="aarch64", glibc="glibc 2.38", interpreter=True):
        return environment.validate_reports(machine, glibc, self.reports, interpreter)

    def test_approved_runtime_fixture(self):
        report = self.validate()
        self.assertEqual(report["architecture"], "aarch64")
        self.assertEqual(len(report["libraries"]["libc.so.6"]["selected_exports"]), 4)

    def test_wrong_arch_glibc_or_missing_loader(self):
        for values in (("x86_64", "glibc 2.38", True), ("aarch64", "glibc 2.36", True), ("aarch64", "glibc 2.38", False)):
            with self.subTest(values=values), self.assertRaises(ValueError):
                self.validate(*values)

    def test_each_critical_export_is_required_exactly(self):
        for symbol, (owner, version) in environment.CRITICAL.items():
            with self.subTest(symbol=symbol):
                original = self.reports[owner]
                self.reports[owner] = original.replace(symbol + "@@" + version, symbol + "@@WRONG_1.0")
                with self.assertRaisesRegex(ValueError, "Missing exact export"):
                    self.validate()
                self.reports[owner] = original

    def test_exact_maxima_use_definitions_not_requirements(self):
        self.reports["libstdc++.so.6"] += "\n  File: libother.so Cnt: 1\n Name: GLIBCXX_99.0"
        self.validate()
        self.reports["libstdc++.so.6"] = self.reports["libstdc++.so.6"].replace("Name: GLIBCXX_3.4.32", "Name: GLIBCXX_3.4.33")
        with self.assertRaisesRegex(ValueError, "exact maximum"):
            self.validate()

    def test_undefined_or_hidden_export_is_not_accepted(self):
        text = elf_fixture(exports=["public@@V1", "hidden@@V1"], imports=["absent@V1 (2)"])
        text = text.replace("DEFAULT 12 hidden", "HIDDEN 12 hidden")
        self.assertEqual(environment.symbols(text), {"public": ["public@@V1"]})

    def test_cli_json_stdout_and_exclusive_output(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(environment, "inspect_environment", return_value=self.validate()):
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(environment.main([]), 0)
            self.assertEqual(json.loads(stdout.getvalue())["glibc_getconf"], "glibc 2.38")
            path = Path(directory) / "abi.json"
            self.assertEqual(environment.main(["--output", str(path)]), 0)
            previous = path.read_bytes()
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(environment.main(["--output", str(path)]), 1)
            self.assertEqual(path.read_bytes(), previous)


if __name__ == "__main__":
    unittest.main()
