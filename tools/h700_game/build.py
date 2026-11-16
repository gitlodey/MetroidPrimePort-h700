#!/usr/bin/env python3
"""GM-01: configure the real game offline using the existing H700 build helper.

No device access. Requires --configure-only until the separate GM-02 build step.
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
    marker=".h700-game", prefix="h700-game", help_check=False)


def main(argv=None):
    return bootstrap.main(argv, profile=GAME, configure_only_required=True)


if __name__ == "__main__":
    sys.exit(main())
