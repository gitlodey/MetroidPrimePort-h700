# H700 — прогресс переноса

**Цель:** новая база игры работает на **ANBERNIC RG40XX H / muOS 2601.0 Jacaranda** с корректными графикой, интерфейсом, управлением, звуком и сохранениями. Оптимизация к 30 FPS при 640×480 — следующий отдельный этап.

**Текущий этап:** подготовка плана. Прежняя Dawn/EGLImage демка прошла bounded pixel/order/teardown проверки на H700. Последующий отдельно разрешённый `capabilities` test создал device с U request, но **оба выбранных GX layouts отклонены: 17 fragment textures при лимите 16**. Отдельно разрешённая mixed-fetch демка затем прошла обе layout checks на Mali, но остановилась до первого workload draw: synthetic shader использовал недопустимый Compatibility interpolation sampling `First`. По разрешению пользователя это исправлено в новом `mixed-v2`; пакет прошёл подготовку, но на Mali ещё не запускался. План production-реализации не утверждён, game/platform code не менялся.

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

По запросу пользователя выполнено [статическое сравнение вариантов 1 и 3](H700_LAYOUT_OPTIONS.md). Вариант 1 — fixed non-PBR profile: 10 fragment textures, но нужны guards материалов/assets/cache и перестройка group 2. Вариант 3 — vertex texture fetch + fragment SSBO: 16 textures + 1 fragment storage buffer; исходный buffer и uploads уже существуют. **Вариант 3 разрешён только как отдельный эксперимент**, не production-реализация. [Mixed-fetch демка](H700_MIXED_FETCH_PROBE.md) собрана в `build/h700-graphics-probe-mixed-v1/package/`: 70 helper tests, inputs/8 reference hashes, compile/link, ABI, CPU и оба Null layout self-tests по 4 cases — PASS. Hashes/licenses/manifest проверены. Затем по отдельному разрешению выполнен **один H700 run**: RequestDevice и оба mixed layouts — PASS; render pipeline setup — **FAIL из-за flat interpolation sampling `First` в shader демки**, exit 1 без timeout. Draw/pixels/updates/growth ещё не выполнены. [Точные результаты и cleanup](H700_MIXED_FETCH_PROBE.md).

**Точечная правка готова:** `flat, either` вместо неявного `First`, без изменения u32 checksum/pixel oracle/8 workload cases. Новый `build/h700-graphics-probe-mixed-v2/package/`: **71 helper tests**, compile/link/ABI/CPU, 4+4 Null layout controls и **2 mixed shader/render pipelines на Null Compatibility** — PASS; hashes/licenses/manifest проверены. Это не GPU execution, новый device run не выполнялся.

## Осталось до плана реализации

- Limits/features, U RequestDevice и proposed mixed layouts измерены в harness; production layout не адаптирован. Compatibility-ошибка synthetic shader исправлена; отдельно проверить mixed execution на H700. Threading/recreation и PF-02 по game-facing SDL/MusyX/nod ещё не проверены. Подробная визуальная проверка и возврат меню также не подтверждены.
- Согласовать способ интеграции; заранее решить production lifetime/recreation и threading/sync.
- Определить build profile полной игры, безопасную установку и приёмку; утвердить итерации по 1–2 часа.

**Реальное состояние:** подтверждён конкретный layout-барьер **неизменённого U** на H700; это не доказательство невозможности Dawn или необходимости менять firmware/backend. Полная игра и 30 FPS не подтверждены. Lifetime и многопоточная интеграция требуют отдельных решений; Null limit не входит в список блокеров порта.

**Следующий шаг:** отдельно согласованный повторный mixed-fetch run с готовым `mixed-v2` package. Layout budget принят на Mali, но 8 draws / 32768 pixels и buffer updates/growth ещё не проверены; это не причина отказываться от варианта 3 или менять backend.

**Нужно от пользователя:** разрешить повторный run и подтвердить свободное окно приставки. Правка и новый пакет готовы без SSH/deploy; прежний test directory удалён, firmware hashes после того run неизменны. Production-реализация не начиналась.
