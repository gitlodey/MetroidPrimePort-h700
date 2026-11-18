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

### PERF06 artifact / установка — PASS (история до live-проверки)

- Код: `4d1c9595feb5b782a581aa049b9326be49298432`, ветка `h700/perf-recovery`, commit/push выполнены. Финальный incremental recheck также86/86CTest +269/269FIFO PASS. `--version` в изолированном builder печатает `source-snapshot`; точная ревизия/bytes привязаны через `source.json` и manifest, не выдуманный embedded revision.
- `build/h700-perf06-recovery/package`:18staged source/ELF/ABI/providers/notices/content/hash/no-init/syntax gates PASS; source-before/after совпал, dirty build-relevant sources0. SDL/dependencies byte-identical RC6 и не пересобирались.
- Source SHA256: `b4f074cf7589b23bdc034501b87bd61ea44c5a783a41ff2c119b99a8de7ccf10`.
- Stripped executable62,860,928bytes, SHA256 `5d6b0b6f61c918e0079cee01bf7ac1b25edbdbcab773d4fe29ac7d7e66f25034`. Unstripped229,466,696bytes сохранён в existing build; transform/hashes записаны в manifest.
- Архив `build/h700-perf06-recovery/h700-perf06.tar.gz`:21,203,501bytes, SHA256 `1327155fa356d03fc39267a70d7c7f6fd46b33bde6e3a8f475aac4cf18d2e41e`;133members, без user/cache/disc/save/symlink payload. Это private incremental test package, не fresh RC и не разрешение public distribution.
- На устройстве отдельный пункт **Metroid Prime Unbound PERF06**,101payload checksum PASS. Физическая копия30user/cache files из неактивного NATIVE verified; source до/после совпал. Отдельный benchmark entry задаёт прежние DRAW_UBO/GATHER/NATIVE1 плюс PROGRAM_CACHE1/GL_TIMING1 и FastBoot menu,1×/640×480/dynamicOff/cap30 сохранены. Existing warm data copy не означает уже прогретый новый Dawn program cache.
- На момент подготовки game starts0/signals0. Device preflight обнаружил другую активную игру; она не прерывалась и fullscreen нагрузка поверх неё не запускалась. Runtime FPS/картинка/physical chord/normal Exit тогда **ещё не проверялись**; последующие live-результаты приведены ниже.
- Тогда следующий шаг — ручной Ports launch после освобождения приставки, проверка FileSelect/звука/картинки/Select+Start и simultaneous power/process/progress/wait capture с различением cold/warm cache. Ошибки, удержанный lock или отсутствие exit не скрывать и не лечить reboot как штатным завершением.

## PERF07: проверенная гипотеза producer flush

Пользователь подтвердил Select+Start exit и разрешил активные tests на свободной приставке. Первый PERF06 capture разделён по одновременно записанному power state; steady active tail138frames/12,1286s=11,378FPS. Submit12,45ms, completion-observed152,89ms; actual EGL waits~0,186ms/frame и множество timeout0/Pending. Значит, steady153ms не объясняются временем внутри измеренных poll/API envelopes. Возврат меню и Released/Done stage11 после физического Select+Start записаны в логе, но следом обнаружен поздний SIGSEGV: чистый process exit НЕ подтверждён.

Проверялась только гипотеза задержанной producer dispatch: exact opt-in `MP_H700_PRODUCER_FLUSH=1` + bench1/menu добавляет один реальный glFlush после успешного создания нового Dawn FENCE/NATIVE_FENCE, пока известный live producer context current. Presenter/imported FD fences исключены; arguments/results/real completion/reverse/swap guards неизменны. Default OFF; glFinish не добавляется. Условие выбора — фактические типы sync/число flush и рост FPS, а не перенос ожидания в Submit. ON capture состоялся, нужного ускорения не показал; default остаётся OFF.

План проверки предусматривал узкие CPU/source tests + existing incremental ARM64 build/package gates, один и тот же binary/меню/settings/cache, OFF→ON и при необходимости обратный контроль; simultaneous process/power/progress, без смешивания idle/transitions. **Строгий same-binary OFF→ON A/B не получен**; фактический ON capture ниже не заменяет его. Штатный muOS content-loader route с существующими configured flags, не bare SDL поверх меню. Firmware/governors/services и исходные установки/сохранения не менять; разрешённую остановку ограничивать identity-bound own game и одним TERM с последующим actual release/lock drain. Launch framework ради A/B не переоткрывать.

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
| **PERF07 /producer flush ON /steady active** |**11,33647**|**88,21087ms**|**92 /8,1154s**|4полных steady reports; simultaneous active ondemand|

Разница PERF06 к NATIVE численно+0,69%, в пределах наблюдаемых колебаний; подтверждённого acceleration нет. PERF07 11,33647FPS также практически совпадает с PERF06 11,378/NATIVE 11,300: эти малые различия **не являются доказательством ни gain, ни regression**, строгого same-binary A/B нет. Producer flush не дал нужного ускорения и остаётся default OFF. Прежний лучший GATHER не превзойдён, но старые runs не matched A/B. Для30FPS по PERF06 всё ещё нужно~2,64×. Весь активный фрагмент PERF06 дал13,073FPS/240frames/18,3582s, однако содержит переходную нестабильность (2s reports11,24–19,29FPS), поэтому не объявлен steady gain. Idle-фрагмент12,223FPS/351frames/28,7174s также не используется как доказательство ускорения.

Steady frame-weighted means: prepare7,53ms, Queue::Submit12,42ms, completion-observed152,89ms, presenter0,572ms/swap0,040ms. Перекрывающиеся интервалы НЕ складывать.97indexed draws/frame:7,045ms API wall;4BufferSubData/frame:0,045ms; map/unmap~0,084/0,012ms. ProcessEvents2,826ms/frame, forwardWaitAny1,610ms/~83,8calls/frame; actual EGL waits0,186ms/~175,6calls/frame. Из24,239EGL polls23,906TimedOut, всеtimeout0/FLUSH, other0. Следовательно, нужно объяснить позднюю готовность fence/serial, не убрать реальный completion guard. GPU util в steady samples6–7%, minimum available в активной фазе381,5MiB; errors/slow-compile warnings в capture0. CPU window samples16→20: всего46,60% четырёхъядерной capacity; один Mali utility24,88% capacity≈одно ядро, роль compiler не установлена. Нет per-frame p95/p99:2s means ими не заменяются.

### Выход PERF06: usability PASS, clean process exit FAIL

Пользователь подтвердил физический chord и возврат меню. Final log после него: `H700 normal exit released: status=1 stage=11` (Released/Done, **НЕ exit code**), затем SIGSEGV. В этом PERF06 capture maps не сохранились, PC тогда не был атрибутирован библиотеке; stack offset соответствует signal handler. Game roots/Dawn/window retirement уже вернулись. Отмеченный тогда следующий caller `aurora_dvd_close` перед return из platform main был лишь гипотезой, не установленной причиной crash. Последующее PERF07 evidence ниже DVD-гипотезу не подтверждает; эту ветку не продолжаем.

### PERF07: LOCAL/package/upload PASS; runtime выполнен, ускорение не подтверждено

Код `f285275cf307140a28f653023e5180e2e44a7998`. Incremental game build19,704s;94/94scoped CTest +269/269FIFO +16/16launcher tests и18staged gates PASS. Executable62,861,920bytes, SHA256 `abbf8b75eb2dd4e93631df37f55882dde600d2a927a4dc5a6e62382db9b22854`; source SHA256 `2fb1d4d05e99375ee0d8eca996979e6d8e1fccab5a43193b500c9ad63b87bfec`. Архив `build/h700-perf07-producer-flush/h700-perf07.tar.gz`, SHA256 `d05c142b55a3a15fe6f5c8af917ec8c3ac1af014bfc72369273c0677bde45f06`. Отдельный Ports пункт **Metroid Prime Unbound PERF07**,101payload checksum PASS, verified own warm PERF06 data copy; прежние установки не изменены.

Штатный9-line content request +safe_quit не был подхвачен уже работающим muOS menu даже после одного поддерживаемого нейтрального BTN_C press/release. Два bounded observation windows не увидели нового game process; это не два game runs. Frontend не остановлен/не перезапущен, process signals0. Только собственный неиспользованный request/четыре runtime flags/marker отменены после проверки identity/bytes/no-child. Final proof: no game/lock/pending request,5route hashes unchanged. Не углубляться в launch framework ради своего A/B.

После этой неудачной automatic preparation состоялся **реальный ручной PERF07 runtime** с `MP_H700_PRODUCER_FLUSH=1`; это не ещё один pending launch. Кодовый default остаётся OFF. Capture:11samples за21,189s (~20s), во всех живой процесс, governor ondemand/idle=false, CPU1,104–1,320GHz; валидные GPU util readings6–8%. Сцена FileSelect `phase4/screen3/next3/movie_ready1`. Minimum available RAM382,34MiB; errors/slow-compile warnings0.

**Steady tail:**4полных reports,92presented frames/8,1154s = **11,3364714FPS**, interval **88,2108696ms**. Frame-weighted means: prepare8,0000ms, Queue::Submit13,5875ms, completion-observed152,0075ms, presenter0,5625ms, swap0,0400ms.97indexed draws/frame занимают7,3188ms API wall. Это перекрывающиеся wall/API intervals, **их нельзя складывать или называть GPU duration**; per-frame p95/p99 не получены.

Producer flush **реально исполнялся**, а не только был включён флагом:

- 252eligible успешных `eglCreateSyncKHR`, все `EGL_SYNC_FENCE_KHR`; native-fence0, failed/other0. Соответствующих `producerGlFlush`252, стоимость0,0025ms/frame.
- 16242`eglClientWaitSyncKHR` polls =15990TimedOut +252Signaled; все timeout0/FLUSH, other0. Actual EGL wait всего0,1825ms/frame. FLUSH-флаг этих polls не следует путать с252отдельными producer glFlush.

Ускорения до нужной производительности нет; producer flush не включать по умолчанию. Сравнение с PERF06/NATIVE выше — историческое, не matched same-binary OFF/ON; tiny delta не объявляется regression/gain.

### PERF07: поздний SIGSEGV и установленный SDL2 AUDIO lifecycle defect

После capture выход состоялся отдельно: сохранены Select+Start marker и `H700 normal exit released: status=1 stage=11` — **Released/Done, НЕ process exit code**. Поздний SIGSEGV повторился; clean process exit **FAIL**. `analysis.json` завершён ещё при живой игре и потому содержит quit not observed; последующий выход подтверждают отдельные exit evidence/final state, а не этот ранний статус.

Теперь сохранены maps до shutdown. Crash PC `0x7fa5f64f80` попадает в executable mapping exact firmware SDL2 с базой `0x7fa5e7f000`; offset **`0xe5f80`**, установленная атрибуция — **`SDL_Delay_REAL`**. Это локализация PC, **не доказательство идентичности crashed thread** и не полная causal chain SIGSEGV. Firmware binary остаётся private ignored input, не публикуется.

Установленный source finding в pinned `build/h700-game-gm03/inputs/sdl/src/audio/sdl2/SDL_sdl2audio.c`: shim вызывает native `SDL2_Init(AUDIO)`, но его `Deinitialize` пуст. Video teardown выгружает native SDL2; native ALSA имеет hotplug thread, который штатная native ALSA deinitialization завершает через join. Неснятый native AUDIO lifetime перед unload — конкретный применимый defect; что именно этот hotplug thread упал в сохранённом SIGSEGV, **не доказано**. DVD-гипотеза не подтверждается и больше не является направлением проверки.

## PERF08 — native AUDIO release; отдельный runtime quit-test отложен

H700-only patch в `platform/main.cpp` и его direct wiring test `tests/port_h700_terminal_wiring.py` добавляют native AUDIO retirement. Scope helper: **после game/audio owners, `AIPortShutdown` и services, до `PollGenerationShutdown`**; guard требует SDL3 audio0. Через `RTLD_NOLOAD` берётся только уже загруженный native SDL2, оставшиеся AUDIO refs снимаются через native AUDIO quit до доказанного0. Если это доказать нельзя — fail-stop. **Никакого полного SDL_Quit, video quit или обхода release/free guards.** Это pending исправление конкретного lifetime, не runtime fix PASS.

Выполнен совместимый existing incremental PERF08 game build с относящимися meaningful tests, новыми hashes/честным source manifest и сохранёнными ABI/provider/notices/content/package checks. Установка отдельная, исходные installs/user/cache/saves не перезаписываются. **Producer flush OFF**, прежние1×/640×480/dynamicOff/cap30 и correctness guards сохранены; нового renderer experiment нет.

Runtime quit-test **pending**: ручной menu run → Select+Start, native AUDIO0 до generation shutdown, Released/Done и затем фактическое завершение процесса без позднего SIGSEGV/удержанного lock. Один release marker не заменяет process exit. Первичная incremental ARM64 compile/link PASS за13,627s,11/11узких CTest PASS; пересобран только `platform/main.cpp`, dependencies не пересобирались. Прямые host wiring tests: terminal7/7, DVD5/5, quit4/4 PASS. Final committed-source incremental recheck0,589s и те же11/11CTest PASS. **Package/deploy PASS, runtime fix пока не заявлен**; новая profiling/launch matrix и продолжение DVD-ветки в этот шаг не входят.

- Код `7a8804a08e657ce8c5a4fd4c903957c34eb047ff`;18staged source/ELF/ABI/providers/notices/content/hash/no-init/syntax gates PASS, build-relevant dirty files0, source-before/after совпал. Новые native `SDL_WasInit`/`SDL_QuitSubSystem` exports также найдены в exact сохранённой firmware SDL2; сама библиотека не менялась.
- Stage `build/h700-perf08-audio-exit/package`, stripped executable62,861,992bytes, SHA256 `3b1fa5a42acfb8409ea2f33d9068001c04e83ec67c117e6e402e16163bc3de8e`; source SHA256 `a9395427d8e362c11572d31faf91362f7c362b53e36271ef9703aa80265713c8`. Reused SDL/notices byte-identical RC6; embedded version остаётся `source-snapshot`, не выдуманный revision.
- Архив `build/h700-perf08-audio-exit/h700-perf08.tar.gz`,21,058,922bytes, SHA256 `0359e3d25579d0fa65f1a80699ca5b7f88c6c4ba9786cbb6433ddc96620cb2b1`; archive inventory/hashes проверены. Это private incremental test package, не fresh RC и не public distribution clearance.
- Отдельный пункт **Metroid Prime Unbound PERF08** установлен:101payload checksum PASS,28own user/cache files физически скопированы с неактивного PERF07, copy/source-before/source-after hashes совпали. Pre/post no-game/launcher/copier и новый lock absent проверены; agent game starts/signals0. Native audio release, картинка/звук и actual process exit этим deploy не проверены.
- **После возражения пользователя отдельная приёмка выхода отложена: PERF08 специально запускать не нужно.** Runtime audio fix/clean exit остаётся непроверенным; это не блокер исследования FPS. Текущий приоритет — адресный разбор медленного FileSelect-пути и обоснованный performance-эксперимент, не дальнейшая работа над выходом или launch framework. Выпуск PERF08 не является прогрессом производительности.

### Evidence

- PERF07: `build/h700-perf07-producer-flush/live-capture/analysis.json` и `capture-complete.json` — steady means/counters и simultaneous live/power capture.
- Историческое сравнение: `build/h700-perf05-native-vertex/comparison.json` и `build/h700-perf06-recovery/comparison.json`; отдельного PERF07 `comparison.json` в сохранённом каталоге нет.
- Выход: PERF07 `live-capture/exit-evidence.json`, `maps-before-shutdown-private.log` и `post-capture-state-private.log` — marker, late crash, PC и mapping. В документ перенесены только обезличенные существенные факты; raw logs, IP/private device paths и firmware binary не публикуются.


## PERF09 — один forward poll одной queue; LOCAL/package/deploy PASS, FPS pending

**Вопрос и граница:** горячий Mali CPU worker + GPU util6–8% требуют разбирать CPU/driver path, но не доказывают причину88ms/frame. Сохранённый профиль также исключает большой upload rewrite как ближайший обоснованный шаг: всего4`glCopyBufferSubData`/frame,0,580ms; map/unmap0,012/0,012ms. Эти API envelopes не измеряют всю работу драйвера.

**Конкретная находка:** `main_progress_once → poll_forward` проверял два flights одной queue отдельными `WaitAny(1,…,0)`:83,89forward calls при43,96ProcessEvents/frame,371,80`eglMakeCurrent`/frame. В exact pinned Dawn ref `118436b3f8f68583eb86e811ca605dc41637e8cc`, `src/dawn/native/EventManager.cpp`, общий wait-set дедуплицируется по queue; отдельные вызовы теряют эту дедупликацию. Scoped context switching/restoration требуется и не удаляется. Измеренные1,780ms внутри MakeCurrent не включают предшествующий context/device mutex и сами не объясняют88ms.

**Выбранный ограниченный PERF09:** один zero-timeout `WaitAny` для0/1/2published, unobserved futures удерживаемого snapshot;0entries не вызывают WaitAny. Общий Success не означает completion обоих: `completed=false` остаётся Pending. Callback acquire, successful work status, descriptor identity, quarantine, ProcessEvents/device-loss/reverse/release guards сохранены. Нового env-флага нет; producer flushOFF, прежние drawUBO/gather/native/program-cache/GLtiming flags,1×/640×480/dynamicOff/cap30 сохранены. Это устранение конкретного повторного CPU/driver polling, **не доказанный ответ о горячем Mali worker и не обещание30FPS**.

- Код `7d5627008d15450bfe644bdad47e76fa199a0bd6`: `presentation.cpp`, маленький production/CPU helper `forward_wait.hpp`, direct integration tests. Incremental ARM64 compile/link12,962s;54/54scoped CTest PASS, включая5новых policy/wiring tests. Final committed-source recheck0,385s + те же54/54PASS. Dependencies не пересобирались.
- 18source/ELF/ABI/providers/notices/content/hash/no-init/syntax gates PASS; dirty build-relevant files0, source-before/after совпал. Source SHA256 `21608c21f577f90606def38a4737be4752e25ed03285e56b8b270074532bd758`.
- Stage `build/h700-perf09-batched-forward/package`; executable62,861,992bytes, SHA256 `3574ff3195af1f1348d0df9213d6f1653cd682b01983ff5b6242e068a2f936ee`. Private incremental archive `h700-perf09.tar.gz`,21,059,903bytes, SHA256 `68d305167edcd31f47a572fdb31aedff19e44268466add9a4ef8cc9957ce3edc`; inventory/hashes verified, не fresh/public RC.
- Отдельный **Metroid Prime Unbound PERF09** установлен:101payload checksum PASS; own user/cache физически скопированы с неактивного PERF07, source/copy hashes verified и исходные данные неизменны. Pre/post no-game/copier/launcher proof, agent game starts/signals0. Ручной запуск нужен для настоящего FPS/correctness capture; текущий FPS PERF09 **ещё не измерен**.
- Для атрибуции горячего потока проверен существующий kernel `perf_event_open`: private bounded userspace-IP sampler прошёл80ms self-test,7samples/lost0/invalid0. Новых packages/sysctl/services не установлено/не изменено, временный helper удалён; ни к игре, ни к драйверному thread ещё не подключался. В следующем run сначала чистый simultaneous FPS/power/process capture, затем отдельно до2s CPU sampling; эти2s не включать в FPS. Сам self-test не доказывает доступность/символизацию game-thread samples.

## PERF09 live — FPS не вырос; горячий поток копирует память

User manual FileSelect run, проверены exact executable hash/PID/starttime.11samples/~20s: все ondemand/idlefalse, CPU1,200–1,416GHz, GPU util6–7%; errors/compile warnings0, min available381,379MiB. Steady4полных reports:92frames/8,1497s = **11,288759FPS**, interval88,5837ms. Prepare8,535ms, Submit14,145ms, completion-observed151,5625ms, presenter0,5675ms/swap0,040ms; перекрывающиеся интервалы не складывать, не GPU time.

| Метрика | PERF07 | PERF09 | Вывод |
| --- | ---: | ---: | --- |
| FPS |11,33647|11,28876|Ускорение не установлено; численно−0,42%, не matched A/B/не доказанная регрессия|
| forward WaitAny/frame |83,8913|43,5652|−48,07% actual calls|
| eglMakeCurrent/frame |371,8043|290,3478|−21,91% actual calls|
| EGL ClientWait/frame |176,5435|135,5217|−23,24% actual calls|

Batch исполняется и убирает повторную работу, но основную88ms задержку **не устранил**. Нельзя объявлять оставшуюся стоимость GPU bound или доказывать gain по уменьшению counters. Source/binaries/runs различаются, в PERF09 producer flushOFF против PERF07ON.

**Новая адресная CPU-находка:** по последним двум одновременным CPU snapshots самый горячий `mali-utility-wo` потреблял25,19% четырёхъядерной capacity≈одно ядро. Отдельно ПОСЛЕ FPS capture выполнен2s kernel userspace-IP profile: **195samples, lost0, invalid0;195/195 в executable libc mapping**. PID/TID/starttime/executable проверены до/после, helper удалён. Exact firmware libc скопирована read-only, source-before/source-after/local SHA256 совпали. ELF LOAD offsets проверены: PCs соответствуют libc offsets `0x9b8c4`, `0x9b8c8`, `0x9b8cc`, `0x9b8d4`. Disassembly показывает NEON `ldp/stp`, увеличение source/destination на64bytes и цикл по оставшейся длине: **цикл копирования памяти**, не spinlock. Сами sampled PCs не в коде компилятора, но без caller нельзя исключить, например, copy из compiler path. Ближайшая подпись objdump `__xpg_strerror_r+…` не является атрибуцией: ни один exported FUNC-range эти PCs не покрывает; реализация copy скрытая/IFUNC.195/195 — доля samples, не доказательство100% полного времени работы.

**Граница вывода:** пока неизвестны caller и размер копирования; нельзя утверждать, что это именно full staging copy, texture upload или установленная причина каждого медленного кадра. Для этих двух вопросов собран LR/register sampler, но его первый attach **не состоялся**: до передачи helper исходные PID/TID уже исчезли. Повторов/сигналов игре не было. Fresh final state: game/copier/launcher0, lock absent; log содержит Select+Start, native audio released refs1, Released/Done, без SIGSEGV. Exit code отдельно не зафиксирован; это не повод снова переключаться с FPS на приёмку выхода.

Следующий минимальный шаг **на момент первого profile** — **тот же PERF09**, новый ручной menu run, fresh identity/hash и1s CPU selection → отдельный2s LR/register profile без повторения FPS-capture/новой сборки. Этот шаг затем выполнен, результат ниже. `sample_callsite_live.py` готов; register sampler скомпилирован, но runtime registers ещё не проверены. Сначала установить caller/размер copy, затем выбирать изменение upload path; не начинать owner rewrite только по общему CPU counter.

Evidence: `build/h700-perf09-batched-forward/live-capture/{analysis.json,comparison.json,cpu-profile-private.json,cpu-profile-libc-symbols.json,cpu-callsite-attempt.json,post-profile-summary.json}`; raw samples/maps/firmware binary остаются private ignored. Никаких изменений firmware/clocks/services/sysctl/game data, game starts/signals0; оба профиля не включены в FPS (второй не был запущен).

### Повторный LR/register profile — caller и операции установлены

User снова запустил тот же PERF09. Fresh executable hash/PID/TID/starttime,1s CPU selection и отдельный2s register sample выполнены; helper удалён, game starts/signals0. User сразу уведомлён, что данные сняты и можно выйти. **21samples/lost0/invalid0**,19вlibmali и2вlibc. Это новый короткий run без simultaneous FPS/power capture; нельзя сравнивать доли с предыдущими195samples или объявлять снижение CPU/FPS gain.

- Два libc samples: LR→`libmali+0x513a90`; exact ELF подтверждает непосредственно перед ним `BL memcpy@plt` на`0x513a8c`. В generic libc copy loop x0 сохраняет исходный destination, x5 — конец destination; оба samples дают **x5−x0=90368B (88,25KiB)**. Оба диапазона находятся в shared `/dev/mali0` mappings. Это не два измеренных вызова/не bytes per frame; cacheability mappings не измерялась. Эти samples не являются доказательством full30MiB staging copy и не описывают размеры всех копирований первого run.
- 15samples: `libmali+0x526af4..0x526bc4`, NEON strided loads/rev/zip/stores. В10stride2560B, в5stride1280B — совместимо с RGBA rows640/320.4samples: indexed32bit pixel loads/stores на`0x522764`, stride16384B. Lookup256bytes на`0x253e170` задаёт16×16 tile permutation. Это CPU-подготовка tiled texture layout; сама форма strides ещё не уникальная идентификация movie plane/vertex mirror. Exported symbols этих PCs не покрывают; ближайшее objdump-имя `clCreateProgramWithILKHR+…` **не означает OpenCL/compiler path**.
- Exact libmali скопирована read-only; source-before/source-after/local SHA256 `08c4c5f65f31a753065cf7332b5596f045a552697cfd9849aef15ea494192b2b` совпали. Firmware не изменена. PC/LR/module offsets проверены по ELF LOAD segments; сырые регистры/бинарник остаются private ignored.

**Сверка actual OLD/NEW movie path:** front-end вызывает `CMoviePlayer::DrawFrame → MyTHPYuv2RgbTextureSetup → GXInitTexObj/GXLoadTexObj` для трёх I8 planes. В обеих версиях I8 разворачивается в RGBA(I,I,I,I), GL allocation=`TexStorage2D(GL_RGBA8)`, update=`TexSubImage2D(GL_RGBA,GL_UNSIGNED_BYTE)`. Movie GX identities создаются заново на draw; OLD fresh upload без replacement, NEW fresh upload на content-cache miss. **Реальное отличие: OLD передаёт CPU pointer без PBO; NEW CopyBufferToTexture replay связывает GL_PIXEL_UNPACK_BUFFER и передаёт offset.** Pitch2560/1280 уже кратен256, padding не объясняет эту разницу; предположение об OLD TexImage2D против NEW immutable storage опровергнуто исходниками.

Source references: OLD `gfx/texture.cpp:66–109`, `gfx/common.cpp:655–664`, `gl/textures.cpp:65–219`, active `gx/gx.cpp:164–196`; NEW `gfx/texture.cpp:139–187,221–237`, `gfx/recording.cpp:1745–1757`, `gfx/encoding.cpp:517–529`, `gx/texture.cpp:811–875` (от`extern/aurora/lib/` соответствующего checkout). Pinned Dawn `CommandBufferGL.cpp:977–1017,2106–2151`, `QueueGL.cpp:166–183`, `DeviceGL.h:154–167` exact ref указан выше.

**Следующий ограниченный кандидат, ещё не реализован:** новые movie-plane uploads из owned CPU payload через `Queue.WriteTexture` на render worker до Submit вместо PBO-copy. Pinned QueueGL ведёт к client-memory TexSubImage; GLDefer может сам копировать payload, поэтому zero-copy не обещать. Сохранить форматы/cache/bounds/validation/lifetime, не обновлять задним числом textures прошлых frames; не глобальный `UseTextureBuffer=false` и не whole-staging owner rewrite. Driver CPU tiling/copy и source-различие установлены, но связь каждого job с конкретным GL API и доля в88ms/frame ещё не доказаны; FPS-эффект только по следующему контролируемому сравнению.

Evidence: `build/h700-perf09-batched-forward/callsite-capture/{cpu-callsite-private.json,callsite-decoded-private.json,callsite-summary.json,firmware-mali-disassembly-private.log,firmware-mali-provenance-private.json}`. Существующий PERF09 binary/package не менялся; новой игры/сборки ради этого profile не было.

Private scripts/evidence/results остаются в ignored `build/h700-perf06-recovery/`, `build/h700-perf07-producer-flush/`, `build/h700-perf08-audio-exit/` и `build/h700-perf09-batched-forward/`. Чужой local diff progress и `.DS_Store` сохранены, в commits не включаются.
