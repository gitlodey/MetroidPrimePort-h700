#!/usr/bin/env python3
"""Offline IG-01 build of real Aurora targets; no deployment or graphics tests.

--check-inputs is read-only and never invokes Docker. A build needs a fresh
project/build/... directory and the already approved native ARM64 image.
InitialCache.cmake owns the build/provider profile; Inputs.cmake owns staging.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = Path("tools/h700_integration_driver")
IMAGE_ID = "sha256:2db4439763783aa796da4268dbd91847ee2c13deef4c8ce2439e0f6bdf926962"
MARKER = ".h700-integration-driver"

# Reuse the ABI/environment inspector, not the old probe's production-hash gate.
sys.path.insert(0, str(ROOT / "tools/h700_graphics_probe"))
import verify_environment as environment


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def relative_path(value):
    rel = PurePosixPath(value)
    require(bool(rel.parts) and not rel.is_absolute() and ".." not in rel.parts
            and "\\" not in value and ":" not in value, f"Unsafe relative path: {value}")
    return rel


def input_path(root, entry):
    path = root / str(relative_path(entry["path"]))
    require(path.resolve().is_relative_to(root.resolve()), f"Input escapes project: {entry['path']}")
    require(path.is_file(), f"Missing input: {entry['path']}")
    require(sha256(path) == entry["sha256"], f"Input SHA256 mismatch: {entry['path']}")
    return path


def check_inputs(root=ROOT):
    manifest = json.loads((root / DRIVER / "inputs.json").read_text())
    require(manifest["schema"] == 1 and manifest["image_id"] == IMAGE_ID
            and manifest["platform"] == "linux/arm64", "Unexpected input manifest/image")
    names, overrides = set(), set()
    for entry in manifest["archives"]:
        name, override = entry["name"], entry["fetchcontent"]
        require(re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", name) and name not in names,
                f"Invalid/duplicate input name: {name}")
        require(re.fullmatch(r"[A-Z0-9_-]+", override) and override not in overrides,
                f"Invalid/duplicate FetchContent name: {override}")
        require(re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]), f"Invalid SHA256: {name}")
        require(entry.get("package") in (None, "DAWN", "NOD"), f"Invalid package: {name}")
        prefix = entry["strip_prefix"]
        require(not prefix or relative_path(prefix).parts == (prefix,), f"Invalid archive root: {name}")
        input_path(root, entry)
        names.add(name)
        overrides.add(override)
    require(bool(names), "Empty input manifest")
    return manifest


def archive_path(name, prefix):
    # Tar packages use ./ paths, while source archives have one pinned root.
    rel = PurePosixPath(name)
    require(not rel.is_absolute() and ".." not in rel.parts and "\\" not in name
            and ":" not in name, f"Unsafe archive path: {name}")
    if prefix:
        require(bool(rel.parts) and rel.parts[0] == prefix, f"Unexpected archive root: {name}")
        rel = PurePosixPath(*rel.parts[1:])
    return rel


def extract_archive(archive, destination, prefix):
    """Validate the complete member list before writing; never extract via links."""
    require(not destination.exists() and not destination.is_symlink(), "Fresh staging directory required")
    is_zip = archive.suffix == ".zip"
    opener = zipfile.ZipFile if is_zip else tarfile.open
    with opener(archive) as source:
        entries = []
        seen, links = set(), set()
        for member in source.infolist() if is_zip else source.getmembers():
            rel = archive_path(member.filename if is_zip else member.name, prefix)
            directory = member.is_dir() if is_zip else member.isdir()
            symlink = False if is_zip else member.issym()
            if is_zip:
                mode = member.external_attr >> 16
                require(stat.S_IFMT(mode) in (0, stat.S_IFREG, stat.S_IFDIR), "Unsupported ZIP member/link")
            else:
                require(directory or member.isfile() or symlink, f"Unsupported tar member: {member.name}")
            if not rel.parts:
                require(directory, "Archive root is not a directory")
                continue
            require(rel not in seen, f"Duplicate archive path: {rel}")
            seen.add(rel)
            if symlink:
                link = relative_path(member.linkname)
                require((destination / str(rel.parent / link)).resolve().is_relative_to(destination.resolve()),
                        f"Unsafe archive link: {rel}")
                links.add(rel)
            entries.append((member, rel, directory, symlink))
        require(bool(entries), "Empty archive")
        for _, rel, _, _ in entries:
            require(not any(parent in links for parent in rel.parents), f"Archive writes through link: {rel}")
        destination.mkdir(parents=True)
        for member, rel, directory, symlink in entries:
            target = destination / str(rel)
            if directory:
                target.mkdir(parents=True, exist_ok=True)
            elif not symlink:
                target.parent.mkdir(parents=True, exist_ok=True)
                stream = source.open(member) if is_zip else source.extractfile(member)
                with stream, target.open("xb") as output:
                    shutil.copyfileobj(stream, output)
                mode = (member.external_attr >> 16) if is_zip else member.mode
                target.chmod(0o755 if mode & 0o111 else 0o644)
        # Links are installed last, and cannot be ancestors of any member.
        for member, rel, _, symlink in entries:
            if symlink:
                target = destination / str(rel)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.symlink_to(member.linkname)


def stage_inputs(root, out):
    manifest = check_inputs(root)  # ALL hashes pass before any source override exists.
    require(not (out / "Inputs.cmake").exists(), "Inputs.cmake already exists")
    lines = ["# Generated only after verification/extraction of all manifest inputs.",
             'set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)',
             'set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)']
    for entry in manifest["archives"]:
        archive = input_path(root, entry)
        stage = out / "inputs" / entry["name"]
        extract_archive(archive, stage, entry["strip_prefix"])
        source = f"/out/inputs/{entry['name']}"
        lines.append(f'set(FETCHCONTENT_SOURCE_DIR_{entry["fetchcontent"]} "{source}" CACHE PATH "" FORCE)')
        if entry.get("package"):
            # Keep URL/hash explicit even though the validated package source is
            # overridden too: FULLY_DISCONNECTED must not skip package population.
            copied = stage / archive.name
            require(not copied.exists(), "Package archive conflicts with extracted member")
            shutil.copyfile(archive, copied)
            require(sha256(copied) == entry["sha256"], "Staged package SHA256 mismatch")
            package = entry["package"]
            lines += [f'set(AURORA_{package}_PACKAGE_URL "file://{source}/{archive.name}" CACHE STRING "" FORCE)',
                      f'set(AURORA_{package}_PACKAGE_SHA256 "{entry["sha256"]}" CACHE STRING "" FORCE)']
    lines += ['set(H700_INPUTS_DIR "/out/inputs" CACHE PATH "" FORCE)',
              'set(H700_INPUTS_STAGED ON CACHE BOOL "" FORCE)']
    write_json(out / "input-manifest.json", manifest)
    (out / "Inputs.cmake").write_text("\n".join(lines) + "\n")
    return manifest


def output_path(root, value):
    root = root.resolve()
    requested = Path(value)
    requested = requested if requested.is_absolute() else root / requested
    require(not (root / "build").is_symlink(), "build/ cannot be a symlink")
    # Do not silently follow a symlink in any existing output ancestor.
    require(not any(p.is_symlink() for p in (requested, *requested.parents)), "Output cannot use symlinks")
    resolved = requested.resolve()
    require(resolved.is_relative_to(root / "build") and resolved != root / "build",
            "Output must be a NEW directory below project/build/")
    require(not resolved.exists(), "Output already exists; choose a fresh --output (nothing overwritten)")
    return resolved


class Commands:
    def __init__(self, out, stem="commands"):
        self.out, self.stem, self.records = out, stem, []

    def run(self, label, command, timeout):
        command = list(map(str, command))
        log = self.out / (label + ".log")
        record = {"command": command, "timeout_seconds": timeout, "log": log.name, "status": "running"}
        self.records.append(record)
        write_json(self.out / (self.stem + ".json"), self.records)
        started = time.monotonic()
        try:
            with log.open("x") as output:
                result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                        stdin=subprocess.DEVNULL, timeout=timeout, check=False)
            record.update(status="finished", exit_code=result.returncode)
            require(result.returncode == 0, f"{label} failed ({result.returncode}); see {log}")
        except subprocess.TimeoutExpired:
            record.update(status="timeout", exit_code=124)
            raise
        except BaseException:
            if record["status"] == "running":
                record["status"] = "interrupted/failed"
            raise
        finally:
            record["elapsed_seconds"] = round(time.monotonic() - started, 3)
            write_json(self.out / (self.stem + ".json"), self.records)
        return log


def configure_command():
    return ["cmake", "-S", "/src/tools/h700_integration_driver", "-B", "/out/build", "-G", "Ninja",
            "-C", "/src/cmake/h700/InitialCache.cmake", "-C", "/out/Inputs.cmake",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]


def build_command(jobs):
    return ["cmake", "--build", "/out/build", "--target", "h700_integration_driver", "--parallel", str(jobs)]


def docker_command(root, out, name, args):
    command = ["docker", "run", "--rm", "--pull=never", "--name", name, "--platform", "linux/arm64",
               "--network=none", "--read-only", "--cap-drop=ALL", "--security-opt=no-new-privileges",
               "--volume", f"{root}:/src:ro", "--volume", f"{out}:/out:rw", "--workdir", "/out",
               "--env", "HOME=/out/home", "--env", "TMPDIR=/out/tmp", "--env", "XDG_CACHE_HOME=/out/cache",
               IMAGE_ID, "python3", "-B", "/src/tools/h700_integration_driver/build.py",
               "--container", "--jobs", str(args.jobs)]
    if args.configure_only:
        command.append("--configure-only")
    return command


def inspect_artifacts(out, commands):
    # CMake's file(GENERATE) must supply these two actual target paths.
    artifacts = json.loads((out / "build/artifacts.json").read_text())
    reports = {}
    for key in ("executable", "sdl"):
        artifact = Path(artifacts[key]).resolve()
        require(artifact.is_relative_to(out / "build") and artifact.is_file(), f"Artifact escapes build: {key}")
        log = commands.run(key + "-elf", ["readelf", "-W", "-h", "-l", "-d", "--dyn-syms",
                                          "--version-info", artifact], 30)
        text = log.read_text()
        require(re.search(r"Class:\s+ELF64\b", text) and re.search(r"Machine:\s+AArch64\b", text),
                f"Wrong artifact architecture: {key}")
        if key == "executable":
            require(environment.INTERPRETER in text, "Wrong executable ELF interpreter")
        definitions, needs = environment.version_nodes(text)
        reports[key] = {"path": str(artifact), "sha256": sha256(artifact),
                        "defined_version_nodes": definitions, "needed_version_nodes": needs,
                        "needed": re.findall(r"\(NEEDED\).*?\[(.*?)\]", text),
                        "undefined_symbols": environment.symbols(text, undefined=True)}
    write_json(out / "elf-metadata.json", {"artifacts": reports,
               "limitations": "ELF metadata only; not a firmware ABI or device runtime PASS"})
    commands.run("help", [artifacts["executable"], "--help"], 30)


def container_build(args):
    out = Path("/out")
    require(ROOT == Path("/src") and (out / MARKER).read_text() == IMAGE_ID + "\n",
            "Use the host build.py entry point")
    commands = Commands(out)
    report = environment.inspect_environment()
    require(report == json.loads(Path("/opt/h700-toolchain/abi.json").read_text()),
            "Image ABI differs from its recorded environment; stop, do not update the image")
    for tool in ("clang-16", "clang++-16", "cmake", "ninja"):
        log = commands.run(tool, [tool, "--version"], 30)
        report.setdefault("tools", {})[tool] = log.read_text().splitlines()[0]
    require("clang version 16." in report["tools"]["clang-16"]
            and "clang version 16." in report["tools"]["clang++-16"], "Clang16 required")
    report.update(image_id=IMAGE_ID, platform="linux/arm64")
    write_json(out / "environment.json", report)
    shutil.copyfile("/opt/h700-toolchain/packages.tsv", out / "packages.tsv")
    stage_inputs(ROOT, out)
    commands.run("configure", configure_command(), 300)
    if not args.configure_only:
        commands.run("build", build_command(args.jobs), 1800)
        inspect_artifacts(out, commands)
    write_json(out / "result.json", {"status": "configure-only" if args.configure_only else "build-complete",
               "device_validated": False, "image_id": IMAGE_ID})


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-inputs", action="store_true", help="Read-only archive hash check; no Docker")
    parser.add_argument("--output", default="build/h700-integration-driver", help="NEW project/build/... directory")
    parser.add_argument("--configure-only", action="store_true", help="Stage inputs and configure, without building")
    parser.add_argument("--jobs", type=int, default=2, choices=range(1, 9), metavar="1..8")
    parser.add_argument("--container", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    try:
        if args.check_inputs:
            require(not args.container, "--container cannot be combined with --check-inputs")
            print(json.dumps(check_inputs(), indent=2, sort_keys=True))
            return 0
        if args.container:
            container_build(args)
            return 0
        out = output_path(ROOT, args.output)
        manifest = check_inputs()
        for rel in (DRIVER / "CMakeLists.txt", DRIVER / "main.cpp", Path("cmake/h700/InitialCache.cmake")):
            require((ROOT / rel).is_file(), f"Missing build source: {rel}")
        require(shutil.which("docker"), "Docker CLI unavailable; no automatic install/pull/build")
        out.mkdir(parents=True)
        for name in ("home", "tmp", "cache"):
            (out / name).mkdir()
        (out / MARKER).write_text(IMAGE_ID + "\n")
        write_json(out / "input-manifest.json", manifest)
        commands = Commands(out, "host-commands")
        inspected = commands.run("image-inspect", ["docker", "image", "inspect", "--format",
                                 "{{.Id}} {{.Os}} {{.Architecture}}", IMAGE_ID], 30).read_text().split()
        require(inspected == [IMAGE_ID, "linux", "arm64"], "Exact approved Linux ARM64 image required")
        native = commands.run("docker-native", ["docker", "info", "--format", "{{.Architecture}}"], 30)
        require(native.read_text().strip() in ("aarch64", "arm64"), "Native ARM64 Docker daemon required")
        name = "h700-integration-" + uuid.uuid4().hex
        try:
            commands.run("container", docker_command(ROOT, out, name, args), 2200)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            commands.run("container-cleanup", ["docker", "rm", "--force", name], 30)
            raise
        print(f"Completed: {out.relative_to(ROOT)} (NOT device-validated; no deploy)")
        return 0
    except (ValueError, KeyError, OSError, tarfile.TarError, zipfile.BadZipFile, subprocess.TimeoutExpired) as error:
        print(f"Integration build refused/failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
