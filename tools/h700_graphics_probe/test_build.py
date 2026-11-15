"""Host-only unit tests: python3 -B tools/h700_graphics_probe/test_build.py."""
import contextlib
import hashlib
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


def contract_reference_fixture(root):
    hashes = {}
    for rel in build.CONTRACT_REFERENCE_SHA256:
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        content = f"reviewed U fixture: {rel}\n".encode()
        path.write_bytes(content)
        hashes[rel] = hashlib.sha256(content).hexdigest()
    return hashes


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
                    build.PROBE / "pattern.hpp", *(build.PROBE / name for name in build.CONTRACT_INPUTS),
                    Path("tools/collect_licenses.sh"), Path("LICENSE"),
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
                    *(build.PROBE / name for name in (*build.CONTRACT_INPUTS, *build.ENVIRONMENT_INPUTS)),
                    *(Path(rel) for rel in build.CONTRACT_REFERENCE_SHA256)):
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("first")
        first = build.source_hashes(self.root)
        for rel in (build.PROBE / "pattern.hpp", *(build.PROBE / name for name in build.CONTRACT_INPUTS),
                    *(Path(rel) for rel in build.CONTRACT_REFERENCE_SHA256)):
            with self.subTest(path=rel):
                (self.root / rel).write_text("second")
                self.assertNotEqual(first, build.source_hashes(self.root))
                (self.root / rel).write_text("first")
        self.assertTrue(set(build.CONTRACT_REFERENCE_SHA256).issubset(first))
        self.assertNotIn(str(self.root), json.dumps(first))

    def test_command_runner_uses_timeout_and_checks_exit_status(self):
        result = build.subprocess.CompletedProcess(["fake"], 2, stdout="failure")
        with patch.object(build.subprocess, "run", return_value=result) as run:
            with self.assertRaisesRegex(ValueError, "fake failed"):
                build.run(["fake"], timeout=7)
        self.assertEqual(run.call_args.kwargs["timeout"], 7)
        self.assertNotIn("shell", run.call_args.kwargs)

    def test_command_runner_can_separate_smoke_stdout_and_stderr(self):
        result = build.subprocess.CompletedProcess(["fake"], 0, stdout="stdout", stderr="stderr")
        with patch.object(build.subprocess, "run", return_value=result) as run:
            self.assertEqual(build.run(["fake"], separate_stderr=True), ("stdout", "stderr"))
        self.assertEqual(run.call_args.kwargs["stdout"], build.subprocess.PIPE)
        self.assertEqual(run.call_args.kwargs["stderr"], build.subprocess.PIPE)

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


class ContractReferenceTests(unittest.TestCase):
    setUp = BuildTests.setUp

    def inputs_fixture(self):
        paths = [build.PROBE / name for name in ("CMakeLists.txt", "build.py", "probe.cpp", "pattern.hpp",
                                                *build.CONTRACT_INPUTS, *build.ENVIRONMENT_INPUTS)]
        paths += [build.SNAPSHOT, Path("tools/collect_licenses.sh"), Path("LICENSE"), Path("NOTICE"),
                  Path("packaging/licenses/dawn.txt"),
                  build.DEPS / f"sdl-subbuild/sdl-populate-prefix/src/{build.SDL_PIN}.tar.gz",
                  build.DEPS / "dawn_prebuilt-subbuild/dawn_prebuilt-populate-prefix/src/dawn-linux-aarch64.tar.gz",
                  build.DEPS / "sdl-src/src/video/sdl2/SDL_sdl2video.c",
                  build.DEPS / "dawn_prebuilt-src/lib/cmake/Dawn/DawnConfig.cmake"]
        for rel in paths:
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture")
        stamp = self.root / build.DEPS / "sdl-subbuild/sdl-populate-prefix/src/sdl-populate-stamp/sdl-populate-urlinfo.txt"
        stamp.parent.mkdir()
        stamp.write_text(f"https://github.com/bmdhacks/SDL/archive/{build.SDL_PIN}.tar.gz")
        return contract_reference_fixture(self.root)

    def test_reference_matches_exact_reviewed_bytes(self):
        expected = contract_reference_fixture(self.root)
        with patch.object(build, "CONTRACT_REFERENCE_SHA256", expected):
            self.assertEqual(build.verify_contract_reference(self.root), expected)
        self.assertNotIn(str(self.root), json.dumps(expected))

    def test_each_reference_drift_requires_mirror_review(self):
        expected = contract_reference_fixture(self.root)
        with patch.object(build, "CONTRACT_REFERENCE_SHA256", expected):
            for rel in expected:
                with self.subTest(path=rel):
                    original = (self.root / rel).read_bytes()
                    (self.root / rel).write_bytes(original + b"changed\n")
                    with self.assertRaisesRegex(ValueError, "Contract reference drift:.*update the contract.*after review") as error:
                        build.verify_contract_reference(self.root)
                    self.assertIn(rel, str(error.exception))
                    self.assertIn("do not edit production sources", str(error.exception))
                    (self.root / rel).write_bytes(original)

    def test_each_missing_reference_is_actionable(self):
        expected = contract_reference_fixture(self.root)
        with patch.object(build, "CONTRACT_REFERENCE_SHA256", expected):
            for rel in expected:
                with self.subTest(path=rel):
                    original = (self.root / rel).read_bytes()
                    (self.root / rel).unlink()
                    with self.assertRaisesRegex(ValueError, "Missing contract reference:.*restore the reviewed U source"):
                        build.verify_contract_reference(self.root)
                    (self.root / rel).write_bytes(original)

    def test_reference_cannot_escape_project(self):
        expected = contract_reference_fixture(self.root)
        rel = next(iter(expected))
        with tempfile.TemporaryDirectory() as directory:
            outside = Path(directory) / "reference.cpp"
            outside.write_bytes((self.root / rel).read_bytes())
            (self.root / rel).unlink()
            (self.root / rel).symlink_to(outside)
            with patch.object(build, "CONTRACT_REFERENCE_SHA256", expected), \
                 self.assertRaisesRegex(ValueError, "Contract reference escapes project"):
                build.verify_contract_reference(self.root)

    def test_real_check_inputs_checks_reference_after_both_archives(self):
        expected = self.inputs_fixture()
        events = []
        verify = build.verify_contract_reference

        def archive(*args):
            events.append("archive")
            return {"archive_sha256": args[2]}

        def reference(root):
            events.append("reference")
            return verify(root)

        with patch.object(build, "CONTRACT_REFERENCE_SHA256", expected), \
             patch.object(build, "verify_archive", side_effect=archive), \
             patch.object(build, "verify_contract_reference", side_effect=reference):
            result = build.check_inputs(self.root)
        self.assertEqual(events, ["archive", "archive", "reference"])
        self.assertEqual(result["contract_reference_sha256"], expected)
        self.assertEqual(result["sdl"]["archive_sha256"], build.SDL_SHA)
        self.assertEqual(result["dawn"]["archive_sha256"], build.DAWN_SHA)

    def test_archive_failure_precedes_reference_check(self):
        self.inputs_fixture()
        for results in ([ValueError("archive rejected")], [{}, ValueError("archive rejected")]):
            with self.subTest(archive=len(results)), \
                 patch.object(build, "verify_archive", side_effect=results), \
                 patch.object(build, "verify_contract_reference") as reference, \
                 self.assertRaisesRegex(ValueError, "archive rejected"):
                build.check_inputs(self.root)
            reference.assert_not_called()

    def test_real_check_inputs_rejects_reference_drift(self):
        expected = self.inputs_fixture()
        (self.root / next(iter(expected))).write_text("drift")
        with patch.object(build, "CONTRACT_REFERENCE_SHA256", expected), \
             patch.object(build, "verify_archive", return_value={}), \
             self.assertRaisesRegex(ValueError, "Contract reference drift"):
            build.check_inputs(self.root)

    def test_contract_files_are_required_inputs(self):
        self.inputs_fixture()
        for name in build.CONTRACT_INPUTS:
            path = self.root / build.PROBE / name
            original = path.read_bytes()
            path.unlink()
            with self.subTest(name=name), patch.object(build, "verify_archive") as archive, \
                 self.assertRaisesRegex(ValueError, "Missing input:.*" + name):
                build.check_inputs(self.root)
            archive.assert_not_called()
            path.write_bytes(original)

    def test_real_check_inputs_cli_is_readonly_and_never_runs_commands(self):
        expected = self.inputs_fixture()
        before = {str(p.relative_to(self.root)): p.read_bytes() if p.is_file() else None
                  for p in self.root.rglob("*")}
        stdout = io.StringIO()
        with patch.object(build, "__file__", str(self.root / build.PROBE / "build.py")), \
             patch.object(build, "CONTRACT_REFERENCE_SHA256", expected), \
             patch.object(build, "verify_archive", return_value={}), \
             patch.object(build, "run") as run, contextlib.redirect_stdout(stdout):
            self.assertEqual(build.main(["--check-inputs"]), 0)
        run.assert_not_called()
        self.assertEqual(json.loads(stdout.getvalue())["contract_reference_sha256"], expected)
        after = {str(p.relative_to(self.root)): p.read_bytes() if p.is_file() else None
                 for p in self.root.rglob("*")}
        self.assertEqual(before, after)


class SmokeTests(unittest.TestCase):
    setUp = BuildTests.setUp

    def smoke_fixture(self):
        package = self.root / "package.partial"
        (package / "metadata").mkdir(parents=True)
        return package, ["ELF fixture"], {"LD_LIBRARY_PATH": str(package)}

    def test_only_safe_modes_run_and_preserve_null_output(self):
        package, reports, env = self.smoke_fixture()
        output = "budget16 normal/shadow controls; budget17 remains unvalidated\n" + build.LAYOUT_SELF_TEST_SUCCESS + "\n"
        with patch.object(build, "run", side_effect=[("Usage\n", ""), ("PASS self-test (CPU-only)\n", ""),
                                                    (output, "Null diagnostic\n")]) as run:
            build.smoke_tests(package, self.root, reports, env)
        self.assertEqual([call.args[0] for call in run.call_args_list],
                         [[package / "h700_graphics_probe", mode]
                          for mode in ("--help", "--self-test", "--layout-self-test")])
        for call in run.call_args_list:
            self.assertEqual(call.kwargs, {"timeout": 30, "env": env, "separate_stderr": True})
        report = (self.root / "elf-report.txt").read_text()
        self.assertIn("ELF fixture", report)
        self.assertIn("--layout-self-test (Dawn Null validation only; no SDL/EGL/GPU)", report)
        self.assertIn(output, report)
        self.assertIn("Null diagnostic", report)
        layout_report = (package / "metadata/layout-self-test.txt").read_text()
        self.assertTrue(layout_report.startswith(output))
        self.assertIn("Null diagnostic", layout_report)

    def test_layout_zero_exit_without_exact_final_four_case_budget16_marker_is_rejected(self):
        package, reports, env = self.smoke_fixture()
        marker = build.LAYOUT_SELF_TEST_SUCCESS
        for output in ("", "Usage\n", "PASS layout-self-test\n", marker.replace("cases=4", "cases=8"),
                       marker.replace("budget=16", "budget=17"),
                       "PASS layout-self-test cases=8 (Dawn Null validation only; no SDL/EGL/GPU)",
                       marker.replace("Dawn Null", "OpenGLES"), "prefix " + marker, marker + " extra", marker + " ",
                       marker + "\nFAIL later check\n"):
            with self.subTest(output=output), \
                 patch.object(build, "run", side_effect=[("Usage", ""), ("PASS self-test (CPU-only)", ""), (output, "")]), \
                 self.assertRaisesRegex(ValueError, "final four-case budget-16 success marker"):
                build.smoke_tests(package, self.root, reports, env)
            self.assertEqual((package / "metadata/layout-self-test.txt").read_text(), output)
            self.assertIn(output, (self.root / "elf-report.txt").read_text())

    def test_cpu_zero_exit_without_pass_marker_is_rejected(self):
        package, reports, env = self.smoke_fixture()
        with patch.object(build, "run", side_effect=[("Usage", ""), ("early exit", "")]) as run, \
             self.assertRaisesRegex(ValueError, "CPU self-test exited without"):
            build.smoke_tests(package, self.root, reports, env)
        self.assertEqual(run.call_count, 2)
        self.assertIn("early exit", (self.root / "elf-report.txt").read_text())

    def test_stderr_success_marker_cannot_substitute_for_stdout(self):
        package, reports, env = self.smoke_fixture()
        for mode in ("--self-test", "--layout-self-test"):
            outputs = [("Usage", "")]
            if mode == "--layout-self-test":
                outputs.append(("PASS self-test (CPU-only)", ""))
            outputs.append(("early exit\n", build.LAYOUT_SELF_TEST_SUCCESS if mode == "--layout-self-test"
                            else "PASS self-test (CPU-only)"))
            with self.subTest(mode=mode), patch.object(build, "run", side_effect=outputs), \
                 self.assertRaisesRegex(ValueError, "success marker"):
                build.smoke_tests(package, self.root, reports, env)
            self.assertIn("stderr", (self.root / "elf-report.txt").read_text())

    def test_layout_nonzero_exit_is_not_accepted_even_with_success_marker(self):
        package, reports, env = self.smoke_fixture()
        result = build.subprocess.CompletedProcess(["probe"], 1, stdout=build.LAYOUT_SELF_TEST_SUCCESS)
        with patch.object(build.subprocess, "run", side_effect=[
                build.subprocess.CompletedProcess(["probe"], 0, stdout="Usage"),
                build.subprocess.CompletedProcess(["probe"], 0, stdout="PASS self-test (CPU-only)"), result]), \
             self.assertRaisesRegex(ValueError, "failed \\(1\\)"):
            build.smoke_tests(package, self.root, reports, env)
        self.assertIn(build.LAYOUT_SELF_TEST_SUCCESS, (self.root / "elf-report.txt").read_text())
        self.assertIn("FAILED", (self.root / "elf-report.txt").read_text())


class ContainerBuildTests(unittest.TestCase):
    setUp = BuildTests.setUp

    def container_fixture(self):
        expected = ContractReferenceTests.inputs_fixture(self)
        out = self.root / "build/h700-graphics-probe-fixture"
        work = out / "build"
        work.mkdir(parents=True)
        (out / build.MARKER).write_text("h700-graphics-probe-v1\n")
        for name in ("probe", "sdl"):
            (work / name).write_text("artifact fixture")
        (work / "artifacts.json").write_text(json.dumps({"executable": str(work / "probe"), "sdl": str(work / "sdl")}))
        (self.root / build.SNAPSHOT).write_text("{}")
        installed = self.root / "installed-verifier.py"
        installed.write_bytes((self.root / build.PROBE / "verify_environment.py").read_bytes())
        image_report = self.root / "image-abi.json"
        image_report.write_text('{"architecture": "aarch64"}')
        packages = self.root / "packages.tsv"
        packages.write_text("package\tversion\n")
        paths = {"/src": self.root, "/out": out, "/usr/local/lib/h700/verify_environment.py": installed,
                 "/opt/h700-toolchain/abi.json": image_report, "/opt/h700-toolchain/packages.tsv": packages}
        return out, expected, paths

    def mock_container_build(self, drift=None, layout_output=None):
        out, expected, paths = self.container_fixture()
        layout_output = build.LAYOUT_SELF_TEST_SUCCESS + "\n" if layout_output is None else layout_output

        def command(argv, **kwargs):
            tool = str(argv[0])
            if tool == "python3":
                return '{"architecture": "aarch64"}'
            if tool == "cmake" and "--version" not in argv:
                return "configured/built fixture"
            if tool == "readelf":
                return ELF
            if tool == "ldd":
                return "libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6\n"
            if tool == str(out / "package.partial/h700_graphics_probe"):
                return ({"--help": "Usage\n", "--self-test": "PASS self-test (CPU-only)\n",
                         "--layout-self-test": layout_output}[argv[1]], "")
            if tool == "bash":
                if drift:
                    (self.root / drift).write_text("changed during build")
                return "licenses collected fixture"
            if argv[1:] == ["--version"]:
                return "tool version fixture\n"
            self.fail(f"Unexpected command: {argv}")

        with contextlib.ExitStack() as stack:
            stack.enter_context(patch.object(build, "Path", side_effect=lambda p: paths.get(str(p), Path(p))))
            stack.enter_context(patch.object(build, "__file__", str(self.root / build.PROBE / "build.py")))
            stack.enter_context(patch.object(build, "CONTRACT_REFERENCE_SHA256", expected))
            stack.enter_context(patch.object(build, "verify_archive", return_value={}))
            stack.enter_context(patch.object(build, "firmware_gate", return_value={}))
            stack.enter_context(patch.object(build.subprocess, "run", side_effect=AssertionError("No real commands allowed")))
            run = stack.enter_context(patch.object(build, "run", side_effect=command))
            args = build.argparse.Namespace(image_id="sha256:" + "a" * 64, jobs=2)
            build.container_build(args)
        return out, expected, run

    def test_manifest_records_reference_sources_and_null_validation_scope(self):
        out, expected, run = self.mock_container_build()
        package = out / "package"
        self.assertFalse((out / "package.partial").exists())
        manifest = json.loads((package / "manifest.json").read_text())
        self.assertEqual(manifest["contract_reference_sha256"], expected)
        self.assertEqual(manifest["dependencies"]["contract_reference_sha256"], expected)
        coverage = manifest["layout_validation_scope"]
        self.assertEqual(coverage["backend"], "Null")
        self.assertEqual(coverage["cases"], 4)
        self.assertEqual(coverage["effective_sampled_texture_budget"], 16)
        self.assertIn("device-budget-17/lightmap layouts", coverage["not_validated"])
        self.assertIn("Mali request/layout", coverage["not_validated"])
        self.assertEqual({rel: manifest["sources_sha256"][rel] for rel in expected}, expected)
        for name in build.CONTRACT_INPUTS:
            self.assertEqual(manifest["sources_sha256"][(build.PROBE / name).as_posix()],
                             build.sha256(self.root / build.PROBE / name))
        self.assertEqual(manifest["files_sha256"]["metadata/layout-self-test.txt"],
                         build.sha256(package / "metadata/layout-self-test.txt"))
        for scope in ("compiler/link/ABI", "Dawn Null layout validation only", "no SDL/EGL/GPU initialization",
                      "no new firmware runtime validation"):
            self.assertIn(scope, manifest["validation"])
        commands = [call.args[0] for call in run.call_args_list]
        self.assertFalse(any("--mode" in command for command in commands))
        self.assertIn("--layout-self-test", [str(arg) for command in commands for arg in command])

    def test_midbuild_reference_drift_prevents_publication(self):
        with self.assertRaisesRegex(ValueError, "sources changed during build"):
            self.mock_container_build(drift=next(iter(build.CONTRACT_REFERENCE_SHA256)))
        out = self.root / "build/h700-graphics-probe-fixture"
        self.assertFalse((out / "package").exists())
        self.assertFalse((out / "package.partial/manifest.json").exists())

    def test_midbuild_contract_drift_prevents_publication(self):
        with self.assertRaisesRegex(ValueError, "sources changed during build"):
            self.mock_container_build(drift=build.PROBE / "contract.cpp")
        self.assertFalse((self.root / "build/h700-graphics-probe-fixture/package").exists())

    def test_layout_early_exit_prevents_manifest_and_publication(self):
        with self.assertRaisesRegex(ValueError, "final four-case budget-16 success marker"):
            self.mock_container_build(layout_output="early exit\n")
        out = self.root / "build/h700-graphics-probe-fixture"
        self.assertFalse((out / "package").exists())
        self.assertFalse((out / "package.partial/manifest.json").exists())
        self.assertIn("early exit", (out / "elf-report.txt").read_text())


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
