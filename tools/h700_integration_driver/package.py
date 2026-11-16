"""IG13 staging/audit only. No graphics entry point and no network access."""
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tarfile

import build

ROOT = build.ROOT
NOTICES = Path('packaging/h700-notices')
CPU_TARGETS = ['h700_driver_scene_tests', 'h700_presentation_state_tests', 'h700_presentation_integration_tests',
               'render_worker_tests', 'gfx_recording_tests', 'gx_fifo_tests', 'bloom_readback_slots_tests']
CPU_FILTER = ('DriverScene|GenerationLifecycle|MapCompletionTest|MapRetirementSource|RenderWorker|Thread|'
              'PipelineWaitTest|PipelineCompilerSource|GfxRecordingTest|GfxFrameWaitOwnership|ReadbackSlots|H700Presentation')
SCENARIO = dict(profile='h700-mixed-v1', backend='hardware OpenGLES Compatibility; no fallback',
                generations=2, generation1_frames=120, generation2_frames=2, generation2_max_frames=8,
                imports_per_generation=2, native_format='RGBA8Unorm', native_size=[640, 480],
                efb_resize_frame=60, efb_sizes=[[640, 480], [320, 240]], cold_fog_variants=4,
                credit_pressure_progress_passes=4, app_deadline_seconds=60, watchdog_seconds=90,
                resampler='Nearest', sharpness=0, validation=True, robustness=True, seed_cache=False,
                fresh_cache_per_generation=True, assets=False, cards=False, device_validated=False)


def source_snapshot(root):
    """Hash build-relevant tracked + uncommitted inputs, not user files/output/docs."""
    def git(*args):
        return subprocess.check_output(['git', '--no-pager', *args], cwd=root, timeout=30)
    prefixes = ('extern/aurora/', 'cmake/', 'tools/h700_integration_driver/', 'tools/h700_graphics_probe/',
                'packaging/', 'platform/glibc_compat.c', 'tools/collect_licenses.sh', 'docs/H700_RUNTIME_ABI.json')
    names = set(git('ls-files', '-z', '--cached', '--others', '--exclude-standard').decode().split('\0'))
    names = sorted(n for n in names if n and (n in ('LICENSE', 'NOTICE') or n.startswith(prefixes))
                   and Path(n).name != '.DS_Store' and '__pycache__' not in Path(n).parts)
    hashes = {}
    for name in names:
        path = root / name
        build.require(path.is_file() and not path.is_symlink(), f'Non-regular/missing build input: {name}')
        hashes[name] = build.sha256(path)
    changed = set(git('diff', '--name-only', 'HEAD').decode().splitlines())
    changed.update(git('ls-files', '--others', '--exclude-standard').decode().splitlines())
    return dict(revision=git('rev-parse', 'HEAD').decode().strip(),
                dirty_source_files=sorted(changed.intersection(hashes)),
                source_sha256=hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest(), files=hashes)


def verify_notices(root):
    base = root / NOTICES
    metadata = json.loads((base / 'provenance.json').read_text())
    build.require(metadata['source_ref'] == '118436b3f8f68583eb86e811ca605dc41637e8cc', 'Wrong notice Dawn ref')
    for item in metadata['documents']:
        name = str(build.relative_path(item['file']))
        path = base / name
        build.require(path.is_file() and not path.is_symlink() and build.sha256(path) == item['sha256'],
                      f'Notice provenance hash mismatch: {name}')
    for name in ('spirv-tools-LICENSE.txt', 'spirv-headers-LICENSE.txt', 'proggyclean-LICENSE.txt',
                 'dawn-abseil-LICENSE.txt', 'partition-alloc-Chromium-LICENSE.txt'):
        build.require(any(d['file'] == name for d in metadata['documents']), f'Missing notice: {name}')
    return metadata


def probe_verifier():
    # Import ONLY for its existing ELF/provider gate; do not invoke its probe build/runtime workflow.
    path = ROOT / 'tools/h700_graphics_probe/build.py'
    spec = importlib.util.spec_from_file_location('h700_probe_elf_verifier', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def firmware_closure(snapshot, roots):
    libraries = snapshot['libraries']; found = {}
    pending = list(roots)
    while pending:
        name = pending.pop()
        if name in found:
            continue
        data = libraries.get(name, {})
        build.require(data.get('status') == 'found' and data.get('machine') == 'AArch64',
                      f'Firmware chain missing provider: {name}')
        found[name] = data
        pending += data['needed']
    for name, data in found.items():
        provided = {node for dep in data['needed'] for node in found[dep]['defined_version_nodes']}
        missing = set(data['required_version_nodes']) - provided
        build.require(not missing, f'Firmware chain version nodes absent for {name}: {sorted(missing)}')
    return found


def audit(out, stage, commands):
    verifier = probe_verifier()
    snapshot = json.loads((ROOT / 'docs/H700_RUNTIME_ABI.json').read_text())
    infos, exports, texts = {}, {}, {}
    for name, rel in [('executable', 'h700_integration_driver'), ('sdl', 'lib/libSDL3.so.0')]:
        text = commands.run('staged-' + name + '-elf', ['readelf', '-W', '-h', '-l', '-d', '--dyn-syms',
                                                       '--version-info', stage / rel], 30).read_text()
        texts[name] = text
        infos[name] = verifier.elf_info(text, executable=name == 'executable')
        exports[name] = build.environment.symbols(text)
        rpaths = re.findall(r'\((?:RUNPATH|RPATH)\).*?\[([^]]+)\]', text)
        build.require(all(p.startswith('$ORIGIN') and '..' not in p and not p.startswith('$ORIGINevil')
                          for entry in rpaths for p in entry.split(':')), f'Nonlocal RPATH: {name}')
        if name == 'executable':
            build.require(rpaths == ['$ORIGIN/lib'], 'Driver requires exactly local $ORIGIN/lib RUNPATH')
        else:
            build.require('Library soname: [libSDL3.so.0]' in text, 'Wrong bundled SDL SONAME')
    bundled = {'libSDL3.so.0': infos['sdl']}
    gates = {name: verifier.firmware_gate(info, snapshot, bundled) for name, info in infos.items()}
    undefined = build.environment.symbols(texts['executable'], undefined=True)
    sdl_imports = {k: v for k, v in undefined.items() if k.startswith('SDL_')}
    build.require(bool(sdl_imports), 'No actual SDL consumers linked')
    for name, tokens in sdl_imports.items():
        build.require(name in exports['sdl'], f'Bundled SDL lacks exact symbol: {name}')
        for token in tokens:
            if '@' in token:
                build.require(token in [v.replace('@@', '@') for v in exports['sdl'][name]], f'SDL symbol version mismatch: {token}')
    roots = {'libSDL2-2.0.so.0', 'libEGL.so.1', 'libGLESv2.so.2', 'ld-linux-aarch64.so.1'}
    roots.update(n for info in infos.values() for n in info['needed'] if n != 'libSDL3.so.0')
    chain = firmware_closure(snapshot, roots)
    providers = (out / 'build/providers.txt').read_text()
    for target in ('SDL3::SDL3', 'ZLIB::ZLIB', 'zstd::libzstd', 'dawn::webgpu_dawn', 'nod::nod_static'):
        build.require(target + '=' in providers, f'Provider report missing {target}')
    build.require(':source:/out/inputs/sdl' in providers and ':package:/out/inputs/dawn/' in providers,
                  'Unexpected SDL/Dawn providers')
    symbols = commands.run('linked-symbols', ['nm', '-C', '--defined-only', stage / 'h700_integration_driver'], 60).read_text()
    inventory = {k: sum(k in line for line in symbols.splitlines()) for k in
                 ('spvtools::', 'tint::', 'absl::', 'partition_alloc::', 'dawn::native::vulkan::', 'google::protobuf::', 'glslang::')}
    report = dict(status='metadata-provider-gate-pass; NOT runtime/device PASS', artifacts=infos,
                  artifact_gates=gates, exact_bundled_sdl_imports=sdl_imports, linked_namespace_inventory=inventory,
                  firmware_snapshot_sha256=build.sha256(ROOT / 'docs/H700_RUNTIME_ABI.json'),
                  firmware_roots=sorted(roots), firmware_chain=chain, providers=providers,
                  limitations=['Firmware snapshot has version-node coverage and only five selected exact exports; not every firmware symbol.',
                               'Transitive firmware snapshot stores flat required nodes, not per-symbol/per-provider associations; each node checked against actual NEEDED providers.',
                               'dlopen SDL2/EGL/GLES providers and their measured dependency chain checked as metadata; loader, Mali, plugins and kernel4.9 paths NOT executed.',
                               'Dawn source workflow/DEPS + actual symbols bound notice coverage, not a reconstruction of every prebuilt compiler flag or legal certification.'])
    build.write_json(stage / 'audit.json', report)
    return report


def collect_notices(out, stage, commands):
    verify_notices(ROOT)
    dest = stage / 'licenses'
    commands.run('collect-licenses', ['bash', ROOT / 'tools/collect_licenses.sh', out / 'build', dest, out / 'inputs'], 120)
    shutil.copytree(ROOT / NOTICES, dest / 'h700-pinned')
    # Keep actual embedded-code grants whose names are not LICENSE/NOTICE files.
    imgui = out / 'inputs/imgui'
    for name in ('imstb_rectpack.h', 'imstb_textedit.h', 'imstb_truetype.h'):
        text = (imgui / name).read_text()
        marker = text.rfind('ALTERNATIVE A - MIT License')
        build.require(marker >= 0, f'Cannot locate embedded STB license: {name}')
        (dest / ('imgui-' + name + '-license.txt')).write_text(text[marker:])
    text = (imgui / 'imgui_draw.cpp').read_text()
    marker = text.index('// ProggyClean.ttf', text.index('// [SECTION] Default font data', 200))
    end = text.index('// File:', marker)
    (dest / 'imgui-default-font-attribution.txt').write_text(text[marker:end])
    text = (out / 'inputs/sqlite3/sqlite3.c').read_text()
    (dest / 'sqlite3-public-domain.txt').write_text(text[:text.index('*/') + 2] + '\n')
    coverage = {}
    for dep in ('sdl', 'abseil-cpp', 'fmt', 'png', 'freetype', 'imgui', 'xxhash', 'tracy', 'zlib', 'zstd'):
        files = sorted(p.name for p in dest.glob(dep + '-src*') if p.is_file())
        build.require(files, f'Collector missed verified source dependency: {dep}')
        coverage[dep] = files
    for rel in ('port-license.txt', 'port-notice.txt', 'aurora.txt', 'dawn.txt', 'freetype-FTL.txt',
                'sqlite3-public-domain.txt', 'imgui-default-font-attribution.txt', 'h700-pinned/proggyclean-LICENSE.txt'):
        build.require((dest / rel).is_file(), f'Missing actual notice: {rel}')
    build.write_json(dest / 'coverage.json', dict(source_dependencies=coverage,
        dawn_transitive='h700-pinned/provenance.json and scope.txt',
        embedded=['ImGui STB license blocks', 'ProggyClean', 'SQLite public-domain declaration'],
        conservative_extras='Unmodified collector also carries vendored/project/prebuilt notices not necessarily linked by this diagnostic slice; no claim they all execute.'))


def safe_file_mode(mode):
    return not mode & 0o6022  # setuid/setgid and group/other writable are never package metadata


def payload(stage):
    result = {}
    for path in sorted(stage.rglob('*')):
        build.require(not path.is_symlink(), f'Package cannot contain symlinks: {path}')
        if path.is_dir():
            continue
        rel = path.relative_to(stage).as_posix()
        build.require(path.is_file() and safe_file_mode(path.stat().st_mode), f'Unsafe package file/mode: {rel}')
        allowed = rel in ('h700_integration_driver', 'lib/libSDL3.so.0', 'run.sh', 'audit.json', 'source.json', 'README.txt') or rel.startswith('licenses/')
        if rel in ('manifest.json', 'SHA256SUMS'):
            continue
        build.require(allowed, f'Unexpected package content: {rel}')
        result[rel] = dict(sha256=build.sha256(path), size=path.stat().st_size, mode=stat.S_IMODE(path.stat().st_mode))
    return result


def validate_package(stage):
    manifest = json.loads((stage / 'manifest.json').read_text())
    build.require(manifest['scenario'] == SCENARIO and manifest['device_status'] == 'pending', 'Scenario/device status changed')
    build.require(manifest['payload'] == payload(stage), 'Package content/hash/mode mismatch')
    expected = {p.relative_to(stage).as_posix(): build.sha256(p) for p in stage.rglob('*') if p.is_file() and p.name != 'SHA256SUMS'}
    entries = {}
    for line in (stage / 'SHA256SUMS').read_text().splitlines():
        digest, rel = line.split('  ', 1)
        build.require(rel not in entries, 'Duplicate checksum entry')
        entries[str(build.relative_path(rel))] = digest
    build.require(entries == expected, 'Checksum inventory mismatch')
    return manifest


def archive_package(stage, target):
    build.require(not target.exists(), 'Archive already exists')
    with target.open('xb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', filename='', mtime=0) as zipped:
        with tarfile.open(fileobj=zipped, mode='w') as archive:
            for path in sorted(stage.rglob('*')):
                if not path.is_file():
                    continue
                info = archive.gettarinfo(str(path), 'H700Integration/' + path.relative_to(stage).as_posix())
                info.uid = info.gid = info.mtime = 0
                info.uname = info.gname = ''
                with path.open('rb') as stream:
                    archive.addfile(info, stream)
    with tarfile.open(target) as archive:
        expected = {'H700Integration/' + p.relative_to(stage).as_posix(): build.sha256(p) for p in stage.rglob('*') if p.is_file()}
        actual = {}
        for info in archive:
            build.require(info.isfile() and not info.issym() and not info.islnk(), 'Archive special member')
            build.relative_path(info.name)
            build.require(info.name not in actual, 'Duplicate archive member')
            actual[info.name] = hashlib.sha256(archive.extractfile(info).read()).hexdigest()
        build.require(actual == expected, 'Archive payload mismatch')
    return dict(path=target.name, sha256=build.sha256(target), size=target.stat().st_size)


def stage_package(out, commands):
    stage = out / 'package'
    build.require(not stage.exists(), 'Fresh package staging required')
    artifacts = json.loads((out / 'build/artifacts.json').read_text())
    stage.mkdir(); (stage / 'lib').mkdir()
    for key, rel in [('executable', 'h700_integration_driver'), ('sdl', 'lib/libSDL3.so.0')]:
        path = Path(artifacts[key]).resolve()
        build.require(path.is_relative_to(out / 'build') and path.is_file(), f'Artifact outside fresh build: {key}')
        shutil.copyfile(path, stage / rel)
        (stage / rel).chmod(0o755 if key == 'executable' else 0o644)
    shutil.copyfile(ROOT / build.DRIVER / 'run.sh', stage / 'run.sh'); (stage / 'run.sh').chmod(0o755)
    shutil.copyfile(out / 'source.json', stage / 'source.json')
    collect_notices(out, stage, commands)
    audit(out, stage, commands)
    (stage / 'README.txt').write_text('H700 integration diagnostic, RG40XX H / muOS 2601.0 Jacaranda. NOT device validated.\n'
        'Do not deploy/run without separate IG14 permission. No assets/cards/settings/saves included.\n'
        'Manual entry after permission: ./run.sh --run /absolute/fresh-test-directory\n'
        'Requires firmware SDL2 mali, EGL/GLES libmali, measured C/C++ runtime, POSIX sh and timeout supporting -s KILL.\n'
        'App deadline 60 s, external watchdog 90 s. Kernel/driver calls may require watchdog termination; that is FAIL, not release proof.\n'
        'Inspect audit.json/manifest.json, and verify firmware snapshot hashes before/after the permitted run.\n')
    manifest = dict(schema=1, source=json.loads((out / 'source.json').read_text()), image_id=build.IMAGE_ID,
                    inputs=json.loads((out / 'input-manifest.json').read_text()), scenario=SCENARIO,
                    toggles=dict(enable='H700 production mixed device request; no runtime overrides', disable='skip_validation,disable_robustness'),
                    device_status='pending', payload=payload(stage))
    build.write_json(stage / 'manifest.json', manifest)
    (stage / 'SHA256SUMS').write_text(''.join(build.sha256(p) + '  ' + p.relative_to(stage).as_posix() + '\n'
        for p in sorted(stage.rglob('*')) if p.is_file() and p.name != 'SHA256SUMS'))
    validate_package(stage)
    commands.run('staged-help', [stage / 'h700_integration_driver', '--help'], 10)
    commands.run('staged-version', [stage / 'h700_integration_driver', '--version'], 10)
    commands.run('launcher-syntax', ['sh', '-n', stage / 'run.sh'], 10)
    commands.run('launcher-help', [stage / 'run.sh', '--help'], 10)
    commands.run('launcher-version', [stage / 'run.sh', '--version'], 10)
    result = archive_package(stage, out / 'h700-integration-ig13.tar.gz')
    build.write_json(out / 'package-result.json', result)
    return result
