"""Reject incompatible H700 overrides before SDL, game owners or config writes."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile


def main():
    executable = Path(sys.argv[1]).resolve()
    cases = [
        ("MP_FRAME_BUFFERS", "1"), ("MP_FRAME_BUFFERS", ""),
        ("MP_STORAGE_BUFFERS", "1"), ("AURORA_FULL_DEVICE_BUFFERS", "0"),
        ("MP_DAWN_ENABLE", "skip_validation"), ("MP_DAWN_DISABLE", "gl_allow_context_on_multi_threads"),
        ("MP_GPU_SELFTEST", "1"), ("MP_GRAPHICS_BACKEND", "Null"),
        ("SDL_VIDEODRIVER", "dummy"), ("SDL3SHIM_SDL2_VIDEODRIVER", "offscreen"),
        ("MP_PIPELINE_LAZY", "0"), ("MP_PIPELINE_SEED", "1"),
    ]
    with tempfile.TemporaryDirectory(prefix="h700-profile-") as temp:
        root = Path(temp)
        user = root / "user"
        user.mkdir()
        settings = user / "port_settings.ini"
        original = b"# Existing choices must not be loaded, rewritten or removed by a rejected launch.\naspect=16:9\n"
        settings.write_bytes(original)
        env = {"PATH": os.defpath, "HOME": str(root), "MP_USER_PATH": str(user)}
        # Preserve only the loader search path if the build uses one. This is a
        # no-init rejection test, never a successful game/graphics invocation.
        if "LD_LIBRARY_PATH" in os.environ:
            env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]
        for name, value in cases:
            result = subprocess.run([str(executable)], env={**env, name: value}, cwd=root,
                                    stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5)
            expected = f"H700 profile: conflicting runtime override {name}; no graphics initialized"
            assert result.returncode == 2, (name, result.returncode, result.stdout, result.stderr)
            assert result.stderr.strip() == expected, (name, result.stderr)
            assert not result.stdout, (name, result.stdout)
            assert settings.read_bytes() == original, name
            assert sorted(p.relative_to(root).as_posix() for p in root.rglob("*")) == [
                "user", "user/port_settings.ini"], name
    print(f"H700 early override rejection: {len(cases)} cases, existing settings preserved; no SDL/GPU init")


if __name__ == "__main__":
    main()
