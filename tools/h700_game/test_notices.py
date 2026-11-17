"""Local source/synthetic tests only: no builds, git, network or device access."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import notices

ROOT = Path(__file__).resolve().parents[2]

# Deliberately synthetic, not replacement licence grants for a shipped package.
STB = '''/* SYNTHETIC fixture, not a licence.
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted SYNTHETIC SOFTWARE.
ALTERNATIVE B - Public Domain
SYNTHETIC end
*/
'''
MINIZ = '''/* miniz.c v1.15 SYNTHETIC fixture
Rich Geldreich, last updated Oct. 13, 2013
*/
int not_part_of_the_notice;
/*
  This is free and unencumbered software SYNTHETIC public domain.
  Anyone is free to copy SYNTHETIC.
  THE SOFTWARE IS PROVIDED SYNTHETIC.
  OTHER DEALINGS IN THE SOFTWARE.
*/
'''


def put(root, name, text):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8')


class GameNoticeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = tempfile.TemporaryDirectory()
        cls.base = Path(cls.fixture.name)
        cls.addClassCleanup(cls.fixture.cleanup)
        out = cls.base / 'out'
        for dep in notices.SOURCE_DEPS:
            put(out, 'inputs/' + dep + '/LICENSE', 'SYNTHETIC ' + dep + ' licence\n')
        put(out, 'inputs/freetype/docs/FTL.TXT', 'SYNTHETIC FTL\n')
        put(out, 'inputs/sdl/nested/LICENSE-extra', 'SYNTHETIC nested SDL notice\n')
        put(out, 'inputs/tracy/profiler/COPYING', 'SYNTHETIC conservative collector extra\n')
        put(out, 'inputs/imgui/imgui_draw.cpp', '// padding\n' * 30 +
            '// [SECTION] Default font data\n// ProggyClean.ttf SYNTHETIC\n// File: font\n')
        for header in ('imstb_rectpack.h', 'imstb_textedit.h', 'imstb_truetype.h'):
            put(out, 'inputs/imgui/' + header, STB)
        put(out, 'inputs/sqlite3/sqlite3.c', '/* SYNTHETIC public domain */\nint not_a_notice;\n')
        put(out, 'inputs/sdl/src/video/stb_image.h', STB)
        put(out, 'inputs/sdl/src/video/miniz.h', MINIZ)
        put(out, 'inputs/sdl/src/video/SDL_stb.c',
            '#define STB_IMAGE_IMPLEMENTATION\n#include "stb_image.h"\n#include "miniz.h"\n')
        (out / 'build').mkdir()
        # Exercise the REAL existing integration collector with synthetic fetched
        # inputs and real repository notices. Separate interpreter avoids its
        # historical unqualified `import build` colliding with game build tests.
        code = '''
import pathlib, subprocess, sys
sys.path.insert(0, str(pathlib.Path('tools/h700_integration_driver').resolve()))
import package
class Commands:
    def run(self, label, args, timeout):
        subprocess.run(args, check=True, timeout=timeout, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
package.collect_notices(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), Commands())
'''
        subprocess.run([sys.executable, '-B', '-c', code, str(out), str(cls.base / 'stage')],
                       cwd=ROOT, check=True, timeout=30, capture_output=True, text=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.out = self.work / 'out'
        self.dest = self.work / 'stage/licenses'
        shutil.copytree(self.base / 'out', self.out)
        shutil.copytree(self.base / 'stage/licenses', self.dest)

    def verify(self):
        return notices.verify_game_notices(ROOT, self.out, self.dest)

    def test_real_common_collector_extras_and_coverage_survive_repeat(self):
        before = {str(p.relative_to(self.dest)): p.read_bytes()
                  for p in self.dest.rglob('*') if p.is_file()}
        self.assertIn('sdl-src/nested/LICENSE-extra', before)
        self.assertIn('tracy-src/profiler/COPYING', before)
        for source in (ROOT / 'packaging/licenses').glob('*.txt'):
            self.assertEqual(before[source.name], source.read_bytes())
        first = self.verify()
        self.assertEqual(first, self.verify())
        self.assertEqual(set(first['common']['files_sha256']), set(before))
        for name, data in before.items():
            self.assertIn(name, first['required_dest_files'])
            self.assertEqual(first['common']['files_sha256'][name], hashlib.sha256(data).hexdigest())
            self.assertEqual((self.dest / name).read_bytes(), data, name)
        self.assertEqual(first, json.loads((self.dest / notices.REPORT).read_text()))

    def test_all_game_documents_have_exact_source_evidence(self):
        report = self.verify()
        self.assertEqual(len(report['documents']), 11)
        for name, doc in report['documents'].items():
            shipped = (self.dest / name).read_bytes()
            self.assertEqual(doc['sha256'], hashlib.sha256(shipped).hexdigest())
            for source in doc['sources']:
                base = self.out if source['file'].startswith('inputs/') else ROOT
                raw = (base / source['file']).read_bytes()
                self.assertEqual(source['source_sha256'], hashlib.sha256(raw).hexdigest())
                lines = raw.decode().splitlines(keepends=True)
                # Whole extracted lines occur verbatim, even Sun's original '.6' typo.
                selected = ''.join(lines[source['first_line']:source['last_line'] - 1])
                self.assertIn(selected.encode(), shipped)
        self.assertIn(b'OR ANY PART THEREOF.6', (self.dest / 'game-g721-license.txt').read_bytes())
        self.assertNotIn(b'not_part_of_the_notice', (self.dest / 'game-sdl-miniz-license.txt').read_bytes())
        self.assertIn(b'SYNTHETIC end', (self.dest / 'game-sdl-stb-image-license.txt').read_bytes())
        for name in report['required_dest_files']:
            self.assertTrue((self.dest / name).is_file(), name)
        self.assertNotIn(str(self.work), json.dumps(report))

    def test_known_missing_primary_grants_are_not_a_distribution_pass(self):
        report = self.verify()
        self.assertEqual(report['notices_status'], 'collected-for-local-rc')
        self.assertFalse(report['public_distribution_authorized'])
        self.assertEqual({item['component'] for item in report['unresolved_primary_grants']}, {
            'tegra_swizzle 0.3.2', 'bcdec_rs 0.1.2', 'astc-decode 0.3.1',
            'retrotool retrolib readers/LZSS', 'Kenney/Xelu prompt icons',
            'decompiled game and SDK/runtime; source-derived game ports'})
        for item in report['unresolved_primary_grants']:
            self.assertTrue((self.dest / item['evidence']).is_file())
        for name in report['coverage']['compiled_mod_codecs']:
            self.assertIn('NOT-a-primary-grant', report['documents'][name]['kind'])
            self.assertNotIn('Permission is hereby granted', (self.dest / name).read_text())
        self.assertIn('astcenc.txt belongs to a different work',
                      report['unresolved_primary_grants'][2]['reason'])

    def test_missing_actual_game_baseline_notice_is_an_error(self):
        for name in ('musyx.txt', 'astcenc.txt', 'gamepadmotionhelpers.txt',
                     'nod-MIT.txt', 'randomprime.txt', 'imgui-imstb_truetype.h-license.txt',
                     'sdl-src.txt', 'h700-pinned/proggyclean-LICENSE.txt'):
            with self.subTest(name=name):
                path = self.dest / name
                data = path.read_bytes()
                path.unlink()
                with self.assertRaises(RuntimeError):
                    self.verify()
                path.write_bytes(data)
        self.assertFalse((self.dest / notices.REPORT).exists())

    def test_modified_baseline_grant_is_not_accepted_by_filename(self):
        (self.dest / 'musyx.txt').write_text('MIT substitute, wrong copyright')
        with self.assertRaisesRegex(RuntimeError, 'content mismatch: musyx.txt'):
            self.verify()

    def test_truncated_embedded_grant_is_an_error(self):
        for name, text in (('miniz.h', MINIZ.rsplit('*/', 1)[0]),
                           ('stb_image.h', STB.replace('Permission is hereby granted', 'REMOVED'))):
            with self.subTest(name=name):
                path = self.out / 'inputs/sdl/src/video' / name
                original = path.read_text()
                path.write_text(text)
                with self.assertRaisesRegex(RuntimeError, 'notice block'):
                    self.verify()
                path.write_text(original)
        self.assertFalse((self.dest / notices.REPORT).exists())

    def test_collector_must_run_first(self):
        (self.dest / 'coverage.json').unlink()
        with self.assertRaisesRegex(RuntimeError, 'coverage.json'):
            self.verify()

    def test_existing_report_or_notice_symlink_is_not_followed(self):
        sentinel = self.work / 'unrelated.txt'
        sentinel.write_text('preserve')
        for name in ('game-g721-license.txt', notices.REPORT):
            with self.subTest(name=name):
                path = self.dest / name
                path.symlink_to(sentinel)
                with self.assertRaisesRegex(RuntimeError, 'Symlink notice output'):
                    self.verify()
                self.assertEqual(sentinel.read_text(), 'preserve')
                path.unlink()

    def test_source_local_actual_game_scope_not_mod_runtime_settings(self):
        cmake = (ROOT / 'CMakeLists.txt').read_text()
        port = cmake.split('set(MP_PORT_SOURCES', 1)[1].split('platform/ai_dma.cpp)', 1)[0]
        for source in ('port_remastered_tegra_swizzle.cpp', 'port_remastered_bcdec.cpp',
                       'port_remastered_astc_decode.cpp', 'port_remastered_txtr.cpp',
                       'port_remastered_pak.cpp', 'port_remastered_cmdl.cpp', 'port_remastered_jpeg.cpp',
                       'port_gyro_motion.cpp'):
            self.assertIn('platform/' + source, port)
        self.assertIn('musyx astcenc)', cmake)
        files = (ROOT / 'files.cmake').read_text()
        self.assertIn('src/Kyoto/Audio/g721.cpp', files)
        self.assertNotIn('src/Kyoto/zlib/', files)
        jpeg = (ROOT / 'platform/port_remastered_jpeg.cpp').read_text()
        audio = (ROOT / 'extern/musyx/src/musyx/runtime/hw_pc.c').read_text()
        self.assertNotIn('jpge', jpeg)
        self.assertNotIn('miniaudio', audio)
        self.assertIn('SDL', audio)
        thp = (ROOT / 'extern/aurora/cmake/aurora_thp.cmake').read_text()
        self.assertIn('lib/dolphin/thp/THPAudio.cpp', thp)
        self.assertIn('lib/dolphin/thp/THPDec.cpp', thp)


if __name__ == '__main__':
    unittest.main()
