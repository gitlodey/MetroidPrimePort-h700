#!/usr/bin/env python3
"""Configure/build the real game offline using the existing H700 build helper.

No device access or GPU execution. The only executable check is no-init --version.
The integration manifest remains the single source of the 14 pinned inputs.
"""
import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "h700_bootstrap", ROOT / "tools/h700_integration_driver/build.py")
bootstrap = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = bootstrap
SPEC.loader.exec_module(bootstrap)

GAME = bootstrap.BuildProfile(
    source="/src", cache="/src/cmake/h700/GameInitialCache.cmake",
    script="/src/tools/h700_game/build.py", target="metroid_prime_port",
    marker=".h700-game", prefix="h700-game", check_argument="--version",
    package_script="/src/tools/h700_game/package.py")


def main(argv=None):
    return bootstrap.main(argv, profile=GAME)


if __name__ == "__main__":
    sys.exit(main())
