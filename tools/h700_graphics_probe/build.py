#!/usr/bin/env python3
"""Offline Linux ARM64 probe build; never deploys or initializes device graphics.

--check-inputs only reads local files (no Docker). Builds require a NEW dedicated
output directory. The source/cache bind mount is read-only; only /out is retained.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import uuid

import verify_environment as environment

IMAGE = "metroid-h700-probe:abi238-v1"
DEPS = Path("build/muos-linux-arm64/_deps")
PROBE = Path("tools/h700_graphics_probe")
SDL_PIN = "6057d79baf8321bf190479a699655f06cc2a962f"
# Observed local archive, NOT an independently authenticated upstream checksum.
SDL_SHA = "0b5fdcb0c1142ba8197ba7ca59fe0371489c4e6e97c82826acce55bf86c974dc"
DAWN_SHA = "cbb550709fdd519d52330b9b59206a8300bcbd8bd698af0359e0ba5bfca2fed8"
LIMITS = {"GLIBC": "2.38", "GLIBCXX": "3.4.32", "CXXABI": "1.3.14"}
MARKER = ".h700-graphics-probe"
SNAPSHOT = Path("docs/H700_RUNTIME_ABI.json")
ENVIRONMENT_INPUTS = ("verify_environment.py", "Dockerfile.abi238", ".dockerignore")
CONTRACT_INPUTS = ("contract.hpp", "contract.cpp")
# Exact U bytes mirrored by contract.cpp; changes require review of the mirror.
CONTRACT_REFERENCE_SHA256 = {
    "extern/aurora/lib/webgpu/gpu.cpp": "bccac03231852388acb82fb69317646f2a8112bec805fbce436f837309df80b7",
    "extern/aurora/lib/gfx/frame.cpp": "af429b9f8757c1ad170ac84814d70aea2d8a84206a08afec7bb837d901f04ab0",
    "extern/aurora/lib/gx/gx.cpp": "259afb0bdc5a7db0e2e5ffd3505b184021958d8788f02eb5d547d37ee7b8ec2a",
    "extern/aurora/lib/gx/gx.hpp": "2b61a2b2d152470eeefd866ea420e8e64a0a4b54afd420eb0a843e51e60f276c",
    "extern/aurora/lib/gfx/probe.hpp": "9317c81e6b21438b0a1d37e4147feee60592b6dc7b4a97d1985f531480a05376",
}
LAYOUT_SELF_TEST_SUCCESS = "PASS layout-self-test cases=4 budget=16 (Dawn Null validation only; no SDL/EGL/GPU)"
LAYOUT_VALIDATION_SCOPE = {
    "backend": "Null", "effective_sampled_texture_budget": 16, "cases": 4,
    "positive": "U storage-fetch normal/shadow layouts without lightmap",
    "negative": "U texture-fetch normal/shadow layouts exceed fragment budget (17 > 16)",
    "not_validated": ["device-budget-17/lightmap layouts", "Mali request/layout", "shaders/pixels/threading/recreation"],
}
ABI_LIMITATIONS = [
    "Firmware snapshot covers version nodes and only five selected exports, not every imported symbol",
    "Other imports are gated by provider/version nodes only; actual device loader validation is pending",
    "dlopen-loaded SDL2/graphics/audio drivers and plugins are not validated by this artifact gate",
]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(stream):
    value = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        value.update(chunk)
    return value.hexdigest()


def sha256(path):
    with path.open("rb") as stream:
        return digest(stream)


def verify_archive(archive, tree, expected, prefix=""):
    require(sha256(archive) == expected, f"Archive checksum mismatch: {archive.name}")
    files = {}
    with tarfile.open(archive, "r:gz") as tar:
        for member in tar:
            rel = PurePosixPath(member.name)
            require(not rel.is_absolute() and ".." not in rel.parts, "Unsafe archive path")
            if prefix:
                require(bool(rel.parts) and rel.parts[0] == prefix, "Unexpected archive root")
                rel = PurePosixPath(*rel.parts[1:])
            if member.isdir():
                continue
            target = tree / str(rel)
            require(member.isfile() and not target.is_symlink(), "Unsupported archive/tree link")
            require(target.resolve().is_relative_to(tree.resolve()), "Source escapes cached tree")
            require(target.is_file(), f"Missing cached source: {rel}")
            with tar.extractfile(member) as stream:
                files[str(rel)] = digest(stream)
            require(sha256(target) == files[str(rel)], f"Cached source differs from archive: {rel}")
    actual = set()
    for path in tree.rglob("*"):
        require(not path.is_symlink(), "Symlink in cached dependency tree")
        if path.is_file() and path.name != ".DS_Store":
            actual.add(path.relative_to(tree).as_posix())
    require(actual == set(files) and bool(files), "Extra or missing files in cached dependency tree")
    return {"archive_sha256": expected,
            "tree_sha256": hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()}


def verify_contract_reference(root):
    hashes = {}
    for rel, expected in CONTRACT_REFERENCE_SHA256.items():
        path = root / rel
        require(path.is_file(), f"Missing contract reference: {rel}; restore the reviewed U source")
        require(path.resolve().is_relative_to(root.resolve()), f"Contract reference escapes project: {rel}")
        actual = sha256(path)
        require(actual == expected,
                f"Contract reference drift: {rel} (expected {expected}, got {actual}); "
                "review the U changes, update the contract.hpp/.cpp mirror and reference hashes after review; "
                "do not edit production sources to bypass this check")
        hashes[rel] = actual
    return hashes


def check_inputs(root):
    required = [PROBE / name for name in ("CMakeLists.txt", "build.py", "probe.cpp", "pattern.hpp",
                                         *CONTRACT_INPUTS, *ENVIRONMENT_INPUTS)]
    required.append(SNAPSHOT)
    required += [Path(p) for p in ("tools/collect_licenses.sh", "LICENSE", "NOTICE",
                                  "packaging/licenses/dawn.txt")]
    sdl_archive = DEPS / f"sdl-subbuild/sdl-populate-prefix/src/{SDL_PIN}.tar.gz"
    dawn_archive = DEPS / "dawn_prebuilt-subbuild/dawn_prebuilt-populate-prefix/src/dawn-linux-aarch64.tar.gz"
    stamp = DEPS / "sdl-subbuild/sdl-populate-prefix/src/sdl-populate-stamp/sdl-populate-urlinfo.txt"
    required += [sdl_archive, dawn_archive, stamp,
                 DEPS / "sdl-src/src/video/sdl2/SDL_sdl2video.c",
                 DEPS / "dawn_prebuilt-src/lib/cmake/Dawn/DawnConfig.cmake"]
    for rel in required:
        require((root / rel).is_file(), f"Missing input: {rel}")
        require((root / rel).resolve().is_relative_to(root.resolve()), f"Input escapes project: {rel}")
    require(f"https://github.com/bmdhacks/SDL/archive/{SDL_PIN}.tar.gz" in (root / stamp).read_text(),
            "SDL source stamp does not identify the expected fork (stamp is advisory)")
    provenance = {"sdl": {"claimed_ref": SDL_PIN,
                    "provenance": "Observed cached archive digest and exact tree match; stamp/ref not independently authenticated",
                    **verify_archive(root / sdl_archive, root / DEPS / "sdl-src", SDL_SHA, f"SDL-{SDL_PIN}")},
            "dawn": {"version": "v20261029.114630", "ref": "118436b3f8f68583eb86e811ca605dc41637e8cc",
                     **verify_archive(root / dawn_archive, root / DEPS / "dawn_prebuilt-src", DAWN_SHA)}}
    provenance["contract_reference_sha256"] = verify_contract_reference(root)
    return provenance


def output_path(root, value, fresh=True):
    root = root.resolve()
    requested = Path(value)
    requested = requested if requested.is_absolute() else root / requested
    require(not (root / "build").is_symlink() and not requested.is_symlink(), "Output cannot be a symlink")
    resolved = requested.resolve()
    require(resolved.parent == root / "build" and
            re.fullmatch(r"h700-graphics-probe(?:[-_][A-Za-z0-9_-]+)?", resolved.name),
            "Output must be build/h700-graphics-probe or a new -suffix/_suffix sibling")
    require(not fresh or not resolved.exists(), "Output already exists; choose a fresh --output (nothing overwritten)")
    return resolved


def elf_info(text, executable=False):
    require(re.search(r"Class:\s+ELF64\b", text) and re.search(r"Machine:\s+AArch64\b", text),
            "Expected an ELF64 AArch64 artifact")
    interpreter = re.search(r"Requesting program interpreter:\s*([^\]]+)\]", text)
    if executable:
        require(interpreter and interpreter[1] == "/lib/ld-linux-aarch64.so.1", "Unexpected ELF interpreter")
    maxima = {}
    for family, limit in LIMITS.items():
        versions = re.findall(r"\b" + family + r"_(\d+(?:\.\d+)+)\b", text)
        key = lambda value: tuple(map(int, value.split(".")))
        maxima[family] = max(versions, key=key) if versions else None
        require(not versions or key(maxima[family]) <= key(limit), f"{family} requirement exceeds {limit}")
    require("GLIBC_PRIVATE" not in text, "Artifact requires private glibc symbols")
    definitions, needs = environment.version_nodes(text)
    return {"interpreter": interpreter[1] if interpreter else None,
            "needed": re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", text), "max_versions": maxima,
            "defined_version_nodes": definitions, "required_version_nodes": needs,
            "critical_imports": {name: tokens for name, tokens in environment.symbols(text, undefined=True).items()
                                 if name in environment.CRITICAL}}


def firmware_gate(info, snapshot, bundled):
    require(snapshot.get("schema") == 1 and snapshot.get("architecture") == "aarch64", "Unexpected firmware ABI snapshot")

    def provider(name):
        if name in bundled:
            return bundled[name]
        data = snapshot["libraries"].get(name, {})
        require(data.get("status") == "found" and data.get("machine") == "AArch64",
                f"NEEDED/version provider absent from firmware snapshot: {name}")
        return data

    for name in info["needed"]:
        provider(name)
    for name, nodes in info["required_version_nodes"].items():
        missing = set(nodes) - set(provider(name)["defined_version_nodes"])
        require(not missing, f"Firmware/bundled {name} lacks version nodes: {sorted(missing)}")
    for symbol, tokens in info["critical_imports"].items():
        owner, _ = environment.CRITICAL[symbol]
        exported = provider(owner).get("selected_exports", {}).get(symbol, "")
        for token in tokens:
            require(exported and (("@" in token and token == exported.replace("@@", "@")) or
                                  ("@" not in token and exported.startswith(symbol + "@@"))),
                    f"Firmware lacks matching selected export: {token}")
    return {"needed": info["needed"], "required_version_nodes": info["required_version_nodes"],
            "selected_imports_checked": info["critical_imports"], "limitations": ABI_LIMITATIONS}


def check_ldd(text):
    require(not re.search(r"not found|version .* not found", text, re.IGNORECASE), "Container ldd has unresolved dependencies")


def run(command, timeout=60, env=None, separate_stderr=False):
    result = subprocess.run([str(x) for x in command], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE if separate_stderr else subprocess.STDOUT, timeout=timeout, env=env)
    error_output = result.stdout + ((result.stderr or "") if separate_stderr else "")
    require(result.returncode == 0, f"{Path(str(command[0])).name} failed ({result.returncode}):\n{error_output}")
    return (result.stdout, result.stderr or "") if separate_stderr else result.stdout


def source_hashes(root):
    paths = [PROBE / name for name in ("probe.cpp", "pattern.hpp", "CMakeLists.txt", "build.py",
                                      *CONTRACT_INPUTS, *ENVIRONMENT_INPUTS)]
    paths.extend((Path("tools/collect_licenses.sh"), SNAPSHOT))
    paths.extend(Path(rel) for rel in CONTRACT_REFERENCE_SHA256)
    return {path.as_posix(): sha256(root / path) for path in paths}


def publish_package(package):
    target = package.with_name("package")
    require((package / "manifest.json").is_file(), "Cannot publish a stage without its final manifest")
    require(not target.exists(), "Final package already exists; nothing overwritten")
    package.rename(target)


def smoke_tests(package, out, reports, env):
    for mode in ("--help", "--self-test", "--layout-self-test"):
        scope = "Dawn Null validation only; no SDL/EGL/GPU" if mode == "--layout-self-test" else "no SDL initialization"
        try:
            text, stderr = run([package / "h700_graphics_probe", mode], timeout=30, env=env, separate_stderr=True)
        except ValueError as error:
            reports.append(f"=== {mode} ({scope}) FAILED ===\n{error}")
            (out / "elf-report.txt").write_text("\n".join(reports))
            raise
        output = text + (f"\n=== {mode} stderr ===\n" + stderr if stderr else "")
        reports.append(f"=== {mode} ({scope}) ===\n" + output)
        (out / "elf-report.txt").write_text("\n".join(reports))
        lines = text.rstrip("\r\n").splitlines()
        if mode == "--self-test":
            require(any(line.startswith("PASS self-test ") for line in lines),
                    "CPU self-test exited without its PASS self-test success marker")
        if mode == "--layout-self-test":
            (package / "metadata/layout-self-test.txt").write_text(output)
            require(bool(lines) and lines[-1] == LAYOUT_SELF_TEST_SUCCESS,
                    "Dawn Null layout-self-test exited without its final four-case budget-16 success marker: " +
                    LAYOUT_SELF_TEST_SUCCESS)


def container_build(args):
    root, out = Path("/src"), Path("/out")
    require(Path(__file__).resolve() == root / PROBE / "build.py" and
            (out / MARKER).read_text() == "h700-graphics-probe-v1\n", "Use the host build.py entry point")
    provenance = check_inputs(root)
    sources = source_hashes(root)
    installed_verifier = Path("/usr/local/lib/h700/verify_environment.py")
    require(installed_verifier.is_file() and sha256(installed_verifier) == sha256(root / PROBE / "verify_environment.py"),
            "Image verifier differs from source; rebuild the approved image before configuring")
    toolchain = json.loads(run(["python3", "-B", root / PROBE / "verify_environment.py"]))
    image_report = Path("/opt/h700-toolchain/abi.json")
    packages = Path("/opt/h700-toolchain/packages.tsv")
    require(image_report.is_file() and packages.is_file(), "Image is missing toolchain ABI/package metadata")
    require(json.loads(image_report.read_text()) == toolchain, "Image ABI changed since environment verification")
    snapshot = json.loads((root / SNAPSHOT).read_text())
    build = out / "build"
    run(["cmake", "-S", root / PROBE, "-B", build, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang-16", "-DCMAKE_CXX_COMPILER=clang++-16",
         "-DCMAKE_C_FLAGS=-mcpu=cortex-a53", "-DCMAKE_CXX_FLAGS=-mcpu=cortex-a53",
         f"-DH700_DEPS={root / DEPS}", "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"], timeout=180)
    run(["cmake", "--build", build, "--target", "h700_graphics_probe", "--parallel", args.jobs], timeout=900)
    artifacts = json.loads((build / "artifacts.json").read_text())
    package = out / "package.partial"
    package.mkdir()  # A failed gate must never leave a seemingly ready package/.
    reports, abi = [], {}
    for key, name in (("executable", "h700_graphics_probe"), ("sdl", "libSDL3.so.0")):
        source = Path(artifacts[key]).resolve()
        require(source.is_relative_to(build) and source.is_file(), "Artifact escapes isolated build")
        target = package / name
        shutil.copy2(source, target)
        text = run(["readelf", "-W", "--dyn-syms", "--version-info", "-h", "-l", "-d", target])
        reports.append(f"=== {name} ===\n{text}")
        (out / "elf-report.txt").write_text("\n".join(reports))
        abi[name] = elf_info(text, key == "executable")
    bundled = {"libSDL3.so.0": abi["libSDL3.so.0"]}
    firmware_checks = {name: firmware_gate(info, snapshot, bundled) for name, info in abi.items()}
    metadata = package / "metadata"
    metadata.mkdir()
    shutil.copy2(packages, metadata / "packages.tsv")
    (metadata / "abi.json").write_text(json.dumps(toolchain, indent=2, sort_keys=True) + "\n")
    env = dict(os.environ, LD_LIBRARY_PATH=str(package))
    ldd = run(["ldd", package / "h700_graphics_probe"], env=env)
    check_ldd(ldd)
    reports.append("=== Container ldd ONLY; firmware and SDL2 dlopen NOT validated ===\n" + ldd)
    smoke_tests(package, out, reports, env)
    run(["bash", root / "tools/collect_licenses.sh", root / "build/muos-linux-arm64", package / "licenses"], timeout=120)
    require(source_hashes(root) == sources, "Probe sources changed during build; use a fresh output and retry")
    versions = {tool: run([tool, "--version"]).splitlines()[0]
                for tool in ("clang-16", "clang++-16", "cmake", "ninja", "ld")}
    manifest = {"image_id": args.image_id, "image_tag": IMAGE, "platform": "linux/arm64", "dependencies": provenance,
                "build_environment": toolchain, "firmware_checks": firmware_checks,
                "firmware_snapshot_sha256": sha256(root / SNAPSHOT), "abi_limitations": ABI_LIMITATIONS,
                "tools": versions, "sources_sha256": sources,
                "contract_reference_sha256": provenance["contract_reference_sha256"],
                "layout_validation_scope": LAYOUT_VALIDATION_SCOPE, "jobs": args.jobs,
                "build_type": "Release", "cpu": "cortex-a53", "abi_limits": LIMITS, "abi": abi,
                "validation": "Container compiler/link/ABI/help/CPU self-test/ldd and Dawn Null layout validation only; "
                              "no SDL/EGL/GPU initialization in smoke tests; no new firmware runtime validation",
                "files_sha256": {p.relative_to(package).as_posix(): sha256(p)
                                 for p in sorted(package.rglob("*")) if p.is_file()}}
    (package / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    publish_package(package)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-inputs", action="store_true", help="Validate cached archives/trees without Docker or writes")
    parser.add_argument("--output", default="build/h700-graphics-probe", help="NEW dedicated directory under project build/")
    parser.add_argument("--jobs", type=int, default=2, choices=range(1, 9), metavar="1..8")
    parser.add_argument("--container", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--image-id", default="", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    try:
        if args.container:
            container_build(args)
            return 0
        root = Path(__file__).resolve().parents[2]
        out = output_path(root, args.output, fresh=not args.check_inputs)
        provenance = check_inputs(root)
        if args.check_inputs:
            print(json.dumps(provenance, indent=2, sort_keys=True))
            return 0
        require(shutil.which("docker"), "Docker CLI missing; install Docker Desktop with Linux ARM64 support, then retry")
        image = run(["docker", "image", "inspect", "--format", "{{.Id}} {{.Os}} {{.Architecture}}", IMAGE], timeout=30).split()
        require(len(image) == 3 and re.fullmatch(r"sha256:[0-9a-f]{64}", image[0]) and
                image[1:] == ["linux", "arm64"], "Local image must be Linux ARM64; no automatic pull/build")
        out.parent.mkdir(exist_ok=True)
        out.mkdir()
        (out / MARKER).write_text("h700-graphics-probe-v1\n")
        name = "h700-probe-" + uuid.uuid4().hex
        command = ["docker", "run", "--rm", "--pull=never", "--name", name, "--platform", "linux/arm64",
                   "--network=none", "--read-only", "--tmpfs", "/tmp:rw,nosuid,nodev", "--cap-drop=ALL",
                   "--security-opt=no-new-privileges", "--volume", f"{root}:/src:ro", "--volume", f"{out}:/out:rw",
                   "--workdir", "/out", image[0], "python3", "-B", "/src/tools/h700_graphics_probe/build.py",
                   "--container", "--image-id", image[0], "--jobs", str(args.jobs)]
        try:
            print(run(command, timeout=1500))
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            subprocess.run(["docker", "rm", "--force", name], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=30, check=False)
            raise
        print(f"Staged: {out.relative_to(root)}/package (NOT device-validated; no deploy/run performed)")
        return 0
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        print(f"Probe build refused/failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
