"""Game-only notice additions for the approved H700 local RC.

Call verify_game_notices(root, out, stage / 'licenses') AFTER the integration
package.collect_notices(out, stage, commands). This does not run that collector,
build, inspect binaries, use git/network, or authorize public distribution.
Missing collected documents raise RuntimeError. Known absent primary grants are
returned AND written to game-coverage.json; they are not silently treated as MIT.
"""
import hashlib
import json
from pathlib import Path


SOURCE_DEPS = ('sdl', 'abseil-cpp', 'fmt', 'png', 'freetype', 'imgui',
               'xxhash', 'tracy', 'zlib', 'zstd')
VENDORED = {
    'LICENSE': 'port-license.txt',
    'NOTICE': 'port-notice.txt',
    'extern/aurora/LICENSE': 'aurora.txt',
    'extern/musyx/LICENSE': 'musyx.txt',
    'extern/astcenc/LICENSE.txt': 'astcenc.txt',
    'extern/GamepadMotionHelpers/LICENSE': 'gamepadmotionhelpers.txt',
}
COMMON_EMBEDDED = (
    'sqlite3-public-domain.txt', 'imgui-default-font-attribution.txt',
    'imgui-imstb_rectpack.h-license.txt', 'imgui-imstb_textedit.h-license.txt',
    'imgui-imstb_truetype.h-license.txt', 'freetype-FTL.txt',
    'h700-pinned/proggyclean-LICENSE.txt', 'h700-pinned/provenance.json',
    'h700-pinned/scope.txt',
)
REPORT = 'game-coverage.json'


def _require(condition, message):
    if not condition:
        raise RuntimeError(message)


def _bytes(path):
    _require(path.is_file() and not path.is_symlink(),
             f'Missing/non-regular notice input: {path.name}')
    data = path.read_bytes()
    _require(bool(data.strip()), f'Empty notice input: {path.name}')
    return data


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _relative(name):
    path = Path(name)
    _require(not path.is_absolute() and path.parts and '..' not in path.parts,
             f'Non-relative notice name: {name}')
    return path


def _common_notices(root, dest, additions):
    """Check the existing collector result, never replace it with a short list."""
    data = _bytes(dest / 'coverage.json')
    coverage = json.loads(data)

    for dep in SOURCE_DEPS:
        names = coverage['source_dependencies'].get(dep)
        _require(isinstance(names, list) and bool(names), f'Common collector missed {dep}')
        for name in names:
            _bytes(dest / _relative(name))

    for name in COMMON_EMBEDDED:
        _bytes(dest / name)
    originals = dict(VENDORED)
    originals.update({str(p.relative_to(root)): p.name
                      for p in sorted((root / 'packaging/licenses').glob('*.txt'))})
    _require(bool(list((root / 'packaging/licenses').glob('nod-*.txt'))),
             'Missing packaged nod grants')
    for source, name in originals.items():
        _require(_bytes(root / source) == _bytes(dest / name),
                 f'Common collector content mismatch: {name}')
    inherited = {str(p.relative_to(dest)): _sha(_bytes(p))
                 for p in sorted(dest.rglob('*')) if (p.is_file() or p.is_symlink())
                 and str(p.relative_to(dest)) not in additions}
    return dict(file='coverage.json', sha256=_sha(data),
                source_dependencies=coverage['source_dependencies'],
                files_sha256=inherited, required_files=sorted(inherited))


def verify_game_notices(root, out, dest):
    """Add/verify source notices and return a JSON-serializable local-RC report.

    root is the game checkout, out the existing verified build output (inputs/),
    dest the ALREADY collected licenses directory, not the package root. Leaves
    common coverage.json and all collector extras untouched. Repeated calls are
    supported. A successful return means collection, NOT licence clearance.
    """
    root, out, dest = Path(root), Path(out), Path(dest)
    _require(dest.is_dir() and not dest.is_symlink(), 'Run common collect_notices first')
    documents, contents = {}, {}

    def extract(base, source, start, end, tokens, *, last=False):
        raw = _bytes(base / source)
        text = raw.decode('utf-8')
        begin = text.rfind(start) if last else text.find(start)
        finish = text.find(end, begin + len(start)) if begin >= 0 else -1
        _require(begin >= 0 and finish >= 0, f'Cannot locate notice block: {source}')
        finish += len(end)
        block = text[begin:finish]
        _require(all(token in block for token in tokens), f'Incomplete notice block: {source}')
        prefix = 'inputs/' if base == out / 'inputs' else ''
        evidence = dict(file=prefix + source, source_sha256=_sha(raw),
                        first_line=text.count('\n', 0, begin) + 1,
                        last_line=text.count('\n', 0, finish - 1) + 1)
        return block, evidence

    def add(name, component, kind, blocks):
        # Keep source text verbatim (including its original spelling/disclaimers).
        text = '\n\n'.join('Source: ' + evidence['file'] + '\n' + block
                           for block, evidence in blocks) + '\n'
        contents[name] = text.encode('utf-8')
        documents[name] = dict(component=component, kind=kind,
                               sources=[evidence for _, evidence in blocks], sha256=_sha(contents[name]))

    def block(source, start, end, tokens):
        return extract(root, source, start, end, tokens)

    add('game-g721-license.txt', 'Sun G.721 ADPCM', 'embedded-grant', [block(
        'src/Kyoto/Audio/g721.cpp', '/* G.721 decoder', '*/',
        ('Sun Microsystems', 'provided', 'unrestricted use', 'Users may copy or modify',
         'SUN SOURCE CODE IS PROVIDED AS IS', 'In no event'))])
    add('game-magic-enum-license.txt', 'magic_enum 0.7.2', 'embedded-grant', [block(
        'extern/aurora/include/magic_enum.hpp', '// Licensed under the MIT License', '// SOFTWARE.',
        ('Daniil Goncharov', 'Permission is hereby', 'copies or substantial portions', 'LIABILITY'))])
    add('game-libogc-sram-license.txt', 'modified libogc SRAM definitions', 'embedded-grant', [block(
        'extern/aurora/lib/card/SRAM.hpp', '// Modified code taken from libogc',
        '-------------------------------------------------------------*/',
        ('Copyright (C) 2004', 'Michael Wiedenbauer', 'Dave Murphy',
         'Permission is granted', '3. This notice may not be removed'))])
    add('game-astcenc-attribution.txt', 'ARM astc-encoder 5.7.0', 'attribution-with-astcenc.txt', [block(
        'extern/astcenc/Source/astcenc.h', '// SPDX-License-Identifier: Apache-2.0',
        '// under the License.', ('Arm Limited', 'Licensed under the Apache License', 'AS IS'))])

    inputs = out / 'inputs'
    # SDL_stb.c is part of the shipped SDL, not Tracy's unbuilt profiler/tests.
    sdl = _bytes(inputs / 'sdl/src/video/SDL_stb.c').decode('utf-8')
    _require(all(token in sdl for token in ('STB_IMAGE_IMPLEMENTATION', '#include "stb_image.h"',
                                           '#include "miniz.h"')), 'SDL embedded codec source changed')
    add('game-sdl-stb-image-license.txt', 'SDL embedded stb_image', 'embedded-grant', [extract(
        inputs, 'sdl/src/video/stb_image.h', 'ALTERNATIVE A - MIT License', '*/',
        ('Sean Barrett', 'Permission is hereby granted', 'SOFTWARE.',
         'ALTERNATIVE B - Public Domain'), last=True)])
    miniz = 'sdl/src/video/miniz.h'
    add('game-sdl-miniz-license.txt', 'SDL embedded miniz 1.15', 'embedded-grant', [
        extract(inputs, miniz, '/* miniz.c v1.15', 'last updated Oct. 13, 2013', ('Rich Geldreich',)),
        extract(inputs, miniz, '/*\n  This is free and unencumbered software', '*/',
                ('public domain', 'Anyone is free to copy', 'THE SOFTWARE IS PROVIDED',
                 'OTHER DEALINGS IN THE SOFTWARE.'), last=True)])

    unresolved = []
    for component, source, marker, name, reason in (
        ('tegra_swizzle 0.3.2', 'platform/port_remastered_tegra_swizzle.cpp', '// tegra_swizzle',
         'game-tegra-swizzle-attribution.txt',
         'Only a combined MIT attribution is present; primary tegra_swizzle grant is absent.'),
        ('bcdec_rs 0.1.2', 'platform/port_remastered_bcdec.cpp', '// bcdec_rs',
         'game-bcdec-rs-attribution.txt',
         'Only a combined MIT attribution is present; primary bcdec_rs/underlying bcdec grants are absent.'),
        ('astc-decode 0.3.1', 'platform/port_remastered_astc_decode.cpp', '// astc-decode',
         'game-astc-decode-attribution.txt',
         'Apache-2.0 is named, but the primary crate/UNC-derived grant and attribution are absent; '
         'astcenc.txt belongs to a different work and does not resolve this.'),
        ('retrotool retrolib readers/LZSS', 'platform/port_remastered_txtr.cpp', '// Metroid Prime Remastered',
         'game-retrotool-attribution.txt',
         'MIT/Apache-2.0 is named, but no primary retrotool grant/copyright text or exact revision is supplied.'),
    ):
        part = block(source, marker, '\n\n', (component.split()[0],))
        add(name, component, 'attribution-only-NOT-a-primary-grant', [part])
        unresolved.append(dict(component=component, evidence=name, reason=reason))

    add('game-prompt-icons-attribution.txt', 'Kenney/Xelu embedded prompt glyphs',
        'repository-attribution-only', [block('tools/prompt_icons/README.md', '# Prompt icons',
                                             '\nWhich game textures', ('Kenney', 'Xelu', 'CC0 1.0'))])
    unresolved.extend([
        dict(component='Kenney/Xelu prompt icons', evidence='game-prompt-icons-attribution.txt',
             reason='Repository README states CC0; the primary packs\' licence texts are not present.'),
        dict(component='decompiled game and SDK/runtime; source-derived game ports', evidence='port-notice.txt',
             reason='NOTICE explicitly excludes src/, include/ and extern/sdk/ from the root MIT grant '
                    'and asserts no redistribution permission. Source-derived Remastered code is also '
                    'compiled (CMakeLists.txt: port_remastered_ground_contacts); platform placement '
                    'does not itself establish a primary grant.'),
    ])
    common = _common_notices(root, dest, set(contents) | {REPORT})
    report = dict(
        notices_status='collected-for-local-rc',
        public_distribution_authorized=False,
        common=common, documents=documents, unresolved_primary_grants=unresolved,
        coverage={
            'game_engine': ['port-notice.txt', 'game-g721-license.txt'],
            'musyx_audio': ['musyx.txt'],
            'astc_encoder': ['astcenc.txt', 'game-astcenc-attribution.txt'],
            'gyro': ['gamepadmotionhelpers.txt'],
            'aurora_thp_and_port_jpeg': ['aurora.txt', 'port-license.txt'],
            'aurora_headers': ['game-magic-enum-license.txt', 'game-libogc-sram-license.txt'],
            'nod_disc': ['nod-MIT.txt', 'nod-Apache-2.0.txt'],
            'compiled_mod_codecs': ['game-tegra-swizzle-attribution.txt', 'game-bcdec-rs-attribution.txt',
                                    'game-astc-decode-attribution.txt', 'game-retrotool-attribution.txt'],
            'sdl_embedded_codecs': ['game-sdl-stb-image-license.txt', 'game-sdl-miniz-license.txt'],
            'embedded_prompts': ['game-prompt-icons-attribution.txt'],
        },
        scope=[
            'Approved root CMake/files.cmake game path, not a full symbol/platform matrix or legal certification.',
            'Runtime mods OFF does not remove the compiled Remastered importers/codecs or their obligations.',
            'All existing collector extras, nested fetched notices, ImGui STB/font, SQLite and pinned Dawn '
            'coverage remain required; coverage.json is not replaced.',
            'No jpge or miniaudio sources/includes were found in the inspected game/vendor sources. '
            'JPEG is platform/port_remastered_jpeg.cpp; audio is extern/musyx/src/musyx/runtime/hw_pc.c '
            'and SDL. Tracy profiler/test stb headers are not the linked TracyClient.',
            'Legacy src/Kyoto/zlib is not in files.cmake; actual ZLIB::ZLIB notices come from the common collector.',
            'No assets from a disc and no ffmpeg program are collected. Known absent primary grants '
            'are disclosed for local RC, not waived or replaced with generic MIT/Apache text.',
        ],
        required_dest_files=sorted(set(common['required_files']) | set(documents) | {REPORT}),
    )
    for name, data in contents.items():
        path = dest / name
        _require(not path.is_symlink(), f'Symlink notice output: {name}')
        path.write_bytes(data)
    _require(not (dest / REPORT).is_symlink(), f'Symlink notice output: {REPORT}')
    (dest / REPORT).write_text(json.dumps(report, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    return report
