# H700 — необходимый остаток до запуска и приёмки игры

**Редакция3, 2026-11-17. УТВЕРЖДЕНА пользователем: «Все, новый план утвержден. Можешь работать». Локальная реализация GM-12…GM-17 и commit/push завершённых итераций разрешены; device-окна отдельно.** Заменяет неутверждённую редакцию2 с16local substeps. Завершённые GM-01…GM-11b и исходные архитектурные решения не переоткрываются. [Коротко](H700_GAME_SUMMARY.md), [история и D1…D11](H700_GAME_PLAN.md), [прогресс](H700_PROGRESS.md), [обязательные правила](H700_WORKFLOW.md).

## 1. Цель, исходная точка и рабочее правило

**Результат — Metroid Prime Unbound v0.46.0, запущенная и проверенная на ANBERNIC RG40XX H / H700, muOS 2601.0 Jacaranda:** нормальные graphics/UI, встроенный pad, слышимый audio, actual CARD save/load. Не новый framework и не максимальное число тестов.30FPS — следующий этап.

Уже есть full ARM64 game build, H700 defaults, actual Aurora backend, healthy owner boundaries и terminal cleanup. Последняя production-итерация `b9b1545fe`: game build,21/21CPU/no-init/DVD,269/269FIFO,53/53render/lifecycle, non-H700 core/GX и5changed game TU compile PASS. **Реальная новая игра на приставке ещё не проверена.** Эти результаты не повторяем ради увеличения таблицы и не называем device readiness.

Hardware, firmware ABI snapshot, display path и build inputs уже изучены и записаны в документах. Используем их: exact approved ARM64 image,14inputs, SDL3-over-SDL2 → firmware SDL2 mali → EGL/GLES + pinned Dawn/nod, namespace `h700-mixed-v1`. Не нужен новый inventory, backend comparison, Dawn rebuild или повтор IG-demo без конкретного противоречия.

**До каждого действия:** какой применимый defect/необходимый deliverable приближает эту игру к запуску → минимальное достаточное действие → проверка результата → остановка. Сначала необходимость, затем безопасный способ. Безопасность избыточного действия не делает его обязательным.

## 2. Что действительно осталось, а что исключено

| Категория | Решение |
| --- | --- |
| **Известная проблема game path** | Skinning init/ring waits и отказ allocation не имеют замкнутой передачи результата до root. Чиним вместе с непосредственными consumers; не ограничиваемся leaf early return |
| **Необходимые deliverables** | Изолированная установка, game launcher, настоящий game package с SDL/ABI/notices/hash, повторная fresh build, реальная game acceptance |
| **Риски, проверяемые на приставке** | Actual game/nod path на kernel4.9, physical effects/pad/audio, память сцен, reset/exit, station save/load. Не пытаемся заранее заменить их бесконечными CPU probes |
| **Необязательное усиление — НЕ часть работы сейчас** | Запрет всех небазовых INI/UI choices, новый settings prevalidation framework, ready/health/final protocol log copier, полная firmware-symbol/platform matrix, универсальная exception/OOM обработка, automatic monitoring/recovery |

Сохранены baseline640×480/4:3, validation/robustness, MEM1 24MiB, unpinned scale1, ring32MiB, pressure192/128MiB и sampled headroom96MiB, original assets/effects, Modern/twin-stick, cap30/sim60, quick-resume/shared card OFF. Пороги — выбранная policy, не измеренная достаточность.

Проверяем **собственную чистую установку с согласованными настройками**, не строим sandbox против произвольной ручной подмены INI/директорий. Собственные settings/bindings сохраняются между запусками. Не молча исправлять чужой/неподходящий INI: при обнаруженном конфликте остановить конкретный запуск и объяснить, что вернуть в согласованный профиль. Production parser/UI не переписывается ради гипотетического будущего toggle.

## 3. Локальная часть: закончить известное и получить игровой RC

### GM-12. Замкнуть skinning/status path

**Зачем:** это реальный путь renderer/animated actors, включая weapon/pause/shadow и внешние swarm workspaces. Сейчас init ждёт token внутри construction, arithmetic может ошибиться до allocation, `Calculate` возвращает void, а null workspace способен привести к обычному draw вместо отказа.

**Scope — уже известные непосредственные участки:**

- `DolphinCSkinnedModel.cpp`/header: root-side first handshake, checked sizing всех его allocation paths, ring wait/progress/health, результат до publication workspace;
- `CAnimData`, `CModelData`, `CProjectedShadow`, `CFishCloud`, `CSnakeWeedSwarm`, `CWallCrawlerSwarm`: отказ расчёта/пары external workspaces не потребляется как успех; нет failed shadow copy/publication или invalid mask/index use;
- затронутые `CCubeRenderer`, `CStateManager`, `CMFGame`, `CIOWinManager` и main boundaries: после ошибки не запускается следующий consumer/pass/message и не удаляется IOWin до root checkpoint;
- известный boot PAK loop `AddPaksAndFactories → PostInitialize → RsMain` должен вернуть terminal status, не продолжить init и не throw из конструктора;
- root проверяет game failure **или** backend failure до normal debug/loader/EndScene. Failed frame tail не освобождает delayed allocations и не выполняет normal reflection retirement/success accounting в верхнем `CCubeRenderer::EndScene`.

**Решение:** небольшой sticky main-owned failure status для выявленных ошибок и явный bool в непосредственно потребляемых невиртуальных APIs; на существующих virtual/C callbacks — проверки статуса, без смены ABI. Latch ничего не join-ит/не освобождает. Вложенный call сначала возвращается через необходимые restore scopes; **только root** использует готовый GM-11b CPU checkpoint/guards/cleanup. При Unknown/nonempty obligations — retain/fail-stop.

Checked products/alignment/narrowing выполняются до pointer use; ring-only capacity с прежним `+32`, heap/external workspace не ограничиваем произвольным новым лимитом. Известный skin allocation/metadata `bad_alloc` преобразуется в failure до write-gather/jobs, не общий catch всех исключений игры. Success — настоящий FIFO token; wrap и reclaim префикса сохраняются. SkinPool `Run` ждёт реальные jobs до последнего обращения к inputs/output/stack; scheduler, число workers и ring capacity не меняем.

**Безопасная последовательность commits внутри GM-12:**

1. Boot/frame/root возврат уже существующего backend failure и приём game status. Не меняем алгоритмы renderer и не вводим новые skin failure producers.
2. Непосредственные consumers/workspace/dispatch checks. Пока новые skin failures доступны только CPU fixtures, прежние ordinary paths не становятся частично отказными.
3. Включить root handshake, checked allocation и progress-aware skin waits **только после** готовности всего пути возврата. Включение и его regressions — вместе.

Отдельная стадия создания универсальной policy-библиотеки не нужна. Проверки добавляются к конкретному production seam по ходу исправления: actual pending/token completion/wrap, impossible size до публикации указателей, no invalid draw/shadow/workspace use, сохранённый running-job lifetime. Source binding связывает fixtures с реальными callers, но не выдаётся за execution всей игры.

**Готово:** каждый включённый skin failure либо не возникает на успешном пути, либо доходит до root без invalid use/раннего destruction; ordinary path сохранён. No deep token/fake completion/silent unskinned fallback. Штатный shim breakpoint/retrace синхронный: новый механизм его ожидания не планируется.

Это самый большой известный остаток; выполняем его проверенными итерациями порядка1–2ч, а не объявляем заранее весь wiring задачей на1час. Прямой caller/header/test того же статуса входит в исправление, не требует очередного approval. Новая ownership/scheduler архитектура — отдельное решение.

### GM-13. Проверить изменённую игровую цепочку

**Зачем:** изменения уже касаются lifetime и real game consumers; нужна проверка перед выдачей artifact, а не новая матрица всех платформ.

**Делаем:** root ARM64 compile/link; относящиеся game/terminal/DVD/CFDk/skinning и Aurora FIFO/render/recording/readback CPU tests; full non-H700 `aurora_core`+`aurora_gx` и compile изменённых game TUs в отдельном output. Достаточно existing targets/seams; не нужен другой Dawn/backend для harness.

**Готово:** нет known unclosed producer→consumer→root пути, actual tests/commands/skips записаны, изменённые guards собираются. GM-11b actual DVD/DMA waits и no-command cleanup не ослаблены. Не повторяем всю прежнюю исследовательскую работу; исправляем только обнаруженную применимую regression. CPU PASS не называется запуском игры/GPU.

**Итерация:**1–2ч активной проверки/исправления, ожидание сборки отдельно.

### GM-14. Отдельная установка и реальные пути данных

**Зачем:** CARD выбирает writable `<exe>/user/` независимо от одного `MP_USER_PATH`, а старые loose files могут мигрировать. Это закрывается чистым layout, не новым settings framework.

**Делаем:** отдельный physical executable directory, собственные writable `user/` и cache, explicit user/cache paths, отсутствие старых loose settings/cards/mods/imports рядом. Начальная установка новая; обновления заменяют только payload, не `user/`. Не создаём symlink/hardlink на старую установку и не ищем её данные. Известные параметры quick-resume/share-card остаются OFF; env не перенаправляет mods/card/cache наружу.

**Проверка:** synthetic old/new installations через actual path functions в новых процессах из-за cached statics; new settings/card/cache routes, нет миграции/изменения старых sentinels. Actual game no-init settings roundtrip плюс отдельный temporary INI persistence case и scoped CARD/atomic/identity tests. Никаких личных disc/save fixtures и default чтения соседнего `../wii-save`.

**Готово:** известен точный layout для launcher, новая установка не обращается к старой по выбранному startup path, own settings/bindings сохраняются новым процессом. Проверка произвольных будущих UI manipulations и запрет upstream настроек не входят в gate.

**Итерация:**1–2ч, production path changes только если synthetic case докажет нарушение этого конкретного layout.

### GM-15. Рабочий Ports launcher без demo-опасностей

**Зачем:** запуск должен использовать правильный display/audio/data path и не прерывать CARD. Не переносим demo watchdog/одноразовый HOME/cache.

**Делаем:** тонкий launcher с одним буквальным absolute readable disc argument; fixed own installation/user/cache; `SDL_VIDEODRIVER=sdl2`, `SDL3SHIM_SDL2_VIDEODRIVER=mali`; сохраняем пять известных frontend SDL variables и существующее caller audio routing. Убираем сторонние MP/AURORA path/debug overrides; реально conflicting renderer/presence pins проверяем до sanitation. Не наследуем loader injection; library path — local package RUNPATH. PipeWire default — только по известному socket/unset правилу; не форсируем ALSA/dummy/gain.

Один game process установки, lock не снимается по одному сигналу shell, пока child ещё жив/неизвестен; перед повтором нужен actual child exit/reap. Нет killall/timeout kill/перезапуска frontend. Exit code сохраняется, fail-stop не превращается в exit0.

**Логи:** reuse существующего log copier/redaction и RAM diagnostics. Нужен только узкий H700 byte cap файлового sink с продолжением drain, без закрытия pipe/SIGPIPE/общего `ulimit -f` и без второго растущего redirected stdout. Не создаём ready/health/final messaging protocol и не держим игровую installation lock из-за уже вышедшей игры только ради log copier: он не graphics/CARD owner. Перед чтением/переносом/очисткой конкретного лога учитываем оставшийся drain; неполный/обрезанный лог не доказывает отсутствие errors. Нет автоматического удаления active logs или убийства copier.

**Проверка:** mock child, spaces/metacharacters одним argument, missing/invalid image, конфликты/env/frontend/audio, own paths, live child+signal+lock/nonzero и два штатных запуска. Для byte cap — flood fixture: pipe дренируется после cap и child не убивается. No real GPU/game запуска в container.

**Готово:** один проверенный invocation для будущего окна A, понятные exit/log paths и сохранённое состояние установки. Работа с логами не становится отдельной observability-системой или новым условием успешности engine.

**Итерации:** launcher/tests и узкий log cap при необходимости разделяются на два безопасных commits порядка1–2ч; это не две новые архитектурные задачи.

### GM-16. Настоящий game package и проверка его зависимостей

**Зачем:** существующий wrapper собирает игру, но game `--package` ещё отсутствует; integration payload/RUNPATH/source inventory не охватывают игру автоматически.

**Делаем:** добавить game packaging profile к существующим staging/build/audit helpers, без копирования framework. Root H700 staging использует local `$ORIGIN/lib`; matched pinned SDL кладётся в `lib/`. Payload — executable, нужная библиотека, launcher/template, existing notices/manifest/checksums. Нет user/settings/cards/кэша/ISO/private config в archive.

Используем existing ELF/provider audit на **game+SDL**, сопоставляя actual required SONAME/version nodes с уже имеющимся firmware snapshot. Указываем остаток неполного exact-symbol evidence, не объявляем его runtime PASS и не начинаем новый общий firmware inventory. Known missing provider/version — реальный blocker; loader uncertainty проверяется в разрешённом окне, не исправляется заменой system libs.

Сборочный manifest охватывает actual game/CMake/version/embedded inputs и используемые vendored/shared helpers,14input hashes и exact image; source bytes и package/ELF hashes фиксируются. Collector/notices дополняются только для реально входящих game components, в том числе compiled codecs; runtime mods OFF не освобождает от их лицензий. Missing primary grant не выдумывается. Локальный RC не является разрешением публичной дистрибуции game/SDK code.

**Готово:** свежий game package из зафиксированных build-relevant bytes проходит compile, relevant CPU/no-init, staged ABI/content/notices/hash checks; его библиотека не зависит от случайного build directory. Это первый qualifying fresh build. Документируем только реально работающий package CLI.

**Итерации:** адаптация/staging и artifact audit допускают отдельные проверенные commits порядка1–2ч; ожидание fresh build отдельно. Новые tools/dependencies/platform matrix не требуются.

### GM-17. Повторить рецепт и предъявить RC для первого запуска

**Зачем:** проверить воспроизводимый build recipe и установить ровно известный пакет, а не incremental смесь.

**Делаем:** второй fresh output по тому же recipe из тех же source/input/config bytes, без параллельных изменений checkout; relevant no-init/CPU и audit gates. Сравнить source/config/provider manifests, ELF/archive hashes. Различия объяснить фактами, не автоматически назвать metadata; bit-identical утверждать только при совпадении. Не нужен отдельный freeze-management framework — достаточно идентифицированного source state и проверки его неизменности.

**Готово к A:** один выбранный immutable RC/hash, manual install/launch/rollback инструкции, один выбранный legal GameCube release/container и конкретная ранняя gameplay сцена. Передаётся package без assets, private disc path задаётся отдельно. **Доступ к Thermal/X-ray не блокирует первый запуск A**: он нужен перед C, а вся финальная приёмка остаётся незавершённой до этих проверок.

**Итерация:** до1–2ч активной проверки/подготовки; cold-build wait отдельно. Новая source/package правка означает новый artifact, не прежний hash.

## 4. На приставке: сначала игра, затем сохранения и полный checklist

Три device-окна остаются отдельно согласуемыми. В запросе каждого окна — RC/hash, установка, действия/число процессов,20/30/30мин active timebox, конкретный доступ для deploy/loader/log observation при необходимости. Не подключаться по SSH и не запускать игру без этого разрешения. Никаких graphics поверх чужой сессии, firmware/services/clocks/swap/mixer changes.

### GM-18 / A — первая реальная игра, до20мин

Новая отдельная установка → обычный Ports launch → logo/movie → new game → ранняя управляемая сцена с movement/aim/fire → PortMenu **Game → Exit game → Exit** → процесс завершён, возврат в Ports.

**PASS:** физическая картинка/orientation/color/HUD/fonts читаемы, pad работает без keyboard, movie/music/SFX слышны, выбранный disc реально читается через SDL/nod на этой firmware, нет observed validation/EGL/GL/device-loss errors, normal exit. Не только меню/exit0 и не только audio stats. Используем existing memory/log diagnostics; никаких повторов демки.

### GM-19 / B — CARD, переходы, reset и память, до30мин

Тот же RC/physical install/user/cache, два заранее разрешённых процесса:

1. Достижимый room route туда/обратно дважды, pause/map; station save с конкретным progress marker → завершение CARD IO → normal Exit.
2. Новый процесс → file select **CARD load** → подтверждение station/progress/settings/bindings → только затем **Restart to menu**, новый archSupport в том же graphics generation → повторный load/управляемая сцена → normal Exit.

**PASS:** реальные card files только в собственной directory, нет identity/atomic-write errors, progress действительно восстановлен; не quick-resume/savestate. Lifecycle/RAM route evidence, sampled MemAvailable≥96MiB, без необъяснимого роста/unsafe reuse/OOM. Память сцены ещё не измерена — текущие thresholds не гарантия. Не требуем искусственно вызывать trim или вернуть RSS ровно к началу.

### GM-20 / C — остальная retail-корректность, до30мин на подготовленных сценах

До окна выбрать законный доступ к сценам: собственное естественно достигнутое сохранение или отдельно разрешённые private test copies, не личная карта поверх baseline. Прохождение к ним не включается скрыто в короткое окно.

Проверяем оставшиеся mandatory пункты: opaque/alpha geometry/lightmaps, animated/skinned actors/weapon/pause doll, particles/water/transparency/reflections/bloom; Scan overlay/text/progress, Thermal/X-ray и штатные EFB/beam effects; beams/visors/morph/bombs/charge/missile/lock/jump/map/pause, оба порядка Back+Start, menu navigation; positional/music/movie/pause-resume audio. Референс — original game того же release, physical user confirmation; screenshots только дополнение.

**PASS:** checklist покрыт на доступных сценах; missing mandatory scene — pending, не N/A и не причина блокировать уже возможный A. Это приёмка выбранного маршрута/release, не всей кампании/других устройств. FPS фиксируется как baseline,30FPS не условие.

### Общая безопасность и исправления по фактам

Сохраняем свою card/settings/cache, не очищаем их перед warm launch ради PASS. Следим за имеющимися RAM diagnostics в разрешённом способе наблюдения; below96MiB/missing evidence/error → прекратить дальнейшую нагрузку, normal Exit в безопасной точке.60с без progress/30с Quit — оценка FAIL/PARTIAL, не разрешение kill. Во время CARD не reset/reboot/kill; Unknown удерживает owners. Нет намеренного device loss/OOM injection и автоматического retry.

Если actual game выявит defect: failing hash/scene/error → минимальная диагностика применимости → узкий fix существующего решения с regression → локальные затронутые gates → commit/push → новый RC/hash. Предлагается использовать тот же exact image/pins и для этих corrections, а не заново согласовывать каждый rebuild. Новая архитектура/доступ/ослабление критериев по-прежнему требует согласования.

Для диагностики повторяем failing и зависимые проверки, не все tests/окна автоматически. Для завершения переноса обязательные A/B/C результаты должны относиться к финальному принятому RC; старый PASS самовольно новому binary не приписывается. Нужные повторные device-действия входят в явное разрешение окна. После update запуск обозначается warm/update, не cold; нового binary под старым hash нет.

### GM-21 — зафиксировать принятую игру

Ровно испытанный payload/hash, build recipe/provenance/notices, реальный checklist/ограничения, manual install/update/uninstall и пользовательская приёмка. Не пересобирать «финальный» executable после acceptance. Удалять свою установку только по разрешению, сохраняя новые test saves по выбору пользователя. Старая игра/данные/оставленный IG launcher не затрагиваются.

## 5. Как выполняем план без нового круга согласований

- После утверждения этого остатка выполняем GM-12…GM-17 с meaningful checks и commit/push каждой завершённой безопасной итерации. Нет16отдельных подготовительных gates; известные прямые callers/tests — часть результата, не новый approval.
- Отдельные файлы/неожиданная длительность/поправка test target не означают смену плана. При частичном результате честно продолжаем то же исправление; не называем его PASS. Не берём unrelated improvements, чтобы «усилить» приёмку.
- Останавливаемся при реальной необходимости изменить backend/image/pins/deps, ownership/scheduler/ABI/safety/критерии, budget/effects, card identity/migration либо при новом device доступе. Не делаем unsafe workaround ради продвижения.
- Срок не вычисляется из числа заголовков. Основной известный объём — GM-12 с конкретными callers; остальные шаги — адаптация уже имеющихся механизмов. Оценки1–2ч относятся к проверяемой итерации, не являются обещанием завершить большой skinning block за1час. Runtime corrections/доступ к поздним сценам нельзя честно оценить до запуска; они не превращаются заранее в обязательную разработку.
- Необходимый остаток утверждён; выполняем согласованный scope без повторных approval прямых callers/tests. До A — выбранный disc/container/private path и отдельное permission на установку/запуск. Station route требуется к B, late scenes — к C, не всё сразу до первого запуска.

**Что выполнено при подготовке:** закреплены постоянные project rules; использованы уже собранные platform facts и source findings, без нового hardware research/production edits/build/GPU/device tests. Избыточные требования редакции2 исключены, а не ослаблены actual lifetime/CARD/effect checks.

## 6. Выполнение утверждённого остатка

### GM-12, итерация 1 — LOCAL PASS: boot/frame/root status

Добавлен allocation-free sticky main-owned game status. Existing backend failure прекращает PAK-loop/последующие PostInitialize consumers и возвращается обычным call return к root; root учитывает latch или backend health, затем прежний actual checkpoint/guards GM-11b. Safe-points после game draw+presentation restore и после AsyncIdle стоят до normal frame tail. Нижний EndScene сохраняет synchronous shim bookkeeping, но на failed frame не выполняет presented/counter/delayed flush; верхний renderer не выполняет reflection retirement/age update. Deep throw/seal не добавлены.

Full incremental ARM64 game compile/link,23/23CPU/no-init/DVD CTest (включая новый latch test и7source-binding cases), non-H700 compile3changed game TUs PASS. Independent read-only review не нашёл blockers этой итерации. Tests различают CPU policy/source binding и actual game/GPU execution; последнее не выполнялось. Commands/logs — ignored `gm12-root-*`.

Это завершённая первая итерация, **не весь GM-12**: новые skin failure producers ещё не включены. Далее direct consumer/workspace/dispatch wiring, затем их единое включение с allocation/wait regressions.

### GM-12, итерация 2 — LOCAL PASS: consumers/workspace/dispatch

H700-only bool wrappers и sticky-status checks останавливают direct model/animation/shadow/swarm consumers. External workspace pairs сначала находятся у local owners и публикуются только целиком; failed Calculate не обновляет draw mask. После отказа не выполняются shadow/reflection copies, следующие actor/pass/message и IOWin removal. Actor hook, time-provider, projection/view/thermal и scan-zoom restores сохранены. Think/Update возвращают отказ до ClearGraveyard; CMFGame восстанавливает random scope. Root проверяет status сразу после architecture Update до audio/reset.

Full incremental ARM64 game compile/link PASS;25/25 CPU/no-init/DVD CTest PASS, включая33 source-binding cases в трёх status/consumer/dispatch tests. Все13 изменённых game TUs компилируются без AURORA_H700. Первый non-H700 invocation остановился из-за отсутствия git в immutable image; список файлов передан явно без установки tools. Повторный CTest использует точный DVD filter. Commands/logs — ignored `gm12-consumers-*`. Source checks не являются runtime failure injection; GPU/device не запускались.

На момент этой итерации новые skin allocation/wait failure producers ещё выключены. Их включение выполнено следующей итерацией.

### GM-12, итерация 3 — LOCAL PASS: handshake, checked allocation и waits

Первый настоящий FIFO handshake перенесён в RsMain до PostInitialize/renderer; constructors больше не ждут token, учитывают даже inert reference и возвращают статус. Layout проверяет signed counts, widened products, alignment, суммы и downstream narrowing до использования/pointer publication. Ring остаётся32MiB с прежним запасом+32; heap/external не ограничены ring capacity. Ring waits выполняют existing main presenter progress без locks/game reentry, проверяют health после progress/token reads и сохраняют настоящие FIFO markers, ushort wrap и prefix reclaim.

Известный bad_alloc persistent/external/averaged-normal workspace, ring/job metadata и initial skin pool переводится в sticky failure до потребления результата. Неучтённая при metadata failure ring reservation остаётся удержанной до terminal retirement, не освобождается вне FIFO-порядка. Direct PC skinning выполняется вне ненужного legacy gather scope; legacy/verify options сохранены, gather/interrupt restore остаётся обязательным. Running SkinPool Run по-прежнему ждёт все реальные jobs; scheduler/число helpers не менялись. Partial pool startup не уничтожает объект уже detached sleepers. Deep throw/checkpoint не добавлены.

Full ARM64 game+skin test target compile/link PASS;38/38 CPU/no-init/DVD CTest PASS. Новые12 behavioral cases связывают production skin TU и CCircularBuffer с stub external services: pending handshake, health/progress failure, ref balancing, invalid sizing, exact-capacity/prefix reuse,65538 wrap iterations, metadata bad_alloc без выдачи указателя. Source bindings отдельно покрывают constructors/publication/consumers/actual Run wait; они не заявлены runtime исполнением renderer/skin jobs. Четыре изменённых game TUs без AURORA_H700 compile PASS. Focused independent read-only review включённых producers/direct consumers не нашёл defects. Initial compile/accessor и diagnostic post-read guard failures исправлены до успешного прогона. Commands/logs — ignored `gm12-skin-*`.

**GM-12 LOCAL PASS:** три безопасные итерации завершены, producers включены после consumers. Commits `fa1eaba58`, `e9af0acbc`, `874128da6` отправлены. Это не game/GPU/device PASS.

### GM-13 — LOCAL PASS: изменённая game/lifecycle цепочка

Full ARM64 game compile/link и38/38CPU/no-init/DVD CTest PASS. Existing Aurora CPU suites:269/269 GX FIFO,53/53 render/terminal lifecycle,13/13 recording/ownership (`GfxRecordingTest.*:GfxFrameWaitOwnership.*`) и9/9 bloom readback slots PASS. Остальные gfx scene/Null-backend tests намеренно не запускались: они не нужны для проверки исправленного status/lifetime path и не заменяют Mali game acceptance.

Найден и исправлен прямой test-target link defect: core lifetime guard ссылается на GX resident registry, поэтому `gfx_recording_tests` использует existing CMake RESCAN static archive group. Production backend/параметры/зависимости не менялись, meaningful tests не отключались. Полные non-H700 aurora_core/aurora_gx compile PASS; все20 game TUs, изменённые с GM-04 по GM-12, отдельно скомпилированы без AURORA_H700. Commands/logs — ignored `gm13-*` в existing H700/non-H700 outputs. GPU/device не запускались. Commit `e1558b3d8` отправлен.

### GM-14 — LOCAL PASS: отдельная установка и реальные пути данных

[Physical layout](../packaging/h700/README.md): отдельный executable в `.metroid-prime-unbound-h700` рядом с Ports entry, собственные user/cache; никаких old loose files/links. Production path/INI/UI policy не переписывалась. Добавлен только H700 no-init fixture для actual settings Load/Save и PortInput parser; write требует явного absolute temporary path и отказывается перезаписать INI, read не сохраняет. Два новых процесса подтверждают настройку/binding. Actual PortPaths+SDL base path probe физически копируется в disposable install, old/home sentinels остаются неизменными.

Full ARM64 game/probe/card/IO build;48/48 scoped CPU/no-init/DVD/CARD/GCI/atomic CTest PASS, включая duplicate identity rejection и nonzero stable CARD serial после remount. Non-H700 compile двух изменённых platform TUs PASS. Initial probe C++17 corrected to20, требуемый existing UTF-8 path header. Initial CARD/GCI failures воспроизведены как Docker Desktop bind-mount `copy_file` permission issue: те же binary/tests прошли на container-native tmpfs; capabilities/image/dependencies/card implementation не менялись. Combined run uses tmpfs `/tmp` (exec для copied probe) и `/out/build/card-test-data`; exact fixture options documented in layout. Meaningful tests не отключались. Logs/commands — ignored `gm14-*`. Personal fixtures/`../wii-save`, GPU/device не использовались. Commit `72811ed0a` отправлен.

### GM-15 — LOCAL PASS: thin launcher и существующий log copier

Добавлены fixed-payload run-game.sh и unconfigured Ports-entry template. Один literal absolute readable disc argument, own persistent paths, точная startup conflict policy до sanitation, preserved frontend SDL/audio и conditional PipeWire socket default. Loader injection/произвольные path/debug overrides не наследуются. Foreground child wait/reap удерживает install lock через shell HUP/INT/TERM, возвращает actual exit code; нет kill/watchdog/retry. Copier descendant сам по себе не удерживает игровой lock.

Existing H700 file sink ограничен8MiB включая header/explicit truncation marker. После cap pipe продолжает дренироваться до EOF; redaction сохранён, второго growing redirected stdout нет. No new logging protocol. [Invocation/layout/log limitations](../packaging/h700/README.md).

Full ARM64 game+copier test build,50/50 scoped CPU/no-init/DVD/CARD/GCI/atomic CTest PASS;13/13 mock-child launcher cases PASS на macOS и Linux, actual production copier flood/redaction/EOF/reap PASS. Non-H700 log TU compile PASS. Commands/logs — ignored `gm15-*`; GPU/device не запускались. Private legal disc path выбирается перед A, template пока намеренно не запускает игру. Commit `56e856111` отправлен.

### GM-16, итерация 1 — LOCAL PASS: game package implementation/staging

Existing BuildProfile/staging/audit helpers расширены game profile без второго input manifest/framework. Actual game+matched SDL используют `$ORIGIN/lib`; package-profile build-tree binary получает ту же local SDL до no-init tests. Common payload/archive/provider gates переиспользуются с game inventory/root. Source snapshot охватывает3344build-relevant files текущего среза, включая game/CMake/embedded и реально используемые shared/vendored helpers. Никаких user/cache/ISO/старых бинарников в payload.

Existing full collector сохранён, added actual-source game notice coverage. Шесть групп отсутствующих primary grant/revision evidence явно перечислены в `licenses/game-coverage.json`; local RC не является public distribution clearance. Полная firmware exact-symbol матрица не добавлялась; known provider/version gate и прежние ограничения сохранены.

Incremental actual staging build+audit+archive/no-init PASS:125/125scoped CPU CTest,269/269FIFO,39/39shared helper и34/34game helper tests в Linux. Initial local-RUNPATH/no-SDL loader failure исправлен копированием same target SDL, не LD_LIBRARY_PATH. Source/helpers also tested on macOS; timeout-only test skipped там иpassed вLinux. Commands/logs — ignored `gm16-*`; smoke archive не qualifying fresh RC. [Проверенный layout/recipe/ограничения](../packaging/h700/README.md).

На этой итерации GM-16 ещё не завершён: нужен qualifying fresh package. Implementation/staging commit `25c15c05d` отправлен. Последующие successful fresh results приведены ниже; GPU/device не запускались.

### GM-16, fresh attempt — PARTIAL: disk full, Docker unavailable

`python3 -B tools/h700_game/build.py --package --output build/h700-game-rc1 --jobs 2` выполнен на clean build-relevant source revision `25c15c05d87931a04b910afa7c444764b346cc54`, source SHA256 `6e613d78a3cf855c7841e88e5076e837fb349eaf41d9888e930abb68bb072b99`, dirty-source list пуст. Fresh configure PASS19,134s; full1700-action compile/link PASS577,370s;125/125CPU CTest,269/269FIFO,39/39shared и34/34game helper tests PASS. ELF metadata собрана.

Далее run завершился с `Errno28 No space left on device` (host command-record finalization также не смогла записаться). Package содержит лишь partial copy executable/lib, нет package-result/завершённого audit/notices/archive: **это НЕ qualifying RC и не GM-16 PASS**. Начальные3,1GiB free оказались недостаточны для peak host/Docker footprint; оставалось164MiB. Удалены только smoke staging/archive, созданные этой сессией внутри `build/h700-game-gm03` (не принятый RC); возвращено около300MiB. Smoke manifests/logs сохранены. Прежние build trees, исходные14archives и незавершённый rc1 не удалялись.

После этого `docker ps` дважды не ответил за20/30s; active-container state не установлен. Не перезапускали Docker глобально, не pruning/убивали процессы. Из-за unknown state rc1 удерживается. Logs/manifests — ignored `build/h700-game-rc1`; host-commands.json остался со stale running status, это не доказательство живого процесса. Нужны свободное место и восстановленный Docker до продолжения; план/image/pins не меняются. После этого выбрать два новых fresh outputs одного unchanged source state; partial rc1 не выдаётся за успешный первый build. На момент остановки GM-17 и device-окна не начинались.

### GM-16 — LOCAL PASS: qualifying fresh game package

Пользователь освободил место и восстановил Docker. Actual daemon/image/native ARM64 подтверждены; старых active containers нет. Без production edits выполнен полный package CLI в новом `build/h700-game-rc2`: fresh1700-action compile/link609,094s;125/125CPU CTest,269/269FIFO,39/39shared и34/34game helper tests PASS. Staged no-init/settings/profile, actual game+SDL local RUNPATH/ABI/provider/notices/content/hash/archive gates PASS. Это первый qualifying fresh package; ENOSPC rc1 не переиспользовался и не засчитан. Public distribution и runtime/device PASS не заявлены.

### GM-17 — LOCAL PASS: повторная fresh build, RC и выбранный ISO

Без изменений checkout повторён тот же CLI в новом `build/h700-game-rc3`: fresh compile/link547,067s, те же125/269/39/34tests и все staged gates PASS. Frozen source revision `e86eac74c`,3344files, source SHA256 `6e613d78a3cf855c7841e88e5076e837fb349eaf41d9888e930abb68bb072b99`, dirty-source list пуст в обоих runs. Source/14input/environment/providers/compile commands/generated build info совпали. CMakeCache отличается только SITE hostname. Из100payload files различается только executable:24bytes — CARD __TIME__ string и GNU build-id; остальные ELF bytes совпали. Exact ranges/readelf evidence и обоих archive/ELF hashes — в [RC report](H700_GAME_RC.md). Bit-identical build не заявлен; код ради timestamp не менялся.

Выбран **ровно rc3**: `build/h700-game-rc3/h700-game-rc.tar.gz`,77462023bytes, SHA256 `2539152dbbece585732db9aa7b9ad5bbc6f8b70c53667aed7d30579b31ccf18a`; после gates не пересобирался. Manual layout/install/update/rollback входят в payload. Build artifacts/logs/comparison report сохранены локально, чужие outputs и original14archives не удалялись.

GM-16 и локальная подготовка GM-17 завершены. User выбрал **GameCube USA1.00/ISO уже на приставке**, приставка свободна; actual image metadata/private path ещё не читались. Маршрут A — New Game→intro→Frigate Orpheon/Exterior Docking Hangar→move/aim/fire→PortMenu Exit; подробности и ограничения в RC report. Далее отдельное permission окна A до20мин: SSH/preflight/transfer/isolation, no-init loader check и один manual game run с log/RAM observation и physical user confirmation. На момент завершения GM-17 сведения о свободной приставке не считались permission и device/GPU/game runs ещё не выполнялись. Late scene access нужен к C, не blocker первого A.

### GM-18 / A — FAIL: первый ручной запуск, затем runtime fail-stop

После отдельного явного разрешения пользователя выполнены SSH preflight, установка неизменённого RC3, один no-init `--version` exit0 и один manual Ports launch. Header ISO подтвердил USA1.00/GM8E01/revision0,102installed file hashes совпали. Пользователь подтвердил запуск и слышимый звук, затем тормоза и зависание. Лог зафиксировал game failure1/FIFO downstream wait failed, terminal owner retirement и incomplete Dawn release; final status3/stage12 удерживал процесс/lock, кадров не было60,1s; пользователь уточнил застывшее меню/отсутствие звука, затем сам перезагрузил приставку. Normal exit и gameplay/physical checklist не подтверждены; причина требует локального разбора, это не только performance issue. Sampled min available~349MiB,5firmware baseline hashes unchanged при ещё живом процессе. Агент kill/reset/retry/новых device runs не выполнял. Позднее пользователь отдельно разрешил read-only SSH после перезагрузки: game/copier/launcher отсутствуют,5firmware hashes совпали, stale lock оставлен без удаления; update/новый run не разрешены. Активная проверка остановлена, B/C pending; новый device доступ/запуск отдельно согласуются. Локально добавлена только диагностика уже latched first_error и причин непройденного context proof: game link,44/44 scoped CPU/no-init,54/54render/lifecycle иnon-H700 core compile PASS. Это не исправление runtime failure; guards/trace capacity/ownership сохранены. Fresh RC4 diagnostics package прошёл126/126CPU +269/269FIFO +39/39shared +34/34game helpers и package gates; новый hash и ограниченный menu-only diagnostic repeat до10мин записаны в RC report; после этого пользователь отдельно разрешил A2, результат ниже. [RC и подробное evidence](H700_GAME_RC.md#gm-18--a--fail-пользователь-затем-перезагрузил-устройство).

### A2 — причина установлена: diagnostic trace exhaustion, narrow fix LOCAL PASS

В разрешённом update/one-no-init/one-manual-menu процессе RC4 подтвердил первичную причину:65536/65536EGL event trace исчерпан, observer trustworthy=false. Whole-group proof при этом показал live contexts0/Dawn bindings0/imports=false. Process снова в fail-stop3/stage12, normal exit не достигнут; min available~333MiB,5firmware hashes unchanged.24own user/cache files при update сохранены, новых signals/kill/reset/retry не было. Подробное private evidence — ignored `build/h700-game-a2-rc4/`.

В рамках разрешённых runtime corrections полный bounded history ограничен существующим IG diagnostic Control; actual EGL validation/live context+binding ledger и retirement/export/fail-stop checks не менялись. Context report — только IG consumer, install до initialization/uninstall после release проверены.45/45 scoped CPU/no-init,55/55render/lifecycle, full game link и review PASS. Новый source regression не выдаётся за device proof. Fresh RC5 trace-fix package готов:127/127CPU +269/269FIFO +39/39shared +34/34game helpers и package gates PASS, SHA256 `fd024470098b5f3556a02f3107a7eebdd249b7b28cf21ee889e378fac05f2240`. Новый A3 до20мин с explicit one-SIGTERM recovery старого RC4/no escalation, update после actual exit, one no-init и one manual early-gameplay/normal Exit run ещё не разрешён; полный scope в RC report. Это не новая архитектура и не отключение meaningful checks; завершённые GM-12…17 не переоткрываются.
