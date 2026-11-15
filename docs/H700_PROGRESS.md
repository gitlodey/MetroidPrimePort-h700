# H700 — прогресс переноса

**Цель:** Metroid Prime Unbound v0.46.0 работает на **ANBERNIC RG40XX H / muOS 2601.0 Jacaranda** с корректными графикой, интерфейсом, управлением, звуком и сохранениями. 30 FPS при 640×480 — следующий отдельный этап.

**Текущий этап:** подготовка плана. По разрешению пользователя разобраны три оставшихся «КАК»: **потоки/presentation, EGLImage lifetime, build-цепочка**. [Единое архитектурное заключение, редакция 1](H700_ARCHITECTURE_DECISION.md) содержит конкретную рекомендацию и границы evidence. Production-план не утверждён, код переноса не менялся.

## Что готово

- [Аудит базы и донора](H700_PORT_REVIEW.md), [краткий обзор](H700_PORT_SUMMARY.md), [правила работы](H700_WORKFLOW.md), [firmware ABI snapshot](H700_RUNTIME_ABI.md).
- [Graphics demo](H700_GRAPHICS_PROBE.md): offscreen 2 кадра и последовательный EGLImage bridge 120 кадров — device PASS в своём ограниченном сценарии. Не production threading/lifetime и не полноценная визуальная приёмка.
- [Contract-v2](H700_CONTRACT_PROBE.md) на Mali воспроизвёл отказ исходных U layouts: 17 fragment textures при лимите 16. RequestDevice PASS, общий exit 1 без timeout. Этот failure не скрыт.
- После [сравнения решений](H700_LAYOUT_OPTIONS.md) **[mixed-v2](H700_MIXED_FETCH_PROBE.md) прошёл на H700:** оба layouts/pipelines, 8 draws / 32768 pixels, updates, GPU-preserved growth и чтение за старой границей; exit 0 без timeout. Исправление shader демки — явный `flat, either`; подготовка: 71 helper tests/build/ABI/CPU/Null PASS. Повторять этот же опыт не требуется.
- Новое исследование определило **main presenter + ранняя reservation двух slots**, progress/cancellation в реальных waits U; **два постоянных imports на lifetime device**, cleanup через всю Dawn share group. Патч Dawn ради per-import cleanup для этого bounded контракта не нужен. Найден необходимый будущий reset static holders `vis_query`; сейчас не исправлялся.
- **PF-02 build/API:** 252 SDL function references, реальные Linux consumers, C++20/Aurora thread, все 33 MusyX C units/whole-archive link, 19 nod API references — PASS. Дополнительно получены exact pinned zlib-ng 2.3.3 / zstd 1.5.7 / nod alpha.12 release archives; hashes, static builds/API link и provider/version-node gate (firmware + bundled SDL) — PASS. Runtime/kernel совместимость и полная сборка игры этим не доказаны.

## Что ещё не подтверждено

**Один архитектурный runtime-допуск:** совместная работа pinned Dawn/Mali с реальными resource/render/compiler потоками, main presenter и полным teardown всей EGL share group. Static contract определён, совместный device PASS отсутствует.

Рекомендация — **один ранний интеграционный gate**, на минимальном production slice, а не серия новых независимых демок. Это требует явного согласования переноса данной проверки в первую реализацию; действующие правила сами собой не изменены. До PASS остальной renderer-перенос блокируется. [Конечный сценарий и FAIL-развилка](H700_ARCHITECTURE_DECISION.md#5-один-конечный-gate-вместо-серии-демок).

Также не проверены полная игра/эффекты/UI, input/audio, kernel 4.9 fallback nod, реальный save/load и возврат в меню. Это приёмка переноса, не обязательный набор предплановых probes. ABI snapshot не является полным списком firmware exports.

## Следующий шаг и согласование

Составить подробный план небольших итераций на основе заключения. В нём согласовать архитектуру, использование прежнего **EOL builder для игры** (пока разрешён только baseline демки), fatal-exit вместо live device-loss recovery и место общего gate. Без такого согласования условная архитектура не становится разрешением на полный перенос.

**В текущей итерации:** только исследование, isolated build/link checks и документация; пакеты/образ/production/донор не менялись, приставка не использовалась. Прежние разрешённые device tests очищены, firmware hashes были неизменны. Новые device runs, production edits и deploy требуют отдельного разрешения.

**Ориентир до каждого действия:** цель → конкретный вопрос целевого пути → минимальная проверка → решение по результату → остановка. Полнота вспомогательной матрицы не цель; правила — [H700_WORKFLOW.md](H700_WORKFLOW.md).
