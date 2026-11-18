"""Disposable CPU launcher fixtures; no game, GPU, firmware or device access."""
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'packaging/h700/run-game.sh'
TEMPLATE = ROOT / 'packaging/h700/Metroid Prime Unbound.sh.in'
FRONTEND = ('SDL_ROTATION', 'SDL_HQ_SCALER', 'SDL_BLITTER_DISABLED',
            'SDL_ASSERT', 'SDL_GAMECONTROLLERCONFIG_FILE')
AUDIO = ('SDL_AUDIODRIVER', 'SDL_AUDIO_DRIVER', 'ALSA_CONFIG_PATH',
         'ALSA_CONFIG_DIR', 'ALSA_CARD', 'ALSA_PCM_CARD', 'PULSE_SERVER',
         'PULSE_RUNTIME_PATH', 'PIPEWIRE_REMOTE', 'PIPEWIRE_RUNTIME_DIR',
         'XDG_RUNTIME_DIR')
# Bind the test matrix to the actual startup policy, not a second handwritten list.
POLICY = (ROOT / 'platform/include/port_h700_profile.h').read_text()
GROUPS = re.findall(r'for \(const char\* name : \{(.*?)\}\)', POLICY, re.S)
PRESENCE, NONEMPTY = [re.findall(r'"([A-Z0-9_]+)"', group) for group in GROUPS[:2]]
EXPECTED = dict(re.findall(r'(?:std::pair)?\{"([A-Z0-9_]+)", "([^"]+)"\}', POLICY))

MOCK = r'''
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

user = Path(__file__).resolve().parent / 'user'
def wait_for_file(path):
    deadline = time.monotonic() + 8
    while not path.exists():
        if time.monotonic() >= deadline:
            sys.exit(99)
        time.sleep(0.01)

if sys.argv[1:] == ['--copier']:
    (user / 'copier-ready').write_text('ready')
    wait_for_file(user / 'copier-release')
    (user / 'copier-done').write_text('done')
    sys.exit(0)

for sig in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
    signal.signal(sig, lambda number, frame: (user / 'child-signal').write_text(str(number)))
count = user / 'launch-count'
count.write_text(str(int(count.read_text()) + 1 if count.exists() else 1))
(user / 'observed.json').write_text(json.dumps({
    'argv': sys.argv[1:], 'env': dict(os.environ), 'cwd': os.getcwd(), 'pid': os.getpid(),
}))
print('mock stdout must not become a frontend log', flush=True)
print('mock stderr must not become a frontend log', file=sys.stderr, flush=True)
if (user / 'hold').exists():
    (user / 'ready').write_text(str(os.getpid()))
    wait_for_file(user / 'release')
if (user / 'start-copier').exists():
    subprocess.Popen([sys.executable, __file__, '--copier'])
    wait_for_file(user / 'copier-ready')
exit_code = user / 'exit-code'
sys.exit(int(exit_code.read_text()) if exit_code.exists() else 7)
'''


def shell_literal(value):
    return "'" + str(value).replace("'", "'\"'\"'") + "'"


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='h700-launcher-', dir='/tmp')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.ports = self.root / 'Ports space ; $literal'
        self.data = self.ports / '.metroid-prime-unbound-h700'
        self.data.mkdir(parents=True)
        (self.data / 'lib').mkdir()
        self.script = self.data / 'run-game.sh'
        shutil.copyfile(SOURCE, self.script)
        self.script.chmod(0o755)
        self.entry = self.ports / 'Metroid Prime Unbound.sh'
        shutil.copyfile(TEMPLATE, self.entry)
        self.user = self.data / 'user'
        self.cache = self.data / 'cache'
        self.lock = self.data / 'run.lock'
        self.game = self.data / 'metroid_prime_port'
        self.game.write_text('#!' + sys.executable + '\n' + MOCK)
        self.game.chmod(0o755)
        self.disc = self.root / "disc ' ; $(touch INJECTED) & [x].iso"
        self.disc.write_text('not a game; CPU fixture only')
        self.old = self.root / 'old-install'
        self.old.mkdir()
        for name in ('port_settings.ini', 'card.raw', 'cache.bin'):
            (self.old / name).write_text('old sentinel: ' + name)
        self.old_before = {p.name: p.read_bytes() for p in self.old.iterdir()}
        # Do not inherit developer renderer/audio overrides into the test baseline.
        self.env = {'PATH': '/usr/bin:/bin:/usr/sbin:/sbin', 'LC_ALL': 'C',
                    'HOME': str(self.old), 'MP_USER_PATH': str(self.old),
                    'MP_CACHE_PATH': str(self.old), 'MP_DISC': str(self.old / 'old.iso')}
        self.socket_path = self.root / 'pipewire-0'
        # Source binding only: no public test knob and no writes to host /run.
        source = self.script.read_text()
        self.assertEqual(source.count('[ -S /run/pipewire-0 ]'), 1)
        self.script.write_text(source.replace('[ -S /run/pipewire-0 ]',
                                             '[ -S ' + shell_literal(self.socket_path) + ' ]'))

    def launch(self, *args, env=None, script=None):
        return subprocess.run(['sh', str(script or self.script), *args],
                              env=self.env if env is None else env, cwd=self.old,
                              capture_output=True, text=True, timeout=12)

    def run_game(self, env=None, status=7):
        result = self.launch(str(self.disc), env=env)
        self.assertEqual(result.returncode, status, result.stderr)
        self.assertEqual(result.stdout, '')
        self.assertEqual(result.stderr, '')
        self.assertFalse(self.lock.exists())
        return json.loads((self.user / 'observed.json').read_text())

    def assert_refused(self, result, message=None):
        self.assertEqual(result.returncode, 2, result.stderr)
        if message:
            self.assertIn(message, result.stderr)
        self.assertFalse((self.user / 'observed.json').exists())
        self.assertFalse(self.lock.exists())

    def wait_file(self, path):
        deadline = time.monotonic() + 5
        while not path.exists():
            if time.monotonic() >= deadline:
                self.fail('bounded mock did not create ' + path.name)
            time.sleep(0.01)

    def test_disc_requires_exactly_one_absolute_readable_regular_file(self):
        for args in ((), ('',), ('relative.iso',), (str(self.root / 'missing.iso'),),
                     (str(self.root),), (str(self.disc), 'extra')):
            with self.subTest(args=args):
                self.assert_refused(self.launch(*args))
        fifo = self.root / 'disc-fifo'
        os.mkfifo(fifo)
        self.assert_refused(self.launch(str(fifo)))

    def test_unreadable_disc(self):
        self.disc.chmod(0)
        self.addCleanup(self.disc.chmod, 0o600)
        if os.access(self.disc, os.R_OK):
            self.skipTest('this user can read mode-000 files (e.g. root)')
        self.assert_refused(self.launch(str(self.disc)), 'readable regular')

    def test_executable_must_be_real_regular_and_executable(self):
        self.game.unlink()
        self.assert_refused(self.launch(str(self.disc)))
        self.game.mkdir()
        self.assert_refused(self.launch(str(self.disc)))
        self.game.rmdir()
        self.game.write_text('not executable')
        self.assert_refused(self.launch(str(self.disc)))
        self.game.unlink()
        self.game.symlink_to(sys.executable)
        self.assert_refused(self.launch(str(self.disc)), 'not a symlink')

    def test_literal_disc_physical_payload_and_persistent_own_paths(self):
        self.user.mkdir()
        (self.user / 'exit-code').write_text('0')
        alias = self.root / 'ports-alias'
        alias.symlink_to(self.ports, target_is_directory=True)
        result = self.launch(str(self.disc), script=alias / self.data.name / self.script.name)
        self.assertEqual(result.returncode, 0, result.stderr)
        observed = json.loads((self.user / 'observed.json').read_text())
        self.assertEqual(observed['argv'], [str(self.disc)])
        self.assertEqual(observed['cwd'], str(self.data.resolve()))
        env = observed['env']
        for name, path in {'HOME': self.user, 'TMPDIR': self.user / 'tmp',
                           'XDG_CONFIG_HOME': self.user, 'XDG_CACHE_HOME': self.cache,
                           'MP_USER_PATH': self.user, 'MP_CACHE_PATH': self.cache}.items():
            self.assertEqual(env[name], str(path.resolve()))
            self.assertTrue(path.is_dir())
        sentinels = [self.user / 'port_settings.ini', self.user / 'USA/Card A/save.gci',
                     self.cache / 'pipelines', self.user / 'home-state']
        for path in sentinels:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('persistent fixture: ' + path.name)
        before = {path: path.read_bytes() for path in sentinels}
        self.run_game(status=0)
        self.assertEqual((self.user / 'launch-count').read_text(), '2')
        self.assertEqual({path: path.read_bytes() for path in sentinels}, before)
        self.assertEqual({p.name: p.read_bytes() for p in self.old.iterdir()}, self.old_before)
        self.assertFalse((self.old / 'INJECTED').exists())
        self.assertEqual({p.name for p in self.data.iterdir()},
                         {'run-game.sh', 'metroid_prime_port', 'lib', 'user', 'cache'})

    def test_own_directories_cannot_link_to_old_data(self):
        for path in (self.user, self.cache, self.user / 'tmp'):
            with self.subTest(path=path.name):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.symlink_to(self.old, target_is_directory=True)
                self.assert_refused(self.launch(str(self.disc)), 'must not be symlinks')
                path.unlink()
        self.assertEqual({p.name: p.read_bytes() for p in self.old.iterdir()}, self.old_before)

    def test_profile_conflicts_rejected_before_sanitation(self):
        self.assertEqual(len(PRESENCE), 5)
        self.assertTrue(NONEMPTY)
        self.assertEqual(EXPECTED, {'SDL_VIDEODRIVER': 'sdl2',
                                   'SDL3SHIM_SDL2_VIDEODRIVER': 'mali', 'MP_OPENGLES': '1'})
        for name in PRESENCE + NONEMPTY:
            values = ('', '0') if name in PRESENCE else ('0',)
            for value in values:
                with self.subTest(name=name, value=value):
                    self.assert_refused(self.launch(str(self.disc), env={**self.env, name: value}), name)
        for name in EXPECTED:
            for value in ('', 'wrong'):
                with self.subTest(name=name, value=value):
                    self.assert_refused(self.launch(str(self.disc), env={**self.env, name: value}), name)

    def test_exact_profile_empty_nonempty_policy_and_env_allowlist(self):
        env = {**self.env, **EXPECTED, **dict.fromkeys(NONEMPTY, ''),
               'MP_ARBITRARY': 'drop', 'AURORA_ARBITRARY': 'drop',
               'LD_LIBRARY_PATH': str(self.old), 'LD_PRELOAD': '', 'LD_AUDIT': '',
               'DYLD_INSERT_LIBRARIES': '', 'SDL_RENDER_DRIVER': 'drop',
               'UNRELATED': 'drop', 'ENV': str(self.old / 'shell-injection')}
        observed = self.run_game(env)['env']
        if sys.platform == 'darwin':
            # macOS adds this at Python framework startup, after env -i.
            observed.pop('__CF_USER_TEXT_ENCODING', None)
        self.assertEqual(observed, {
            'PATH': '/usr/bin:/bin:/usr/sbin:/sbin', 'LC_ALL': 'C',
            'HOME': str(self.user.resolve()), 'TMPDIR': str((self.user / 'tmp').resolve()),
            'XDG_CONFIG_HOME': str(self.user.resolve()), 'XDG_CACHE_HOME': str(self.cache.resolve()),
            'MP_USER_PATH': str(self.user.resolve()), 'MP_CACHE_PATH': str(self.cache.resolve()),
            'MP_LOG_FILE': '1', **EXPECTED,
        })

    def test_only_explicit_render_diagnostics_are_forwarded(self):
        diagnostics = {'MP_H700_GL_TIMING': '1', 'MP_H700_BENCH': '1',
                       'MP_PASS_LOG': '60', 'AURORA_FRAME_PEAKS': '1'}
        baseline = self.run_game()['env']
        for name in diagnostics:
            self.assertNotIn(name, baseline)
        observed = self.run_game({**self.env, **diagnostics,
                                  'MP_UNLISTED_DIAGNOSTIC': 'drop'})['env']
        for name, value in diagnostics.items():
            self.assertEqual(observed[name], value)
        self.assertNotIn('MP_UNLISTED_DIAGNOSTIC', observed)

    def test_fast_boot_is_forwarded_only_for_explicit_bench_route(self):
        for mode in (None, '', '0', 'invalid'):
            env = {**self.env, 'MP_FAST_BOOT': '1'}
            if mode is not None:
                env['MP_H700_BENCH'] = mode
            with self.subTest(mode=mode):
                self.assertNotIn('MP_FAST_BOOT', self.run_game(env)['env'])
        for mode in ('1', 'menu'):
            observed = self.run_game({**self.env, 'MP_H700_BENCH': mode,
                                      'MP_FAST_BOOT': '1'})['env']
            self.assertEqual(observed['MP_FAST_BOOT'], '1')

    def test_experimental_render_flags_are_forwarded_only_for_explicit_bench_route(self):
        for name in ('MP_H700_DRAW_UBO', 'MP_H700_FETCH_GATHER', 'MP_H700_NATIVE_VERTEX',
                     'MP_H700_PROGRAM_CACHE', 'MP_H700_PRODUCER_FLUSH',
                     'MP_H700_MOVIE_CLIENT_UPLOAD', 'MP_H700_FRAME_DEST_RING'):
            for mode in (None, '', '0', 'invalid'):
                env = {**self.env, name: '1'}
                if mode is not None:
                    env['MP_H700_BENCH'] = mode
                with self.subTest(name=name, mode=mode):
                    self.assertNotIn(name, self.run_game(env)['env'])
            for mode in ('1', 'menu'):
                self.assertNotIn(name, self.run_game({**self.env, 'MP_H700_BENCH': mode})['env'])
                for value in ('1', '0', '', "1 ; $(touch INJECTED) '"):
                    with self.subTest(name=name, mode=mode, value=value):
                        observed = self.run_game({**self.env, 'MP_H700_BENCH': mode,
                                                  name: value})['env']
                        self.assertEqual(observed[name], value)
                        self.assertFalse((self.old / 'INJECTED').exists())

    def test_frontend_and_audio_keep_values_including_explicit_empty(self):
        for value in ('', "route with spaces ; $(touch INJECTED) ' = 2"):
            with self.subTest(value=value):
                env = {**self.env, **dict.fromkeys(FRONTEND + AUDIO, value)}
                observed = self.run_game(env)['env']
                for name in FRONTEND + AUDIO:
                    self.assertEqual(observed[name], value)

    def test_pipewire_default_only_for_actual_socket_and_unset_caller(self):
        self.assertNotIn('PIPEWIRE_RUNTIME_DIR', self.run_game()['env'])
        self.socket_path.write_text('regular file is not a socket')
        self.assertNotIn('PIPEWIRE_RUNTIME_DIR', self.run_game()['env'])
        self.socket_path.unlink()
        with socket.socket(socket.AF_UNIX) as server:
            server.bind(str(self.socket_path))
            self.assertEqual(self.run_game()['env']['PIPEWIRE_RUNTIME_DIR'], '/run')
            for value in ('', '/explicit route'):
                observed = self.run_game({**self.env, 'PIPEWIRE_RUNTIME_DIR': value})
                self.assertEqual(observed['env']['PIPEWIRE_RUNTIME_DIR'], value)

    def test_wrapper_unconfigured_and_literal_configured_disc(self):
        self.assert_refused(self.launch(script=self.entry), 'disc path must be absolute')
        self.assert_refused(self.launch('unexpected', script=self.entry), 'no runtime options')
        template = self.entry.read_text()
        self.assertEqual(template.count("DISC=''"), 1)
        self.entry.write_text(template.replace("DISC=''", 'DISC=' + shell_literal(self.disc)))
        result = self.launch(script=self.entry)
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(json.loads((self.user / 'observed.json').read_text())['argv'], [str(self.disc)])

    def test_existing_lock_is_not_removed(self):
        self.lock.mkdir()
        result = self.launch(str(self.disc))
        self.assertEqual(result.returncode, 2)
        self.assertIn('already running or lock retained', result.stderr)
        self.assertTrue(self.lock.is_dir())
        self.assertFalse((self.user / 'observed.json').exists())

    def test_shell_signals_defer_unlock_until_actual_child_exit_and_keep_exit7(self):
        for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
            with self.subTest(signal=sig):
                self.user.mkdir(exist_ok=True)
                for name in ('release', 'ready', 'observed.json', 'child-signal'):
                    (self.user / name).unlink(missing_ok=True)
                (self.user / 'hold').touch()
                with subprocess.Popen(['sh', str(self.script), str(self.disc)], env=self.env,
                                      cwd=self.old, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      text=True) as parent:
                    try:
                        self.wait_file(self.user / 'ready')
                        child_pid = int((self.user / 'ready').read_text())
                        os.kill(parent.pid, sig)
                        time.sleep(0.1)
                        self.assertIsNone(parent.poll())
                        os.kill(child_pid, 0)  # Liveness observation only; never signal the game.
                        self.assertTrue(self.lock.is_dir())
                        result = self.launch(str(self.disc))
                        self.assertEqual(result.returncode, 2)
                        self.assertTrue(self.lock.is_dir())
                        self.assertFalse((self.user / 'child-signal').exists())
                    finally:
                        (self.user / 'release').touch()
                        stdout, stderr = parent.communicate(timeout=12)
                    self.assertEqual(parent.returncode, 7, stderr)
                    self.assertEqual((stdout, stderr), ('', ''))
                self.assertFalse(self.lock.exists())
                self.assertFalse((self.user / 'child-signal').exists())

    def test_completed_game_does_not_hold_lock_for_copier_descendant(self):
        self.user.mkdir()
        (self.user / 'start-copier').touch()
        try:
            self.run_game()
            self.assertTrue((self.user / 'copier-ready').exists())
            self.assertFalse((self.user / 'copier-done').exists())
            self.assertFalse(self.lock.exists())
        finally:
            (self.user / 'copier-release').touch()
            self.wait_file(self.user / 'copier-done')


if __name__ == '__main__':
    unittest.main()
