# H700 performance recovery

## Разрешение и цель

Пользователь передал автономное исправление производительности, разрешил отдельную ветку/commit, сборку, SSH/deploy/tests и явно попросил Select+Start → закрытие игры. Работа ведётся в `h700/perf-recovery`; прежние эксперименты сохраняются. Разрешение на выход заменяет старую H700-привязку Back+Start → PortMenu; другие платформы не меняются.

Инварианты: 1×/640×480, dynamic resolution Off, cap30, исходная simulation, retail effects, validation/robustness и все bounds/lifetime/release guards. Не менять firmware/services/clocks/swap и реальные данные. Сторонний local diff `H700_PROGRESS.md` и `.DS_Store` сохраняются.

## Исходное evidence и выбор

NATIVE уже уменьшил Queue::Submit до ~12,49 ms, но показанный кадр занимает ~88,50 ms (11,300 FPS), completion-observed ~153,37 ms. Последний simultaneous capture: активный ondemand/CPU1,2–1,416 GHz, GPU utilisation6–7%, ошибок/compile warnings0. Поэтому не начинать ещё один vertex/layout rewrite и не выдавать этот capture за GPU frametime.

Pinned Dawn zero-timeout completion poll действительно захватывает device/context locks и вызывает EGL ClientWait. Existing GL timing не измерял эти waits. Также upstream `glWithoutCache` отключает Dawn persistent shader/program cache для всех Linux GL/GLES по причине Mesa; H700 использует firmware Mali, не Mesa. Это отдельный применимый источник повторной cold compilation, не установленная причина steady low FPS.

## Минимальная итерация

1. **QUIT:** H700-only event-based same-pad Select+Start, обе очередности, one-shot, до UI visibility. Использовать прежний RequestQuit → SDL quit → normal game shutdown, без process kill и confirmation. CPU state/wiring tests + actual ARM64 compile. Hard driver deadlock не превращать в гарантию выхода.
2. **WAIT:** расширить существующий opt-in GL timing фиксированными counters ProcessEvents/forward WaitAny/device loss/reverse poll и EGL ClientWait core/KHR. OFF не читает часы; API args/results и completion policy не меняются. Проверка counters/reset/concurrency и настоящих call sites. Это необходимая диагностика оставшегося >33,33ms критического пути, не profiler framework.
3. **CACHE:** включаемый в тестовой установке H700-only persistent Dawn cache на уже проверенном Mali adapter, с возможностью OFF на том же binary. Сохранить прежнюю policy других платформ и fallback pinned Dawn при неподходящем binary. Проверить существующий cache consumer, direct tests и ABI/package; не обещать рост steady FPS от cold-cache исправления.
4. Совместимый incremental ARM64 build, относящиеся CTest/launcher tests; новый stripped executable + честные source/ABI/provider/notices/content/hash manifest. Без fresh/dependency rebuild.
5. Отдельный Ports пункт с физической копией собственных user/cache и rollback, старые установки не перезаписывать. Агент не прерывает другую активную игру. Когда приставка свободна: matched FileSelect, simultaneous process/power/progress, warm/cold cache distinction; по wait breakdown выбрать исправление, а не новый speculative renderer rewrite. Физическую картинку/pad/normal Select+Start exit подтверждает реальное наблюдение.

## Приёмка и остановка

Локальный результат — отдельный commit, тестовая сборка и её точный hash, реальная установка или конкретный blocker. Runtime improvement/30FPS только по настоящему capture, не compile/tests. Ошибка/нет progress≥60s/MemAvailable<96MiB — сохранить evidence и прекратить нагрузку; не выдавать Pending за completion. Никаких KILL/reboot или вмешательства в сторонний RetroArch ради теста.

## Результат

Первичная incremental ARM64 game build — PASS за31,737s;86/86scoped CTest +269/269FIFO и16/16host launcher tests PASS. Включены CPU-тест аккорда,4source wiring cases и7новых timing/progress tests; cache opt-in/env/descriptor guards покрыты. Это LOCAL PASS, не FPS/device PASS.

Добавлен exact `MP_H700_PROGRAM_CACHE=1` + `MP_H700_BENCH=1|menu`; default OFF. Используется существующий checksummed Dawn cache и закреплённый GL program-binary loader с fallback, не собственный новый cache format. Linux Mesa/non-H700 policy не меняется. Existing `MP_H700_GL_TIMING=1` теперь отдельно считает ProcessEvents/forwardWaitAny/deviceLostPoll/reversePoll и core/KHR EGL waits с timeout/flags/outcomes; вложенные суммы не складывать.

### Artifact / установка — PASS, runtime pending

- Код: `4d1c9595feb5b782a581aa049b9326be49298432`, ветка `h700/perf-recovery`, commit/push выполнены. Финальный incremental recheck также86/86CTest +269/269FIFO PASS. `--version` в изолированном builder печатает `source-snapshot`; точная ревизия/bytes привязаны через `source.json` и manifest, не выдуманный embedded revision.
- `build/h700-perf06-recovery/package`:18staged source/ELF/ABI/providers/notices/content/hash/no-init/syntax gates PASS; source-before/after совпал, dirty build-relevant sources0. SDL/dependencies byte-identical RC6 и не пересобирались.
- Source SHA256: `b4f074cf7589b23bdc034501b87bd61ea44c5a783a41ff2c119b99a8de7ccf10`.
- Stripped executable62,860,928bytes, SHA256 `5d6b0b6f61c918e0079cee01bf7ac1b25edbdbcab773d4fe29ac7d7e66f25034`. Unstripped229,466,696bytes сохранён в existing build; transform/hashes записаны в manifest.
- Архив `build/h700-perf06-recovery/h700-perf06.tar.gz`:21,203,501bytes, SHA256 `1327155fa356d03fc39267a70d7c7f6fd46b33bde6e3a8f475aac4cf18d2e41e`;133members, без user/cache/disc/save/symlink payload. Это private incremental test package, не fresh RC и не разрешение public distribution.
- На устройстве отдельный пункт **Metroid Prime Unbound PERF06**,101payload checksum PASS. Физическая копия30user/cache files из неактивного NATIVE verified; source до/после совпал. Отдельный benchmark entry задаёт прежние DRAW_UBO/GATHER/NATIVE1 плюс PROGRAM_CACHE1/GL_TIMING1 и FastBoot menu,1×/640×480/dynamicOff/cap30 сохранены. Existing warm data copy не означает уже прогретый новый Dawn program cache.
- Game starts0/signals0. Device preflight обнаружил другую активную игру; она не прерывалась и fullscreen нагрузка поверх неё не запускалась. Runtime FPS/картинка/physical chord/normal Exit **не проверены**.
- Следующий шаг: после освобождения приставки ручной Ports launch, проверить FileSelect/звук/картинку и фактический Select+Start exit; в отдельном run собрать simultaneous power/process/progress и новые wait counters, различить cold/warm cache. Ошибки, удержанный lock или отсутствие exit не скрывать и не лечить reboot как штатным завершением.

## Следующий live шаг: PERF07 / producer flush A/B

Пользователь подтвердил Select+Start exit и разрешил активные tests на свободной приставке. Первый PERF06 capture разделён по одновременно записанному power state; steady active tail138frames/12,1286s=11,378FPS. Submit12,45ms, completion-observed152,89ms; actual EGL waits~0,186ms/frame и множество timeout0/Pending. Значит, steady153ms не объясняются временем внутри измеренных poll/API envelopes. Возврат меню и Released/Done stage11 после физического Select+Start записаны в логе, но следом обнаружен поздний SIGSEGV: чистый process exit НЕ подтверждён.

Проверяем только гипотезу задержанной producer dispatch: exact opt-in `MP_H700_PRODUCER_FLUSH=1` + bench1/menu добавляет один реальный glFlush после успешного создания нового Dawn FENCE/NATIVE_FENCE, пока известный live producer context current. Presenter/imported FD fences исключены; arguments/results/real completion/reverse/swap guards неизменны. Default OFF; glFinish не добавляется. Считать фактические типы sync и число flush. Если FPS не растёт или ожидание просто переезжает в Submit — кандидат не включать.

Проверка: узкие CPU/source tests + existing incremental ARM64 build/package gates; один и тот же binary/меню/settings/cache, OFF→ON и при необходимости обратный контроль; simultaneous process/power/progress, без смешивания idle/transitions. Штатный muOS content-loader route с существующими configured flags, не bare SDL поверх меню. Не менять firmware, governors или services; остановка только identity-bound own game и один TERM с последующим actual release/lock drain. Исходные установки/сохранения не трогать.

## Live результаты: прирост steady FPS не подтверждён

PERF06: один user-launched process,21samples/~61,2s, simultaneous process identity/power/RAM/GL/progress. Сначала12samples powersave/480MHz/idle=true, затем9samples ondemand/1,008–1,512GHz/idle=false. Переход не смешивается со steady результатом; agent governor/services/clocks не менял.

| Сохранённый запуск | FPS | Средний interval | Presented frames / report duration | Ограничение |
| --- | ---: | ---: | ---: | --- |
| PERF-02 |8,299|120,50ms|452 /54,4674s|История; power не записан|
| UBO /GL timing OFF |7,381|135,48ms|123 /16,6635s|История; power не записан|
| UBO /GL timing ON |7,837|127,60ms|131 /16,715s|История; power не записан|
| GATHER /FPS ON |14,465|69,13ms|208 /14,38s|История; power не записан|
| GATHER /FPS OFF |4,095|244,18ms|71 /17,3368s|Диагностика, не контролируемый FPS A/B|
| NATIVE /FPS ON |11,300|88,50ms|207 /18,319s|Активный ondemand, прежний capture|
| **PERF06 /cache ON /steady active** |**11,378**|**87,89ms**|**138 /12,1286s**|6последних полных report groups, после переходного участка|

Разница PERF06 к NATIVE численно+0,69%, в пределах наблюдаемых колебаний; подтверждённого acceleration нет. Прежний лучший GATHER не превзойдён, но старые runs не matched A/B. Для30FPS всё ещё нужно~2,64×. Весь активный фрагмент дал13,073FPS/240frames/18,3582s, однако содержит переходную нестабильность (2s reports11,24–19,29FPS), поэтому не объявлен steady gain. Idle-фрагмент12,223FPS/351frames/28,7174s также не используется как доказательство ускорения.

Steady frame-weighted means: prepare7,53ms, Queue::Submit12,42ms, completion-observed152,89ms, presenter0,572ms/swap0,040ms. Перекрывающиеся интервалы НЕ складывать.97indexed draws/frame:7,045ms API wall;4BufferSubData/frame:0,045ms; map/unmap~0,084/0,012ms. ProcessEvents2,826ms/frame, forwardWaitAny1,610ms/~83,8calls/frame; actual EGL waits0,186ms/~175,6calls/frame. Из24,239EGL polls23,906TimedOut, всеtimeout0/FLUSH, other0. Следовательно, нужно объяснить позднюю готовность fence/serial, не убрать реальный completion guard. GPU util в steady samples6–7%, minimum available в активной фазе381,5MiB; errors/slow-compile warnings в capture0. CPU window samples16→20: всего46,60% четырёхъядерной capacity; один Mali utility24,88% capacity≈одно ядро, роль compiler не установлена. Нет per-frame p95/p99:2s means ими не заменяются.

### Выход: usability PASS, clean process exit FAIL

Пользователь подтвердил физический chord и возврат меню. Final log после него: `H700 normal exit released: status=1 stage=11` (Released/Done, НЕ exit code), затем SIGSEGV. PC не атрибутирован библиотеке: maps не сохранены, stack offset соответствует signal handler. Game roots/Dawn/window retirement уже вернулись; первый прямой caller после release marker — `aurora_dvd_close`, затем return из platform main. Это точка следующей проверки, НЕ установленная причина crash. Нужны узкие before/after DVD close/return markers и maps до shutdown, без перестановки owner destruction наугад.

### PERF07: LOCAL/package/upload PASS, runtime A/B НЕ выполнен

Код `f285275cf307140a28f653023e5180e2e44a7998`. Incremental game build19,704s;94/94scoped CTest +269/269FIFO +16/16launcher tests и18staged gates PASS. Executable62,861,920bytes, SHA256 `abbf8b75eb2dd4e93631df37f55882dde600d2a927a4dc5a6e62382db9b22854`; source SHA256 `2fb1d4d05e99375ee0d8eca996979e6d8e1fccab5a43193b500c9ad63b87bfec`. Архив `build/h700-perf07-producer-flush/h700-perf07.tar.gz`, SHA256 `d05c142b55a3a15fe6f5c8af917ec8c3ac1af014bfc72369273c0677bde45f06`. Отдельный Ports пункт **Metroid Prime Unbound PERF07**,101payload checksum PASS, verified own warm PERF06 data copy; прежние установки не изменены.

Штатный9-line content request +safe_quit не был подхвачен уже работающим muOS menu даже после одного поддерживаемого нейтрального BTN_C press/release. Два bounded observation windows не увидели нового game process; это не два game runs. Frontend не остановлен/не перезапущен, process signals0. Только собственный неиспользованный request/четыре runtime flags/marker отменены после проверки identity/bytes/no-child. Final proof: no game/lock/pending request,5route hashes unchanged. Не углубляться в launch framework ради своего A/B.

Manual PERF07 entry теперь явно включает эксперимент `MP_H700_PRODUCER_FLUSH=1`; кодовый default остаётся OFF. Реального создания/типа sync/числа eager flush/FPS на Mali этим запуском ещё НЕ проверяли; прирост не заявлен. Следующий минимальный шаг — ручной PERF07 launch пользователем; затем actual capture и адресный разбор позднего quit. Разрешение уже дано, препятствие — неподхваченный automatic route, не approval.

Прежний пользовательский diff progress и `.DS_Store` не включены в commits. Private scripts/evidence/results сохранены в ignored `build/h700-perf06-recovery/` и `build/h700-perf07-producer-flush/`.
