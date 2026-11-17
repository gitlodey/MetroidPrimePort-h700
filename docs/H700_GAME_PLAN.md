# H700 — план подключения полной игры

**Редакция 1, 2026-11-16. УТВЕРЖДЁН пользователем: «Прочитал короткую версию плана. Даю добро на все». Решения D1…D11 и локальные GM-01…GM-17 разрешены; commit/push после каждой завершённой итерации подтверждены отдельно. Device-окна по D11 согласуются отдельно.**

**Согласованные уточнения, 2026-11-17:** GM-11 разделён на GM-11a/GM-11b; затем пользователь разрешил узкое DVD-lifetime расширение GM-11b. Оба подшага завершены/отправлены, результаты ниже.

**Актуальное предложение по всему остатку:** [необходимый остаток, редакция3](H700_GAME_REMAINING_PLAN.md), [коротко](H700_GAME_SUMMARY.md). По текущему требованию пользователя цель/здравый смысл закреплены в project rules, неутверждённая редакция2 с16local gates заменена: UI/settings sandbox, новый log protocol и late-scene prerequisites до A исключены. Actual skinning/lifetime, isolated install, game package/build и полная device-приёмка сохранены. Редакция3 утверждена пользователем2026-11-17; локальное выполнение GM-12…17 разрешено, device-окна отдельно. Повторное одобрение завершённых шагов/прямых callers не требуется.

Цель: воспроизводимо собрать **Metroid Prime Unbound v0.46.0** и получить корректно работающую игру на **ANBERNIC RG40XX H / H700, muOS 2601.0 Jacaranda**: графика, интерфейс, встроенный контроллер, звук, штатные сохранение/загрузка. **Достижение 30 FPS — следующий отдельный этап**, не критерий завершения этого плана.

Короткая версия — [H700_GAME_SUMMARY.md](H700_GAME_SUMMARY.md). Правила — [H700_WORKFLOW.md](H700_WORKFLOW.md); состояние — [H700_PROGRESS.md](H700_PROGRESS.md). Основание: новая база `c872da704` / v0.46.0, интеграция на `03dff0827`, [архитектурное решение](H700_ARCHITECTURE_DECISION.md), [IG-план](H700_INTEGRATION_PLAN.md) и [device evidence](H700_INTEGRATION_GATE.md).

## 1. От чего продолжаем и что получаем

IG-01…IG-13 дали постоянный production backend, а не одноразовую замену renderer. Последний корректный IG-14 manual run прошёл bounded-сценарий, пользователь подтвердил видимый запуск через Ports. Три других manual runs остались incomplete/exit1 из-за отсутствия observed actual overlap. Это не 4/4 PASS; причина расхождения `fbgrab` с точными native/GL samples не установлена. Новые повторы демки ради зелёной матрицы не нужны.

**Сохраняем:** SDL3-over-SDL2 → firmware SDL2 `mali` → EGL/GLES + pinned Dawn; настоящие FIFO/render/compiler workers; main presenter; два permanent 640×480 imports; forward/reverse completion; mixed vertex-texture/fragment-SSBO generator; cache namespace `h700-mixed-v1`; IG11 drain, map retirement и whole-share-group teardown.

**Результат данного плана:** настоящий `metroid_prime_port`, воспроизводимый offline build recipe, проверенный ARM64 пакет с notices, отдельная установка Ports и отчёт об ограниченной, но содержательной приёмке реальной игры. Это не прохождение всей кампании и не сертификация всех дисков/настроек upstream.

**Разрешена локальная реализация GM-01…GM-17 в рамках утверждённых решений.** Утверждение локальных итераций не означает разрешения на SSH/deploy/device tests. Каждое из трёх device-окон согласуется отдельно. Подготовка плана не требует commit/push; после утверждения действует цикл небольшая итерация → проверки → документы → отдельный commit/push.

## 2. Утверждённые решения

D1…D11 утверждены пользователем. Значения профиля ещё предстоит реализовать и проверить; утверждение не превращает их в измеренные гарантии.

| ID | Предлагаемое решение | Граница / причина остановки |
| --- | --- | --- |
| D1 | Расширить разрешение на существующий exact EOL ARM64 image **для correctness RC этого плана: configure/build/tests GM-01…17, включая повторную fresh build GM-17**: `sha256:2db4439763783aa796da4268dbd91847ee2c13deef4c8ce2439e0f6bdf926962`, Ubuntu 23.10, Clang 16.0.6, CMake 3.27.4, Ninja 1.11.1 | [Риск EOL](H700_BUILD_ENVIRONMENT.md) принят явно, это не поддерживаемая долгосрочная build platform. Несовместимость root game с toolchain → остановка/согласование, без apt/new image/Rust/Dawn rebuild |
| D2 | Те же 14 hashed inputs из `tools/h700_integration_driver/inputs.json`: pinned Dawn static, matched SDL fork/headers, nod alpha.12 release static, остальные прежние providers. Offline build; `MP_ANDROID_NOD_STUB=OFF` | Новая зависимость/неожиданный system provider/изменение pins → остановка. Использовать существующие staging/audit helpers, не создавать второй framework |
| D3 | Первый профиль — offline retail GameCube; `CMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON`, без WSS/Archipelago acceptance. Embedded controller glyphs/resources ON, smoke driver OFF, RmlUi OFF | OpenSSL здесь optional desktop WSS dependency, не причина собирать Rust или отключать чтение disc. Полный online/modded профиль вне этапа; upstream-возможности не удаляем |
| D4 | Hardware OpenGLES, validation и robustness ON, без AUTO/CPU/Null fallback, skip-present и conflicting overrides. Native/EFB baseline 640×480, 4:3, MSAA1, anisotropy1 | Ошибка init/unsupported override → понятный nonzero до game owners. Не молчаливый переход на иной backend |
| D5 | Original assets; MEM1 24 MiB, room/resident geometry reserve OFF, effective frame/storage scale **1 unpinned**, resident0, lazy pipelines ON/no old seed | Не `MP_FRAME_BUFFERS=1`: наличие env pin-ит полную allocation. Нужен H700 default; прежние desktop defaults не менять |
| D6 | Pipeline pressure: moderate **192 MiB**, severe **128 MiB** доступной памяти; replacement pressure те же thresholds, replacements в baseline OFF. Сохраняются штатные eviction/global-pipeline правила | Это начальная policy, **не измеренный бюджет**. На устройстве проверяем маршрут и headroom; менять thresholds автоматически после неудачи нельзя |
| D7 | Dynamic resolution, interpolation, PBR/remastered/HD, additional sun shadows и дополнительные scan-эффекты OFF; штатные visors, EFB copies, transparency, reflections, bloom остаются | Нельзя выключить retail effect ради PASS или уменьшить разрешение незаметно. `Original Experience` не заменяет этот профиль |
| D8 | Modern/twin-stick, штатный first-real-controller policy; Back+Start → PortMenu → Quit, gamepad navigation. Stock audio balance. Output cap30 / simulation60 / adaptive OFF — начальная pacing policy | Это **не обещание 30 FPS**. Не переносим donor immediate-exit/gain/volume/PAD-ID hacks |
| D9 | Healthy reset/exit: game destructor cleanup при работающих FIFO/render; terminal failure: CPU quiescence **до** unwinding, typed internal C++ token, milestone-based no-new-graphics cleanup | Детальный контракт в §4. Нельзя вызвать обычные dtors после закрытия FIFO или освободить owners из-за timeout |
| D10 | Отдельный физический executable и собственный `<exe>/user/`; `MP_USER_PATH` указывает туда же. `share_emulator_card=0`, `quick_resume=0`; стабильный installation path | Никаких старых settings/cards/mods/caches, symlink на старую установку, migration или serial sidecar. Старый порт не затрагиваем |
| D11 | Три коротких ручных device-окна с реальной игрой, без фоновой второй графической сессии, destructive fault injection и автоматического убийства при записи CARD | Time/headroom/watchdog policy в §6. Каждый запуск требует отдельного разрешения, approval плана этого не даёт |

Изменение любого из этих выборов требует остановки и нового согласования по workflow.

## 3. Сборка, память и платформенные границы

### 3.1. Настоящий root target, не расширенная демка

Root `CMakeLists.txt` создаёт OBJECT library `mp_game` и executable **`metroid_prime_port`**, включая `src/MetroidPrime/main.cpp`, MusyX и остальные game dependencies. `cmake/h700/InitialCache.cmake` — профиль slice, его нельзя объявлять готовой сборкой игры.

Предлагаемые **новые** пути (сейчас отсутствуют): `cmake/h700/GameInitialCache.cmake`, `tools/h700_game/`, `packaging/h700/`. Они тонко связывают root build с существующими safe staging/offline/audit helpers. Сначала root-scope `find_package(nod CONFIG GLOBAL … NO_DEFAULT_PATH)` до Aurora: это известное решение directory-local imported targets из IG, не смена package/version. Сохраняются `-fsigned-char`, `-fno-strict-aliasing`, `-fwrapv`, ABI compatibility и meaningful diagnostics. ISO не нужен для compilation: glyphs встроены, игровые fonts берутся из retail DOL при запуске.

Fresh output, read-only source, writable output, `--pull=never --network=none --read-only`; контейнер не является Jacaranda sysroot. В build-контейнере **не запускаем GPU**. `--version` и settings selftest допустимы только по проверенному no-init пути. Рецепт и реальные команды публикуем после их выполнения, не выдаём планируемый CLI за существующий.

ELF audit включает actual game + bundled SDL и их loader/NEEDED/version nodes, затем firmware SDL2 `dlopen` chain. Нынешний snapshot содержит только пять выбранных exact exports, **не полную symbol closure**. Если статического evidence недостаточно, помечаем unresolved imports и проверяем loader в разрешённом device-окне; metadata не называется runtime PASS. Firmware libraries не подменяем.

### 3.2. RAM: известные размеры и проверяемая policy

Source-level desktop MEM1 reserve256 MiB / room reserve / scale12 — риск профиля, **не измеренный OOM и не immediate RSS**: часть reserve virtual/lazy. Linux `LowRamDevice()` сейчас false, поэтому H700 defaults задаём явно.

При scale1 staging начинает с **30 MiB ×5 =150 MiB**, ceiling **63 MiB ×5 =315 MiB**; это не вся память и не доказательство одновременного resident peak. Дополнительно есть 32 MiB native skinning ring вне MEM1, MEM1, game heaps, textures, pipelines, mirrors и shared driver allocations. Retail texture cache128 MiB и desktop replacement cache4 GiB — разные сущности; выключенный HD cache не освобождает всю обычную графику.

В D6 выбраны moderate192/severe128, чтобы desktop thresholds1024/512 не держали 1-GB устройство постоянно в наиболее жёстком режиме, сохранив pressure handling до исчерпания памяти. Это обоснование начального выбора, **не evidence достаточности**. Не уменьшать/увеличивать staging/ring/retail cache, не отключать eviction, не добавлять swap/governor/clock tuning в этом плане.

Для согласованного маршрута фиксируем `/proc` MemAvailable и process RSS/HWM примерно раз в секунду, доступные штатные memory/growth/trim diagnostics и точки scene/reset. Не складываем RSS и GPU accounting как независимые величины: память общая. Peak между samples может быть пропущен — ограничение отчёта.

**Gate:** ни allocation failure/overflow/OOM-kill, ни unsafe reuse; sampled MemAvailable не ниже **96 MiB**, после повторного маршрута нет неограниченного роста owners/arenas, все high-water/growth events объяснены. 96 MiB — предлагаемый запас для первой приёмки, не гарантия ОС. При пересечении порога окно не PASS: остановить нагрузку и выйти штатно при ближайшей безопасной возможности; не прерывать CARD write. При невозможности безопасного выхода — сохранить partial и запросить решение. Не внедряем автоматический memory-killer. Стабильное plateau может быть выше начального RSS из-за caches; возврат точно к стартовому RSS не требуем.

### 3.3. Input/audio/disc/cards — сохранить новые реализации

- **Input:** `extern/aurora/lib/input_first_port.hpp`, `lib/input.cpp`, `platform/input/port_input_devices.cpp`, `platform/port_controls.cpp`, `platform/debug_ui/ui_common.cpp`. Проверить настоящий встроенный pad и PortMenu, оба порядка Back+Start; не переписывать scheduler/input только ради переноса.
- **Audio:** MusyX32kHz, штатный SDL stream, существующий `MP_AUDIO_STATS` в `extern/musyx/src/musyx/runtime/hw_pc.c`. Silent fallback возможен: driver string/exit0 не доказывают звук. `PIPEWIRE_RUNTIME_DIR=/run` задаётся только при существующем socket и незаданной переменной; никаких mixer/services/forced ALSA изменений. Music/SFX/spatial/movie/pause-resume проверяются на слух пользователем.
- **Disc:** actual `SDL_IOFromFile("rb") → nod_disc_open_stream`, callbacks read/seek в `extern/aurora/lib/dolphin/dvd.cpp`. Kernel4.9 compatibility ещё не подтверждена. Не добавлять io_uring/pread fallback/Rust по одним weak imports. Launcher принимает один явный absolute readable regular image, quoted одним argument, без glob/диалога/поиска старого install. Первый диск — одна выбранная пользователем legal GameCube release; рекомендуем USA1.00, но это не обязательная новая покупка/смена образа. Другие revisions/Trilogy вне первой матрицы.
- **Cards:** `platform/include/port_paths.h`: `CardFolder()` использует writable `<exe>/user/` независимо от `MP_USER_PATH`. Поэтому изоляция требует отдельного executable directory, не одной env. Существующие nonzero serial, identity checks, atomic writes сохраняем. Real station save → обычный exit → **новый процесс** → file select/CARD load → gameplay. Quick-resume/savestate не заменяют проверку.

## 4. Game lifecycle: обязательный контракт реализации

### 4.1. Healthy boundary без закрытия поколения

`archSupport` — локальный owner в `RsMain`, `x164_archSupport` — raw alias. Деструкторы renderer/models/palettes/textures/ARAM создают новые delayed frees; `CStateManager` может отправлять `GXDestroyPBR*`/resident cleanup. `GXDrawDone` — **FIFO barrier, не render/GPU completion**. Диск/loader должны жить до уничтожения `CResLoader`.

Reset и normal exit выполняются на main safe-point:

1. Остановить game ticks/draw, без повторного entry из progress callbacks.
2. Если открыт Aurora frame — закончить **ровно один раз**, с учётом `s_auroraFrameOpen` в `src/Kyoto/Graphics/DolphinCGraphics.cpp`.
3. Status-bearing FIFO drain → render sync, включая **возврат** post-submit callbacks и уничтожение их game-facing captures. Queue-empty и GX marker недостаточны.
4. Уничтожить `archSupport` при живых workers/backend. Raw alias не должен оставаться доступным как рабочий owner.
5. Drain destructor tail → render sync. После этого flush delayed frees, **без лишнего GX command** за уже доказанной границей.
6. **Reset:** создать новый `archSupport`, продолжить в том же generation; root admission глобально не закрывать, device/compiler не join-ить.
7. **Exit:** дополнительно `ShutdownSubsystems`, game globals и их destructor tail, final delayed flush/ARAM в исходно необходимом порядке; loader живёт достаточно долго. Затем существующий **полный IG11**, platform/disc cleanup и `aurora_shutdown` без двойного shutdown.

Для исходных game buffers CPU boundary достаточна: FIFO копирует arrays/vertices, texture uploads имеют owned staging; GPU использует Aurora-owned resources. Universal GPU-idle/native retirement/compiler join **не добавлять к healthy reset**. Compiler configs value-captured. Если обнаружен конкретный late callback к game raw userdata за этой границей — остановка и пересмотр, не speculative universal framework.

### 4.2. Terminal failure без раннего unwinding

Транспорт: отдельный **typed internal C++ token**, не `std::runtime_error` (он ловится boot-кодом), только с main safe-point. Это не исключение из callbacks, C ABI, workers, `noexcept` или деструкторов.

1. H700 terminal state прекращает обычные waits/frame/growth paths, будит waiters и передаёт статус main, не симулирует success.
2. **До throw** main получает фактическую CPU quiescence FIFO/render/compiler. Reuse IG11 polling **до стадии Maps** после compiler joins, сохраняя device/exports; добавить явную остановку/статус `CPU quiescent`, не `Released`/GPU complete. Одна проверка текущего enum без барьера продвижения недостаточна.
3. Если driver/worker call завис или completion неизвестен — **не throw/unwind используемых owners**. Удерживать owners, диагностический fail-stop; watchdog/оператор не объявляют это успешным cleanup.
4. `gameGlobalObjects` и `archSupport` объявляются снаружи encompassing boot+loop `try`; typed `catch` находится **внутри живого owner scope**. Inner generic loop catch rethrow-ит этот конкретный token, а не вызывает прежний crash abort.
5. После CPU boundary — явная milestone-based terminal cleanup **без новой graphics work**. CFDk имеет no-command flush; частично созданные owners учитываются. Normal и terminal destruction paths не подменяют друг друга.
6. Для retail profile room-env/resident resources не создаются. Если фактически созданы ресурсы, destructor cleanup которых требует уже закрытого FIFO, это terminal-cleanup blocker: не игнорировать `GXDestroy*` и не отмечать released; остановка/согласование. Пустота должна быть проверена **до throw/unwinding и уничтожения соответствующего owner**, не предположена и не проверена постфактум.
7. Продолжить оставшиеся IG11 map/native/holder/device/export/window стадии. После device loss GPU completion не обещаем; Unknown не разрешает освобождать потенциально используемые данные или начинать новое поколение. CPU quiescence не равна безопасному GPU release.
8. Провести nonzero через `RsMain → metroid_main → platform main`: нынешний `metroid_main` игнорирует RsMain return. Boot failure должен иметь ту же безопасную границу, а не только inner-loop catch.

**Не делаем:** live device-loss recovery, новый универсальный exception framework, искусственные GPU-completion tokens или полный rewrite game ownership. Existing SkinPool detached/process-lifetime сам по себе не основание переделывать потоки: `Run` синхронно ждёт настоящие jobs. Cancelled waiter не разрешает освободить running job inputs.

### 4.3. Skinning ring

В `src/Kyoto/Animation/DolphinCSkinnedModel.cpp` init/reclaim/allocation могут бесконечно ждать draw-sync token. Нужны main progress/health **вне locks**, настоящий FIFO-ordered token/wrap contract, invalid-size check и failure **до** установки workspace pointers. Не возвращать фиктивный allocation/token. Проверка должна различать pending/success/terminal и сохранность данных running skin jobs.

## 5. Локальные итерации

Для каждой карточки: до работы goal gate по [workflow](H700_WORKFLOW.md), собственный небольшой diff, только относящиеся проверки, запись результата/команд/ограничений в этот документ и [прогресс](H700_PROGRESS.md), отдельный commit/push **после утверждения**. Новые paths ниже — проектируемые. Донор read-only; `.DS_Store` и чужие local changes не трогаем.

Общая безопасность/откат: локальные шаги не подключаются к устройству; не менять image/pins/firmware и прежние builds; при failure сохранить partial и не продвигать artifact. Откат только собственных изменений после проверки diff, без destructive reset/clean. Каждая карточка — **1–2 часа активной работы**, кроме явно более короткой; ожидание cold builds, пользователя и доступов отдельно. Если не помещается или требуется новый значимый выбор — согласовать split, не превращать карточку в неограниченное «починить всё».

### GM-01. Full-game build profile

- **Цель/риск:** slice cache не собирает root игру; закрепить providers до первой попытки.
- **Зависимости:** утверждение D1–D3. **Файлы:** root `CMakeLists.txt`, новый `cmake/h700/GameInitialCache.cmake`, тонкий `tools/h700_game/` entry поверх существующих helpers.
- **Решение:** root game target, GLOBAL nod import до Aurora, те же hashes/offline mounts, SDL headers/library одного snapshot; никаких новых providers.
- **Проверка/готово:** hash/missing-input fail до configure; root configure в fresh output перечисляет expected providers, source manifests и compile definitions. Никакой сети/GPU. Только configure PASS, не full build.
- **Safety/docs:** общий rollback; D1/EOL scope и actual configure command записать здесь; существующий slice не ломать.

### GM-02. Первый compile/link настоящей игры

- **Цель/риск:** MusyX/API slice не доказывает C++ game dependencies/toolchain compatibility.
- **Зависимости:** GM-01. **Файлы:** только доказанные root/H700 build seams; существующие game manifests не урезать.
- **Решение:** собрать `metroid_prime_port` с embedded resources, без disc, nod stub и smoke mode. Допустимы узкие H700 build fixes внутри D1/D2; новая библиотека/toolchain или semantic game fix требует остановки.
- **Проверка/готово:** full compile/link и artifact manifest, `--version` лишь после подтверждения no-init. Никаких no-op symbols/исключённых game sources ради линковки.
- **Safety/docs:** artifact пока не deployable; сохранить actual errors/решение/команды. Оценка1–2ч **на ограниченный compatibility diff**, cold build отдельно; неизвестные новые blockers не считаются включённым пакетом исправлений.

### GM-03. Детерминированный H700 retail profile

- **Цель/риск:** desktop scale12, Linux low-RAM=false и NDEBUG toggles неприменимы как baseline устройства.
- **Зависимости:** GM-02, D4/D5/D7/D8. **Файлы:** `platform/main.cpp`, settings/profile seams, Aurora init config.
- **Решение:** H700-only effective defaults из §2, scale1 unpinned, lazy/no-seed, validation/robustness ON; reject противоречащих backend/skip-present/toggle overrides. Existing user settings не перезаписывать: создавать baseline только новой установки и логировать effective значения/источник.
- **Проверка/готово:** focused profile tests на effective values, absent-vs-present env pin и conflict rejection; non-H700 defaults неизменны. Не использовать GPU Null profile17 для этой проверки.
- **Safety/docs:** generic settings migration вне scope; сохранить профиль и список отклоняемых override-классов, не raw environment.

### GM-04. Pressure policy и наблюдаемость RAM

- **Цель/риск:** shared1GB и staging150…315MiB требуют реальных данных, не вывода OOM из reserve.
- **Зависимости:** GM-03, D6. **Файлы:** `platform/main.cpp`, `platform/port_memory_log.cpp`, существующие pressure interfaces/tests.
- **Решение:** H700 moderate192/severe128 с сохранением штатных trims; bounded diagnostics для effective reserve/growth/pressure, reuse existing memory log. Минимальный внешний `/proc` observer входит в device procedure, не новая telemetry subsystem.
- **Проверка/готово:** CPU tests thresholds/boundaries и сохранение GLOBAL pipeline semantics; переполнение/allocation failure не success. Подготовлены записи для gate96MiB, но **RAM device PASS ещё нет**.
- **Safety/docs:** не менять sizes/eviction алгоритм/ОС; описать accounting limitations и конкретный sampled headroom gate.

### GM-05. Init gate до создания игры

- **Цель/риск:** platform игнорирует `aurora_initialize` result, crash marker допускает AUTO fallback.
- **Зависимости:** GM-03. **Файлы:** `platform/main.cpp`, actual H700 init/status interface.
- **Решение:** проверить result и actual backend/terminal state до game owners, strict OpenGLES; partial init → существующий безопасный teardown по milestones, nonzero, без второго backend.
- **Проверка/готово:** focused injected status tests before-owner boundary; failure не входит в `metroid_main`, не double shutdown; success не повторяет init. Real loader/GPU проверяется только позже.
- **Safety/docs:** не расширять recovery; сохранить порядок и exit codes.

### GM-06. Healthy game-owner barrier

- **Цель/риск:** `GXDrawDone` не защищает owner от render callback tail.
- **Зависимости:** GM-05, D9. **Файлы:** Aurora FIFO/render status-bearing seams; `src/Kyoto/Graphics/DolphinCGraphics.cpp`; соответствующие existing tests.
- **Решение:** reusable main-only boundary §4.1: finish-open-frame once → FIFO drain → render sync/callback-return; root admission остаётся открыт. No-command CFDk flush отделить от command-producing варианта.
- **Проверка/готово:** pending FIFO/render/callback work задерживает boundary; callback capture уничтожен до success; opened/closed frame без double-end; terminal возвращает failure, а не success. Healthy GPU-idle/compiler joins не добавлены.
- **Safety/docs:** вызовы progress вне locks, без game reentry; documented boundary semantics. Не менять existing GX callback ABI.

### GM-07. Reset игровых owners

- **Цель/риск:** reset `archSupport` создаёт новый owner, пока старые commands ещё могут его использовать.
- **Зависимости:** GM-06. **Файлы:** `src/MetroidPrime/main.cpp`, `src/Kyoto/CFrameDelayedKiller.cpp`, узкие lifecycle tests.
- **Решение:** порядок §4.1 до/после destruction, raw alias discipline; после delayed flush новый archSupport в том же generation. Не закрывать root admission.
- **Проверка/готово:** ordering regression: pre-dtor work → destructor commands → tail barrier → frees → new owner; reset не вызывает generation shutdown. Реальный reset остаётся device gate.
- **Safety/docs:** normal-only path не принимает terminal как completion; сохранить evidence и точный reset trigger для device-процедуры.

### GM-08. Нормальный выход всей игры

- **Цель/риск:** shutdown globals/ARAM/loader имеет дополнительные destructor tails за archSupport.
- **Зависимости:** GM-07. **Файлы:** game main, platform main, CFDk; existing lifecycle integration seams.
- **Решение:** §4.1 exit: owner → subsystem/globals tails → final delayed flush/ARAM → IG11 → platform/disc final cleanup. Loader/disc живут до CResLoader dtor, shutdown вызывается один раз.
- **Проверка/готово:** ordering test с destructor-created cleanup и live loader; all normal accepted work drains. Back+Start не превращён в immediate kill.
- **Safety/docs:** не освобождать используемые owners при timeout; зафиксировать ownership order и unresolved conditions.

### GM-09. Terminal CPU-quiescence checkpoint

- **Цель/риск:** исключение до joins раскрутит RAII и освободит данные executing worker.
- **Зависимости:** GM-06, D9. **Файлы:** `extern/aurora/lib/h700/lifecycle.hpp`, existing IG11 implementation и lifecycle tests.
- **Решение:** explicit poll mode/checkpoint до Maps, после producers/FIFO/render/compiler joins; device/exports ещё удержаны. Pending/CPU-quiescent/Unknown различаются; старый full-shutdown default сохраняется.
- **Проверка/готово:** executing CPU job/callback/compiler блокирует checkpoint; на checkpoint Maps/native/release ещё не выполнены; hung/unknown не success. No real GPU-loss injection требуется для CPU ordering test.
- **Safety/docs:** не дублировать state machine и не объявлять CPU status GPU-complete; сохранить stop/resume contract полного IG11.

### GM-10. Terminal status transport через boot и loop

- **Цель/риск:** boot/runtime catches и прежний crash abort могут swallow failure; `metroid_main` теряет RsMain return.
- **Зависимости:** GM-09. **Файлы:** `src/MetroidPrime/main.cpp`, platform/game wait safe-points, internal token declaration.
- **Решение:** encompassing owner scope и typed catch §4.2, token только после CPU checkpoint; inner generic catch rethrow; nonzero до platform. Все relevant waits обязаны завершить обычный путь при terminal, не продолжить void-call как success.
- **Проверка/готово:** boot partial-init и loop terminal transport, token не caught как runtime_error, owners остаются живы до checkpoint; unknown/hung case не unwinds. Запрет throw через C/noexcept/worker проверен по затронутым call sites.
- **Safety/docs:** никакого global exception redesign; таблица safe-points и return propagation.

### GM-11. Terminal owner cleanup по milestones — разделён на GM-11a/GM-11b

Исходные дизайн и критерии D9/§4.2 сохранены; пользователь утвердил разделение карточки, затем 2026-11-17 — узкое расширение GM-11b для pending-DVD lifetime. Каждый подшаг закрывается своей проверкой, документацией, commit/push.

#### GM-11a. Read-only guards/query seams

- **Цель/риск:** после закрытия FIFO нельзя разрушать owners с actual room/resident command-producing obligations; инертные retail placeholders не являются такими obligations.
- **Зависимости:** GM-08/GM-10. **Файлы:** room-env/geo/liquid implementation+headers, producer+processor resident registries, узкий H700 query bridge и CPU tests.
- **Решение:** side-effect-free predicates фактических future/queue/worker/resource states, обе resident registries; без Reset/Free/enqueue или общего режима ignore-GX. Проверять только после CPU checkpoint; пока typed catch удерживает owners.
- **Проверка/готово:** inert/empty разрешены, pending/nonempty и producer/processor mismatch отвергаются; query не меняет ownership. Actual nonempty unsupported obligation — fail-stop, не silent skip. Runtime/device claims не ставятся.
- **Safety/оценка/docs:**1–2ч, no device; rollback собственных изменений без удаления partial. Guards/read scope и результаты записать здесь/в прогресс.

#### GM-11b. Milestone cleanup и terminal return

- **Цель/риск:** освободить только фактически созданных retail owners после CPU joins, без новых graphics commands и преждевременного GPU release.
- **Зависимости:** GM-10/GM-11a. **Файлы:** game main/terminal policy, CFDk и установленные retail destructor seams/tests; согласованное расширение — `CWorld`/`CDummyWorld`, `CResFactory::SLoadingData`, DVD request retirement и focused CPU tests.
- **Решение:** milestones вне try, actual guards после checkpoint **до throw/destruction**, clear aliases/retire game owners+subsystems/globals, final no-command delayed flush/ARAM, return1 через RsMain/metroid_main; platform resume существующего полного IG11. Unknown удерживает необходимые holders, не fake release.
- **Проверка/готово:** partial boot, уже уничтоженный arch/globals, failure guards, no enqueue after terminal seal, сохранённые DMA/in-flight waits и nonzero propagation. Pending DVD request завершает безопасную отмену/drain, включая callback return, **до** освобождения destination; также проверить completed/null/transferred ownership и ARAM wait. Каждый created owner released по доказанному пути либо retained в fail-stop.
- **Safety/оценка/docs:**1–2ч; более широкий destructor redesign или unsupported retail enqueue — stop/replan. Обновить этот план/прогресс; separate commit/push. GPU/device не запускать.

### GM-12. Skinning waits и allocation failure

- **Цель/риск:** busy-wait ring token на main может остановить present/progress; failure не должен установить неверный workspace.
- **Зависимости:** GM-10/GM-11b. **Файлы:** `src/Kyoto/Animation/DolphinCSkinnedModel.cpp`, связанные headers/tests; SkinPool только если конкретный call site требует status propagation.
- **Решение:** §4.3, progress/health вне locks; ordered token/wrap, invalid-size guard, pending/terminal до pointer assignment; running jobs retain inputs.
- **Проверка/готово:** pending→actual token completion, wrap/reclaim, impossible size, terminal без allocation/workspace use, cancelled waiter не освобождает running data. Не immediate-token mock вместо FIFO semantics.
- **Safety/docs:** ring32MiB и scheduler не менять; записать token invariants и реальные failure paths.

### GM-13. Локальная проверка game/lifecycle diff

- **Цель/риск:** H700 guards не должны сломать upstream core/GX или корректность освобождения.
- **Зависимости:** GM-04…GM-12. **Файлы:** затронутые existing tests/manifests; production changes только correction уже выбранных контрактов.
- **Решение:** explicit targeted suites из §7, полный compile `aurora_core`+`aurora_gx` non-H700, root game ARM64 build; no GPU в container, не all-platform matrix.
- **Проверка/готово:** relevant CPU/source tests и оба guard builds PASS, actual counts/commands сохранены. Meaningful tests не отключены. Device behaviour остаётся unknown.
- **Safety/docs:** нет unrelated test fixes; fresh outputs не переиспользуют чужие конфигурации. Build waits отдельно.

### GM-14. Изоляция data и settings без переписывания IO

- **Цель/риск:** одного MP_USER_PATH недостаточно для cards, implicit migrations могут прочесть старые данные.
- **Зависимости:** GM-03/GM-08. **Файлы:** существующие `platform/include/port_paths.h`, settings/path tests и будущая package layout specification; production IO лишь если тест обнаружил нарушение нового isolated contract.
- **Решение:** отдельный physical executable dir + same-path user dir, new-install baseline, quick-resume/share OFF; literal disc argument. Не включать old loose-data рядом.
- **Проверка/готово:** temporary two-install fixtures: settings/cards/cache не пересекаются, no old autodiscovery/migration; existing atomic/identity tests PASS; `port_settings_roundtrip` реально исполняет game selftest без GPU.
- **Safety/docs:** не открывать личные cards/saves и не менять serial; схема package dirs и first-run policy.

### GM-15. Ports launcher полной игры

- **Цель/риск:** корректный executable не равен корректному muOS invocation/audio/data path.
- **Зависимости:** GM-05/GM-14, D8/D10. **Файлы:** новые launcher/template в `packaging/h700/`, тонкие package helpers в `tools/h700_game/`.
- **Решение:** фиксированный new install path, две SDL env, sanitized H700 overrides, one quoted image arg, own user/cache/log path; conditional PipeWire runtime, preserved caller audio configuration. Сохранять frontend-exported `SDL_ROTATION`, `SDL_HQ_SCALER`, `SDL_BLITTER_DISABLED`, `SDL_ASSERT`, `SDL_GAMECONTROLLERCONFIG_FILE` по контракту существующего manual launcher, без безусловной перезаписи. Lock только своей установки, traps cleanup; не завершать menu/чужой процесс автоматически.
- **Проверка/готово:** shell/static + mock child tests: spaces в path, missing/unreadable/not-regular image, conflicting overrides, existing lock/exit code, socket absent/present, сохранение переданных frontend SDL variables. Никаких firmware writes/remote deployment в helper.
- **Safety/docs:** пользователь выбирает разрешённый absolute disc path перед device-окном; launcher не ищет старый install. README с реальным использованием после проверки, не PortMaster submission.

### GM-16. ABI, provenance и licenses настоящего пакета

- **Цель/риск:** slice artifact audit не распространяется автоматически на game ELF/dependencies.
- **Зависимости:** GM-13/GM-15. **Файлы:** reuse audit/package helpers, `packaging/h700-notices/`, `tools/collect_licenses.sh`, новый package manifest/report.
- **Решение:** §3.1 audit полной цепочки, bundled SDL only по установленной policy; complete notices через существующий collector, matching pinned Dawn primary texts. Source/input/image/config hashes и build commands; никаких игровых данных.
- **Проверка/готово:** artifact architecture/interpreter/NEEDED/version/import report, package content/licenses/hash checks; отсутствие container EGL/GLES/SDL2/libmali и private paths. Неполный firmware exact-symbol evidence явно listed, не замаскирован PASS.
- **Safety/docs:** unresolved loader requirement блокирует deploy до согласованного loader check, не install system libs. Обновить ABI report только по новым facts.

### GM-17. Release candidate и маршрут приёмки

- **Цель/риск:** устройство должно проверять один идентифицированный full-game artifact, не incremental смесь.
- **Зависимости:** GM-16; согласованные disc revision/container и доступ к маршрутам §8.
- **Файлы:** build/package recipe, новая обезличенная запись приёмки; production code не менять.
- **Решение:** второй fresh offline build по опубликованному рецепту; сравнить inputs/config/dependency manifests, объяснить возможную nondeterministic binary metadata, package hash каждого artifact. Reproducible recipe не называть bit-identical без сравнения hashes. Выбрать один RC для device-окон.
- **Проверка/готово:** same source/pins/options снова compile/link/audit/CPU checks; packaged RC hash; no-init selftests. Финальный список сцен/кнопок/ожидаемых эффектов готов **до** запроса device access, late-save access не оставлен «потом».
- **Safety/docs:** никаких загрузок saves/ISO в Git. Если нет legal доступного late route, согласовать ожидание прохождения или private test copies; не урезать coverage молча. Оценка1–2ч подготовки/проверки, два cold builds могут ожидаться существенно дольше.

## 6. Device policy: отдельно разрешаемые окна

Все окна — через правильный Ports invocation с пользователем перед экраном, без SSH fullscreen поверх живой menu/session. При необходимости SSH согласовать endpoint/host identity/разрешённые команды и deploy отдельно. Device checks не начаты этим документом.

- Перед каждым окном: аппарат/firmware, package hash, выбранный образ, собственная isolated installation/user directory, отсутствие чужой активной игры, достаточно места на storage. Новая установка создаётся **один раз** и остаётся по стабильному пути; между окнами сохраняем её card/settings/cache. Новыми делаем только каталоги отчётов; разрешённые late-save copies подготавливаются отдельным явным действием, не сбросом user data. Использовать уже известные firmware baselines, не повторять общий inventory ради inventory. Проверить только затрагиваемые libraries до/после, без их замены.
- Запуск пользовательский/наблюдаемый. Все logs ограничены размером и остаются private/ignored до очистки; environment, paths, saves, ISO, core не публиковать. Нет forced backend/audio fallback, dummy audio, автоматического пропуска всех роликов или smoke mode.
- **Time box:** окно A до20мин, B до30мин, C до30мин активной device-сессии; это предел эксперимента, не kill timeout. Время естественного прохождения к late content отдельно согласуется. По окончании — PortMenu/Quit в безопасной точке, не power-cut.
- **Hang bound:** 60с без наблюдаемого progress — FAIL/partial и диагностическая остановка эксперимента; normal Quit свыше30с — FAIL/partial. Это deadlines для результата, **не разрешение убить процесс**. Во время CARD operation не посылать timeout/SIGKILL и не перезагружать устройство. При зависании сохраняем наблюдение, удерживаем owners там, где CPU/GPU use неизвестно; отдельно согласуем принудительное завершение с риском для новой test card. Автоматический destructive watchdog из IG-demo к save-capable игре не переносим.
- Terminal fault injection допускается локально на CPU seams с временными fixtures; на device не вызываем loss/OOM/forced kill ради доказательства failure policy. Естественный failure не скрываем; no-return driver call нельзя обещать исправить C++ exception.
- Headroom gate96MiB и критерии §3.2 применяются ко всем окнам. При warning остановить нагрузку/выйти безопасно; не продолжать ради screenshots. Никаких clocks/governors/swap/mixer/system-service изменений.
- Rollback: штатный выход, удостовериться в отсутствии своего процесса/lock, убрать только свою новую launcher/data installation **лишь по согласованию**, сохранив новые test saves по желанию пользователя. Не удалять оставленный по просьбе IG launcher. Старую игру/settings/cards не трогаем и не «восстанавливаем» из ненужного backup.

### GM-18. Окно A — первая настоящая игра

- **Цель/риск:** loader/kernel4.9/nod/display/pad/audio не доказаны демкой.
- **Зависимости:** GM-17 и отдельное разрешение A. **Объём:** выбранный RC, одна новая установка, один legal disc; до20мин.
- **Процедура:** Ports launch → logo/movie с реальным звуком → file select → new game → управляемая сцена/движение/стрельба → PortMenu → Quit. Проверить physical image orientation/цвет/читаемость HUD, controller без keyboard, music/SFX/movie audio, нет silent fallback; записать actual backend/errors/exit и memory baseline.
- **Готово:** игровая сцена, звук и управление подтверждены пользователем, readable disc callbacks фактически работают, штатный exit, возврат в Ports, нет validation/GL/EGL/device-loss errors. Одно меню не PASS. Это не все effects/save/load.
- **Safety/docs:** §6; failure → минимальный root-cause report, не новый nod backend или бесконечный rerun. Результат в отдельный game acceptance report, кратко в прогресс. Оценка1–2ч на подготовку/разбор, device time отдельно.

### GM-19. Окно B — lifecycle, RAM и настоящий save/load

- **Цель/риск:** игра может boot-иться, но unsafe reset/tail, нехватка RAM или неверная card изоляция проявятся позже.
- **Зависимости:** GM-18 PASS, отдельное разрешение B; заранее выбранный достижимый маршрут к station/area transitions/reset. **Объём:** до30мин, тот же RC.
- **Процедура:** переходы между несколькими rooms с возвратом по одному маршруту дважды; pause/map/PortMenu; один штатный reset в file select/пересоздание archSupport без нового generation; продолжение игры. Save station → дождаться подтверждения записи/исчезновения pending card IO → normal Quit → новый process → file select/CARD load → управляемое gameplay в сохранённой точке.
- **Готово:** ordering/lifecycle diagnostics согласованы с §4, game owners не удерживаются/не растут бесконтрольно; RAM §3.2 PASS; реальные card files только в new user dir, identity/atomic-write errors отсутствуют, user подтвердил loaded progress. Settings/bindings persist в новом процессе. Quick-resume OFF; no savestate substitute.
- **Safety/docs:** §6, не копировать personal save без разрешения; не reset/kill во время write. Отчёт содержит маршрут, sampled minimum/HWM, growth/trim и limits, не обещание всей кампании. Оценка1–2ч + отдельно окно.

### GM-20. Окно C — retail correctness и полное управление

- **Цель/риск:** shader demo/ранняя сцена не доказывают visors/EFB/materials/menus/input coverage.
- **Зависимости:** GM-19 PASS, подготовленная в GM-17 карта доступных сцен, отдельное разрешение C. **Объём:** до30мин на известные сцены, тот же RC; непокрытые пункты остаются pending, не PASS.
- **Процедура:** конечный checklist §8, пользователь видит и слышит результат; screenshots/clips при необходимости только supplementary evidence. Проверить поздние visors/beam effects на разрешённых private test copies или naturally reached save, не включать mods/smoke cheats ради снятия флажка.
- **Готово:** все строки §8 имеют observed PASS/обоснованный N/A для выбранного retail release; любой missing scene/access — pending и блокирует полную приёмку. Наличие логов без physical confirmation недостаточно.
- **Safety/docs:** private copies никогда не пишутся поверх личных saves; только разрешённая копия в собственной установке. Source effect defect — конкретный минимальный fix+regression в выбранной архитектуре после согласования новой карточки, не заранее обещанное «починить все эффекты за2ч».

### GM-21. Итоговый пакет и граница переноса

- **Цель/риск:** не выдать испытанный частичный artifact/демку за готовую новую игру.
- **Зависимости:** GM-18…GM-20 PASS **одного RC**; любые code/package changes требуют повторения затронутых gates, не автоматического зачёта старых runs.
- **Файлы:** game acceptance report, README, `docs/NATIVE_PORT.md`, прогресс, проверенные package/build instructions. Production code не менять.
- **Решение/готово:** manifest/hash ровно проверенного пакета, reproducible build evidence, полный checklist/ограничения, безопасный manual install/uninstall; user acceptance. Frametime/performance baseline сохранён без обещания30FPS; будущие идеи только в optimization notes. Невыполненный критерий явно блокирует статус полного переноса.
- **Safety/docs:** обезличить logs и проверить package на отсутствие assets/saves/secrets; старую установку не заменять автоматически. Оценка1–2ч. Не пересобирать «финальный» binary после приёмки и не объявлять непроверенную пересборку тем же artifact.

## 7. Минимальная локальная валидация

Точные команды/regex записываются по actual configure/`ctest -N` в своей итерации; здесь указаны **существующие targets/entries**, не вымышленные команды нового build wrapper.

| Изменение | Минимальные относящиеся проверки |
| --- | --- |
| Root build/providers | Настоящий `metroid_prime_port`, whole MusyX в нём, provider/hash/ABI audit; no-init version; второй fresh build GM-17 |
| Lifecycle/waits/frame | Существующие `render_worker_tests`, `gfx_recording_tests`, `gx_fifo_tests`, `bloom_readback_slots_tests`; относящиеся cases GenerationLifecycle/PipelineWaitTest/MapCompletionTest + новые game-boundary/terminal/skinning regressions. Это разные target/suite/case names, выбрать actual CTest inventory, а не выдать все за build targets |
| Non-H700 guards | Полный compile `aurora_core` и `aurora_gx` в отдельном non-H700 output; не несколько случайных TU вместо библиотек |
| Disc/path isolation | `port_disc_tests`, `port_disc_verify_tests`; temporary fixtures, отсутствие чтения чужого install. Real disc/kernel остаются device gate |
| Controller/settings | `port_input_gate_tests`, `port_input_engine_tests`, `port_input_bindings_tests`, `port_input_user_presets_tests`, `port_input_map_tests`, `port_input_settings_tests`, relevant InputFirstPort cases |
| Audio/cards/atomic IO | `port_audio_math_tests`, `port_card_tests`, `port_gci_tests`, `port_save_convert_tests`, existing relevant atomic-IO cases; temporary files only |
| Settings roundtrip | CTest entry **`port_settings_roundtrip`**, НЕ build target: использует game executable `--settings-selftest` с isolated MP_USER_PATH; exits до graphics |
| Launcher/package | Missing/mismatched input, quoted spaces, invalid image/lock/conflicts, notices/content/hash checks; не generic новый orchestration framework |

Не строить все тесты всех платформ ради отчёта. Не запускать GPU в image. `ctest -L port` не означает whole Aurora PASS. Если нужный CPU-test backend не способен представить H700 feature, проверяем контракт на существующем seam; не меняем Dawn/лимиты ради новой матрицы. Это не разрешение ослаблять meaningful tests.

## 8. Конечный checklist реальной игры

Перед GM-17 для каждой строки зафиксировать конкретную доступную сцену/действие и ожидаемый результат по оригинальной игре, без private paths. Поздние сцены требуют заранее выбранного законного доступа. В отчёте: RC hash, disc region/revision/container, scene/route, observed result, user confirmation, ограничения. Нельзя назначить N/A эффекту только потому, что к нему ещё не дошли.

| Область | Обязательная наблюдаемая проверка |
| --- | --- |
| Boot/display/UI | Logo и movie, file select, HUD/reticle/fonts/glyphs, pause/inventory/map, читаемый PortMenu 640×480/4:3; нет flip/black frame/color corruption |
| Materials/light | Оpaque/alpha-tested geometry, lighting/lightmaps, animated/skinned actors, weapon, particles, water/transparency, reflections и retail bloom; no missing surfaces после pipeline trim/возврата |
| EFB/visors | Scan visor с читаемым overlay/text и scan progress; Thermal и X-ray в характерных сценах; штатные EFB-dependent visor/beam/post effects, без отключённых retail функций |
| Gameplay pad | Movement/aim, fire/charge/missile, jump, lock-on/scan, morph/bombs, beams/visors, pause/map. Всё без keyboard; оба порядка Back+Start; navigation/close/Quit; bindings persist |
| Audio | Music, SFX, positional balance, movie sound, pause/resume и выход без треска/потери stream; actual audible confirmation + available MP_AUDIO_STATS, no dummy/silent fallback. Rumble отдельно observed/unsupported, не доказан controller name |
| Lifecycle | Area transition/return, reset/new owner same generation, normal exit after real work, новый process и повторный launch; no device-loss/validation/EGL/GL errors/unknown teardown |
| Saves | New isolated card → actual station write → normal quit → новый process → file select load → сохранённое gameplay. Существующие personal cards не затронуты; quick-resume OFF |
| RAM/pacing | Маршрут дважды, samples/HWM/growth/trim и headroom gate; приблизительные frametimes/observable stutters как baseline, без суммирования overlapping thread times и без FPS PASS по screenshot |

Эта приёмка подтверждает **играбельный перенос на проверенном маршруте и release**, не всю кампанию, все контроллеры, HD/PBR, optional photo/profiler/readback paths, другие muOS или30FPS. Если дополнительный эффект реально нужен выбранной retail сцене, его нельзя объявить optional только из-за дороговизны.

## 9. Неизвестности, решения по исходам и стоп-условия

| Что ещё неизвестно | Минимальное evidence / решение |
| --- | --- |
| Full root game совместим с exact image? | GM-01/02 compile/link: PASS → продолжать; новый toolchain/dependency нужен → остановка и новая договорённость, не install |
| Реальные game callbacks выходят за healthy CPU boundary? | GM-06…08 ordering/call-site review и tests; конкретный late userdata → stop/replan. Universal GPU-idle «на всякий случай» не вводим |
| Terminal retail cleanup укладывается в no-new-graphics? | GM-09…11 explicit milestones/empty-resource assertions; фактический nonempty unsupported obligation → blocker, не ignore |
| nod actual SDL stream работает на kernel4.9? | GM-18 real legal disc reads/seeks: успех → без нового IO backend; failure → определить actual operation/error, отдельно согласовать необходимый fix |
| Достаточен RAM budget при настоящих scenes? | GM-18…20 measurements и §3.2 gate: PASS → baseline; fail → остановка, факты, варианты. Не уменьшать эффекты/sizes и не менять thresholds автоматически |
| Полны ли hardware effects/pad/audio/save? | §8 physical checks; missing/failure → partial report и конкретная карточка исправления/доступа, а не общий rewrite |
| Driver hang/device loss даёт безопасный normal release? | Это не гарантируется. CPU/GPU Unknown удерживает необходимые owners, fail-stop; отчёт FAIL, отдельное разрешение на forced termination |

**Общая остановка:** изменение architecture/pins/image/security/correctness/acceptance; исчерпанный time box карточки с новым значимым решением; memory/loader/graphics/card failure; отсутствие требуемого device разрешения или late-scene access. Сохранить partial, объяснить fact vs inference, предложить варианты и дождаться согласия. Не продолжать параллельно другие шаги переноса при необходимом изменении плана.

## 10. Оценка и порядок утверждения

**Историческая оценка редакции1:**21 исходная карточка, GM-11 разбита на два согласованных подшага; ориентир **около 30–42 часов активной работы всего первоначального плана**, не оставшегося после GM-11b объёма при отсутствии новых фундаментальных блокеров: local integration GM-01…17, три отдельных device-окна GM-18…20, итог GM-21. Это не гарантия срока: cold full builds/очередь пользователя/прохождение к late scenes/новые согласуемые исправления отдельно. GM-02 и GM-11 особенно чувствительны к ещё не измеренному объёму; их completion не гарантирован одной карточкой.

Утверждённый порядок: D1…D11 и **локальные GM-01…GM-17** выполняем последовательно с commit/push после завершённых проверок. Перед устройством показать RC evidence, конкретные scenes и запросить окно A; затем отдельно B и C. GM-21 закрывается только после реальной приёмки. Пользователь выбрал полный план, не ограничение первым блоком GM-01/02.

**Текущий статус:** GM-01…GM-17 LOCAL PASS; user выбрал GameCube USA1.00/ISO уже на свободной приставке (actual path/metadata ещё не читались). Необходимый остаток [редакции3](H700_GAME_REMAINING_PLAN.md) утверждён; результаты и история записаны там. Две fresh package build прошли, выбран [RC3 с immutable hash](H700_GAME_RC.md). Ранний маршрут A — New Game→Frigate Orpheon/Exterior Docking Hangar→move/aim/fire→normal Exit. Далее — отдельное permission на SSH/deploy/loader check и один manual game run окна A. Редакция2 не действует. Device actions не разрешены общим approval.

## 11. Проверка черновика до утверждения

Выполнены два независимых read-only review: lifecycle/CPU checkpoint и build/safety/acceptance. После уточнения D1 scope, сохранения одной установки между окнами и frontend SDL environment contract блокеров в проверенном design не осталось. Отдельно закреплена проверка unsupported cleanup obligations до throw/destruction. Это не верификация ещё не написанного кода; последующее утверждение пользователем записано в начале документа.

Проверены локальные Markdown link targets, уникальность/порядок 21 GM-карточки, whitespace новых документов и diff. Production files не изменялись; build/test/device actions не выполнялись. Существующий whitespace в upstream README вне изменённых строк не исправлялся. Чужие `.DS_Store` сохранены; commit/push подготовки не выполнялись.

## 12. Результаты реализации

### GM-01 — LOCAL PASS: root configure, не compile/link

Добавлены `cmake/h700/GameInitialCache.cmake`, `GameBuild.cmake`, shared `VerifyProviders.cmake` и тонкий `tools/h700_game/build.py` поверх существующего helper. Root H700 branch импортирует GLOBAL nod до Aurora и генерирует actual game/SDL artifact paths. Provider assertions integration driver вынесены без изменения policy; отдельного manifest/build framework нет.

Проверенные команды из корня (output всегда новый):

```sh
python3 -B tools/h700_game/build.py --check-inputs
python3 -B tools/h700_game/build.py --configure-only --output build/h700-game-gm01 --jobs 2
python3 -B tools/h700_integration_driver/build.py --configure-only --output build/h700-integration-gm01-regression --jobs 2
python3 -B -m unittest discover -s tools/h700_game -p 'test_build.py' -v
```

- Все14 archive hashes PASS; exact image identity/native Linux ARM64 подтверждены. Fresh offline root configure/generate PASS, no GPU/disc/deploy. Logs/environment/commands и cache — в ignored `build/h700-game-gm01/`.
- Compile database:1592 entries, в том числе642 `mp_game` units и настоящий `src/MetroidPrime/main.cpp`; MusyX включён. Все game units сохраняют signed-char/no-strict-aliasing/wrap flags. Embedded resources/bundled deps ON, stub/smoke/tests/TLS OFF по approved profile. Это сгенерированные команды, **не выполненная компиляция**.
- `providers.txt`: SDL3 source `/out/inputs/sdl`, zlib-ng source, zstd static source; Dawn и nod — staged static archives. Game target/artifact — `metroid_prime_port`, не driver. Локальные private absolute paths в versioned report не включены.
- 5/5 новых profile tests PASS на host;10/10 существующих bootstrap tests PASS на host. Полный integration-helper suite **30/30 PASS в утверждённом Linux image**, включая package/launcher safety. На macOS тот же полный suite имел4 failures/errors и1 skip: отсутствует prerequisite `setsid`; Linux-only launcher fixture не адаптировали и tests не ослабляли. Integration fresh configure также PASS.
- Independent read-only code review: blockers не обнаружены. Общий slice cache/pins/image не изменены; production runtime ещё не затронут. Этот результат не ABI/runtime/device PASS; последующий compile/link зафиксирован отдельно ниже.

### GM-02 — LOCAL PASS: первая полная compile/link

Game wrapper разрешает полный build через тот же helper. Единственная post-build executable check — **`--version`**: `platform/main.cpp` возвращается до settings/game/graphics init; обычный game запуск и `--help` не выполнялись. Integration driver сохраняет прежний `--help`, package mode не изменён. C++/CMake compatibility fixes не потребовались, manifests/pins/image неизменны.

```sh
python3 -B tools/h700_game/build.py --output build/h700-game-gm02 --jobs 2
```

Fresh offline configure18.172s, compile/link494.098s, все команды exit0. `metroid_prime_port --version`: `Metroid Prime native port 0.46.0 (source-snapshot)`, exit0 за0.035s. Строка `source-snapshot` не является доказательством embedded Git revision; source/provenance manifest RC ещё предстоит в GM-16/17.

| Artifact | Bytes на диске (не RSS) | SHA256 |
| --- | --- | --- |
| unstripped `metroid_prime_port` | 228548416 | `e85b356f16e0637b88bab2fb6727861cb62913ae4be66d223aa6988da914549e` |
| matched `libSDL3.so.0.5.0` | 7263800 | `8df6e362b508d44cad5a253b4f7533ec4d8a976a8e49f49320d2baa42846a13b` |

Actual executable и SDL — ELF64/AArch64, loader `ld-linux-aarch64.so.1`; `NEEDED` game: firmware EGL/GLES, SDL3, libc/libm/libstdc++/libgcc и loader. Полные imports/version nodes/hashes/command timings — ignored `build/h700-game-gm02/{elf-metadata.json,commands.json,result.json}`. Это **ELF metadata, не полный firmware ABI gate**, не package/deploy/device PASS.

Проверки:10/10bootstrap +5/5game-profile tests на host;30/30integration helper +5/5game-profile tests в exact Linux image. Independent read-only review без blockers; diff/link/документальные проверки выполнены. Следующий шаг GM-03: deterministic H700 retail runtime profile. На момент GM-02 бинарник сохранял существующие game defaults и неполную game lifecycle интеграцию; запускать его на приставке преждевременно.

### GM-03 — LOCAL PASS: H700 retail defaults и startup policy

- Новый `platform/include/port_h700_profile.h` — testable CPU policy: actual AuroraConfig получает hardware OpenGLES/native640×480, MEM1=24MiB, frame1/storage0-following **unpinned**, resident0, lazy/no-seed, без старого embedded seed. Наличие `MP_FRAME_BUFFERS`/`MP_STORAGE_BUFFERS` (включая `1` и пустое значение) отвергается, а не превращается в полную pinned allocation. Defaults других платформ не менялись.
- H700 settings initializer работает **до INI**, один раз и без dirty/save:4:3/render1/MSAA1/anisotropy1; mods/HD/per-pixel extras/interpolation/dynamic resolution/sun shadows/sharp scan OFF; Modern+twin-stick, cap30/sim60/adaptiveOFF, fullscreen, quick-resume/share-card OFF. Existing INI сохраняет приоритет, getters не принуждают эти значения каждый кадр. Retail projected shadows/visors/EFB/audio не отключены. Startup baseline и обычные settings logs различают defaults/file/env.
- Fixed SDL sdl2→mali выбирается только при отсутствующих переменных; frontend display/controller variables не затрагиваются. Explicit wrong backend/display, diagnostic/shader/thread/cache overrides отклоняются до normal startup. Presence-sensitive staging pins проверяются отдельно. Реальный `MP_GPU_SELFTEST` включён в guard после independent review.
- После settings workarounds guard проверяется повторно; unsafe toggle config отвергается, `skip_validation`/`disable_robustness` обязательно входят в disabled list, поэтому NDEBUG не выключает validation/robustness. Отключение multi-thread GL toggle запрещено. Environment Dawn toggles не могут переопределить это позже.
- Старый GLES crash marker не запускает AUTO fallback и остаётся для diagnosis; новый marker создаётся лишь после успешных profile checks. Actual init-result/partial-init gate остаётся GM-05. Pressure thresholds остаются прежними до отдельной GM-04, этот шаг не объявляет RAM device PASS.

Проверки: fresh offline root configure (`build/h700-game-gm03`), полный compile/link actual game + `port_h700_profile_tests`, **5/5 CTest PASS**: allocation/override/toggle policy, H700 settings profile (**27ключей defaults + existing overrides**), actual executable early rejection (**12cases**, byte-identical existing INI/no new files), `port_version`, `port_settings_roundtrip`. Standalone policy test также PASS на host. Компиляция трёх изменённых game TU без `AURORA_H700` PASS; Aurora sources не менялись, это не новый full non-H700 game/runtime PASS.

В staged container выполнены:

```sh
cmake -S /src -B /out/build -G Ninja -C /src/cmake/h700/GameInitialCache.cmake -C /out/Inputs.cmake -DMP_BUILD_TESTS=ON -DBUILD_TESTING=ON
cmake --build /out/build --target metroid_prime_port port_h700_profile_tests --parallel 2
ctest --test-dir /out/build -R '^(port_h700_profile_tests|port_h700_settings_profile|port_h700_profile_startup|port_settings_roundtrip|port_version)$' --output-on-failure --timeout 30
```

Ни GPU, ни устройство не запускались. Logs/results — ignored build; independent read-only review после исправления guard без blockers. GM-03 commit/push: `119418fae`; новое разрешение на локальные карточки не требуется.

### GM-04 — LOCAL PASS: pressure policy и bounded RAM diagnostics

H700 pipeline/replacement thresholds192/128MiB теперь заданы явными constants и логируются как policy, не измеренный headroom. Environment overrides thresholds/eviction отвергаются, включая отключение через0; actual startup rejection suite расширен до16cases. Другие платформы сохраняют прежние defaults.

`PortMemoryLog::Tick` использует прежний sample1s, H700 пишет не чаще1строки/sample, без catch-up/thread/extra polling loops. Добавлены kernel RSS `VmHWM`, sampled minimum `MemAvailable` и status `sampled-ok`/`below-96MiB`/`unknown`. Неудачное чтение sampled данных не стирается поздним успешным чтением; известный breach также не стирается. Нет auto-kill/exit или изменения CARD handling. Минимум не видит провалы между samples, RSS/HWM не равны всей shared GPU/system RAM. Staging/ring/cache limits не менялись; existing growth/allocation diagnostics сохраняются.

Pressure алгоритм не переписан: poll5s, repeat30s, moderate3600/severe1800 idle frames; texture logic и Aurora GLOBAL/pinned eviction неизменны. Новый CPU test включает production implementation с узкими Aurora stubs и проверяет реальные threshold decisions/equality/timing, byte conversion, unknown/disabled cases, `/proc` parser, sticky gate и Tick cadence. Это **не execution test реального eviction/GLOBAL retention**; сохранность его алгоритма проверена по отсутствию соответствующих source changes.

Проверки: полный incremental game build на staged output GM-03 + оба H700 CPU targets, **6/6 CTest PASS**,27settings keys/16actual startup rejects, non-H700 compile изменённых main/memory TU PASS. Первый test run остановился на `tmpfile()` fixture: glibc игнорирует TMPDIR для этого API, `/tmp` read-only. Fixture заменён на `fmemopen` для того же FILE parser, sandbox не расширялся, meaningful checks сохранены. Initial failure и recheck logs остались в ignored `build/h700-game-gm03/gm04-*`; recheck Commands records сохраняют actual commands/timeouts. Independent review без blockers.

Ни GPU, ни device RAM route не проверялись; sampled-ok не означает acceptance PASS. GM-04 commit/push: `2a231b2a7`.

### GM-05 — LOCAL PASS: actual initialization gate до game owners

Actual `AuroraInfo` больше не игнорируется в H700 main: OpenGLES + window + native640×480 и main-only `initialization_ready()` проверяются сразу после init, до post-init GX/settings и `metroid_main`. Readiness query требует Running lifecycle без failure, gfx/FIFO/UI milestones, native presenter/import readiness и render health. Это startup safe-point, не универсальный runtime-health API.

При отказе game owners не создаются. `PollInitFailureShutdown` использует существующий request/poll IG11; один app-side запрос, без второго `aurora_shutdown`. Только Released/FailedReleased разрешают диагностический return1. Unknown или Pending после30s диагностического deadline → fail-stop с сохранением main stack/runtime, без unwinding, `_Exit`, retry или automatic kill. Synchronous driver call не может быть прерван C++ deadline; release не подделывается. Normal game exit/owner destruction остаются задачами GM-06…11.

Верификация: incremental полный H700 game/core/GX compile/link и **7/7 CPU/no-init CTest PASS** (добавлен `port_h700_init_tests`). Injected policy cases: actual-info acceptance, partial/no-owner init outcomes, pending→Released/FailedReleased, Unknown, timeout с неизменным Pending, exactly-once request/no second cleanup. Это не исполнение настоящего GPU partial-init failure. В отдельном fresh output `build/h700-game-gm05-nonh700` выполнен **полный compile `aurora_core` + `aurora_gx` с AURORA_H700=OFF**, не выборочные TU. Image/pins не менялись, GPU/device не запускались; independent lifecycle review без blockers.

Actual commands/timeouts сохранены в ignored `build/h700-game-gm03/gm05-*` и отдельном non-H700 output. GM-05 commit/push: `e5fdeac74`; дополнительное согласование локальных итераций не требуется.

### GM-06 — LOCAL PASS: healthy game-owner boundary

Добавлен H700-only `CGraphics::PortGameOwnerBoundary()`: проверка ongoing health → окончание открытого frame ровно один раз через существующий latch/`AuroraFrameEnd` → повторная health → actual status-bearing FIFO drain с unpublished tail → ordered render synchronize Completed → финальная health. Полный `EndScene()` с его GX/UI actions не вызывается. Новые Aurora `game_work_healthy`/`synchronize_game_work` main-only, не используют startup-only `g_initialFrame`; unfinished recording и terminal/rejected outcome не считаются completion.

Root admission не закрывается; compiler joins, shutdown, universal GPU-idle/native-fence retirement не добавлены. `false` требует сохранить owners. Existing no-command `CFrameDelayedKiller::FlushAllAllocations` документирован как flush только после доказанной CPU/destructor-tail boundary; нового дублирующего free API нет. Reset/exit wiring ещё впереди.

Проверки: full H700 game/core/GX build; **8/8 game CPU/no-init CTest,45/45 render/lifecycle/wait tests,262/262 FIFO tests PASS**. Новый actual worker regression удерживает возврат after-submit-подобного callback и отдельно destruction capture; sync должен дождаться обоих, roots остаются открыты и следующая работа принимается. Первый run выявил test-cleanup assertion: healthy hook проверял roots уже в shutdown. Hook снимается только после тестовых задержек в RAII cleanup; проверка открытого admission в boundary не удалена. Initial/recheck logs сохранены в ignored `gm06-*`. Полный non-H700 core+GX compile PASS; GPU/device не запускались. Independent lifecycle review production blockers не нашёл.

GM-06 commit/push: `5e184e0c5`. Локальные шаги продолжаются без нового approval.

### GM-07 — LOCAL PASS: same-generation reset owners

H700 reset использует production/test `ResetGameOwner`: pre-dtor boundary → raw alias=null → old archSupport destruction → destructor-tail boundary → existing no-command delayed flush → восстановление clear-FB policy → новый archSupport/alias/audio preload. Новое generation не создаётся, roots/backend/workers остаются живыми. Non-H700 reset сохранён. При false до/после destruction — diagnostic fail-stop с удержанием main scope/remaining owners, без flush/new owner/throw/return; GM-09…11 позднее добавят terminal CPU checkpoint/transport.

Проверки: полный incremental game compile/link, **8/8 CPU/no-init CTest PASS**, non-H700 compile game main PASS. Boundary suite дополнен ordering/destructor-generated queued cleanup/delayed-free и alias/new-owner assertions, включая отказ обеих границ и failure после обработки tail. Final compile/boundary rerun после сохранения clear-FB default PASS. Independent lifecycle review без blockers. Actual reset на приставке остаётся GM-19, не засчитывается CPU tests. Logs/commands — ignored `gm07-*` staged output.

GM-07 commit/push: `0292af263`.

### GM-08 — LOCAL PASS: normal exit и final destructor tails

Healthy retirement helper теперь общий для reset/exit. ArchSupport уничтожается явно внутри живого scope (alias=null), затем его tail drain и no-command flush; отдельная такая граница охватывает ShutdownSubsystems и game globals. Прежний ранний CFDk::ShutDown/GXDrawDone+flush отключён только на H700: final flush идёт после всех соответствующих destructor tails, перед ARAM Shutdown. Loader/disc services остаются живыми до CResLoader destruction.

Platform порядок учитывает SDL ownership: AIPortShutdown (`SDL_AudioStream`) и stop background disc services **до** IG11/SDL_Quit; затем полный status-bearing IG11, лишь Released/FailedReleased разрешают disc close/return. FailedReleased даёт nonzero; Unknown/Pending по30s deadline удерживают main scope/runtime/disc в fail-stop. H700 normal path не вызывает aurora_shutdown второй раз. Poll helper переименован в `PollGenerationShutdown` без изменения поведения; normal/failed init используют один механизм. Non-H700 порядок сохранён.

Проверки: полный incremental game compile/link, **8/8 CPU/no-init CTest PASS**, non-H700 game/platform main compile PASS. Новые ordering/failure cases используют production retire helper и проверяют loader/services lifetime, subsystem/global-generated cleanup, final flush перед ARAM. Independent lifecycle review без blockers; реальный normal exit/GPU/loader execution остаётся device gate, terminal exception transport ещё не реализован. Logs/commands — ignored `gm08-*`.

GM-08 commit/push: `6f7f12d32`.

### GM-09 — LOCAL PASS: terminal CPU checkpoint перед Maps

Новый `poll_generation_cpu_quiescence()` проходит существующие actual FIFO/render/compiler exit acknowledgements/joins и удерживается **до тела Maps и до дальнейшего main progress** на checkpoint. Отдельный `CpuQuiescent` добавлен в конец status enum, старые numeric diagnostics сохранены. Это не Released/GPU-complete. Repeated CPU polls не retire maps/abandoned recorder/native/holders/device/exports; только explicit full poll возобновляет тот же cursor. Если Maps body уже начиналось и осталось Pending, новый флаг исключает ложный checkpoint постфактум. Terminal mode помечает generation failed и запрещает clean restart; default full poll API/semantics сохранены.

Проверки: full H700 game/core/GX build; **8/8 game CPU/no-init CTest,50/50 render/lifecycle/wait tests,262/262 FIFO tests PASS**; полный non-H700 core+GX compile PASS. Добавлены cursor/source-contract cases stop/resume, Pending/Unknown, запрета false checkpoint после Maps entry и numeric-status compatibility. Это CPU/model/source evidence, не GPU device failure injection. Independent lifecycle review без blockers; raw logs/commands — ignored `gm09-*`.

Перед GM-10 узко проверен unwind set root safe-points RsMain: два owning single_ptr должны быть вне boot+loop try, throws только после возврата вложенных calls. Root typed catch сначала удерживает owners; explicit terminal cleanup — GM-11. Нельзя подключать deep throw из произвольного graphics/skinning call без проверки guards: `SPortVisorGuard`/`PortInventoryScope` действительно отправляют GX-команды из деструкторов. Применимость к конкретным skinning waits проверяется в GM-12, не speculative scheduler rewrite. Пока это не заявляется отменой существующих deep busy-waits.

GM-09 commit/push: `5a83894e5`.

### GM-10 — LOCAL PASS: root-only terminal transport

Новые `port_h700_terminal.h`/CPU tests: H700 owners вне encompassing boot+loop try, root safe-points после возврата вложенных calls, typed inner rethrow, outer catch удерживает owners в fail-stop, H700 RsMain→metroid_main return propagation подготовлена. Exact CpuQuiescent/Maps требуется до throw; Pending/Unknown/Released и неверная стадия не разрешают unwind. Token не наследует std::exception/runtime_error. Actual deep throw не подключён; explicit terminal cleanup/return из typed catch остаются GM-11b, а не выдаются за уже работающий exit.

Выполнены полный incremental game compile/link, **9/9 CPU/no-init CTest PASS**, non-H700 game-main compile PASS. Cases проверяют checkpoint-before-throw, partial boot/loop owners alive in catch, inner typed rethrow, отсутствие destruction/unwind при Unknown/timeout и запрет не-main polling. Independent source review root unwind set ранее не нашёл blockers; graphics/skinning command-producing guards этим transport не пересекаются. GPU/device не запускались; commands/logs — ignored `gm10-*`. После согласования split остановка снята; GM-10 завершён до начала GM-11a.

### История остановки: разделение GM-11 утверждено 2026-11-17

Source review GM-11 установил actual write/query scope, который не следует скрывать в одной карточке1–2ч:

1. Retail texture/palette/model/renderer dtors преимущественно освобождают CPU ownership; общий режим «игнорировать GX» не нужен. DMA/replacement in-flight lifetime waits нельзя отключать. Существующий no-command CFDk flush подходит.
2. `CStateManager` destructor вызывает room cleanup; room-env `SetLoadedAreas(empty)` может перейти в RegisterReady/SubmitWorld и `GXDestroyPBR*`, room-geo ResidentKeep публикует `GXPortReleaseResident`. Для liquid empty guard консервативный, отдельный FIFO release не доказан.
3. Полных read-only predicates нет. Нужно проверять actual pending futures/queues/worker-running/resources, не `Areas().empty()` — retail создаёт инертные placeholders. Для resident нужны **обе** registries: producer `sRetained` в GXDispList и processor entries в gx/resident; used_bytes0 недостаточно.
4. Guards должны быть выполнены после CPU checkpoint **до throw/owner destruction**. Непустая неподдержанная obligation означает fail-stop, не silent skip.

Утверждено без смены D9/архитектуры/providers/приёмки:

- **GM-11a (1–2ч):** read-only guards/query seams для трёх room owners и producer+processor resident, CPU regressions на инертные/непустые/pending состояния; catch по-прежнему удерживает owners.
- **GM-11b (1–2ч):** milestones вне try, pre-throw guard wiring, no-new-graphics terminal owner/subsystem/global cleanup, final no-command flush/ARAM и actual nonzero return; full IG11 остаётся platform path, с existing audio/disc lifetime order.

Минимальные затрагиваемые seams: game main/terminal policy/tests; `platform/port_room_{env,geo,liquid}.cpp` и их headers; resident producer/processor + узкий H700 query bridge. Переписывать retail resource dtors или поддерживать optional mod cleanup этим не предлагается. Это изменение **границ итерации**, а не новый renderer или ослабление safety. До согласования реализация/build/tests были остановлены по workflow; GM-10 partial и чужие local changes сохранены. После явного согласия пользователя остановка снята: сначала завершаем GM-10, затем два подшага GM-11.

Deep guards `SPortVisorGuard`/`PortInventoryScope` остаются отдельным вопросом применимости для конкретных skinning waits GM-12. Их существование не разрешает заранее расширять scheduler или вводить deep exceptions.

### GM-11a — LOCAL PASS: read-only room/resident guards

Добавлены H700 `CanDestroyWithoutGraphicsCommands` для room env/geo/liquid. Проверяются actual futures (включая ready/deferred без consumption), queues, worker-running под mutex, tile/registration/dirty-world и ненулевые GPU IDs; geo model/LOD/cluster/light/resident ownership, liquid items консервативно запрещены. Metadata-only retail placeholders разрешены. Geo/liquid queries не создают lazy registries. Idle joinable room worker допустим: без Submit он не читает освобождаемый File, его штатный destructor сохраняет stop/wake/join. Это не утверждение о join room worker в GM-09.

Resident bridge `aurora::h700::resident_resources_empty()` до чтения проверяет main affinity, отсутствие polling и actual CpuQuiescent/Maps до начала Maps retirement; затем требует пустоты producer retains **и** processor entries. Entry с zero GPU bytes и любой mismatch запрещают cleanup. Ни один query не делает enqueue/progress/reset/release. Room contract требует stopped game producers и того же checkpoint; true не означает GPU-complete.

Выполнены full incremental ARM64 game compile/link, **10/10 CPU/no-init CTest** (включая actual `--h700-room-cleanup-selftest`), **269/269 FIFO/resident**, **51/51 render/lifecycle/source** и full non-H700 `aurora_core`+`aurora_gx` compile — PASS. Independent read-only source review не нашёл safety blockers; bridge gate покрыт source assertion, не runtime race injection. Commands/logs: ignored `gm11a-*`. GPU/device не запускались. Catch по-прежнему fail-stop: guards подключаются до throw/destruction в GM-11b, не раньше. Commit/push GM-11a: `591f0ff75`.

### GM-11b — STOP до изменения плана: pending DVD destination lifetime

До реализации actual terminal cleanup выполнен узкий read-only destructor audit. Найден дополнительный retail lifetime blocker, не закрытый GM-09 FIFO/render/compiler checkpoint или GM-11a room/resident guards:

- `CWorld` начинает async read в `x40_loadBuf` (`src/MetroidPrime/CWorld.cpp:112–115`), но destructor явно освобождает его в строке273, до member-tail `x3c_loadToken`.
- `CDummyWorld` начинает async read в `x34_loadBuf` (`CWorld.cpp:499–502`); destructor пустой. В `include/MetroidPrime/CWorld.hpp:223–224` request объявлен перед buffer, поэтому buffer уничтожается первым. Это retail path: `CSaveWorldIntermediate` создаёт dummy world при инициализации memory card (`src/MetroidPrime/CMemoryCard.cpp:42–44`).
- У `CResFactory::SLoadingData` request тоже объявлен перед buffers (`include/Kyoto/CResFactory.hpp:30–39`), destructor не делает предварительный drain. Уже выполненный `CancelBuild` может синхронно retire request; это не доказательство безопасности произвольной оставшейся загрузки.
- `CRealDvdRequest::~CRealDvdRequest` сохраняет `DVDClose`/drain, но поздний drain не защищает уже освобождённый destination. DVD worker пишет непосредственно в `block->addr` (`extern/aurora/lib/dolphin/dvd/dvd.cpp:561–570`); GM-09 его не останавливает.

Это доказанный unsafe порядок **при pending non-ARAM read**, не наблюдавшийся device crash. `DVDGetDriveStatus` не подходит для guard (возвращает END по initialized). Existing `DVDCancelAll` ждёт active request, но вызывает callbacks и не закрывает admission; его безусловный вызов не принят как доказанный terminal barrier. Диск закрывать до owners нельзя; game/factory pump после seal не является безопасным решением.

**На момент остановки:** production code GM-11b не менялся, typed catch удерживал owners. GM-10 и GM-11a завершены/отправлены, их результаты не отменены. Нужно отдельно согласовать расширение GM-11b: узкое исправление request-before-destination retirement с сохранением actual cancel/drain/callback waits и pending-read regressions либо дополнительный проверенный terminal DVD guard/barrier. Нельзя молча включать cleanup, менять renderer/dependencies или выдавать этот блокер за исправленный. GPU/device не запускались; новых build/tests после read-only аудита не требовалось.

**Расширение утверждено пользователем 2026-11-17:** выбран узкий request-before-destination retirement в выявленных retail destructors с сохранением штатного cancel/drain/callback lifetime и CPU regressions. Не вводим global DVD shutdown/barrier, game pump после terminal seal или смену renderer/dependencies. Это часть GM-11b, затем milestone cleanup/return по прежнему контракту. Остановка снята; результат ниже.

### GM-11b — LOCAL PASS: terminal owner cleanup и DVD destination lifetime

H700 `CWorld`/`CDummyWorld` явно уничтожают request перед destination; `CResFactory::SLoadingData` присваивает пустой `auto_ptr`, сохраняя transfer/borrowed semantics. `CRealDvdRequest` по-прежнему использует no-throw `DVDClose`/actual worker drain, включая callback return, без `IsComplete`/искусственного completion. H700 `CARAMDvdRequest` destructor сохраняет фактическое DMA wait. Глобального DVD barrier/close/pump не добавлено; другие платформы не меняют поведение.

`RsMain` хранит nullable owners и initialization/retirement milestones вне try. После exact checkpoint **до throw** проверяются actual resident+room predicates; catch повторяет живую проверку. Отказ сохраняет fail-stop до destruction. Terminal helper явно выполняет architecture → только живые initialized subsystems → globals → final no-command delayed flush → только живой ARAM → return1. Owner aliases очищаются до destruction, service aliases — после нужного destructor tail; CMain/allocator и disc остаются живы до конца. Unknown/timeout не стали разрешением на освобождение. Existing platform audio/services → full IG11 → known release → disc-close order сохранён, double shutdown не добавлен.

Independent source review дополнительно установил retail pause-screen dtors, вызывающие CFDk stall. В предусмотренном CFDk scope исправлено: `generation_cpu_quiescence_held()` read-only проверяет main/!polling и exact held CpuQuiescent/Maps до начала Maps retirement; **только тогда** CFDk выполняет flush без `GXDrawDone`. Healthy/non-H700 barrier сохранён. Это не общий ignore-GX режим; actual resident bridge использует тот же checkpoint query.

Проверки в exact offline ARM64 image: full game build; **21/21 CPU/no-init/DVD CTest**, включая реальные DVD worker и SLoadingData destructor, END/FATAL callback-return, pending/transferred/null owners, deferred ARQ/ARAM, production CFDk queues/frees, root policy и source bindings; **269/269 FIFO**, **53/53 render/lifecycle/source** — PASS. Full non-H700 core/GX и changed game TUs (`main`, `CWorld`, `CResFactory`, `CDvdRequest`, CFDk) compile PASS. Source tests миров/двух pause dtors не подменяются runtime game coverage. Одна первая новая Python wiring-проверка имела ошибку вызова helper (отсутствующий аргумент), исправлена без ослабления assertions; повторный набор PASS. Final read-only review снял pause blocker, новых blockers в проверенном terminal scope не нашёл.

Commands/logs: ignored `gm11b-*`. GPU/device, реальный device-loss и игра не запускались. Синхронные DVD/DMA waits не получили auto-timeout/free: неизвестное завершение сохраняет данные, не объявляется success. Deep skinning throw пока не подключён. Commit/push GM-11b: `b9b1545fe`.

### История GM-12 — scope согласован в редакции3, выполнение начато

После завершения GM-11b выполнена минимальная read-only проверка применимости waits. Production GM-12 не менялся. В `DolphinCSkinnedModel.cpp`:

- init handshake `0xFFFF` находится внутри `AddSkinnedRef`/конструкторов; первый retail вызов — dummy ref при `CCubeRenderer` construction. Его можно вынести на root перед `PostInitialize`, но нельзя бросать root token из конструктора;
- `TickAllocations` не ждёт, а освобождает завершённый FIFO prefix; блокируется именно `EnsureAllocation`. Alignment/products вычисляются уже в `Construct`/`Calculate`/`AllocateStorage`/`AllocateNewWorkspace`, поэтому проверки только входного int недостаточно;
- `Calculate` имеет void API и после `AllocateStorage` без проверки использует workspace. `Draw` при null workspace рисует обычную модель, то есть null/ранний return сам по себе скрывает failure;
- actual consumers — `CAnimData`, пять animated branches `CModelData`, `CProjectedShadow` (после skinning выполняет texture copy/публикует shadow), а также `CFishCloud`, `CSnakeWeedSwarm`, `CWallCrawlerSwarm` с external workspaces. Root safe-point сейчас только после normal EndScene; нужен до debug draw/loader/EndScene;
- границы `CIOWinManager::Draw`/renderer dispatch также продолжают обход после void callback. Нужна узкая failure propagation, а не изменения только leaf cpp/header;
- известные `SPortVisorGuard`/`PortInventoryScope` не доказаны как enclosing scopes именно ring wait (у doll skinning раньше visor scope), но это не разрешение бросать из общего GX/deep helper. Status должен вернуться к root до seal/throw;
- `SkinPool::Run` ждёт `mPending==0` после выполнения jobs, которые захватывают caller stack/input/output. Нельзя делать early-cancel return или считать FIFO/render/compiler checkpoint его join. Scheduler/ring32MiB не меняем.

**Историческое изолированное предложение, НЕ утверждено и заменено общим пересмотром редакции2:** расширить GM-12 на перечисленные непосредственные consumer/dispatch seams и разделить на GM-12a (checked-sizing/wait/token policy + CPU regressions без включения незамкнутого нового failure path) и GM-12b (единое production wiring: root init handshake, leaf allocation/status, root-visible failure и short-circuit consumers, root handling до normal frame tail, проверки). Подшаги — отдельные commit/push; GM-12 считается завершённым только после обоих. Не вводить deep exceptions, generic ignore-GX, fake tokens, unskinned fallback при ошибке или SkinPool rewrite. Альтернатива — явно согласовать тот же расширенный scope одной более крупной карточкой, но не оставлять его скрытым под прежним списком файлов. До решения пользователя перенос остановлен по workflow; это source-level scope finding, не device failure.

## 13. История неутверждённой редакции2 — заменена редакцией3

По запросу пользователя проверены **все GM-12…GM-21**: три независимых read-only audit по status/lifetime, isolation/build/package и device/correction procedure; findings сопоставлены с actual source. [Полный предлагаемый остаток](H700_GAME_REMAINING_PLAN.md), [короткая версия](H700_GAME_SUMMARY.md).

Прежний split GM-12a/b был недостаточен. Новое предложение заранее включает skin consumers/dispatch/boot/frame/root, отдельную сквозную проверку, physical path/profile validation до side effects и live UI rejection, child+log copier lifetime, game-specific package/test/source closure/RUNPATH/full notices, два fresh builds одного freeze и цельный A/B/C correction loop. Synchronous shim breakpoint pulse проверен: hypothetical новый wait mechanism не требуется. Existing same-image/Dawn/nod/SDL/backend/RAM/effects решения не переоткрываются.

Редакция2 предлагала16local substeps и20–40ч; она **не утверждена и не действует**. Findings source-level сохраняются, но из них не следовала обязательность всех предложенных усилений. По требованию пользователя [редакция3](H700_GAME_REMAINING_PLAN.md) оставляет минимальный безопасный путь к игре: direct skin/boot/frame consumers, scoped validation, clean own install/launcher, adapted game package и реальные A/B/C checks. Нет settings/UI sandbox, нового copier acknowledgement protocol или нового hardware research; late-scene access требуется к C, а не до A. Завершённое GM-01…11b не переоткрывается. Новых build/GPU/device runs и production edits при подготовке не выполнялось.
