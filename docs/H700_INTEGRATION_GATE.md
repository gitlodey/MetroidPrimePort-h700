# H700 integration gate — IG‑13 local readiness / IG‑14 pending

**Дата:** 2026-11-16. **Source revision:** `6a58db6e3a71dac843a02eb616cc8e149ec0f72c` (IG‑12 accepted).

**IG‑13: LOCAL READINESS PASS. IG‑14: PENDING / STOP — отдельного разрешения на device/deploy/run нет.**

Ни один runtime-критерий §5 интеграционного плана не объявляется выполненным по этой сборке. Valid graphics `--run`, GPU/backend initialization и обращения к устройству не выполнялись.

## Готовый artifact

- **Archive:** `build/h700-integration-ig13-final/h700-integration-ig13.tar.gz`
- **SHA‑256:** `43f31a20d4d31fb06ffaf4e1f083484d31710a300835cefca80e5d4f23bdaeb9`
- **Размер:** 23 237 286 bytes, около 22.16 MiB.
- Внутри — `H700Integration/`: настоящий executable, `lib/libSDL3.so.0`, explicit `run.sh`, manifest/source metadata, ABI/provider audit, checksums и 81 notice/provenance file. Всего 87 payload files плюс manifest и SHA256SUMS.
- Только два ELF payloads: executable и SDL. Нет container libc/libstdc++/EGL/GLES, assets, ISO, settings, saves или test executables. Нет symlinks, privileged/group/world-writable file modes.
- Executable RUNPATH **ровно `$ORIGIN/lib`**; bundled SDL — `libSDL3.so.0`. Реальные staged help/version успешно загружаются без graphics.

## Source/build provenance

Manifest хранит source revision **отдельно** от фактически собранных uncommitted packaging/build inputs:

- Aggregate source SHA‑256: `8479967f51fa5ad6e0a6635b7c1314c70d3207e0db0509676f804f25ca353e86`.
- 503 build-relevant source/metadata files, включая 37 dirty/new own files; per-file hashes и список — в `package/source.json` и `package/manifest.json`.
- `.DS_Store`, build outputs и этот отчёт не являются build inputs. Отчёт финализирован после packaging; чистый checkout на момент сборки не заявляется. Затем parent reviewed результат и зафиксировал/отправил IG-13; baseline/hash manifest архива намеренно не переписывается задним числом.
- Image: `sha256:2db4439763783aa796da4268dbd91847ee2c13deef4c8ce2439e0f6bdf926962`, native Linux ARM64, Clang16.0.6, Cortex-A53, signed char, RelWithDebInfo.
- Все 14 исходных archive hashes проверены до source overrides/extraction. Source inputs распакованы заново в fresh output; старые build objects/executable/SDL не копировались. Закреплённые prebuilt Dawn/nod остаются разрешёнными package inputs, не пересобирались.
- Build: `--pull=never --network=none --read-only`, `/src:ro`, только новый `/out:rw`. Без installs, Rust, downloads или image/Dawn rebuild внутри build.

### Воспроизведение

```sh
python3 -B tools/h700_integration_driver/build.py --package --output build/h700-integration-ig13-final --jobs 2
```

Указанный output уже занят: повтор требует **другого нового** `build/...` path. Existing outputs и symlink ancestors отклоняются, ничего автоматически не удаляется.

**Измерено отдельно:** clean configure 18.852 s; clean actual target + explicit CPU targets build 210.160 s; сумма 229.012 s. В build log — 638 C/C++ compile steps. Это не время только core/GX и не FPS measurement.

## ABI/provider audit и пределы доказательности

Повторно используются существующие `verify_environment`, `elf_info`, `firmware_gate`; legacy probe build/production-hash gate не запускается. Image runtime сверяется с его `/opt/h700-toolchain/abi.json`.

- Проверены ELF64/AArch64, loader `/lib/ld-linux-aarch64.so.1`, реальный NEEDED/version-node набор executable/DSO, local RUNPATH и providers из actual CMake report.
- Executable NEEDED: EGL, GLESv2, bundled SDL3, libm, libstdc++, libgcc_s, libc. SDL NEEDED: libm, libc.
- Максимальные требования executable: GLIBC 2.38, GLIBCXX 3.4.32, CXXABI 1.3.11; SDL: GLIBC 2.38. Проверялись конкретные nodes, не только максимумы.
- Все **153 SDL imports** сопоставлены с actual bundled DSO exports. Пять selected exact firmware exports проверяет существующий verifier.
- По measured `docs/H700_RUNTIME_ABI.json` проверена цепочка **10 SONAME entries**, включая dlopen firmware SDL2, EGL/GLES→Mali и loader. Hashes/paths/NEEDED/nodes сохранены в `package/audit.json`; host `ldd` не использовался.
- **Ограничение:** snapshot не содержит полного набора exact firmware exports. Для остальных imports результат — provider/version-node coverage, не проверка каждого symbol. У транзитивных firmware entries required nodes плоские: проверяется их наличие у фактических NEEDED providers, не восстанавливается отсутствующая per-symbol association.
- Не проверены actual device loader, Mali/WGSL/pixels/fences/concurrency/context destruction, plugins или kernel4.9 fallbacks. Ни наличие Vulkan code в prebuilt Dawn, ни CPU tests не разрешают fallback с OpenGLES.

## Notices: устранение исторического preflight gap

Первоначальный preflight верно установил: cached Dawn package не содержит notices, project `dawn.txt` сам не описывает transitive scope, а real executable содержит SPIRV‑Tools. Evidence сохранено в `build/h700-integration-ig13-preflight/`.

**Исправление прежней трактовки:** offline restriction относится к **build**, а не запрещает получать primary documentation/license texts для тех же pinned компонентов. Документы получены на host; новые dependency binaries, build sources для компиляции или версии не добавлялись.

- Exact Dawn mirror `Timdeuces/dawn@118436b3f8f68583eb86e811ca605dc41637e8cc`: DEPS, Linux release workflow/CMake/fetch metadata.
- Matching primary LICENSE/attribution для SPIRV‑Tools `5b5a25231a3543559b4f2cb27fe725a08b76b5d0`, SPIRV‑Headers `942fe4b988359a0750b79f0ae7ed735994d3147d`, Vulkan headers/utility, Dawn Abseil и Chromium raw_ptr; Khronos registry attributions. Pins, URLs, hashes, extraction rules и root NOTICE listings — `packaging/h700-notices/`.
- Existing full `tools/collect_licenses.sh` сохранён и дополнен optional verified source-overrides root. Собраны source dependency и nested notices; FreeType FTL включена отдельно.
- Actual ImGui STB license blocks и default-font attribution извлечены из pinned sources. Full ProggyClean MIT grant получен из primary author's repository; copyright совпадает с embedded font attribution. SQLite public-domain declaration извлечена из actual amalgamation.
- `scope.txt` и `licenses/coverage.json` отделяют runtime/header coverage, metadata/tool-only dependencies и консервативные extras исходного collector. Generic grants не подставлялись вместо missing provenance. Это bounded source/build metadata + symbols audit, не юридическое заключение и не реконструкция всех флагов prebuilt binary.

## Сценарий и launcher

Manifest фиксирует `h700-mixed-v1`: hardware OpenGLES Compatibility без fallback, validation/robustness ON, Nearest/sharpness0, 120 frames gen1, 2 frames gen2 (плановый максимум8), четыре cold fog variants, native RGBA8Unorm640×480, два imports на generation, EFB resize на frame60 и pressure4 progress passes. Accepted pending work/drain semantics не менялись.

`run.sh` не делает auto-discovery: требуется explicit `--run /absolute/fresh-test-directory`; existing/root/relative paths и loader overrides отклоняются. Передача SDL environment фиксирована. **App deadline60 s + external `timeout -s KILL 90`** на весь процесс/сценарий. Ненулевой driver exit сохраняется; watchdog termination — failure, не proof of release.

Runtime prerequisites: firmware из measured snapshot, POSIX sh и `timeout`, поддерживающий `-s KILL`. Linux CPU case проверил kill stalled dummy process, launcher case — точные аргументы90 и propagation через shell stand-in. Это не подтверждение наличия utility на текущей firmware. После отдельного разрешения перед graphics запуском нужны проверка utility, firmware hashes до/после и IG‑14 процедура из плана. Launcher не запускался с valid graphics driver arguments.

## Actual checks / evidence

Final output `build/h700-integration-ig13-final/`:

- `host-commands.json`, `commands.json`, `environment.json`, `packages.tsv`, `input-manifest.json`, `source.json`.
- `configure.log`, `build.log`, `cpu-tests.log` (**108/108**, включая19 scene cases), `fifo-tests.log` (**262/262**), `helper-tests.log` (**23/23**, без skip).
- `staged-{executable,sdl}-elf.log`, `linked-symbols.log`, `collect-licenses.log`, `staged-{help,version}.log`, `launcher-{syntax,help,version}.log`.
- `package/manifest.json`, `package/SHA256SUMS`, `package/audit.json`, `package/licenses/coverage.json`, `package-result.json`, `result.json` (`local-readiness-complete`, device_validated=false).

Все 20 container commands и три host commands завершились успешно. Package tests проверяют tampering/missing/extra files, unsafe paths/modes, exact archive content/hashes, fresh output, notice hashes, revision vs dirty-source bytes и запрет подменить pending на device PASS.

История исправлений не скрыта: первый helper invocation обнаружил Python module-name collision с legacy `build.py` (исправлено до сборки); первый clean output прошёл build/audit/CPU, но helper mode-test предполагал сохранение setuid на Docker bind filesystem. Production bit predicate теперь тестируется напрямую, filesystem case использует world-writable mode. Полный fresh build повторён в final output; первый output не является принятым readiness artifact.

**IG‑14 остаётся STOP до отдельного разрешения. Пакет не отправлялся; ни устройство, ни firmware/user data не затрагивались. Parent повторил helper23/23 и сверил archive SHA-256; independent read-only review проверил все89 archive files и503 source hashes, blockers не нашёл. Последующая фиксация IG-13 не меняет provenance уже собранного архива.**
