# H700 — весь оставшийся путь до принятой игры

**2026-11-17: общий пересмотр GM-12…GM-21 подготовлен, но ещё НЕ утверждён.** Это предложение одной связной редакции, не начало реализации. Полный текст: **[пересмотр остатка, редакция 2](H700_GAME_REMAINING_PLAN.md)**. [Прежний утверждённый план и история](H700_GAME_PLAN.md), [текущий прогресс](H700_PROGRESS.md).

## Что уже готово

GM-01…GM-10, GM-11a и GM-11b завершены локально и отправлены. Есть настоящий ARM64 executable, retail H700 defaults, healthy reset/exit boundaries, terminal CPU checkpoint/cleanup и исправление request-before-buffer lifetime с реальными DVD/DMA waits.

Последняя production-итерация — `b9b1545fe`: сборка,21/21CPU/no-init/DVD,269/269FIFO,53/53render/lifecycle и H700/non-H700 compile PASS. **Новая полная игра на приставке ещё не запускалась.** Работающая IG-demo — опора, но не доказательство game graphics/input/audio/CARD. Достижение30FPS — следующий отдельный этап.

## Почему пересмотрели не только GM-12

У прежнего плана были недооценены связи между частями:

- Ошибку skinning мало обнаружить: её должны остановить реальные model/shadow/boid consumers, draw/message обходы и root до обычного завершения кадра.
- Безопасные defaults не гарантируют безопасный сохранённый INI. Он может снова включить shared card, quick-resume или mods; некоторые настройки применяют эффекты уже при чтении.
- Игрового package/test mode пока нет. Diagnostic package не охватывает все game sources/notices и не задаёт loader path игровому executable.
- Demo-launcher имеет watchdog и временные каталоги. Игра должна сохранять user/card/cache, не убивать процесс и не снимать lock, пока не завершились игра **и** log copier.
- Save/load нужно доказать **до reset**. После исправления требуется новый идентифицированный RC и повторная приёмка, а не перенос старого PASS на другой binary.

Это подтверждено исходниками; renderer, image, pinned dependencies и цели приёмки менять из-за этих findings не требуется.

## Предлагаемый остаток целиком

| Блок | Что делаем | Когда считаем завершённым |
| --- | --- | --- |
| **GM-12a…d** | Единые sizing/status/wait правила → непосредственные consumers → dispatch/boot/frame/root → только затем включение новых failure producers | Ни неверный workspace, ни failed shadow/draw не используются; ошибка достигает существующего безопасного cleanup или честного fail-stop. Без deep throw, fake tokens и SkinPool rewrite |
| **GM-13a…b** | Независимый сквозной lifetime review, production-linked CPU regressions, ARM64 game и полный non-H700 core/GX compile | Проверен весь включённый status-path, а не только helpers; CPU/source evidence не выдано за игру на Mali |
| **GM-14a…b** | Физическая изоляция установки; проверка paths/migration; безопасное применение INI и protected UI changes | Старая установка не читается/не меняется. Startup mismatch отвергается до effects/owners; live incompatible toggle не меняет state/INI и не завершает игру. Bindings и разрешённые настройки сохраняются |
| **GM-15a…c** | Launcher/environment, child/lock/signals, bounded logging | Один буквальный disc argument, сохранены frontend/audio routing; нет kill watchdog. Exit и log finalization различаются; живой/неизвестный child/copier не разрешает следующий запуск |
| **GM-16a…c** | Настоящий game test/package mode, local loader path, полный provenance/ABI/import report и notices | Полный игровой payload проверен; никакой подмены system libs/неполного slice audit. Неизвестная exact firmware-symbol coverage названа явно |
| **GM-17a…b** | Source freeze → fresh build A → независимый fresh B → сравнение → один RC; заранее подготовленные маршруты | Один immutable package/hash, все local gates, известный доступ к station и поздним effects. Recipe reproducibility не выдаётся за byte-identical без сравнения |
| **GM-18 / A** | Первый normal Ports launch, logo/movie, new-game gameplay, HUD/pad/слышимый audio, normal Exit | Не только меню; физический результат подтверждён пользователем. До20мин по отдельному разрешению |
| **GM-19 / B** | Повторяемый маршрут/RAM → station save → Exit → новый процесс/CARD load → только затем reset/reload | Собственная карта, реальный сохранённый progress, settings/bindings persist и same-generation reset. До30мин, разрешение сразу на два перечисленных процесса |
| **GM-20 / C** | Materials, skinned actors/weapon/pause doll, water/reflections/bloom, Scan/Thermal/X-ray/EFB, все кнопки/audio | Полный конечный checklist на заранее доступных сценах; missing mandatory scene — pending, не N/A. До30мин по отдельному разрешению |
| **GM-21** | Отчёт и manual install/update/uninstall именно испытанного пакета | A/B/C одного финального RC, ограничения и пользовательская приёмка. Не пересобирать binary после PASS |

Подготовительные GM-12 commits **не включают новые skin failures**, пока consumers/root ещё не готовы. Каждый подшаг проверяется и получает отдельный commit + push. Известный прямой consumer того же status-контракта входит в согласованный scope, а не становится поводом повторно утверждать каждую правку.

## Существенные решения для одного согласования

1. **Расширенный сквозной scope GM-12/13**, а не leaf-only исправление; ring32MiB и scheduler сохраняются.
2. **H700 acceptance-profile validation** до side-effectful чтения/применения protected settings. Не переписываем INI молча; startup rejection — nonzero до owners, live rejection — объяснение без изменения состояния/внезапного выхода. Common parser selftests не ослабляются.
3. **Game-specific packaging/launcher и ограниченные логи:** собственные постоянные каталоги, один image argument, сохранённый audio route, local `$ORIGIN/lib`, unique per-run bounded sinks и подтверждённое завершение log copier. Потеря evidence не становится PASS.
4. **Same-image локальные corrections после device findings:** узкий fix существующего решения + regression + новый RC, без нового approval каждой механической правки. Смена архитектуры/pins/image/security/критериев всё равно требует остановки.
5. **Device permission на конечную последовательность окна**, включая обязательный второй процесс B. Это не разрешение device access сейчас; deploy/SSH/log observation и дополнительные retries согласуются явно.

Все D1…D11 технически сохраняются; явно расширяется только временной scope same-image сборок на corrections этой приёмки и уточняется модель согласования запусков. Новые dependencies/backend, generic exception framework, отключение retail effects, изменение RAM thresholds/sizes и автоматический kill не разрешаются.

## Как действуем при runtime-дефекте

Фиксируем failing RC/scene → минимальная диагностика → узкий local fix/test → commit/push → новый frozen RC/hash. Обновление только stopped test installation и только payload; `user/`/карты/настройки/кэш сохраняются. Обновление на устройстве требует разрешения.

Сначала можно повторить затронутый сценарий для диагностики. Для итогового GM-21 A/B/C повторяются **на одном финальном RC**. Повтор A на сохранённой установке обозначается warm/update, не притворяется новым cold-install test. User/cache ради PASS не очищаются.

При below96MiB, потере RAM/log evidence или зависании прекращаем дальнейшую нагрузку, normal Exit в безопасной точке, не прерываем CARD. Unknown/fail-stop — не нормальный выход и не разрешение следующего запуска. Force termination — отдельное решение.96MiB — sampled начальная policy, не измеренная гарантия.

## Сколько осталось и что нужно от пользователя

Осталось **16 локальных подшагов: ориентир16–32ч активной работы**, плюс4–8ч подготовки/разбора device/final report — **около20–40ч активной работы**, без cold-build waits, ожидания пользователя, прохождения к нужным сценам и correction loops. Это новая оценка остатка, не обещание срока; прежние30–42ч относились ко всему исходному плану.

**Сейчас нужен один ответ на редакцию2 целиком**, а не согласование очередного подпункта GM-12. Production work не продолжается до этого ответа.

Позже, до готовности device-окон: один legal GameCube release/container, новый stable install path/Ports invocation, конкретный station/room маршрут и доступ к Thermal/X-ray/другим обязательным сценам. Private image/save paths и данные не публикуются. Окна A/B/C, SSH/deploy и private test-copy preparation разрешаются отдельно.
