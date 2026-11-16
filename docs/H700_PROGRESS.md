# H700 — прогресс переноса

**Цель:** Metroid Prime Unbound v0.46.0 работает на **ANBERNIC RG40XX H / muOS 2601.0 Jacaranda** с корректными графикой, интерфейсом, управлением, звуком и сохранениями. 30 FPS при 640×480 — следующий отдельный этап.

**Текущий этап:** пользователь согласовал **ранний integration slice на настоящем коде** до остального переноса. Подготовлен [подробный план IG-01…IG-14](H700_INTEGRATION_PLAN.md): 13 локальных итераций и один общий device-gate. **План ожидает утверждения; все IG не начаты.** Исследовательское основание — [архитектурное заключение](H700_ARCHITECTURE_DECISION.md). Production-код не менялся, устройство в этой итерации не использовалось.

## Что готово

- [Аудит базы и донора](H700_PORT_REVIEW.md), [краткий обзор](H700_PORT_SUMMARY.md), [правила работы](H700_WORKFLOW.md), [firmware ABI snapshot](H700_RUNTIME_ABI.md).
- [Graphics demo](H700_GRAPHICS_PROBE.md): offscreen 2 кадра и последовательный EGLImage bridge 120 кадров — device PASS в своём ограниченном сценарии. Не production threading/lifetime и не полноценная визуальная приёмка.
- [Contract-v2](H700_CONTRACT_PROBE.md) на Mali воспроизвёл отказ исходных U layouts: 17 fragment textures при лимите 16. RequestDevice PASS, общий exit 1 без timeout. Этот failure не скрыт.
- После [сравнения решений](H700_LAYOUT_OPTIONS.md) **[mixed-v2](H700_MIXED_FETCH_PROBE.md) прошёл на H700:** оба layouts/pipelines, 8 draws / 32768 pixels, updates, GPU-preserved growth и чтение за старой границей; exit 0 без timeout. Исправление shader демки — явный `flat, either`; подготовка: 71 helper tests/build/ABI/CPU/Null PASS. Повторять этот же опыт не требуется.
- Новое исследование определило **main presenter + ранняя reservation двух slots**, progress/cancellation в реальных waits U; **два постоянных imports на lifetime device**, cleanup через всю Dawn share group. Патч Dawn ради per-import cleanup для этого bounded контракта не нужен. Найден необходимый будущий reset static holders `vis_query`; сейчас не исправлялся.
- **PF-02 build/API:** 252 SDL function references, реальные Linux consumers, C++20/Aurora thread, все 33 MusyX C units/whole-archive link, 19 nod API references — PASS. Дополнительно получены exact pinned zlib-ng 2.3.3 / zstd 1.5.7 / nod alpha.12 release archives; hashes, static builds/API link и provider/version-node gate (firmware + bundled SDL) — PASS. Runtime/kernel совместимость и полная сборка игры этим не доказаны.

## Что ещё не подтверждено

**Один архитектурный runtime-допуск:** совместная работа pinned Dawn/Mali с реальными resource/render/compiler потоками, main presenter и полным teardown всей EGL share group. Static contract определён, совместный device PASS отсутствует.

**Подход раннего slice согласован.** Не отдельная копия renderer: diagnostic frontend линкует штатные Aurora targets, постоянный H700 path остаётся в игре. До PASS единственного общего gate остальной renderer-перенос блокируется. Конкретные итерации и [приёмка](H700_INTEGRATION_PLAN.md#5-конечная-device-приёмка) ещё утверждаются; device-запуск отдельно не разрешён.

Также не проверены полная игра/эффекты/UI, input/audio, kernel 4.9 fallback nod, реальный save/load и возврат в меню. Это приёмка переноса, не обязательный набор предплановых probes. ABI snapshot не является полным списком firmware exports.

## Следующий шаг и согласование

Утвердить [план ранней интеграции](H700_INTEGRATION_PLAN.md), затем начать IG-01 без новых исследований того же вопроса. План включает прежний **EOL image только для slice**, fatal-exit вместо live recovery, реальные mixed device request/shader/resources и normal-drain contract. Предлагает **EFB resize при постоянных native slots + два поколения** вместо live EGLSurface recreation; это изменение приёмки выделено явно, не объявлено автоматически принятым. План всей игры ещё не составлен.

**В этой итерации:** адресная сверка source seams и подготовка плана/документации; новых сборок, tests, downloads и device runs не было. Пакеты/образ/production/донор не менялись, `.DS_Store` сохранены. Прежние device tests очищены, firmware hashes были неизменны. Утверждение IG-01…IG-13 разрешит локальную реализацию по плану; IG-14/deploy/device window требуют отдельного согласия.

**Ориентир до каждого действия:** цель → конкретный вопрос целевого пути → минимальная проверка → решение по результату → остановка. Полнота вспомогательной матрицы не цель; правила — [H700_WORKFLOW.md](H700_WORKFLOW.md).
