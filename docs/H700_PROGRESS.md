# H700 — прогресс переноса

**Цель:** новая база игры работает на **ANBERNIC RG40XX H / muOS 2601.0 Jacaranda** с корректными графикой, интерфейсом, управлением, звуком и сохранениями. Оптимизация к 30 FPS при 640×480 — следующий отдельный этап.

**Текущий этап:** подготовка плана; изолированная Dawn/EGLImage демка прошла программные проверки на реальной приставке. Пользователь видел квадранты и маркер; артефактов при кратком наблюдении не заметил. Возврат меню не проверен: после закрытия демки приставка выключена. Завершён следующий [статический разбор требований, потоков и lifetime](H700_DAWN_INTEGRATION_GAPS.md); план реализации ещё не утверждён, game/platform code не менялся.

## Готово

- Исследованы донорские коммиты и база `v0.46.0`; подготовлены [полный отчёт](H700_PORT_REVIEW.md), [короткая версия](H700_PORT_SUMMARY.md) и справочники платформы.
- Записаны [правила итераций](H700_WORKFLOW.md), [предплановые проверки](H700_PREFLIGHT.md) и [находки для оптимизации](H700_OPTIMIZATION_NOTES.md).
- Найден точный Dawn ref в `Timdeuces/dawn`, выполнен [разбор EGL/import/sync](H700_DAWN_EGL_RESEARCH.md) и сохранён [firmware ABI snapshot](H700_RUNTIME_ABI.md).
- **ENV-01/02:** [отдельный ARM64 builder](H700_BUILD_ENVIRONMENT.md) с подходящим ABI; **43/43 tests**, compile/link, metadata gate и container smoke tests — PASS. EOL-компромисс документирован; старый image сохранён.
- **Device demo:** по отдельному разрешению на RG40XX H прошли loader/CPU checks, Dawn offscreen **2 кадра** и EGLImage bridge **120 кадров**; exit 0, pixels/order и dedicated-context teardown assertions — PASS. [Точные результаты и ограничения](H700_GRAPHICS_PROBE.md).
- Тестовая установка в `/tmp` удалена, процессов демки не осталось. Проверенные firmware libraries не изменились; игра и сохранения не затрагивались.
- После демки статически установлен texture-fetch layout budget mismatch U (17/18 textures против 16/17), уточнены границы depth/optional fallbacks и схема потоков. Применимость fetch path к Mali ещё не измерена; это не hardware failure. [Находки, варианты и предлагаемые проверки GAP-A…E](H700_DAWN_INTEGRATION_GAPS.md).

## Осталось до плана реализации

- Полный набор graphics limits/features U и PF-02 по game-facing SDL/MusyX/nod ещё не проверены. Подробная визуальная проверка и возврат меню остаются отдельными непроверенными пунктами.
- Согласовать способ интеграции; заранее решить production lifetime/recreation и threading/sync.
- Определить build profile полной игры, безопасную установку и приёмку; утвердить итерации по 1–2 часа.

**Блокеры до плана:** конкретные device/layout budgets, production alias lifetime и main-presenter progress при многопоточных ожиданиях ещё не закрыты. Прежний link blocker и минимальный interop barrier сняты для демки; полная игра и 30 FPS не подтверждены.

**Предлагаемый следующий шаг:** согласовать GAP-A (полный U capability/request gate отдельной демки) и GAP-B (validation реальных GX layouts). Затем по результатам — shader/formats, threading/progress и выбранный lifecycle contract. Новые probes и production-перенос не начаты.

**Нужно от пользователя:** согласование ближайшего объёма GAP-A/B, если продолжаем подготовку probes. Включать приставку для подготовки пока не нужно; SSH/fullscreen tests потребуют отдельного окна.
