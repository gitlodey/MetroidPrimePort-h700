# H700 — прогресс переноса

**Цель:** новая база игры работает на **ANBERNIC RG40XX H / muOS 2601.0 Jacaranda** с корректными графикой, интерфейсом, управлением, звуком и сохранениями. Оптимизация к 30 FPS при 640×480 — следующий отдельный этап.

**Текущий этап:** подготовка плана. Прежняя Dawn/EGLImage демка прошла bounded pixel/order/teardown проверки на H700. Последующий отдельно разрешённый `capabilities` test создал device с U request, но **оба выбранных GX layouts отклонены: 17 fragment textures при лимите 16**. До согласования адаптации новые эксперименты остановлены; план реализации не утверждён, game/platform code не менялся.

## Готово

- Исследованы донорские коммиты и база `v0.46.0`; подготовлены [полный отчёт](H700_PORT_REVIEW.md), [короткая версия](H700_PORT_SUMMARY.md) и справочники платформы.
- Записаны [правила итераций](H700_WORKFLOW.md), [предплановые проверки](H700_PREFLIGHT.md) и [находки для оптимизации](H700_OPTIMIZATION_NOTES.md).
- Найден точный Dawn ref в `Timdeuces/dawn`, выполнен [разбор EGL/import/sync](H700_DAWN_EGL_RESEARCH.md) и сохранён [firmware ABI snapshot](H700_RUNTIME_ABI.md).
- **ENV-01/02:** [отдельный ARM64 builder](H700_BUILD_ENVIRONMENT.md) с подходящим ABI; **43/43 tests**, compile/link, metadata gate и container smoke tests — PASS. EOL-компромисс документирован; старый image сохранён.
- **Device demo:** по отдельному разрешению на RG40XX H прошли loader/CPU checks, Dawn offscreen **2 кадра** и EGLImage bridge **120 кадров**; exit 0, pixels/order и dedicated-context teardown assertions — PASS. [Точные результаты и ограничения](H700_GRAPHICS_PROBE.md).
- Тестовая установка в `/tmp` удалена, процессов демки не осталось. Проверенные firmware libraries не изменились; игра и сохранения не затрагивались.
- После демки выполнен [статический разбор](H700_DAWN_INTEGRATION_GAPS.md): texture-fetch layout mismatch U, границы depth/optional fallbacks, потоки и lifetime.
- **Новый device test `contract-v2`:** default U policy действительно выбирает texture fetch (Compatibility vertex storage buffers = 0), no-compute = 0, lightmap = 0. RequestDevice — PASS; normal/shadow GX layouts — **FAIL 17 > 16**, общий exit 1 без timeout. [Artifact, limits/features, точные diagnostics и cleanup](H700_CONTRACT_PROBE.md). Временная установка удалена, firmware hashes неизменны; новых запусков после отказа нет.

## Обязательный ориентир текущей работы

**До любого действия/исследования/гипотезы — цель → конкретный вопрос H700 → минимальная проверка → решение по исходам → условие остановки.** Полнота вспомогательной матрицы не цель. Каноническое правило — [H700_WORKFLOW.md](H700_WORKFLOW.md), entry points — [AGENTS.md](../AGENTS.md) и [CLAUDE.md](../CLAUDE.md).

**Подготовка расширенной демки прошла, selected-layout device gate — нет.** 62 helper tests, compile/link, ABI, CPU checks и четыре Null controls бюджета 16 — PASS. Отдельно разрешённый H700 run выявил реальную несовместимость неизменённого U layout, не проблему Null. Null profile 17 не является gate и не объявлен проверенным; Dawn/окружение не менялись.

## Сравнение решений текущего барьера

По запросу пользователя выполнено [статическое сравнение вариантов 1 и 3](H700_LAYOUT_OPTIONS.md). Вариант 1 — fixed non-PBR profile: 10 fragment textures, но нужны guards материалов/assets/cache и перестройка group 2. Вариант 3 — vertex texture fetch + fragment SSBO: 16 textures + 1 fragment storage buffer; исходный buffer и uploads уже существуют. **Рекомендован вариант 3 как более локальный кандидат, выбор и реализация ещё не утверждены.** Его shader/readback workload на Mali не проверен; новых сборок/device runs нет.

## Осталось до плана реализации

- Limits/features и U RequestDevice измерены в harness; selected layout требует адаптации. Shader/resource workload, threading/recreation и PF-02 по game-facing SDL/MusyX/nod ещё не проверены. Подробная визуальная проверка и возврат меню также не подтверждены.
- Согласовать способ интеграции; заранее решить production lifetime/recreation и threading/sync.
- Определить build profile полной игры, безопасную установку и приёмку; утвердить итерации по 1–2 часа.

**Реальное состояние:** подтверждён конкретный layout-барьер **неизменённого U** на H700; это не доказательство невозможности Dawn или необходимости менять firmware/backend. Полная игра и 30 FPS не подтверждены. Lifetime и многопоточная интеграция требуют отдельных решений; Null limit не входит в список блокеров порта.

**Следующий предлагаемый шаг:** согласовать ограниченный mixed-fetch probe по [сравнению](H700_LAYOUT_OPTIONS.md): layouts бюджета 16, vertex texture + fragment buffer чтение и контрольные pixels/updates. Это новый вопрос, не повтор неизменённого `capabilities` и не начало production-переноса. Не менять visibility без замены потребителя и не форсировать vertex SSBO при limit 0.

**Нужно от пользователя:** согласовать подготовку следующей probe-итерации; device run требует отдельного окна. Production-реализация не разрешена. Предыдущий device test завершён и очищен.
