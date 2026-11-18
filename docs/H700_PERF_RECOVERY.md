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

Device preflight обнаружил другую активную игру; она не затронута. Ожидается упаковка отдельного тестового stage и ручной запуск пользователем после освобождения приставки. Прежний пользовательский diff progress не включается в commit.
