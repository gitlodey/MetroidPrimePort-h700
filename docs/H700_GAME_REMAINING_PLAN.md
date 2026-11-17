# H700 — пересмотр всего остатка GM-12…GM-21

**Редакция 2, 2026-11-17 — ПРЕДЛОЖЕНИЕ НА СОГЛАСОВАНИЕ, не разрешение реализации.** Подготовлена по запросу пользователя пересмотреть остаток целиком, а не согласовывать расширения по одной карточке. [Редакция 1 и история результатов](H700_GAME_PLAN.md) сохраняются. После одобрения этот документ заменяет её оставшиеся карточки GM-12…GM-21 и связанные процедуры; завершённые шаги не переоткрываются. До одобрения production work остановлена. Device access этим документом не предоставляется.

Цель неизменна: воспроизводимо собрать и запустить **Metroid Prime Unbound v0.46.0 на ANBERNIC RG40XX H / H700, muOS 2601.0 Jacaranda** с корректными graphics/UI/input/audio и настоящим CARD save/load. **30 FPS — отдельный следующий этап.** [Короткая версия](H700_GAME_SUMMARY.md), [прогресс](H700_PROGRESS.md), [правила](H700_WORKFLOW.md).

## 1. Исходная точка и почему пересматриваем весь путь

GM-01…GM-10, GM-11a и GM-11b завершены локально и отправлены. Последняя production-итерация — `b9b1545fe`: root terminal cleanup, actual DVD callback/DMA lifetime, pre-throw room/resident guards, no-command CFDk только на held CPU checkpoint. Её evidence: game build,21/21CPU/no-init/DVD,269/269FIFO,53/53render/lifecycle, non-H700 core/GX и5changed game TU compile PASS. Это **не запуск новой игры на устройстве**. IG-demo не заменяет game acceptance.

Прежний план недостаточно явно оценивал не алгоритмы, а **сквозные связи**: failure producer → consumer → dispatch → root → lifetime; executable → persisted INI → physical card path; fresh build → настоящий game package → immutable RC → последовательность device-процессов. Поэтому исправление только GM-12 снова оставило бы скрытую работу дальше.

В этом пересмотре выполнены три независимых read-only проверки: GM-12/13, GM-14…17, GM-18…21. Их выводы сопоставлены с фактическими файлами. Новые build/test/device runs не выполнялись; новая production-реализация не начиналась.

### Подтверждённые основания решений

Все пути ниже относятся к этому checkout; номера строк — ориентир на момент пересмотра, не вечный контракт.

| Факт исходников | Следствие для остатка плана |
| --- | --- |
| `DolphinCSkinnedModel.cpp`: handshake внутри `AddSkinnedRef`, unchecked arithmetic в нескольких allocation paths, void `Calculate`, null-workspace draw fallback | Нужны root init, checked layout и замкнутый status-path; один early return не исправление |
| `CAnimData`, `CModelData`, `CProjectedShadow`, три swarm consumers и renderer/IOWin dispatch продолжают работу после void calls | Эти конкретные consumers и границы обхода входят в scope заранее; virtual/C callback ABI не меняем |
| `main.cpp:816–838`: boot PAK loop ещё внутри `PostInitialize`, root safe-point после него не достигнут | Нужен status return через boot loop/PostInitialize, без deep throw и без создания renderer после failure |
| `CMemory.cpp:72–83`: PC allocation может бросать `std::bad_alloc`; SkinPool jobs используют caller inputs/stack/output | Null checks недостаточны; узкий skin allocation failure переводится в status, jobs нельзя покидать до завершения |
| `platform/shims.cpp:48–59`, `sdk_stubs.cpp:195–213`: breakpoint/retrace pulse синхронный | Не добавляем hypothetical breakpoint scheduler/wait mechanism. Retrace pulse всё равно не FIFO/render completion |
| `DolphinCGraphics.cpp:891–930`: normal frame tail содержит GX/UI work и delayed frees после void frame end | Root должен обработать failure до обычного tail; известный failed frame не разрешает delayed free |
| `port_paths.h:175–238,242–327`: explicit user path и CARD path различны; CARD вызывает loose-data migration | Изоляция обеспечивается физическим layout/preflight, не одним env и не hash-проверкой чужой карты |
| `debug_ui.cpp:459–573`: defaults → INI → env; `DolphinCMemoryCardSys.cpp:424–451`: sharing может переключить card base | Нужна явная acceptance-profile validation; не молчаливая перезапись INI |
| `platform/main.cpp:1023–1028`: pending import/room scanning раньше графики; `port_mods.cpp:352–384`: внешний `MP_MODS` | Path/env/INI policy должна сработать до import/discovery, не только перед game owners |
| `tools/h700_game/build.py`: build wrapper; bootstrap запрещает его `--package`, cache выключает tests | Нужен настоящий game test/package mode, а не переименование diagnostic payload |
| Integration `package.py:30–48`: source digest не охватывает всю игру; local RUNPATH задан только integration target | Game source closure и H700-only `$ORIGIN/lib` link/staging policy — явная работа до freeze |
| `NOTICE:40–76`, `collect_licenses.sh`: full-game inventory шире slice notices; codecs имеют attribution, не полные grants в headers | Сохраняем collector и дополняем matching grants по фактическому full-game составу |
| Demo launcher имеет kill watchdog, fresh per-run HOME/cache и shell-exit lock cleanup | Для save-capable игры нужны иные child/lock/log lifetimes, без автоматического kill |
| `debug_ui/tabs.cpp:43–79`: Restart не сохраняет, Exit предупреждает о потере прогресса | В B сначала station save → новый процесс → load, затем reset |

Это findings применимого H700-пути, не приглашение исследовать все upstream features/платформы.

## 2. Что сохраняется и что нужно согласовать одним пакетом

### Сохраняем без повторного выбора

- D1/D2: exact ARM64 image `sha256:2db4439763783aa796da4268dbd91847ee2c13deef4c8ce2439e0f6bdf926962`,14 hashed inputs, offline/read-only build, pinned Dawn/nod/SDL. Ни apt/Rust/new image, ни Dawn rebuild.
- SDL3-over-SDL2 → firmware SDL2 mali → EGL/GLES, main presenter и настоящие workers; namespace `h700-mixed-v1`.
- D3…D8: retail/offline profile, validation/robustness,640×480/4:3, MEM1 24MiB, unpinned scale1, ring32MiB, lazy/no-seed, pressure192/128MiB, sampled gate96MiB, штатные effects, Modern/twin-stick, cap30/sim60 без FPS-обещания.
- D9: root-only typed transport после actual CPU checkpoint, no-new-graphics cleanup после seal, Unknown/hang удерживает owners; DVD/DMA waits GM-11b сохраняются.
- D10/D11: отдельная физическая установка, своя card/user/cache без migration и чужих данных; отдельные разрешения device-окон. Старую установку, firmware/services/clocks/swap/mixer не трогаем.

### Предлагаемое общее разрешение редакции 2

| Решение | Явная граница |
| --- | --- |
| **R1. Расширенный scope GM-12/13 и безопасная разбивка ниже** | Конкретные skin/boot/frame consumers и dispatch seams; не universal exception framework, не переписывание scheduler, не deep throw |
| **R2. H700 acceptance-profile validation для persisted settings и опасных live toggles** | Несовместимая настройка отвергается с причиной до её внешних side effects; INI не переписывается. Non-H700 и разрешённые bindings/language/обычные volume controls сохраняются |
| **R3. Game-specific launcher/package/test/provenance и bounded logging** | Reuse существующих helpers; узкие H700 link/log/preflight изменения разрешены в перечисленных карточках. Не перенос demo watchdog и не новая build-система |
| **R4. Same-image локальные correction builds после device findings** | Расширить D1 на bounded correction/revalidation этой приёмки с теми же pins/image/options policy. Не бессрочное разрешение на любые новые задачи |
| **R5. Один permission request на конечную последовательность процессов каждого окна** | В частности B включает save-run и новый load-run. Это предложение модели согласия, не само разрешение запуска/SSH/deploy |
| **R6. Локальные узкие fixes внутри уже выбранного решения** | Карточка с конкретным failing evidence, regression, commit/push и новым RC; не новое approval каждой механической правки. Архитектура/границы безопасности/критерии/новый доступ по-прежнему требуют остановки |

Нужен **один ответ на редакцию 2 целиком**, а не ответы перед каждым подшагом. Диск, install path, scene access и device-разрешения запрашиваются позже в своих реальных границах.

## 3. Контракты, общие для всех оставшихся карточек

### 3.1. Failure status и lifetime

Один sticky H700 main-owned game-failure latch: первая причина/статический site, без allocation/throw/reset при architecture reset. Он **не** lifecycle machine, CPU checkpoint или terminal seal. Invalid layout/skin allocation failure может возникнуть при healthy Aurora, поэтому root входит в существующий `PortH700PrepareRootUnwind`, если **latch установлен ИЛИ backend unhealthy**; одной backend health недостаточно.

Непосредственно потребляемые результаты skin/pose/setup/workspace становятся status-bearing в невиртуальных H700 APIs. Через virtual/C callbacks не протягиваем новый ABI: на конкретных dispatch boundaries проверяем latch до/после callback и прекращаем следующий callback/pass/message. Ошибка не разрешает удалить текущий IOWin до root boundary. Failed shadow не копируется/не публикуется; partial workspace arrays/masks не коммитятся.

Вошедший глубокий scope сначала завершает необходимое безопасное восстановление и возвращается, **потом root** начинает CPU quiescence/seal. Не обещаем «ни одного GX с момента latch»: запрет новой graphics work относится к cleanup **после checkpoint/seal**. Не используем silent unskinned fallback при failure.

Root выполняет первый skin handshake после subsystem init и до PostInitialize/renderer construction. Constructor не делает скрытый blocking handshake и не бросает terminal token; refcount остаётся сбалансированным у inert failed object. Checked layout общий для Construct/Calculate/ring/external workspace: signed counts, widened products, alignment/sums/narrowing; ring-only capacity check с прежним дополнительным32bytes. Heap/external workspace не ограничивается ring-capacity произвольно. Известные skin allocation/metadata `bad_alloc` переводятся в status до write-gather/jobs, не catch-all всего engine OOM.

Wait progress — существующий main progress, вне locks, без game reentry; success только по actual FIFO token, wrap/prefix reclaim сохраняются. `TickAllocations` остаётся неблокирующим reclaim. SkinPool `Run` возвращает только после последнего доступа всех jobs к inputs/output/stack; ни latch, ни graphics checkpoint не позволяют ранний return.

Root checks нужны после boot/tick/draw/loader calls, **до** debug/loader/normal EndScene continuation. При failure внутри frame end статус проходит `CGraphics::EndScene → CCubeRenderer::EndScene → RsMain`: нижний tail не выполняет delayed free, верхний — reflection texture retirement/age update, root — success accounting. Сохраняются необходимый synchronous compatibility bookkeeping и allocations до terminal boundary. Синхронный retrace не подменяет completion. Новый механизм ожидания breakpoint-loop не входит в scope без отдельного evidence применимости.

**Не обещаем:** обработку всех C++ exceptions, arbitrary constructor/thread/job OOM, прерывание synchronous driver/DVD/DMA call, live recovery или GPU completion после device loss. Прежний generic exception path не переименовывается в безопасный transport задним числом.

### 3.2. Физическая установка и effective settings

Generic RC payload содержит executable, `lib/`, licenses, manifest и launcher template. **Не содержит** user/settings/cards/mods/cache/disc path. Установка создаёт собственные writable `user/`, постоянные HOME/config/cache и временный/run-log каталог; `MP_USER_PATH=<physical-exe>/user/`, cache остаётся внутри той же установки. Конкретные private paths задаются локально при установке, не зашиваются в Git/archive.

Preflight до child проверяет regular physical executable/payload, собственные writable directories и отсутствие loose legacy data/redirected links к другой установке. Системный alias пути монтирования можно канонизировать; нельзя объявлять любой firmware symlink дефектом. Наши payload/user/cache не должны становиться ссылками/hardlinks на старый install. Нет migration, autobackup/scan чужих каталогов или удаления contamination ради продолжения: понятный отказ.

Protected profile из D4…D8 плюс share/quick-resume OFF и acceptance logging ON проверяется **до вызова side-effectful setting readers/apply функций**, включая duplicate/legacy keys. Текущий parser применяет часть эффектов уже во время чтения, поэтому одного check после `LoadSettings`/`EnsureInitialized` недостаточно. Выбираем узкий H700 prevalidation/apply boundary: сначала проверить protected assignments без effects, затем разрешённое применение и final effective-profile check после разрешённых env. Import/mod/card discovery, services и persistence/atexit-save не разрешены до startup validation. Активные external-card/mod/seed/import paths запрещены.

Startup mismatch → nonzero/key/reason до graphics/owners, без перезаписи INI. Live incompatible request → отказ с причиной **без изменения state/dirty/INI/effects**, игра продолжает работать: никакого exit/throw из UI callback. В scope входят конкретные direct widgets/mutations и discovery/actions **до** setter, а не только функции `SetX`; допустимы H700 disabled/guarded protected controls с backend guard. Обычные bindings/language/game volume controls сохраняются, gain/renderer/pacing не становятся overrides. Parser/serializer selftests по-прежнему вправе применять upstream sample values отдельно от acceptance validator. Это не общий rewrite parser и не sandbox против внешнего редактирования/filesystem races.

### 3.3. Launcher environment, child и логи

- Всегда один explicit absolute readable regular disc argument, quoted буквально; нет `eval`, glob, MP_DISC/remembered-path fallback, autodiscovery либо retry без аргумента.
- Две video env только `sdl2`/`mali`; сохраняются ровно frontend `SDL_ROTATION`, `SDL_HQ_SCALER`, `SDL_BLITTER_DISABLED`, `SDL_ASSERT`, `SDL_GAMECONTROLLERCONFIG_FILE`, с различением unset/empty.
- До env sanitization отвергаются реальные renderer/presence-pin conflicts из H700 policy (включая empty presence pins), loader injection `LD_PRELOAD`/`LD_AUDIT` и внешние path redirects. Inherited `LD_LIBRARY_PATH` не наследуется: используется audited local RUNPATH. Остальные неразрешённые MP/AURORA gameplay/debug overrides удаляются с сообщением о категории, без публикации значений.
- Сохраняются заданные audio-routing env: `SDL_AUDIODRIVER`, `SDL_AUDIO_DRIVER`, `PIPEWIRE_RUNTIME_DIR`, `PIPEWIRE_REMOTE`, `XDG_RUNTIME_DIR`, `PULSE_SERVER`, `PULSE_SINK`, `ALSA_CONFIG_PATH`, `ALSA_CARD`, `AUDIODEV`. Это сохранение caller route, не утверждение, что firmware использует каждый ключ. Explicit dummy/disk audio в acceptance отвергается. PipeWire `/run` default только при socket и **unset**, не поверх empty/заданного значения. Не задаём новый driver/gain/mixer route.
- Lock принадлежит установке, не всей системе. Сигнал launcher не означает смерть child и не удаляет lock живой/неизвестной игры. **Game termination/reap и log-copy finalization — разные milestones.** Child exit code записывается после reap, complete run result и освобождение installation lock — после подтверждённого drain/close copier. Unknown/retained — не exit0/не permission второго запуска. Stale lock не убирается автоматически по одному `kill -0`/PID.
- Нет `timeout -s KILL`, kill по времени/памяти/размеру лога, остановки frontend или глобального `ulimit -f`. Core OFF. Ограничение stdout/log — не закрытие pipe, вызывающее SIGPIPE, и не прекращение drain.
- Log policy: existing H700 log-copy worker пишет в **уникальный per-run sink**, два сегмента по8MiB и finite summary/status. Нет второго растущего `driver.log` из terminal duplicate; bounded startup rejection reason/launcher metadata — отдельная запись до16KiB (при unwritable storage — best-effort stderr/nonzero, не PASS).
- Bounded sink обязателен/подтверждён до graphics-нагрузки: `logging=0` несовместим с acceptance profile, initial sink/ready failure отвергает запуск до owners. Copier публикует per-run ready, sticky health/error/loss flags и final drain/close acknowledgement после EOF; game не ждёт EOF, удерживая свои pipe writers. Launcher использует acknowledgement для finalization, не убивает copier по timeout. Неизвестное завершение оставляет incomplete result/lock и запрещает reuse этого run/sink.
- При cap продолжаются drain/rolling без влияния на CARD/child; write/rotation/status failure не убивает игру. Missing acknowledgement/health или overwritten raw evidence не позволяют утверждать отсутствие errors: gate UNKNOWN/PARTIAL. Маленький bounded startup/effective-profile + existing cumulative RAM minimum/incomplete summary сохраняется отдельно; он не превращает потерянный error history в PASS. Runtime observer видит loss/failure и останавливает продвижение маршрута безопасно.
- Private raw logs не входят в RC/Git; очистка старых runs — только собственных, после game и copier finalization, сохраняя выбранные acceptance records.

### 3.4. Freeze, payload и доказательность

Game mode использует прежний verified staging/offline runner, но имеет собственные test inventory/payload/scenario. Перед package включены MP_BUILD_TESTS/BUILD_TESTING/AURORA_ENABLE_TESTS; no-init entries отделены от настоящего game execution. Не публиковать будущий CLI как существующий.

Source closure охватывает root CMake/files/version inputs, game src/include, platform, MusyX/Aurora/astcenc/прочие реально используемые vendored inputs, embedded resources/generators, game/shared build/package helpers и notices. Hash фактических bytes, не только HEAD; отрицательные fixtures меняют digest при правке каждого класса inputs. Raw host paths/commands не пакуются. Archive manifest строгий, assets/saves/secrets/лишние files запрещены.

H700-only staged executable использует local `$ORIGIN/lib` RUNPATH; SDL — из того же pin, не container SDL/EGL/GLES/SDL2/libmali. Audit actual staged ELF + SDL: architecture/interpreter/SONAME/NEEDED/providers/version nodes, exact bundled imports, полный undefined inventory с категориями exact checked / provider-version covered / weak-optional / missing exact evidence. Missing provider/node — blocker; неполный exact firmware snapshot — явно unknown, не доказанный loader failure и не runtime PASS.

Notices: unmodified collector как основа, existing pinned Dawn primary texts, full-game link inventory и matching grants/attributions compiled codecs/embedded components. Mods OFF не исключает compiled component из состава. Не искать лицензии всех неиспользуемых upstream features. Missing primary text → конкретный запрос материала/доступа, не выдуманный grant. NOTICE не утверждает разрешения публичной дистрибуции game/SDK code; результат — локальный correctness RC.

Freeze фиксирует build-relevant bytes до первого qualifying fresh build A; read-only bind mount редактируемого checkout сам по себе не freeze. Fresh B — из того же freeze, в другом output, независимое staging. Inputs/config/provider/source manifests должны совпасть; разные timings/output paths в logs не binary metadata. ELF/archive hashes сравнить, различия объяснить фактами. Воспроизводимый recipe не называется bit-identical без совпадения bytes. Выбирается один immutable RC; после приёмки binary не пересобирается под прежним названием.

## 4. Пересмотренные локальные карточки

Для каждой: отдельные meaningful checks → docs/progress → commit/push. Оценка **1–2 часа активной работы**, builds/очередь/доступы отдельно. Откат — только собственный diff/предыдущий immutable payload, не reset чужой работы. Подготовительные commits не дают partial artifact статус RC. Новые APIs/paths ниже — проектируемые, не уже существующие.

### GM-12a — sizing/failure/wait policy без включения producers

- **Риск/результат:** зафиксировать единые arithmetic/status/token контракты §3.1 до расширения call sites; не скрывать overflow за int.
- **Зависит от:** GM-11b. **Scope:** узкий H700 policy header/module + CPU tests; без изменения ring capacity, completion или SkinPool.
- **Решение/приёмка:** checked layout ring/heap/external, sticky first reason, pending/success/failure wait semantics; bounds/alignment/narrowing и wrap cases. Actual FIFO token ordering проверяется существующим FIFO seam, не immediate-token echo.
- **Безопасный commit:** новые production failure producers ещё не активны. Документы различают scaffolding и закрытый GM-12; actual game linked-tests продолжают проходить.

### GM-12b — непосредственные consumer status seams

- **Риск/результат:** failed workspace не используется и не превращается в unskinned draw/shadow success.
- **Зависит от:**12a. **Scope:** `CSkinnedModel`/`CAnimData` невиртуальные H700 APIs; `CModelData` normal/frozen/thermal/flat/disintegrate/multi branches; `CProjectedShadow`; `CFishCloud`, `CSnakeWeedSwarm`, `CWallCrawlerSwarm` allocation/render consumers и headers.
- **Решение:** явный status, пары workspace arrays/masks публикуются лишь после успеха; failed shadow не копируется/не enabled. Все Draw overloads отвергают latched failure, но сохраняют штатный single-bone/null fallback без ошибки.
- **Проверка/приёмка:** injected failed result до publication, no draw/copy/callback/mask commit; partial arrays безопасны для destructor. Production-linked tests + source bindings реальных consumers.
- **Безопасный commit:** до12d **ни один** новый skin producer (sizing, constructor/heap/metadata, external workspace, wait) не активен в ordinary execution. Подготовительные APIs/consumer checks сохраняют прежний control flow; failure injection — только в tests. Не включать producer раньше root.

### GM-12c — dispatch, boot/frame и root closure

- **Риск/результат:** ошибка не теряется в void dispatch, не вызывает очередной pass/message или owner destruction раньше checkpoint.
- **Зависит от:**12b. **Scope:** `CCubeRenderer`, `CStateManager` pre/render dispatch, `CMFGame`, `CIOWinManager`, main tick/root; `AddPaksAndFactories`/`PostInitialize`; `DolphinCGraphics` frame-tail negative gate.
- **Решение:** latch checks до/после конкретных callbacks/loops, root учитывает latch **или** backend failure до debug/loader/EndScene; boot status прекращает initialization до renderer после failure. Frame failure проходит через оба EndScene layers, без delayed flush/reflection retirement/success accounting. Существующий GM-11b transport/guards, без смены virtual/C ABI или deep token.
- **Проверка/приёмка:** нет следующего consumer/message/remove-owner/pass, failed EndScene tail не flush-ит delayed allocations; entered restore scopes возвращаются до seal. Shim retrace остаётся compatibility bookkeeping, не completion proof.
- **Безопасный commit:** новые skin producers dormant до12d. Обработка уже существующего backend failure в boot/frame может включаться здесь, но только целиком до root/GM-11b transport в том же commit; не частично активный cleanup.

### GM-12d — единое включение skin producers

- **Риск/результат:** init/ring wait и allocation failure реально достигают root без invalid use.
- **Зависит от:**12a…c. **Scope:** `DolphinCSkinnedModel`, root handshake и выбранные skin allocation/metadata seams; SkinPool scheduler не меняется.
- **Решение:** checked layout во всех allocation paths, balanced inert constructors, narrow `bad_alloc` translation; main progress/health вне locks, true ordered markers, unchanged ring/wrap/reclaim. После начатых skin jobs — actual Run completion/restore перед status return.
- **Проверка/готово:** init pending/completed/failure, impossible size/heap failure до pointer publication, actual token completion/wrap/prefix reclaim, running job last-access lifetime. Все producer→root paths замкнуты; GM-12 закрывается только теперь. No GPU/device claims.

### GM-13a — независимый сквозной safety gate

- **Риск/результат:** не принять набор локальных helpers за безопасную интеграцию.
- **Зависит от:**12d. **Scope:** relevant production-linked tests и source wiring: skin→consumers→dispatch→root→GM-11b, boot/frame status, DVD/DMA/CFDk.
- **Проверка/приёмка:** owner жив до actual checkpoint; Unknown/непустые obligations удерживают; нет post-seal GX cleanup; no healthy delayed free на failed tail; job/callback return до lifetime release. Ревью каждого включённого failure producer, не speculative аудит всех исключений engine.
- **Граница:** если найден конкретный consumer в этом же контракте — узкое исправление с regression до PASS; новый ownership/scheduler design требует остановки. Не откладывать известные fixes в device.

### GM-13b — compatibility/regression build gate

- **Зависит от:**13a. **Scope:** root ARM64 game; полные non-H700 `aurora_core`+`aurora_gx` и все изменённые game TUs в отдельном output.
- **Проверка/приёмка:** actual CTest inventory/targets, relevant lifecycle/render/FIFO/recording/readback, game-boundary/terminal/DVD/CFDk/skinning и no-init entries; реальные counts/skips/commands в отчёте. Non-H700 TUs не выдаются за полную non-H700 игру.
- **Граница:** нет GPU/container runtime, all-platform matrix, нового backend/deps ради harness; не выключать meaningful tests.

### GM-14a — physical isolation и two-install fixtures

- **Риск/результат:** user env не предотвращает CARD migration/discovery сам по себе.
- **Зависит от:**13b. **Scope:** layout/preflight shared seam, actual PortPaths fixtures, own-path checks; production path logic только для утверждённого H700 boundary.
- **Проверка/готово:** новые процессы из-за cached statics; synthetic old/new/HOME/XDG sentinels, no old read/discovery через call-path evidence и no old writes через hashes. Contaminated/redirected/read-only paths отвергаются до child; clean layout даёт один physical user/card/cache. Нет глобального отключения upstream migration.
- **Safety:** только temporary data, не личные saves. Установка/update policy §3.2 документируется до launcher.

### GM-14b — effective profile и persistence

- **Риск/результат:** старый собственный INI не включает shared card/quick resume/mods после безопасного первого запуска.
- **Зависит от:**14a. **Scope:** H700 prevalidation до side-effectful readers/apply, final effective check, protected live setters/direct widgets/discovery actions и related settings/card/path tests, по §3.2.
- **Проверка/готово:** startup protected mismatch → key/reason/nonzero, без effects/save/discovery и с неизменным INI; duplicate/legacy intermediate forbidden value не успевает вызвать effect. Live rejection не изменяет state/dirty/INI и не завершает игру. Safe settings/bindings сохраняются новым процессом. `--settings-selftest` отдельно от acceptance validator сохраняет upstream parser/serializer coverage; real disk persistence проверяется отдельно.
- **Дополнительно:** scoped disc/input/audio-math/CARD/GCI/atomic/identity suites; save-converter fixtures явно temporary (не default соседний `../wii-save`). Skip не называется real-fixture PASS.

### GM-15a — invocation/env/Ports launcher

- **Зависит от:**14b. **Scope:** новый `packaging/h700/` launcher/template, тонкий game helper; контракт §3.3.
- **Проверка/готово:** mock child получает один буквальный image argument с spaces/metacharacters; invalid/missing/relative/unreadable image не запускает child; conflicts проверяются до sanitation; frontend/audio unset/empty/socket cases; own persistent paths.
- **Safety:** no eval/autodiscovery/forced audio/firmware writes, нет normal game run в container. Private disc config устанавливается отдельно от generic archive.

### GM-15b — child/lock/signal lifetime

- **Зависит от:**15a. **Scope:** launcher ownership/wait/traps; не переписываем game lifecycle.
- **Проверка/готово:** actual child exit/nonzero/reap, два штатных запуска; launcher INT/HUP/TERM при живом child не снимает lock/не убивает child; unknown exit/stale lock не разрешает новый процесс. Exit code отдельно от complete result. Подготовлен per-run finalization hook, завершённое gate game+copier включается15c; до него launcher не считается release-ready. Exit evidence не фабрикуется по timeout.
- **Safety:** mock held child, не save-capable game; никаких killall/automatic recovery. Lock/test processes очищаются лишь после доказанного завершения fixture.

### GM-15c — bounded diagnostics без воздействия на CARD

- **Зависит от:**15b. **Scope:** existing `port_log_file.cpp` H700 sink/copy worker, launcher metadata, memory-summary extraction; не новая observability-система.
- **Решение/приёмка:** §3.3 unique per-run two8MiB segments; ready/health/final acknowledgements copier, finite startup/result records и existing cumulative RAM summary; drain продолжается при cap/error, нет unbounded duplicates/SIGPIPE/file-size ulimit/kill. Initial unavailable sink не допускает нагрузки; runtime failure/loss → incomplete evidence, не kill.
- **Тесты:** startup rejection/logging OFF, flood/cap/write/rotation-error, held game и **held copier после game exit**; complete result/lock release только после actual finalization. Не ждать EOF из игры до закрытия её writers. Overwritten error history не даёт PASS по отсутствию строки.
- **Safety:** paths только собственные; active sinks не очищаются, raw logs private. Полнота доказательств отдельна от exit code; неподтверждённый copier сохраняет lock, не запускает следующий child.

### GM-16a — настоящий game test/package mode и local loader

- **Зависит от:**15c и13b. **Scope:** `tools/h700_game`, shared build/package helpers с явным profile, root H700 stage-link option; reusable helpers не копируются целиком.
- **Проверка/готово:** один game package entry выполняет actual game/test inventory и no-init checks, strict layout executable+matched SDL+notices/manifest. Staged RUNPATH ровно local `$ORIGIN/lib`, копирование SDL проверено, нет build-directory/runtime dependency. Negative tamper/extra/link/mode/path/archive cases.
- **Граница:** sample CLI становится документированной командой только после реального выполнения; diagnostic scenario/test paths не выданы за game.

### GM-16b — full-game provenance и ABI evidence

- **Зависит от:**16a. **Scope:** §3.4 source closure, generated inputs/version metadata, artifact/provider/import reports.
- **Проверка/готово:** изменения каждого build-input класса меняют source digest; actual staged game/SDL dependency chain и full undefined inventory категоризированы, firmware snapshot bound по hash, нет private host paths в publishable report. Missing SONAME/node/provider — fail; incomplete exact exports явно listed для разрешённого loader check.
- **Граница:** metadata/host version command не называется Jacaranda/Mali/loader PASS; не нужен новый общий firmware inventory.

### GM-16c — full-game notices/content gate

- **Зависит от:**16b. **Scope:** existing collector + pinned texts/full-game link inventory/codec grants и package coverage manifest.
- **Проверка/готово:** matching actual component/version/license evidence, complete grants + attribution где нужны; отсутствие assets/ISO/saves/settings/private data/container graphics libs. Имена namespaces в binary — evidence состава, не юридическая сертификация. Missing primary text блокирует этот gate, не подменяется коротким header attribution.
- **Safety:** локальный RC, без публичной публикации бинарника/новых dependency installs.

### GM-17a — freeze и первый qualifying fresh build A

- **Зависит от:**16c. **Scope:** зафиксированные build-relevant bytes и опубликованный recipe; production code не меняется.
- **Проверка/готово:** fresh output A,14inputs/exact image/offline; game+весь выбранный CPU inventory, staged audit/content/notices, source/config/provider/ELF/archive hashes. Нет uncommitted active production/package changes за freeze. Старый incremental game build не засчитывается как qualifying A.
- **Safety:** любое изменение build closure отменяет freeze до нового A; docs отчёта не требуют пересобирать binary.

### GM-17b — независимый fresh build B, выбор RC и готовность маршрутов

- **Зависит от:**17a; выбранные disc revision/container и scene-access plan до PASS, но не для компиляции.
- **Проверка/готово:** новый output B из того же freeze проходит те же gates; сравнение §3.4, один выбранный archive/hash; заполнены конкретные A/B/C scene/action/reference/access/process-count таблицы, storage/log/RAM-observer prerequisites. Только теперь запрос окна A.
- **Safety:** нет late-save/ISO uploads; unavailable scene access — blocked device readiness, не сокращение checklist. Никакого final rebuild после принятия RC.

## 5. Device-этапы GM-18…GM-21: одна связная процедура

### Общие prerequisites и permission request

Перед каждым окном сообщить: immutable RC hash, один legal GameCube region/revision/container, стабильную физическую test installation, конкретные scenes/actions/ожидаемые результаты, максимальное число процессов,20/30/30мин active timebox, необходимую read-only log observation и loader/deploy operations. SSH endpoint/host identity/команды и deploy согласуются отдельно и явно; запуск через Ports при пользователе у экрана, не fullscreen поверх живого menu.

Обязательный RAM observer — заранее разрешённый bounded read-only просмотр собственного лога во время нагрузки; если он недоступен, согласовать другой существующий способ наблюдения **до** окна. Посмертный minimum позволяет отвергнуть run, но не выполняет обещание прекратить нагрузку при warning. Не создаём новый монитор/auto-killer. Missing/malformed samples — UNKNOWN, а не PASS;96MiB — sampled policy, не измеренная гарантия или minimum между samples. Нет обязательного искусственного pressure/trim события. При below96MiB, log-health failure или потере observer прекратить дальнейшее продвижение нагрузки и normal Exit при ближайшей безопасной возможности, не прерывая CARD IO. Окно не PASS; если безопасный выход невозможен — FAIL/PARTIAL и отдельное решение без kill/retry.

Одна установка/user/card/settings/cache сохраняется между окнами; first install/cold persistent cache, warm new-process cache и in-process повтор маршрута отмечаются раздельно. «No seed» не запрещает собственный warm cache. Не сбрасывать cache/settings ради повторного PASS.

Deadline60с без progress /30с normal exit — границы **оценки результата**, не способ прервать driver/IO call. FAIL/PARTIAL/отсутствие exit record различаются. При card IO не reset/kill/reboot; при fail-stop не запускать следующий процесс. Force termination требует отдельного разрешения. Terminal cleanup/nonzero — ошибочный run, не успешная приёмка. Device loss/OOM/kill намеренно не инъецируются.

### GM-18 / окно A — первый обычный запуск новой игры

- **Зависит от:**17b + отдельное A permission. До20мин; один normal game process; optional loader-only invocation только если включён в разрешение.
- **Путь первоначального A:** preflight нового install → actual effective profile/backend/cache/path evidence → Ports logo/movie → file select/new game → конкретная ранняя управляемая сцена (движение/aim/fire) → PortMenu **Game → Exit game → Exit** → actual release/exit/Ports.
- **Повтор A после correction/update RC:** тот же preserved physical install/user/cache, явно warm/update run: обычный startup/effective profile/paths, gameplay/audio/input/normal exit. Не называется cold-install проверкой нового binary. Если необходим именно новый cold-device test, подготовка состояния отдельно согласуется; automatic user/cache reset нет.
- **PASS:** пользователь подтвердил панель/orientation/HUD/fonts/glyphs, gameplay pad и слышимые movie/music/SFX; actual SDL/nod open/seek/read выбранного контейнера работают на kernel4.9; без validation/EGL/GL/device-loss errors, нормальный выход. Меню, audio stats и exit0 по отдельности недостаточны.
- **Неизвестное ABI evidence:** явно указанный loader check закрывает только исполнявшийся путь, не все deferred plugins. Ошибка nod сначала локализуется по операции/container/partition, не ведёт автоматически к новому IO backend.
- **Результат:** отдельный private run record и обезличенный acceptance report; commit/push отчёта, не сырых логов.

### GM-19 / окно B — сохранение, повторный процесс, reset и RAM

- **Зависит от:**A PASS, отдельное B permission с двумя процессами; тот же RC/install; заранее достижимая station и room route. До30мин, прохождение отдельно.
- **Процесс B1:** маршрут туда/обратно дважды, pause/map/PortMenu, RAM/growth/trim evidence → station save с заранее записанным проверяемым progress marker → подтверждение записи и отсутствия pending CARD IO → штатный Exit.
- **Процесс B2:** новый launch того же Ports entry/user/cache → file select **CARD load**, пользователь подтверждает station/progress и settings/bindings → только затем **Game → Restart to menu** без card operation → новый archSupport, то же graphics generation → повторный CARD load/управляемая сцена → штатный Exit.
- **PASS:** реальные card files присутствуют только в собственной изолированной card directory, identity/atomic-write errors отсутствуют; не quick-resume/savestate; lifecycle ordering и same-generation reset, memory/headroom policy; physical confirmation сохранённого прогресса. Два круга не доказывают отсутствие всех leaks, warm plateau не обязан равняться стартовой RSS.
- **Safety:** сначала доказанное save/load, затем действие, отбрасывающее несохранённый прогресс. Никаких личных save reads по умолчанию. Недостижимая станция в timebox — PARTIAL/access plan, не бесконечный run.

### GM-20 / окно C — конечный retail checklist

- **Зависит от:**B PASS, подготовленные scenes/references/access, отдельное C permission с конечным числом запускаемых сцен/процессов. До30мин проверки, не обещание прохождения кампании.
- **Путь:** checklist ниже; late Thermal/X-ray/beam scenes только naturally reached save либо отдельно разрешённые private test copies. Baseline card B сначала сохраняется отдельно по разрешению; копии не пишутся поверх личных saves и не подменяют B new-card acceptance.
- **PASS:** каждая строка observed PASS/обоснованный N/A именно для выбранного release; недоступный обязательный effect — PENDING/BLOCKED, не N/A. Скриншот/клип supplementary, физическая панель и слышимый звук подтверждаются пользователем.
- **Граница:** defect → §6 correction loop, не отключение retail effect/понижение разрешения/подгонка memory budget.

### GM-21 — ровно испытанный пакет и пользовательская приёмка

- **Зависит от:**A/B/C PASS одного финального RC; отдельные исправления не наследуют старые результаты как будто проверялся новый binary.
- **Scope:** acceptance report, README/build/manual install/update/uninstall, hashes/notices/ограничения. Production/package bytes не менять.
- **Готово:** frozen payload ровно совпадает с испытанным, full checklist и explicit limits/user acceptance; сохранена performance baseline, но30FPS не обещаны. Недостающий критерий блокирует статус полного переноса.
- **Safety:** удалить свою установку/launcher только по разрешению после завершения процессов и сохранения новых test saves по выбору пользователя; старый порт и оставленный IG launcher не удалять.

### Checklist, который GM-17b обязан привязать к конкретным сценам

| Область | Минимальное наблюдение |
| --- | --- |
| Boot/UI/display | Logo/movie/file select,4:3/orientation/color/HUD/fonts/glyphs, pause/inventory/map, читаемый PortMenu |
| Skin/model | Animated actors, weapon, pause doll, frozen/thermal/shadow и доступные boid/scenes; нет unskinned fallback/partial geometry |
| Materials/EFB | Opaque/alpha-tested, lightmaps, particles, water/transparency/reflections/bloom, штатные beam/post effects; возврат после естественного trim, если trim наблюдался |
| Visors | Scan overlay/text/progress, Thermal и X-ray в характерных доступных сценах |
| Pad | Movement/aim/fire/charge/missile/jump/lock/scan/morph/bombs/beams/visors/pause/map; оба порядка Back+Start, navigation/close/Exit; persist bindings; rumble observed/unsupported отдельно |
| Audio | Слышимые music/SFX/position/movie/pause-resume, без silent/dummy fallback; stats не доказательство слышимости |
| Lifecycle/data | Rooms/return, normal exit, новый процесс, same-generation reset; station CARD write/load нового процесса; settings persist |
| RAM/pacing | Samples/HWM/min-available/growth и route twice; no unsafe reuse/OOM/unknown teardown; frametimes/stutters — baseline, не FPS PASS |

На каждую строку: RC/disc identity, scene/action, reference expectation, observed/user confirmation, log interval, limitation. Original-game reference того же release, не произвольный screenshot. Все поздние сцены должны иметь реальный access plan, но их прохождение не входит в локальные часы.

## 6. Цикл runtime-дефектов и новые RC без скрытых расширений

Предлагается однократно разрешить **ограниченные локальные corrections существующего решения** на протяжении этой приёмки. Каждая итерация: failing RC/run/expected-actual → минимальная applicable diagnosis → focused fix/regression → relevant H700/non-H700 checks → docs/commit/push → новый freeze/immutable RC с новым hash. Same image/pins и те же security/correctness/acceptance boundaries. Это не обещание исправить все возможные дефекты за конечное время.

Обновление на устройстве — только по явному разрешению, при завершённом своём процессе: stable physical executable directory, замена лишь manifest-owned payload, **не** `user/`/cards/settings/cache, не symlink switch versioned installs. Перед заменой при необходимости отдельно разрешённый backup test card; rollback payload не означает автоматический rollback карты.

Сначала повторить failing scenario и dependency-related checks для диагностики; старые runs остаются evidence старого RC. **Для GM-21 окончательный A/B/C набор повторяется на одном финальном RC**, в отдельно разрешённых окнах. Мы не переносим physical evidence между разными binaries без нового согласованного правила. Docs-only исправление, не меняющее payload/config, не требует game rerun.

### Когда не спрашиваем новое approval

После утверждения редакции2 выполняем перечисленные local substeps и необходимые механические call-site/test corrections этого контракта. Уточнение номера строки, реального target/path или добавление обнаруженного прямого consumer того же статуса не становится само по себе новым архитектурным решением. Если карточка не закончена, сохраняем честный partial, но не выдаём любой дополнительный файл за повод заново утверждать весь план.

### Когда останавливаемся обязательно

- Новый backend/image/pin/dependency, иной ownership/scheduler/virtual-C ABI, wider exception/OOM redesign; late raw-userdata access после доказанного completion.
- Изменение D4…D10 safety/profile/memory sizes/thresholds, fake tokens/GPU completion, generic ignore-GX, отказ от meaningful test/retail effect/security.
- Не удаётся сохранить job/callback/request destination до actual completion или safely destructible partial owner без нового дизайна.
- Новый device/deploy/SSH/forced termination/firmware/services доступ, непредусмотренные retry processes, migration/card identity change.
- Невыполненный gate/недоступная mandatory scene/неполное evidence не может быть закрыт в согласованном контракте. Отчёт FAIL/PARTIAL, не продолжение нагрузки ради зелёной таблицы.

## 7. Порядок, оценка и что действительно требуется от пользователя

**Последовательность:**12a→12b→12c→12d→13a→13b→14a→14b→15a→15b→15c→16a→16b→16c→17a→17b→A→B→C→21. Не запускаем следующую production-ветку, обходя незакрытый gate. Подготовка disc/scene access может обсуждаться заранее, но не разрешает device actions.

**Осталось16 локальных подшагов**, ориентир **16–32 часа активной локальной работы**, плюс ориентировочно4–8ч подготовки/разбора device/final report: **около20–40ч активной работы** при отсутствии новых фундаментальных дефектов. Cold builds, ожидание пользователя/доступов, прохождение к station/late scenes и correction loops отдельно. Исходные30–42ч были оценкой всего первоначального плана, не достоверным остатком после GM-11b. Новая оценка тоже не гарантия срока.

**Сейчас:** согласовать редакцию2 целиком — R1…R6 и весь оставшийся scope/порядок. Завершённые GM-01…11b повторно утверждать не требуется.

**До17b/device, не для начала локальной реализации:** выбрать один legal disc release/container (private absolute path позже), stable physical install/Ports invocation, station/room route, late scene access и допустимость private test copies. Можно заранее выбрать подготовку маршрутов; невозможность доступа честно блокирует готовность окна, но не требует искать чужие saves.

**Перед каждым окном:** отдельное конкретное разрешение процессов/действий, loader/log observation/deploy/SSH при необходимости. Новая локальная игра не считается принятой до physical graphics/input/audio и CARD evidence. После приёмки остаются неизвестными вся непроверенная кампания, другие releases/controllers/firmwares, полная driver-loss recovery и достижимость30FPS — это не скрытые цели текущего этапа.

## 8. Проверка самого пересмотра

Три независимых read-only review; после corrections повторно проверены startup/live validation, game/copier/log finalization и device/update/RAM процедура. Замечания по верхнему renderer EndScene tail, dormant activation, side-effectful parser/UI, late copier completion и warm revalidation A учтены. Нового архитектурного блокера в проверенном scope не найдено; это source/planning review, не runtime гарантия.

Локально проверены74Markdown links/anchors в изменённых документах, порядок/уникальность20оставшихся карточек (16local substeps +18/19/20/21), `git diff --check` и docs-only diff — PASS. Production/build/dependencies/device не менялись и не запускались. Эти проверки не означают утверждения редакции2 пользователем или прохождения её будущих implementation tests.
