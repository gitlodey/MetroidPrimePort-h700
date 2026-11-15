# H700 — прогресс переноса

**Цель:** новая база игры работает на **ANBERNIC RG40XX H / muOS 2601.0 Jacaranda** с корректными графикой, интерфейсом, управлением, звуком и сохранениями. Оптимизация к 30 FPS при 640×480 — следующий отдельный этап.

**Текущий этап:** подготовка плана. **Mixed-fetch демка `mixed-v2` прошла на реальном H700:** оба layouts/pipelines, 8 draws / 32768 pixels, updates и bounded buffer growth — PASS, exit 0 без timeout. Тем самым подтверждён локальный путь обхода исходного U layout-барьера 17 > 16, но production renderer ещё не адаптирован. Прежняя EGLImage bridge демка также прошла свой bounded сценарий; полная интеграция/игра не проверены. План production-реализации не утверждён.

## Готово

- Исследованы донорские коммиты и база `v0.46.0`; подготовлены [полный отчёт](H700_PORT_REVIEW.md), [короткая версия](H700_PORT_SUMMARY.md) и справочники платформы.
- Записаны [правила итераций](H700_WORKFLOW.md), [предплановые проверки](H700_PREFLIGHT.md) и [находки для оптимизации](H700_OPTIMIZATION_NOTES.md).
- Найден точный Dawn ref в `Timdeuces/dawn`, выполнен [разбор EGL/import/sync](H700_DAWN_EGL_RESEARCH.md) и сохранён [firmware ABI snapshot](H700_RUNTIME_ABI.md).
- **ENV-01/02:** [отдельный ARM64 builder](H700_BUILD_ENVIRONMENT.md) с подходящим ABI; **43/43 tests**, compile/link, metadata gate и container smoke tests — PASS. EOL-компромисс документирован; старый image сохранён.
- **Device demo:** по отдельному разрешению на RG40XX H прошли loader/CPU checks, Dawn offscreen **2 кадра** и EGLImage bridge **120 кадров**; exit 0, pixels/order и dedicated-context teardown assertions — PASS. [Точные результаты и ограничения](H700_GRAPHICS_PROBE.md).
- Тестовая установка в `/tmp` удалена, процессов демки не осталось. Проверенные firmware libraries не изменились; игра и сохранения не затрагивались.
- После демки выполнен [статический разбор](H700_DAWN_INTEGRATION_GAPS.md): texture-fetch layout mismatch U, границы depth/optional fallbacks, потоки и lifetime.
- **Новый device test `contract-v2`:** default U policy действительно выбирает texture fetch (Compatibility vertex storage buffers = 0), no-compute = 0, lightmap = 0. RequestDevice — PASS; normal/shadow GX layouts — **FAIL 17 > 16**, общий exit 1 без timeout. [Artifact, limits/features, точные diagnostics и cleanup](H700_CONTRACT_PROBE.md). Его временная установка удалена, firmware hashes неизменны. Последующий mixed-fetch run описан ниже.

## Обязательный ориентир текущей работы

**До любого действия/исследования/гипотезы — цель → конкретный вопрос H700 → минимальная проверка → решение по исходам → условие остановки.** Полнота вспомогательной матрицы не цель. Каноническое правило — [H700_WORKFLOW.md](H700_WORKFLOW.md), entry points — [AGENTS.md](../AGENTS.md) и [CLAUDE.md](../CLAUDE.md).

**Подготовка расширенной демки прошла, selected-layout device gate — нет.** 62 helper tests, compile/link, ABI, CPU checks и четыре Null controls бюджета 16 — PASS. Отдельно разрешённый H700 run выявил реальную несовместимость неизменённого U layout, не проблему Null. Null profile 17 не является gate и не объявлен проверенным; Dawn/окружение не менялись.

## Сравнение решений текущего барьера

[Сравнены варианты 1 и 3](H700_LAYOUT_OPTIONS.md). Вариант 1 — fixed non-PBR profile с 10 fragment textures, требует guards материалов/assets/cache. Вариант 3 — vertex texture fetch + fragment SSBO, сохраняет 16 графических textures. Первый `mixed-v1` run остановился на Compatibility interpolation `First` в shader демки; история отказа сохранена в [отчёте](H700_MIXED_FETCH_PROBE.md), это не отказ SSBO.

**Исправленный `mixed-v2` — device PASS:** явный `flat, either`, прежний exact pixel oracle. Подготовка: 71 helper tests, build/ABI/CPU, 4+4 Null layout controls, 2 Null shader pipelines, hashes/licenses/manifest — PASS. Затем отдельно разрешённый Mali run: **8/8 draws и 32768/32768 pixels**, updates, GPU-preserved growth и чтение за старой границей — PASS. Финальный marker и exit 0; test directory удалён, процессов нет, firmware hashes неизменны. [Artifact, точные результаты и границы](H700_MIXED_FETCH_PROBE.md).

## Осталось до плана реализации

- Mixed data path подтверждён на H700 в harness; перенести его в настоящий shader generator/resource lifecycle можно только по согласованному плану. Другие GX shader/resource operations, threading/recreation и PF-02 по game-facing SDL/MusyX/nod остаются открыты. Подробная визуальная проверка и возврат меню также не подтверждены.
- Согласовать способ интеграции; заранее решить production lifetime/recreation и threading/sync.
- Определить build profile полной игры, безопасную установку и приёмку; утвердить итерации по 1–2 часа.

**Реальное состояние:** исходный U layout всё ещё требует адаптации, но выбранный mixed обход теперь подтверждён реальным shader execution, не только limits/Null. Замена Dawn/firmware/backend для этого вопроса не потребовалась. Полная игра и 30 FPS не подтверждены; lifetime и многопоточная интеграция требуют отдельных решений.

**Следующий предлагаемый шаг:** согласовать включение mixed resource contract в план renderer-переноса с конкретными source/cache/lifecycle изменениями и приёмкой, учитывая оставшиеся известные вопросы интеграции. Повторять ту же демку для уже отвеченного вопроса не требуется.

**Нужно от пользователя:** согласовать следующий шаг планирования; device experiment завершён и очищен. Production-реализация и дополнительные graphics runs не начинались.
