"""GPU-free IG13 package/security/provenance/watchdog regressions."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

import build
import package


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stage = self.root / 'package'
        self.stage.mkdir()

    def fixture(self):
        (self.stage / 'lib').mkdir()
        for name in ('h700_integration_driver', 'lib/libSDL3.so.0', 'run.sh'):
            path = self.stage / name
            path.write_text('CPU fixture, not ELF\n'); path.chmod(0o644)
        self.seal()

    def seal(self, scenario=package.SCENARIO, allowed_files=None):
        build.write_json(self.stage / 'manifest.json', dict(scenario=scenario, device_status='pending', payload=package.payload(self.stage, allowed_files)))
        (self.stage / 'SHA256SUMS').write_text(''.join(build.sha256(p)+'  '+p.relative_to(self.stage).as_posix()+'\n'
             for p in sorted(self.stage.rglob('*')) if p.is_file() and p.name != 'SHA256SUMS'))

    def test_checksum_and_content_inventory_reject_tamper_missing_and_extra(self):
        self.fixture(); package.validate_package(self.stage)
        path = self.stage / 'run.sh'; original = path.read_bytes()
        path.write_bytes(original+b'tamper')
        with self.assertRaisesRegex(ValueError, 'mismatch'): package.validate_package(self.stage)
        path.write_bytes(original); path.unlink()
        with self.assertRaisesRegex(ValueError, 'mismatch'): package.validate_package(self.stage)
        path.write_bytes(original)
        (self.stage / 'lib/libc.so.6').write_text('forbidden container library')
        with self.assertRaisesRegex(ValueError, 'Unexpected'): package.validate_package(self.stage)

    def test_symlink_and_privileged_modes_rejected_even_with_valid_hash(self):
        self.fixture()
        (self.stage / 'licenses').mkdir()
        link = self.stage / 'licenses/escape'
        link.symlink_to('/etc/passwd')
        with self.assertRaisesRegex(ValueError, 'symlinks'): package.payload(self.stage)
        link.unlink()
        # Docker's host bind filesystem can strip setuid/setgid on chmod. Exercise every
        # privileged bit through the SAME production predicate, not a filesystem assumption.
        for mode in (0o4755,0o2755,0o777,0o664,0o646):
            self.assertFalse(package.safe_file_mode(mode))
        self.assertTrue(package.safe_file_mode(0o755)); self.assertTrue(package.safe_file_mode(0o644))
        (self.stage / 'run.sh').chmod(0o777)
        with self.assertRaisesRegex(ValueError, 'mode'): package.payload(self.stage)

    def test_asset_settings_saves_and_extra_executables_not_allowed(self):
        for name in ('game.iso', 'settings.json', 'card.raw', 'extra-test'):
            with self.subTest(name=name):
                path = self.stage / name; path.write_text('not allowed')
                with self.assertRaisesRegex(ValueError, 'Unexpected'): package.payload(self.stage)
                path.unlink()

    def test_archive_members_are_exact_and_reproducible_no_old_binaries(self):
        self.fixture()
        a, b = self.root/'one.tar.gz', self.root/'two.tar.gz'
        first = package.archive_package(self.stage, a); second = package.archive_package(self.stage, b)
        self.assertEqual(first['sha256'], second['sha256'])
        with tarfile.open(a) as archive:
            self.assertTrue(all(m.isfile() and m.uid == 0 and m.gid == 0 and m.mtime == 0 for m in archive))
        with self.assertRaisesRegex(ValueError, 'already exists'): package.archive_package(self.stage, a)

    def test_custom_payload_and_scenario_keep_exact_inventory_and_library_rejection(self):
        self.fixture()
        (self.stage / 'h700_integration_driver').rename(self.stage / 'metroid_prime_port')
        allowed = {'metroid_prime_port', 'lib/libSDL3.so.0', 'run.sh'}
        scenario = {'profile': 'game', 'device_validated': False}
        self.seal(scenario, allowed)
        package.validate_package(self.stage, scenario=scenario, allowed_files=allowed)
        with self.assertRaisesRegex(ValueError, 'Scenario'):
            package.validate_package(self.stage, allowed_files=allowed)
        with self.assertRaisesRegex(ValueError, 'Unexpected'):
            package.payload(self.stage)
        for name in ('h700_integration_driver', 'game.iso', 'lib/libc.so.6', 'lib/extra.so'):
            with self.subTest(name=name):
                path = self.stage / name
                path.write_text('unapproved extra')
                with self.assertRaisesRegex(ValueError, 'Unexpected'):
                    package.validate_package(self.stage, scenario, allowed)
                path.unlink()
        # A supplied empty allowlist is not a request for integration defaults.
        with self.assertRaisesRegex(ValueError, 'Unexpected'):
            package.payload(self.stage, set())
        (self.stage / 'metroid_prime_port').write_text('tampered game')
        with self.assertRaisesRegex(ValueError, 'mismatch'):
            package.validate_package(self.stage, scenario, allowed)

    def test_custom_archive_root_inventory_and_unsafe_components(self):
        self.fixture()
        a, b = self.root / 'game-a.tar.gz', self.root / 'game-b.tar.gz'
        self.assertEqual(package.archive_package(self.stage, a, 'H700Game')['sha256'],
                         package.archive_package(self.stage, b, root_name='H700Game')['sha256'])
        with tarfile.open(a) as archive:
            self.assertEqual({m.name for m in archive},
                             {'H700Game/' + p.relative_to(self.stage).as_posix()
                              for p in self.stage.rglob('*') if p.is_file()})
        target = self.root / 'unsafe.tar.gz'
        for name in ('', '.', '..', '../escape', '/absolute', 'nested/root', 'root\\escape',
                     'root:escape', './Game', 'Game/'):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'Unsafe'):
                package.archive_package(self.stage, target, root_name=name)
            self.assertFalse(target.exists())

    def test_pending_cannot_be_relabelled_device_pass(self):
        self.fixture()
        m = json.loads((self.stage/'manifest.json').read_text()); m['device_status'] = 'PASS'
        build.write_json(self.stage/'manifest.json', m)
        with self.assertRaisesRegex(ValueError, 'status changed'): package.validate_package(self.stage)

    def test_firmware_closure_requires_actual_needed_nodes_and_providers(self):
        snap = dict(libraries={'a':dict(status='found',machine='AArch64',needed=['b'],required_version_nodes=['GLIBC_2.38'],defined_version_nodes=[]),
                              'b':dict(status='found',machine='AArch64',needed=[],required_version_nodes=[],defined_version_nodes=['GLIBC_2.38'])})
        self.assertEqual(set(package.firmware_closure(snap,['a'])), {'a','b'})
        snap['libraries']['b']['defined_version_nodes'] = ['GLIBC_2.39']
        with self.assertRaisesRegex(ValueError, 'nodes absent'): package.firmware_closure(snap,['a'])
        del snap['libraries']['b']
        with self.assertRaisesRegex(ValueError, 'missing provider'): package.firmware_closure(snap,['a'])

    def test_existing_exact_provider_gate_not_maximum_version_only(self):
        gate = package.probe_verifier()
        info = dict(needed=['libc.so.6'], required_version_nodes={'libc.so.6':['GLIBC_2.35']}, critical_imports={})
        snapshot = dict(schema=1, architecture='aarch64', libraries={'libc.so.6':dict(status='found',machine='AArch64',defined_version_nodes=['GLIBC_2.38'])})
        with self.assertRaisesRegex(ValueError, 'lacks version nodes'): gate.firmware_gate(info,snapshot,{})

    def test_source_revision_and_uncommitted_own_bytes_are_distinct(self):
        folder = self.root/'tools/h700_integration_driver'; folder.mkdir(parents=True)
        file = folder/'own.py'; file.write_text('new source')
        def git(command, **kwargs):
            args = command[2:]
            if args[:1] == ['rev-parse']: return b'6a58db6e3\n'
            if args[:1] == ['diff']: return b''
            if '--cached' in args: return b'tools/h700_integration_driver/own.py\0.DS_Store\0'
            return b'tools/h700_integration_driver/own.py\n'
        with mock.patch.object(package.subprocess, 'check_output', side_effect=git):
            first = package.source_snapshot(self.root)
            file.write_text('changed source')
            second = package.source_snapshot(self.root)
        self.assertEqual(first['revision'], second['revision'])
        self.assertNotEqual(first['source_sha256'], second['source_sha256'])
        self.assertEqual(first['dirty_source_files'], ['tools/h700_integration_driver/own.py'])
        self.assertNotIn('.DS_Store', first['files'])

    def test_source_snapshot_extensions_union_existing_inputs(self):
        prefixes = ('src/', 'include/', 'platform/', 'tests/', 'tools/h700_game/', 'extern/musyx/',
                    'extern/astcenc/', 'extern/GamepadMotionHelpers/', 'textures/', 'assets/', 'config/')
        exact = ('CMakeLists.txt', 'files.cmake', 'version.txt')
        selected = ['tools/h700_integration_driver/build.py', 'LICENSE'] + [p + 'fixture' for p in prefixes] + list(exact)
        ignored = ['unrelated.txt', 'src/.DS_Store', 'tools/h700_game/__pycache__/cache.pyc']
        for name in selected + ignored:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(name)
        def git(command, **kwargs):
            args = command[2:]
            if args[:1] == ['rev-parse']: return b'fixture-revision\n'
            if args[:1] == ['diff']: return b'src/fixture\nCMakeLists.txt\nunrelated.txt\n'
            if '--cached' in args: return ('\0'.join(selected + ignored) + '\0').encode()
            return b''
        with mock.patch.object(package.subprocess, 'check_output', side_effect=git):
            default = package.source_snapshot(self.root)
            first = package.source_snapshot(self.root, extra_prefixes=prefixes, extra_names=exact)
            (self.root / 'src/fixture').write_text('changed game bytes')
            second = package.source_snapshot(self.root, prefixes, exact)
        self.assertEqual(set(default['files']), {'tools/h700_integration_driver/build.py', 'LICENSE'})
        self.assertEqual(set(first['files']), set(selected))
        self.assertEqual(first['dirty_source_files'], ['CMakeLists.txt', 'src/fixture'])
        self.assertEqual(first['revision'], second['revision'])
        self.assertNotEqual(first['source_sha256'], second['source_sha256'])

    def test_audit_uses_actual_executable_for_elf_symbols_and_strict_runpath(self):
        out = self.root / 'out'
        (out / 'build').mkdir(parents=True)
        (out / 'build/providers.txt').write_text(
            'SDL3::SDL3=:source:/out/inputs/sdl\nZLIB::ZLIB=fixture\nzstd::libzstd=fixture\n'
            'dawn::webgpu_dawn=:package:/out/inputs/dawn/\nnod::nod_static=fixture\n')
        (self.root / 'docs').mkdir()
        build.write_json(self.root / 'docs/H700_RUNTIME_ABI.json', {'libraries': {}})
        for rpath in ('$ORIGIN/lib', '/out/lib', '$ORIGINevil/lib', '$ORIGIN/../lib'):
            with self.subTest(rpath=rpath):
                elf = self.root / 'elf.txt'
                elf.write_text('(RUNPATH) Library runpath: [' + rpath + ']\nLibrary soname: [libSDL3.so.0]\n')
                symbols = self.root / 'symbols.txt'
                symbols.write_text('spvtools::fixture\n')
                commands = mock.Mock()
                commands.run.side_effect = lambda label, *args: symbols if label == 'linked-symbols' else elf
                verifier = mock.Mock()
                verifier.elf_info.return_value = {'needed': ['libSDL3.so.0']}
                verifier.firmware_gate.return_value = {'status': 'fixture'}
                with mock.patch.object(package, 'ROOT', self.root), \
                     mock.patch.object(package, 'probe_verifier', return_value=verifier), \
                     mock.patch.object(package, 'firmware_closure', return_value={}), \
                     mock.patch.object(build.environment, 'symbols', return_value={'SDL_Fixture': ['SDL_Fixture']}):
                    if rpath != '$ORIGIN/lib':
                        with self.assertRaisesRegex(ValueError, 'RPATH|RUNPATH'):
                            package.audit(out, self.stage, commands, executable='metroid_prime_port')
                        continue
                    report = package.audit(out, self.stage, commands, executable='metroid_prime_port')
                calls = {c.args[0]: c.args[1] for c in commands.run.call_args_list}
                self.assertEqual(calls['staged-executable-elf'][-1], self.stage / 'metroid_prime_port')
                self.assertEqual(calls['linked-symbols'][-1], self.stage / 'metroid_prime_port')
                self.assertEqual(calls['staged-sdl-elf'][-1], self.stage / 'lib/libSDL3.so.0')
                self.assertEqual(verifier.firmware_gate.call_count, 2)
                self.assertEqual(report['exact_bundled_sdl_imports'], {'SDL_Fixture': ['SDL_Fixture']})

    def test_package_profile_and_fresh_output_are_explicit(self):
        args = type('Args', (), dict(jobs=2,configure_only=False,package=True))()
        command = build.docker_command(self.root,self.root/'out','cpu-test',args)
        self.assertIn('--package', command); self.assertIn('--network=none',command)
        self.assertNotIn('--privileged',command)
        (self.root/'build').mkdir()
        output = build.output_path(self.root,'build/fresh'); output.mkdir()
        with self.assertRaisesRegex(ValueError, 'already exists'): build.output_path(self.root,'build/fresh')

    def test_real_pinned_notice_hashes_match_without_network(self):
        metadata = package.verify_notices(build.ROOT)
        self.assertGreater(len(metadata['documents']), 15)
        text = (build.ROOT/package.NOTICES/'dawn-DEPS.txt').read_text()
        for pin in ('5b5a25231a3543559b4f2cb27fe725a08b76b5d0','942fe4b988359a0750b79f0ae7ed735994d3147d'):
            self.assertIn(pin,text)

    def test_launcher_requires_explicit_fresh_path_and_preserves_failure(self):
        script = self.stage/'run.sh'; shutil.copyfile(build.ROOT/build.DRIVER/'run.sh',script)
        executable = self.stage/'h700_integration_driver'
        executable.write_text('#!/bin/sh\nprintf "%s\\n" "CPU stand-in only"\nexit 17\n'); executable.chmod(0o755)
        result = subprocess.run(['sh',str(script),'--run','relative'], capture_output=True,text=True,timeout=5)
        self.assertEqual(result.returncode,2)
        # The only executable here is the above shell stand-in, NEVER real Aurora/GPU.
        fake = self.root/'bin'; fake.mkdir()
        timeout = fake/'timeout'
        timeout.write_text('#!/bin/sh\n[ "$1 $2 $3" = "-s KILL 90" ] || exit 98\nshift 3\nexec "$@"\n'); timeout.chmod(0o755)
        env = dict(os.environ, PATH=str(fake)+':'+os.environ['PATH']); env.pop('LD_LIBRARY_PATH',None); env.pop('LD_PRELOAD',None)
        result = subprocess.run(['sh',str(script),'--run',str(self.root/'fresh-test')],env=env,capture_output=True,text=True,timeout=5)
        self.assertEqual(result.returncode,17); self.assertIn('CPU stand-in only',result.stdout)
        self.assertFalse((self.root/'fresh-test').exists())

    @unittest.skipUnless(shutil.which('timeout'), 'timeout utility test runs in approved Linux image')
    def test_external_timeout_can_kill_cpu_stall_without_waiting_on_driver(self):
        result = subprocess.run(['timeout','-s','KILL','0.1','sh','-c','sleep 3'], timeout=2,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.assertIn(result.returncode,(-9,137))

    def test_collector_preserves_source_overrides_and_nested_notices(self):
        inputs = self.root/'inputs'; src = inputs/'narrow'; (src/'nested').mkdir(parents=True)
        (src/'LICENSE').write_text('fixture license'); (src/'nested/NOTICE').write_text('fixture attribution')
        dest = self.root/'notices'
        subprocess.run(['bash',str(build.ROOT/'tools/collect_licenses.sh'),str(self.root/'empty-build'),str(dest),str(inputs)],
                       check=True,capture_output=True,text=True,timeout=20)
        self.assertEqual((dest/'narrow-src.txt').read_text(),'fixture license')
        self.assertEqual((dest/'narrow-src/nested/NOTICE').read_text(),'fixture attribution')
        self.assertTrue((dest/'port-license.txt').is_file())
        self.assertTrue((dest/'dawn.txt').is_file())


if __name__ == '__main__':
    unittest.main()
