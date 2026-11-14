#!/usr/bin/env python3
"""Read system ELF metadata over verified SSH, without remote files or graphics.

Usage: python3 -B tools/h700_runtime_inventory.py --host USER@HOST --output NEW.json
The destination is deliberately not stored in the report. Output is exclusive.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

# Fixed remote program: no target-supplied shell interpolation or arbitrary paths.
REMOTE = r'''
import datetime
import glob
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess

if not shutil.which("readelf"):
    raise SystemExit("Required device tool readelf is missing; no fallback installed")

required = ["ld-linux-aarch64.so.1", "libc.so.6", "libm.so.6", "libstdc++.so.6",
            "libgcc_s.so.1", "libdl.so.2", "libpthread.so.0", "librt.so.1",
            "libSDL2-2.0.so.0", "libEGL.so.1", "libGLESv2.so.2"]
# These are candidates for SDL/audio and the full port, not mandatory probe deps.
optional = ["libasound.so.2", "libpulse.so.0", "libsndio.so.7", "libsndio.so.7.0",
            "libssl.so.3", "libcrypto.so.3", "libsqlite3.so.0", "libzstd.so.1",
            "libfreetype.so.6", "libpng16.so.16", "libz.so.1"]
interesting = {"__isoc23_strtol", "__isoc23_strtoul", "__isoc23_strtoll",
               "__isoc23_strtoull",
               "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE15_M_replace_coldEPcmPKcmm"}
directories = [Path(p) for p in ["/lib", "/usr/lib", "/usr/local/lib"] +
               sorted(glob.glob("/lib/*-linux-gnu") + glob.glob("/usr/lib/*-linux-gnu"))]

def run(args):
    p = subprocess.run(args, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=15)
    if p.returncode:
        raise RuntimeError("metadata command failed: " + args[0])
    return p.stdout

def version_names(text):
    return sorted(set(re.findall(r"\bName:\s+(\S+)", text)))

def maximum(names, prefix):
    values = [s for s in names if re.fullmatch(prefix + r"_\d+(?:\.\d+)+", s)]
    return max(values, key=lambda s: tuple(map(int, s.split("_", 1)[1].split(".")))) if values else None

def resolve(name):
    for directory in directories:
        p = directory / name
        if p.is_file():
            return p.resolve()
    return None

cache = {}

def inspect(path):
    key = str(path)
    if key in cache:
        return cache[key]
    dynamic = run(["readelf", "-d", str(path)])
    header = run(["readelf", "-h", str(path)])
    versions = run(["readelf", "--version-info", str(path)])
    definition = re.search(r"Version definition section.*?(?=Version needs section|\Z)", versions, re.S)
    needs = re.search(r"Version needs section.*", versions, re.S)
    definitions = version_names(definition[0]) if definition else []
    requirements = version_names(needs[0]) if needs else []
    symbol_matches = {}
    if path.name.startswith(("libc-", "libc.so", "libstdc++")):
        symbols = run(["readelf", "--dyn-syms", "--wide", str(path)])
        for line in symbols.splitlines():
            fields = line.split()
            if len(fields) >= 8 and fields[6] != "UND" and fields[4] in ("GLOBAL", "WEAK"):
                name = fields[7].split("@", 1)[0]
                if name in interesting:
                    symbol_matches[name] = fields[7]
    soname = re.search(r"\(SONAME\).*?\[([^\]]+)\]", dynamic)
    with path.open("rb") as source:
        sha = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            sha.update(chunk)
    data = {
        "resolved_path": key,
        "soname": soname[1] if soname else None,
        "machine": re.search(r"Machine:\s+([^\n]+)", header)[1].strip(),
        "sha256": sha.hexdigest(),
        "needed": re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", dynamic),
        "defined_version_nodes": definitions,
        "required_version_nodes": requirements,
        "max_defined": {p: maximum(definitions, p) for p in ("GLIBC", "GLIBCXX", "CXXABI")},
        "max_required": {p: maximum(requirements, p) for p in ("GLIBC", "GLIBCXX", "CXXABI")},
        "selected_exports": symbol_matches,
    }
    cache[key] = data
    return data

os_release = {}
for line in Path("/etc/os-release").read_text().splitlines():
    if "=" in line:
        key, value = line.split("=", 1)
        if key in ("ID", "VERSION_ID", "PRETTY_NAME", "BUILD_ID"):
            os_release[key] = value.strip('"')

libraries = {}
queue = required + optional
while queue:
    name = queue.pop(0)
    if name in libraries:
        continue
    path = resolve(name)
    if path is None:
        libraries[name] = {"status": "not_found_in_searched_system_directories"}
        continue
    data = inspect(path)
    libraries[name] = {"status": "found", **data}
    queue.extend(n for n in data["needed"] if n not in libraries)

exports = {}
for name, data in libraries.items():
    for symbol, versioned in data.get("selected_exports", {}).items():
        exports[symbol] = {"library": name, "export": versioned}

report = {
    "schema": 1,
    "captured_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "method": "SSH stdin Python; readelf and file reads only; no remote files, dlopen, ldd or graphics",
    "os": os_release,
    "kernel": platform.release(),
    "architecture": platform.machine(),
    "glibc_getconf": run(["getconf", "GNU_LIBC_VERSION"]).strip(),
    "searched_system_directories": [str(p) for p in directories],
    "probe_runtime_candidates": required,
    "optional_full_port_audio_candidates": optional,
    "selected_dawn_symbols": {s: exports.get(s) for s in sorted(interesting)},
    "libraries": libraries,
    "limitations": [
        "ABI version nodes and resolved filenames are not package semantic versions",
        "No final executable exists yet: its complete imported symbol set is not validated",
        "dlopen-loaded plugins/drivers are not an exhaustive NEEDED closure",
        "No EGL/GL context, audio/input/game test or firmware modification performed",
        "Not-found candidates are not automatically mandatory dependencies",
    ],
}
print(json.dumps(report, sort_keys=True, indent=2))
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Current explicitly authorized SSH destination")
    parser.add_argument("--output", required=True, type=Path, help="New local sanitized JSON report")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.:@-]*", args.host) or args.host.count("@") > 1:
        parser.error("Invalid SSH destination")
    if not shutil.which("ssh"):
        parser.error("Required ssh tool missing; install it before retrying")
    if args.output.exists() or args.output.is_symlink():
        parser.error("Output already exists; refusing overwrite")
    result = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
         "-o", "ConnectTimeout=8", "--", args.host, "python3 -B -"],
        input=REMOTE, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180)
    if result.returncode:
        print("Read-only SSH inventory failed:\n" + result.stderr, file=sys.stderr)
        return 1
    report = json.loads(result.stdout)
    if report.get("architecture") != "aarch64":
        raise ValueError("Unexpected device architecture; report not saved")
    with args.output.open("x") as output:
        json.dump(report, output, indent=2, sort_keys=True)
        output.write("\n")
    print("Saved sanitized runtime inventory:", args.output)
    print("Target:", report["os"].get("PRETTY_NAME"), report["glibc_getconf"])
    for family in ("libc.so.6", "libstdc++.so.6"):
        data = report["libraries"].get(family, {})
        print(family, data.get("resolved_path"), data.get("max_defined"))
    for name, value in report["selected_dawn_symbols"].items():
        print(name, value["export"] if value else "NOT EXPORTED")
    missing = [n for n, d in report["libraries"].items() if d["status"] != "found"]
    print("Unresolved candidates/NEEDED in searched directories:", missing)
    return 0


if __name__ == "__main__":
    sys.exit(main())
