# H700 integration gate — IG‑13 local PASS / IG‑14 manual-run PASS с ограничениями

**Дата:** 2026-11-16. **Source revision:** `6a58db6e3a71dac843a02eb616cc8e149ec0f72c` (IG‑12 accepted).

**IG‑13: LOCAL READINESS PASS. IG‑14: последний корректный manual run — PASS bounded-сценария.** После назначения PortMaster пользователь подтвердил нормальный видимый запуск; последний сохранённый run завершил оба поколения с exit0. Из четырёх сохранённых ручных runs три остаются incomplete (`real_overlap=0`, exit1), не переименованы в PASS. Успех одного прогона не означает повторяемость4/4, готовность полной игры или30FPS.

Ни один runtime-критерий §5 не объявлялся выполненным только по локальной сборке IG-13. Subsequent actual device run и доказательства IG-14 приведены ниже; это не приёмка полной игры или FPS.

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

**На момент завершения IG‑13:** IG‑14 оставался STOP, package ещё не отправлялся. Parent повторил helper23/23 и сверил archive SHA-256; independent read-only review проверил все89 archive files и503 source hashes, blockers не нашёл. Последующие commit и device run не меняют provenance исходного архива.

## IG-14 — actual device run, 2026-11-16

**Разрешение:** пользователь явно предоставил приставку для IG-14, затем актуальный SSH endpoint с key authentication. Strict known-host verification и BatchMode включены, host-key bypass не использовался. Адрес и уникальный session path в Git не включены.

### Preflight / deploy

- Firmware: MustardOS2601.0 Jacaranda, AArch64/Linux4.9.170, Mali-G31. Игра/другая демка не обнаружены; работал `muxlaunch`. Разовый MemAvailable888276kB, SwapTotal0; `/tmp` available498296KiB. Это не memory/FPS budget полной игры.
- Проверены прежние6SONAME baselines: loader/libc/libstdc++/EGL/GLES/SDL2,5уникальных hashes (EGL/GLES оба Mali). Совпадение до run, после run и после cleanup.
- Firmware BusyBox1.36.1 `timeout -s KILL` проверен на безвредном sleep в отдельной session/process group. Исходная неотделённая SSH-команда завершилась signal/255; isolated `setsid` check вернул управление родителю. Сам IG-14 также запускался через `setsid`, с внешним90s watchdog и app60s.
- Archive SHA256 **тот же `43f31a20d4d31fb06ffaf4e1f083484d31710a300835cefca80e5d4f23bdaeb9`**. Firmware tar не поддерживает `-z`: первая распаковка отказала до graphics. Gzip снят на host штатным Python; тот же tar payload передан/проверен по hash и распакован обычным `tar -xf`. Затем все файловые `SHA256SUMS` и device help/version PASS. Binary, dependencies, acceptance/scenario не менялись.
- Установка только в свежем временном `/tmp` session directory, HOME/XDG/TMPDIR изолированы, core dumps OFF. Без Ports/game/assets/cards discovery, system settings/governors/libraries не менялись. Перед cleanup скачаны только наши results/logs.

### Программный результат: PASS

| Критерий | Actual evidence |
| --- | --- |
| Процесс / bounds | Один исходный run, exit0 самостоятельно; SSH+run около10.508s, watchdog не сработал. Два поколения в одном процессе |
| Поколение1 | 120/120frames,4cold fog variants,2permanent imports,120Presented, pressure4progress passes |
| EFB/native | EFB640×480 →320×240 на frame60; native640×480 и2imports остаются постоянными; поколение2 снова640×480 |
| Oracle | 2440RGBA samples в обоих stages (`native`/`after-blit`):976fog +1464corner/frame/ImGui. Независимый пересчёт §5.3: maximum error0LSB при допуске±1 дляfog, всеmarkers exact. Native=after-blit на всех точках |
| Actual overlap | Gen1 compiler/resource-texture/render-encoder интервалы на3разных threads пересекаются217866ns (~0.218ms), после barrier-leave. Barrier/cache waits не засчитаны; параллельное исполнение внутри libMali не заявляется |
| Pending normal stop | Gen2 frame0published/forward+reverse+swap complete, credit held; frame1accepted/unpublished, compiler-held/render-wait одного pipeline key. Stop до release compiler gate; затем actual compilation, render completion и Submit/Presented обоихframes |
| Context teardown | В каждом поколении observer trustworthy=1, все6context lifetimes destroyed: временная группа1context + вся Dawn share group5contexts. Успешный unbind/destroy; snapshot на Exports barrier до native cleanup. После первого teardown реально создано второе поколение |
| Validation/errors | Hardware OpenGLES/Mali, validation/robustness ON. Нет validation/EGL/GL errors/device loss/fail-stop. Найдены8slow-compile warnings,4dynamic-limit clamping messages и2unsupported-screensaver warnings: это не graphics/lifetime failures |
| Cleanup | Все test processes завершены, остался menu process; наша remote временная установка удалена и отсутствие проверено. Baseline firmware hashes неизменны |

Parent выполнил независимый CPU пересчёт сохранённых samples/intervals/context events; read-only reviewer отдельно проверил то же evidence, существенных расхождений не нашёл. TimestampQuery unsupported на этом device: profiler GPU maps этим run не проверены; lazy visibility-query/прочие optional effects не активировались. Runtime nod/kernel4.9, полная игра, input/audio/save-load и FPS остаются вне данного gate.

### Evidence / что ещё требуется

Ignored `build/h700-integration-ig14/`: `device-run.log`, `run-exit.json`, `technical-result.json`, `audit_evidence.py`, `firmware-{before,after-run,after-cleanup}.log`, payload/cleanup logs; `device-results/generation-{1,2}/{pixels,intervals,contexts,exports}.tsv`, gen2`stop.tsv`. Raw logs/private orchestration не публикуются. Device run не требовал production fixes/rebuild или повторного graphics запуска.

**Последующее сообщение пользователя: на экране всё время было меню ОС, демка не отображалась. Визуальная часть FAIL, полный IG-14 NOT PASS.** Native/post-blit readback не доказывает scanout на физическую панель. Прежний статус visual pending заменён этим фактическим наблюдением; программное evidence не удалено и не переименовано в полный PASS. Остальной перенос игры заблокирован.

## Повтор через меню muOS — launcher и результаты ручных запусков

Пользователь разрешил повтор либо оставить `.sh` для самостоятельного запуска. Выбран отдельный manual launcher, без изменений renderer/binaries и без второго SSH graphics run.

- Штатные firmware `frontend.sh`/`ext-general.sh` показывают: muOS вызывает Ports entry после выхода из menu-приложения и задаёт SDL display environment. Первый SSH-run происходил при активном menu process и чистом `env -i`, без этих SDL settings. **Это гипотеза причины, не доказанный диагноз и не обещание исправления.**
- На первой карте добавлены только собственные новые `ROMS/Ports/H700 Integration Test.sh` и скрытая `ROMS/Ports/.h700-integration-test/`. Существующий Metroid/другие порты и их assignments не изменены. Binary/SDL/payload — тот же IG-13 archive SHA256 `43f31a20d4d31fb06ffaf4e1f083484d31710a300835cefca80e5d4f23bdaeb9`; sidecar launcher не выдаётся за часть исходного package manifest.
- Entry source: `tools/h700_integration_driver/muos-launch.sh`. Запуск без аргументов из **Explore → SD1 → Ports → H700 Integration Test**. Если muOS запросит core, нужен штатный external/Ports shell handler, не emulator; автоматических изменений assignments нет.
- Launcher сохраняет только allowlist SDL settings штатного handler: rotation/scaler/blitter/assert/controller-file. Остальной driver environment чистый: own HOME/XDG/TMPDIR, без inherited LD/renderer/debug overrides. Atomic lock запрещает одновременные запуски. Перед каждым run проверяются firmware baseline и payload checksums; после — повтор firmware hashes, исходный exit code и отдельная пометка visual acceptance required.
- Исходный bounded scenario и app60s/watchdog90s не меняются; `setsid` отделяет timeout/driver от frontend. Не убивает menu, не правит framebuffer/sysfs/governors/firmware и не читает assets/cards. Выход0 сам по себе не закрывает physical-display criterion.
- Каждый run пишет свежие `ROMS/Ports/.h700-integration-test/runs/run.*/{launcher.log,driver.log,driver.exit,result.txt,firmware-after.log,results/}`; `latest-run.txt` указывает последний. Старые результаты не удаляются. Установка оставлена по прямому запросу пользователя; это не забытый cleanup первого временного run.
- Проверки: **7/7 новых CPU launcher tests и30/30 всех helper tests PASS** в прежнем offline ARM64 image; только shell stand-ins, не Aurora/GPU. На device проверены transfer/payload/baseline hashes, `sh -n`, help и отсутствие created run/lock на момент установки. Read-only review blockers не нашёл. Ignored installation evidence: `build/h700-integration-ig14/manual/`, без endpoint/session details в Git.
- Удаление после приёмки: убрать только наш `H700 Integration Test.sh` и `.h700-integration-test/` после получения logs и проверки отсутствия активного diagnostic процесса. Сейчас они намеренно остаются для ручного запуска. На момент установки graphics не запускался агентом; последующие ручные наблюдения и logs приведены ниже.

### Ручная проверка logs и уточнение пользователя

Пользователь сообщил, что первый раз запускал script без выбранного PortMaster, затем после назначения всё было нормально. Этот initial frontend invocation не следует считать проверкой корректного штатного launcher path. При этом **три сохранённых exit1 не приписываются автоматически неверному core**: в каждом есть120rendered/Presented frames и2400successful native/post-blit samples; их конкретная причина неполного сценария — отсутствие тройного actual API overlap. Сопоставление каждого такого run с действиями по выбору core не установлено.

| Сохранённый manual run | Итог |
| --- | --- |
| Последний, по `latest-run.txt` | exit0; gen1=120frames,4coldvariants,2imports,pressure4passes,real_overlap1; gen2=2frames,2imports,normal pending-stop завершён;2440samples PASS |
| Остальные3, включая run со скриншотом | exit1 после gen1:120frames/Presented,4coldvariants,2imports,pressure4,pixels_ok1, но real_overlap0. Gen2 не запускался, эти runs **не PASS** |

- Parent прочитал `driver.log`/`result.txt` и TSV; read-only reviewer отдельно проверил все4runs. В последнем actual3-thread API overlap **271947ns (~0.272ms)** после barrier-leave. Независимый pixel oracle —0LSB error, все markers exact; pending-stop того же compiler/render pipeline key и destruction6context lifetimes(1temporary+5Dawn) каждого поколения подтверждены.
- Во всех4runs нет validation/EGL/GL errors/device-loss/timeout; firmware-after=unchanged. Остались известные slow-compilation/dynamic-limit/screensaver warnings. После завершения test process отсутствует, lock released, menu process работает. Запуски выполнял пользователь вручную; агент не делал новую graphics-серию для получения PASS.
- По прямой срочной просьбе пользователя `fbgrab` снял demo с framebuffer, PNG скачан в ignored `build/h700-integration-ig14/manual/screen.png`. Это уже не меню ОС. Снимок не является дополнительным полным pixel oracle: на нём не видны все expected markers (верхний левый red/нижний центральный frame marker), причина отличия от GL/native samples не установлена. **Pixel-perfect физический scanout по одному `fbgrab` не заявляется.** Пользователь сообщил, что после правильного PortMaster запуска всё нормально; evidence screenshot сохраняется без ретуши/подмены.
- Таким образом есть **один полный технический manual PASS + пользовательское подтверждение видимого штатного запуска**, но не4/4PASS и не гарантия детерминированного overlap. Исходный SSH physical-display failure остаётся историческим фактом, не доказывающим неработоспособность корректного Ports path. Повторные запуски/ослабление overlap criterion автоматически не выполняются.
- Скачанные raw manual logs: ignored `build/h700-integration-ig14/manual/user-runs/`; очищенные `manual-runs-summary.json`/`latest-audit-summary.json`. Unique run paths/endpoint в Git не включены. Диагностическая установка остаётся по прежнему прямому запросу; после неё нужен отдельно согласованный план подключения полной игры.
