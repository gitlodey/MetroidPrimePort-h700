# H700 — прогресс переноса

**Цель:** Metroid Prime Unbound v0.46.0 на **ANBERNIC RG40XX H / muOS 2601.0 Jacaranda** с корректными графикой, интерфейсом, управлением, звуком и сохранениями. 30 FPS при 640×480 — следующий отдельный этап.

## Текущий статус

- **IG-01…IG-13 LOCAL PASS:** постоянный H700 backend, mixed resources/shaders/cache, main presenter, progress-aware render/FIFO/compiler waits, normal drain/full generation lifecycle, GX+ImGui diagnostic scene и clean ARM64 package.
- **IG-14: последний корректный manual run PASS bounded-сценария.** После назначения PortMaster пользователь подтвердил видимый нормальный запуск. Последний run=120+2frames/exit0; три других сохранённых manual runs — incomplete/exit1 из-за real_overlap0, не ошибки pixels. Это не4/4PASS/не готовность полной игры. Подробные ограничения — в gate report.
- [Подробный утверждённый план и результаты каждой итерации](H700_INTEGRATION_PLAN.md), [простая версия этапа](H700_INTEGRATION_SUMMARY.md), [artifact/device evidence и ограничения](H700_INTEGRATION_GATE.md).

## Что подтверждено

- **Локально:** fresh ARM64 build из14 pinned inputs,108/108CPU/source +262/262FIFO +23/23helper tests; non-H700 core/GX compile и targeted regressions. Пакет23 237 286bytes, SHA256 `43f31a20d4d31fb06ffaf4e1f083484d31710a300835cefca80e5d4f23bdaeb9`; notices/provenance, ABI/provider audit и watchdog. Ограничения firmware-symbol coverage отражены в отчёте.
- **На приставке:** hardware OpenGLES/Mali-G31, validation/robustness ON;120+2frames в двух поколениях одного процесса,2permanent imports каждого поколения, EFB640×480→320×240 при native640×480. Все2440RGBA samples native/post-blit совпали с независимым oracle, maximum error0LSB.
- Настоящие compiler/resource/render API intervals пересеклись на3threads(~0.218ms), без учёта artificial barrier waits. Gen1 pressure4progress passes; gen2 normal Draining при подтверждённой accepted pending работе одного pipeline key; оба accepted frames завершились.
- Все6observed context lifetimes каждого поколения(1temporary +5Dawn share group) destroyed до native export cleanup. Нет validation/EGL/GL errors/device loss. Процесс exit0 без watchdog; это не FPS measurement и не утверждение о параллельности внутри Mali.
- Test processes/временная установка удалены;6baseline SONAME hashes(5уникальных) до run/после run/после cleanup совпали. Firmware, игровые установки, настройки и saves не менялись. Raw logs/SSH/session paths — только в ignored build.

## Что ещё не подтверждено

- Повторяемость actual3-thread overlap не подтверждена: наблюдение получено в1из4manual runs (последний, ~0.272ms); прочие3 честно incomplete. Пользователь уточнил неверный core при первом invocation, но это не доказанная причина logged overlap failures. `fbgrab` показывает demo, но не все expected markers; источник расхождения с точными GL/native samples не установлен, pixel-perfect физический scanout этим снимком не доказан.
- Полная игра, все retail effects/UI, game-owner quiescence/`archSupport`, skinning waits, input/audio, kernel4.9 nod fallback, память реальных сцен, save/load и обычная установка Ports. Optional profiler/visibility-query/readback paths не активировались ради gate.
- Целевые30FPS и полная firmware exact-symbol closure не заявлены. EOL image принят только для integration slice, не автоматически для полной игры.

## Следующий шаг

Новых повторов ради зелёной матрицы не выполнять: один actual bounded manual PASS и видимый Ports path получены, неполные runs/ограничения сохранены. Следующий шаг — отдельно согласовать план подключения полной игры, не начинать его автоматически. `H700 Integration Test.sh` и hidden diagnostic data намеренно оставлены по запросу; raw logs скачаны, активных test processes/lock нет, firmware hashes unchanged.

## Основания и история

[Архитектурное решение](H700_ARCHITECTURE_DECISION.md), [аудит базы/донора](H700_PORT_REVIEW.md), [runtime ABI snapshot](H700_RUNTIME_ABI.md). Прежние [graphics demo](H700_GRAPHICS_PROBE.md), [contract-v2 failure17>16](H700_CONTRACT_PROBE.md) и [mixed-v2 device PASS](H700_MIXED_FETCH_PROBE.md) сохраняют отдельные границы доказательности; не подменяют actual integration run. Результаты PF-02 и IG-01…IG-13 сохранены в тематических документах/плане, не требуют повторных probes.

**До любого действия:** цель → конкретный риск целевого пути → минимальная проверка → решение → остановка; [правила работы](H700_WORKFLOW.md). Донор read-only, чужие `.DS_Store` сохранены.
