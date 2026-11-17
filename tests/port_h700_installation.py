"""GM14: disposable physical installs, production paths and real INI persistence.

No disc, graphics, actual game loop or personal saves. CARD/atomic/identity
regressions run separately against synthetic data in the existing test targets.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME, PROBE = map(lambda p: Path(p).resolve(), sys.argv[1:3])
del sys.argv[1:3]


def snapshot(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob("*") if p.is_file()}


def run(command, env, cwd, expected=0):
    result = subprocess.run(list(map(str, command)), env=env, cwd=cwd,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=20)
    if result.returncode != expected:
        raise AssertionError(f"fixture command returned {result.returncode}, expected {expected}:\n{result.stdout}")
    return result.stdout


class H700Installation(unittest.TestCase):
    def test_physical_install_owns_paths_and_persists_across_processes(self):
        with tempfile.TemporaryDirectory(prefix="h700-install-") as temp:
            root = Path(temp)
            old, new = root / "old install", root / "new install"
            home = root / "old home"
            user, cache = new / "user", new / "cache"
            for folder in (old, user, cache, home):
                folder.mkdir(parents=True)
            # Inputs which production would migrate/select if it used the old
            # executable, working directory or legacy per-user folder by mistake.
            for relative in ("port_settings.ini", "MemoryCardA.USA.raw", "mods/old.mod",
                             "pipeline_cache.db", "user/USA/Card A/old.gci"):
                target = old / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b"old-install-sentinel\n")
            legacy = home / ".local/share/Metroid Prime/port_settings.ini"
            legacy.parent.mkdir(parents=True)
            legacy.write_bytes(b"old-home-sentinel\n")
            binary = new / "path probe"
            shutil.copy2(PROBE, binary)  # physical copy, never symlink/hardlink
            self.assertFalse(binary.is_symlink())
            self.assertNotEqual(binary.stat().st_ino, PROBE.stat().st_ino)
            before_old, before_home = snapshot(old), snapshot(home)
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(("MP_", "AURORA_", "LD_"))
                   and k not in ("APPIMAGE", "XDG_DATA_HOME")}
            env.update(HOME=str(home), MP_USER_PATH=str(user), MP_CACHE_PATH=str(cache))

            def probe():
                values = dict(line.split("=", 1) for line in run([binary], env, old).splitlines())
                self.assertEqual(Path(values["user"]), user)
                self.assertEqual(Path(values["card"]), user)
                self.assertEqual(values["portable"], "1")
                self.assertEqual(values["migrations"], "0")

            probe()
            command = [GAME, "--h700-settings-persistence-selftest"]
            run([*command, "write", user], env, old)
            ini = user / "port_settings.ini"
            saved = ini.read_bytes()
            self.assertIn(b"\nshow_fps_counter=0\n", saved)
            self.assertFalse(ini.with_suffix(".ini.tmp").exists())
            run([*command, "read", user], env, old)  # fresh actual game process
            self.assertEqual(ini.read_bytes(), saved)  # read cannot rewrite INI
            run([*command, "write", user], env, old, expected=2)  # no overwrite
            self.assertEqual(ini.read_bytes(), saved)
            mismatched = dict(env, MP_USER_PATH=str(old))
            run([*command, "read", user], mismatched, old, expected=2)
            run([GAME, "--settings-selftest"], env, old)
            self.assertEqual(ini.read_bytes(), saved)
            probe()  # cached statics are resolved again in a new process
            self.assertEqual(snapshot(old), before_old)
            self.assertEqual(snapshot(home), before_home)
            self.assertEqual(list(cache.iterdir()), [])  # no GPU/cache initialization
            self.assertEqual({p.name for p in user.iterdir()}, {"port_settings.ini"})

    def test_actual_startup_passes_explicit_cache_and_card_paths(self):
        # Cache selection remains local in main, not a public path API. Bind the
        # chosen fixture/launcher env to that actual config without inventing a
        # second resolver or starting a renderer solely to observe a path.
        source = (ROOT / "platform/main.cpp").read_text()
        self.assertIn('port::EnvString("MP_CACHE_PATH")', source)
        self.assertIn("cacheEnv != nullptr && cacheEnv[0] != '\\0' ? cacheEnv : defaultCache", source)
        self.assertIn(".cachePath = cacheFolder.empty() ? nullptr : cacheFolder.c_str()", source)
        card = (ROOT / "src/Kyoto/DolphinCMemoryCardSys.cpp").read_text()
        self.assertIn("const std::string cardFolder = PortPaths::CardFolder();", card)
        self.assertIn("base = cardFolder.empty() ? nullptr : cardFolder.c_str();", card)


if __name__ == "__main__":
    unittest.main()
