"""CPU shell fixtures only; never invoke Aurora or the device display."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class MuosForegroundLauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        source = Path(__file__).resolve().parent
        self.script = self.root / 'H700 Integration Test.sh'
        shutil.copyfile(source / 'muos-launch.sh', self.script)
        self.data = self.root / '.h700-integration-test'
        self.package = self.data / 'H700Integration'
        self.package.mkdir(parents=True)
        (self.data / 'runs').mkdir()
        shutil.copyfile(source / 'run.sh', self.package / 'run.sh')
        driver = self.package / 'h700_integration_driver'
        driver.write_text('#!/bin/sh\n'
            'printf "rotation=%s scaler=%s blitter=%s\\n" "${SDL_ROTATION-unset}" "${SDL_HQ_SCALER-unset}" "${SDL_BLITTER_DISABLED-unset}"\n'
            'printf "renderer=%s ld=%s preload=%s\\n" "${MP_STORAGE_CLAMP-unset}" "${LD_LIBRARY_PATH-unset}" "${LD_PRELOAD-unset}"\n'
            'printf "args=%s %s %s\\n" "$1" "$2" "$3"\n'
            'exit 17\n')
        driver.chmod(0o755)
        import hashlib
        checksum = hashlib.sha256(driver.read_bytes()).hexdigest()
        (self.package / 'SHA256SUMS').write_text(checksum + '  h700_integration_driver\n')
        marker = self.root / 'firmware-fixture'
        marker.write_text('CPU fixture, NOT a device library\n')
        checksum = hashlib.sha256(marker.read_bytes()).hexdigest()
        (self.data / 'firmware.sha256').write_text(checksum + '  ' + str(marker) + '\n')
        self.env = dict(os.environ)
        self.env.pop('LD_PRELOAD', None)
        self.env.pop('LD_LIBRARY_PATH', None)

    def run_script(self, *args, env=None):
        return subprocess.run(['sh', str(self.script), *args], env=env or self.env,
                              capture_output=True, text=True, timeout=8)

    def latest(self):
        return Path((self.data / 'latest-run.txt').read_text().strip())

    def test_help_does_not_run_or_create_session(self):
        result = self.run_script('--help')
        self.assertEqual(result.returncode, 0)
        self.assertIn('muOS Ports', result.stdout)
        self.assertEqual(list((self.data / 'runs').iterdir()), [])
        self.assertFalse((self.data / 'run.lock').exists())

    def test_passes_display_allowlist_but_not_debug_loader_overrides(self):
        self.env.update(SDL_ROTATION='270', SDL_HQ_SCALER='2', SDL_BLITTER_DISABLED='1',
                        SDL_ASSERT='always_ignore', MP_STORAGE_CLAMP='1', LD_LIBRARY_PATH='/fixture/no-provider')
        result = self.run_script()
        self.assertEqual(result.returncode, 17)
        run = self.latest()
        log = (run / 'driver.log').read_text()
        self.assertIn('rotation=270 scaler=2 blitter=1', log)
        self.assertIn('renderer=unset ld=unset preload=unset', log)
        self.assertIn('--run --test-dir', log)
        self.assertIn(str(run / 'results'), log)
        self.assertEqual((run / 'driver.exit').read_text(), '17\n')
        self.assertIn('firmware=unchanged', (run / 'result.txt').read_text())
        self.assertIn('visual_acceptance=requires_user', (run / 'result.txt').read_text())
        self.assertFalse((self.data / 'run.lock').exists())

    def test_each_run_has_fresh_results_and_does_not_remove_prior_logs(self):
        self.assertEqual(self.run_script().returncode, 17)
        first = self.latest()
        before = (first / 'driver.log').read_bytes()
        self.assertEqual(self.run_script().returncode, 17)
        second = self.latest()
        self.assertNotEqual(first, second)
        self.assertEqual((first / 'driver.log').read_bytes(), before)
        self.assertIn('driver_exit=17', (second / 'result.txt').read_text())

    def test_lock_refuses_concurrent_run_without_removing_owners_lock(self):
        (self.data / 'run.lock').mkdir()
        self.assertEqual(self.run_script().returncode, 2)
        self.assertTrue((self.data / 'run.lock').is_dir())
        self.assertFalse((self.data / 'latest-run.txt').exists())

    def test_bad_firmware_hash_prevents_driver_launch_and_releases_lock(self):
        (self.root / 'firmware-fixture').write_text('modified fixture')
        self.assertNotEqual(self.run_script().returncode, 0)
        self.assertFalse((self.latest() / 'driver.log').exists())
        self.assertFalse((self.data / 'run.lock').exists())

    def test_package_tamper_prevents_driver_launch(self):
        (self.package / 'h700_integration_driver').write_text('tampered fixture')
        self.assertNotEqual(self.run_script().returncode, 0)
        self.assertFalse((self.latest() / 'driver.log').exists())
        self.assertFalse((self.data / 'run.lock').exists())

    def test_no_runtime_argument_can_change_scenario(self):
        self.assertEqual(self.run_script('--run', '/other/path').returncode, 2)
        self.assertFalse((self.data / 'latest-run.txt').exists())


if __name__ == '__main__':
    unittest.main()
