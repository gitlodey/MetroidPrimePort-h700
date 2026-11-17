# H700 — прогресс переноса

**Цель:** Metroid Prime Unbound v0.46.0 на **ANBERNIC RG40XX H / muOS 2601.0 Jacaranda** с корректными графикой, интерфейсом, управлением, звуком и сохранениями. 30 FPS при 640×480 — следующий отдельный этап.

## Текущий статус

- **IG-01…IG-13 LOCAL PASS:** постоянный H700 backend, mixed resources/shaders/cache, main presenter, progress-aware render/FIFO/compiler waits, normal drain/full generation lifecycle, GX+ImGui diagnostic scene и clean ARM64 package.
- **IG-14: последний корректный manual run PASS bounded-сценария.** После назначения PortMaster пользователь подтвердил видимый нормальный запуск. Последний run=120+2frames/exit0; три других сохранённых manual runs — incomplete/exit1 из-за real_overlap0, не ошибки pixels. Это не4/4PASS/не готовность полной игры. Подробные ограничения — в gate report.
- [Подробный утверждённый IG-план и результаты](H700_INTEGRATION_PLAN.md), [простая версия IG-этапа](H700_INTEGRATION_SUMMARY.md), [artifact/device evidence и ограничения](H700_INTEGRATION_GATE.md).
- **[План полной игры GM-01…GM-21 утверждён](H700_GAME_PLAN.md)** ([коротко](H700_GAME_SUMMARY.md)): D1…D11, локальные GM-01…GM-17, commit/push каждой завершённой итерации. Три device-окна требуют отдельных разрешений.
- **GM-01 LOCAL PASS:** fresh root game configure в exact offline ARM64 image,14 verified inputs,642game TU в manifest;5/5game-profile +10/10bootstrap host tests,30/30integration helper tests в Linux. GM-01 commit/push: `f6129b4d4`.

- **GM-02 LOCAL PASS:** fresh full compile/link `metroid_prime_port` за494s, no-init `--version` exit0, ELF64/AArch64 metadata; image/pins/исходники игры не менялись.30/30helper +5/5profile tests в Linux PASS. Это ещё не lifecycle/ABI/device готовность. Commit/push: `95c156a5a`.
- **GM-03 LOCAL PASS:** H700 new-install defaults + fixed unpinned retail allocation/backend, Dawn validation/robustness protection и override rejection; fresh full build,5/5CPU/no-init CTest (27settings keys/12actual startup rejections), non-H700 compile изменённых game TU PASS. Existing INI не перезаписывается; GPU/device не запускались. Commit/push: `119418fae`.
- **GM-04 LOCAL PASS:** pressure192/128MiB, bounded1s RAM/RSS-HWM/min-available diagnostics с96MiB sampled gate;6/6CPU/no-init CTest (16startup rejects), non-H700 changed-TU compile PASS. Eviction/GLOBAL algorithm и buffer limits не менялись; device RAM PASS не заявлен. Commit/push: `2a231b2a7`.
- **GM-05 LOCAL PASS:** actual Aurora init backend/window/native/readiness gate до game owners; partial init использует IG11, Unknown/timeout не разрешают unwind.7/7CPU/no-init CTest и полный non-H700 core+GX compile PASS; реальный GPU partial-init failure не инъецировался. Commit/push: `e5fdeac74`.
- **GM-06 LOCAL PASS:** reusable healthy owner barrier/frame-once/FIFO→render+callback-return/capture release, без закрытия roots/GPU-idle.8/8game +45/45render/lifecycle +262/262FIFO CPU tests и full non-H700 core/GX compile PASS; Commit/push: `5e184e0c5`.
- **GM-07 LOCAL PASS:** reset boundary→alias clear/owner destruction→destructor-tail boundary→no-command flush→new owner в том же generation;8/8CPU/no-init tests и non-H700 game-main compile PASS. На failure сохраняется fail-stop до будущего terminal transport; actual device reset ещё не проверен.

## Что подтверждено

- **Локально:** fresh ARM64 build из14 pinned inputs,108/108CPU/source +262/262FIFO +23/23helper tests; non-H700 core/GX compile и targeted regressions. Пакет23 237 286bytes, SHA256 `43f31a20d4d31fb06ffaf4e1f083484d31710a300835cefca80e5d4f23bdaeb9`; notices/provenance, ABI/provider audit и watchdog. Ограничения firmware-symbol coverage отражены в отчёте.
- **На приставке:** hardware OpenGLES/Mali-G31, validation/robustness ON;120+2frames в двух поколениях одного процесса,2permanent imports каждого поколения, EFB640×480→320×240 при native640×480. Все2440RGBA samples native/post-blit совпали с независимым oracle, maximum error0LSB.
- Настоящие compiler/resource/render API intervals пересеклись на3threads(~0.218ms), без учёта artificial barrier waits. Gen1 pressure4progress passes; gen2 normal Draining при подтверждённой accepted pending работе одного pipeline key; оба accepted frames завершились.
- Все6observed context lifetimes каждого поколения(1temporary +5Dawn share group) destroyed до native export cleanup. Нет validation/EGL/GL errors/device loss. Процесс exit0 без watchdog; это не FPS measurement и не утверждение о параллельности внутри Mali.
- Test processes/временная установка удалены;6baseline SONAME hashes(5уникальных) до run/после run/после cleanup совпали. Firmware, игровые установки, настройки и saves не менялись. Raw logs/SSH/session paths — только в ignored build.

## Что ещё не подтверждено

- Повторяемость actual3-thread overlap не подтверждена: наблюдение получено в1из4manual runs (последний, ~0.272ms); прочие3 честно incomplete. Пользователь уточнил неверный core при первом invocation, но это не доказанная причина logged overlap failures. `fbgrab` показывает demo, но не все expected markers; источник расхождения с точными GL/native samples не установлен, pixel-perfect физический scanout этим снимком не доказан.
- Полная игра, все retail effects/UI, game-owner quiescence/`archSupport`, skinning waits, input/audio, actual nod SDL-stream path на kernel4.9, память реальных сцен, save/load и обычная установка Ports. Optional profiler/visibility-query/readback paths не активировались ради gate.
- Целевые30FPS и полная firmware exact-symbol closure не заявлены. EOL image явно разрешён для correctness RC GM-01…17, включая повторный fresh build; это не долгосрочная поддерживаемая build platform.

## Следующий шаг

Новых повторов ради зелёной матрицы не выполнять: один actual bounded manual PASS и видимый Ports path получены, неполные runs/ограничения сохранены. Следующий шаг — утверждённая GM-08: normal exit/subsystem/global destructor tails, без device runtime. После commit/push продолжаем локальные карточки без нового approval. RAM policy и96MiB headroom gate утверждены как начальные параметры, не измеренные гарантии. Три device-окна требуют отдельных разрешений. `H700 Integration Test.sh` и hidden diagnostic data намеренно оставлены по запросу; raw logs скачаны, активных test processes/lock нет, firmware hashes unchanged.

## Основания и история

[Архитектурное решение](H700_ARCHITECTURE_DECISION.md), [аудит базы/донора](H700_PORT_REVIEW.md), [runtime ABI snapshot](H700_RUNTIME_ABI.md). Прежние [graphics demo](H700_GRAPHICS_PROBE.md), [contract-v2 failure17>16](H700_CONTRACT_PROBE.md) и [mixed-v2 device PASS](H700_MIXED_FETCH_PROBE.md) сохраняют отдельные границы доказательности; не подменяют actual integration run. Результаты PF-02 и IG-01…IG-13 сохранены в тематических документах/плане, не требуют повторных probes.

**До любого действия:** цель → конкретный риск целевого пути → минимальная проверка → решение → остановка; [правила работы](H700_WORKFLOW.md). Донор read-only, чужие `.DS_Store` сохранены.
