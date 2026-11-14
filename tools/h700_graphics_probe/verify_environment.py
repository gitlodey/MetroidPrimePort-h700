#!/usr/bin/env python3
"""Inspect the ABI-2.38 build runtime; no installs, patches or graphics calls."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess
import sys

INTERPRETER = "/lib/ld-linux-aarch64.so.1"
COLD_STRING = "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE15_M_replace_coldEPcmPKcmm"
CRITICAL = {name: ("libc.so.6", "GLIBC_2.38") for name in
            ("__isoc23_strtol", "__isoc23_strtoul", "__isoc23_strtoll", "__isoc23_strtoull")}
CRITICAL[COLD_STRING] = ("libstdc++.so.6", "GLIBCXX_3.4.31")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def version_nodes(text):
    """Keep VERDEF separate from VERNEED, preserving the latter's provider names."""
    definitions, needs, section, library = set(), {}, None, None
    for line in text.splitlines():
        if line.startswith("Version "):
            section = "definitions" if line.startswith("Version definition section") else (
                "needs" if line.startswith("Version needs section") else None)
        if section == "needs":
            provider = re.search(r"\bFile:\s+(\S+)", line)
            if provider:
                library = provider[1]
                needs.setdefault(library, set())
        names = re.findall(r"\bName:\s+(\S+)", line)
        if section == "definitions":
            definitions.update(names)
        elif section == "needs" and names:
            require(library is not None, "VERNEED entry without provider")
            needs[library].update(names)
    return sorted(definitions), {name: sorted(nodes) for name, nodes in needs.items()}


def symbols(text, undefined=False):
    result = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 8 or not re.fullmatch(r"\d+:", fields[0]):
            continue
        if (fields[6] == "UND") != undefined or fields[4] not in ("GLOBAL", "WEAK"):
            continue
        if not undefined and fields[5] not in ("DEFAULT", "PROTECTED"):
            continue
        token = fields[7]
        result.setdefault(token.split("@", 1)[0], set()).add(token)
    return {name: sorted(tokens) for name, tokens in result.items()}


def maximum(nodes, prefix):
    values = [n for n in nodes if re.fullmatch(prefix + r"_\d+(?:\.\d+)+", n)]
    return max(values, key=lambda n: tuple(map(int, n.split("_", 1)[1].split(".")))) if values else None


def validate_reports(machine, glibc, reports, interpreter_exists):
    require(machine == "aarch64", "Build environment must be aarch64")
    require(glibc == "glibc 2.38", "Build environment must provide glibc 2.38")
    require(interpreter_exists, "AArch64 ELF interpreter is missing")
    libraries = {}
    for name in ("libc.so.6", "libstdc++.so.6"):
        text = reports[name]
        require(re.search(r"Class:\s+ELF64\b", text) and re.search(r"Machine:\s+AArch64\b", text),
                f"Wrong runtime ELF architecture: {name}")
        definitions, _ = version_nodes(text)
        exported = symbols(text)
        selected = {}
        for symbol, (owner, version) in CRITICAL.items():
            if owner == name:
                expected = symbol + "@@" + version
                require(expected in exported.get(symbol, []), f"Missing exact export: {expected}")
                selected[symbol] = expected
        libraries[name] = {"defined_version_nodes": definitions, "selected_exports": selected}
    nodes = libraries["libstdc++.so.6"]["defined_version_nodes"]
    for prefix, expected in (("GLIBCXX", "GLIBCXX_3.4.32"), ("CXXABI", "CXXABI_1.3.14")):
        require(maximum(nodes, prefix) == expected, f"Expected exact maximum {expected}")
    return {"schema": 1, "architecture": machine, "glibc_getconf": glibc,
            "interpreter": INTERPRETER, "libraries": libraries,
            "limitations": ["Build runtime inspection only; no firmware or graphics validation"]}


def run(command):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    require(result.returncode == 0, f"Environment check command failed: {command[0]}: {result.stderr.strip()}")
    return result.stdout.strip()


def inspect_environment():
    paths = {"libc.so.6": Path("/lib/aarch64-linux-gnu/libc.so.6"),
             "libstdc++.so.6": Path("/usr/lib/aarch64-linux-gnu/libstdc++.so.6")}
    reports = {name: run(["readelf", "-W", "-h", "--dyn-syms", "--version-info", str(path)])
               for name, path in paths.items()}
    report = validate_reports(platform.machine(), run(["getconf", "GNU_LIBC_VERSION"]), reports,
                              Path(INTERPRETER).is_file())
    for name, path in paths.items():
        with path.open("rb") as stream:
            sha = hashlib.sha256()
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                sha.update(chunk)
        report["libraries"][name].update({"resolved_path": str(path.resolve()), "sha256": sha.hexdigest()})
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="Write a NEW JSON file instead of stdout; parent must exist")
    args = parser.parse_args(argv)
    try:
        report = json.dumps(inspect_environment(), indent=2, sort_keys=True) + "\n"
        if args.output:
            with args.output.open("x") as output:
                output.write(report)
        else:
            print(report, end="")
        return 0
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        print(f"Environment verification failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
