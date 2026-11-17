# H700 — первая performance-итерация

**PERF-01 УТВЕРЖДЁН пользователем: «Делай» в ответ на запрос утверждения плана и окна до10мин.** Разрешены локальная подготовка, пассивное SSH-наблюдение и один manual warm RC5 process/180s фиксированного меню/штатный Exit по scope ниже. Production code, параметры, deploy/no-init, второй run и принудительное завершение этим permission не разрешены. [Текущий статус](H700_PROGRESS.md), [исходное evidence](H700_OPTIMIZATION_NOTES.md#opt-006--rc5-неиграбельная-отзывчивость-drawcompile-stalls).

## Цель и минимальное решение

Нужна отзывчивая Metroid Prime Unbound v0.46.0 на **RG40XX H / H700, muOS 2601.0 Jacaranda**, с сохранением хорошей картинки. Ближайшая задача — выбрать адресное исправление неиграбельности, а не обещать30FPS по смешанному логу. Непройденные gameplay/input/normal Exit/CARD проверки не отменяются и не становятся PASS.

**Сначала используем неизменённый RC5 и пассивный CPU/wait-профиль одного фиксированного меню.** Новая диагностическая сборка, пересборка Dawn и profiler framework для этой итерации не нужны. Только если этот минимальный замер не различит нужный путь, отдельно предложить один узкий следующий замер, а не расширять исследование автоматически.

## Что уже установлено локальным разбором

- В RC5 пользователь увидел хорошую графику начального ролика, но счёл игру неиграбельной; невозможно уверенно оценить навигацию. Момент menu→New Game не помечен: прежние9,1FPS — **не отдельный benchmark меню или ролика**.
- В сохранённом final log **82 slow-compile warnings/82разных hashes**,217–2507ms, медиана782,5ms среди этих warnings. Это не все compilations и не сумма потерь frame time. **Blocking draw-wait warnings0**: source пишет их при таком ожидании≥50ms, но другие waits этим не исключаются. Лимит64new-miss log lines также не равен полному числу misses.
- Main-loop `draw` охватывает код после tick до workEnd, включая BeginScene/recording/EndScene/waits; он не измеряет GPU execution. `begin_frame` может ждать native credit/frame/staging, `end_frame` — FIFO drain/admission. Compile warning измеряет wall-time callback, включая возможные waits.
- В pinned Dawn есть проверка фактического current context/surfaces и nested-scope fast path; H700 observer её не отключает. При достигнутом `observed_make_current` выполняются driver bind, proc lookup и live-ledger bookkeeping. Их стоимость на H700 пока не измерена; context/pool waits выше EGL wrapper им не видны. Оснований менять topology/guards или пересобирать Dawn сейчас нет.
- `Main thread`, render и FIFO имеют известные source roles; OS names Linux ограничены15символами. Pipeline worker имеет только условный Tracy name: **без подтверждения нельзя назвать произвольный TID с именем Main thread компилятором**.

Проверенные участки: `src/MetroidPrime/main.cpp` (frame timers), `platform/debug_ui/options.cpp` (`RecordFrame`), `extern/aurora/lib/aurora.cpp` (frame boundaries), `gfx/frame.cpp` (slots/recording), `gfx/pipeline_cache.cpp` (compile/lookup), `gfx/render_worker.cpp`, `gx/fifo.cpp`, `h700/presentation.cpp`; пути от `extern/aurora/lib/` здесь сокращены. Pinned Dawn context path прочитан из сохранённых sources той же revision, без downloads/build/device actions.

## Карточка PERF-01

| Поле | Согласуемый объём |
| --- | --- |
| **Вопрос / зачем** | Какие игровые потоки расходуют CPU во время медленного меню, какие преимущественно ожидают, и меняется ли это между startup/наблюдаемыми compilations и последующим участком того же меню? Это выбирает путь первого исправления вместо догадки по общему draw timer. |
| **Зависимости** | Уже установленный RC5, известный ISO USA1.00, свободная приставка; отдельное permission на один ручной запуск и пассивное SSH-наблюдение. Новых инструментов/dependencies не требуется. |
| **Artifact / data** | Archive SHA256 `fd024470098b5f3556a02f3107a7eebdd249b7b28cf21ee889e378fac05f2240`; executable SHA256 `680988304655cc0cd59137d0645df96b67bd98817891e00a4e97739037aba9de`. Та же отдельная установка, сохранённые user/cache; это warm run, не cold. Никаких очисток/миграций/переписывания INI. |
| **Локальная подготовка** | Небольшой bounded collector/разбор только в ignored evidence рядом с прежними SSH records, не новый versioned profiling subsystem. Проверить разбор stat с comm в скобках, starttime и неотрицательные deltas на локальных fixtures. |
| **Device окно** | До10мин active testing, **один** manual Ports game process. Агент не запускает игру по SSH; нет deploy/повторного --version/IG-demo/новой сборки ради профиля. |
| **Сцена** | Пользователь сообщает о появлении того же главного меню. Далее180s без навигации, PortMenu и New Game: первые30s помечаются startup/settling, следующие150s — фиксированное меню. Это не гарантия полного прогрева; интервалы с наблюдаемыми compilations отмечаются отдельно. Если показанная сцена сама сменилась — разметить границу, не смешивать её с меню. |
| **Read-only данные** | В начале — PID/starttime, список TID/comm; далее примерно каждые3s, максимум60snapshots за180s: process/thread stat (utime/stime/starttime/state), wchan, `/proc/stat`, uptime/MemAvailable. TID/starttime отличают новые/reused threads; неизвестные роли остаются неизвестными. SSH overhead/фактические timestamps сохраняются, missed samples не догоняются. |
| **CPU / время** | Сравнивать deltas CPU ticks по тем же интервалам; доля общей CPU capacity — относительно deltas `/proc/stat`, без предположения HZ=100. Это приближённые non-atomic samples, **не** точные per-frame CPU milliseconds. State/wchan — выборки ожиданий, не длительность конкретного mutex или GPU execution. |
| **Existing log** | Сохранить timestamps чтения и штатные frame/stall/compile/RAM records; не включать IG Control, full EGL history, GPU queries/readbacks или новые per-draw logs. Старые milestone FPS не выдавать за continuous FPS выбранного180s окна. |
| **Выход** | После180s пользователь пробует штатный PortMenu→Game→Exit game→Exit. Проверить actual child/launcher completion, copier drain, lock release и5baseline firmware hashes.30s без выхода — PARTIAL/FAIL, **не permission** на SIGTERM/SIGKILL/reboot. Любое принудительное завершение — отдельно. |
| **Stop / safety** | Ошибка, missing evidence, MemAvailable<96MiB или отсутствие progress≥60s: прекратить дальнейшую нагрузку, попросить безопасный Exit; не запускать второй процесс, не удалять active lock/log и не прерывать CARD. Нет automatic retry/recovery. Firmware/services/audio/clocks/swap не меняются. |
| **Приёмка итерации** | Идентифицированный один run и размеченные scene/window boundaries; CPU/wait-картина известных roles, compilations и память сопоставлены по времени; выбран **один** следующий source path либо честно записана конкретная неоднозначность. Это диагностический результат, **не FPS improvement/game acceptance**. |
| **Оценка / фиксация** | До1–2ч активной подготовки/разбора, device time отдельно до10мин. Краткий результат/одно следующее решение в OPT-006 и progress, raw records только ignored build/, затем docs commit/push. |

## Как результат изменит следующее решение

| Наблюдение | Следующая адресная работа, не автоматическая правка |
| --- | --- |
| Известный main или FIFO расходует основную CPU долю | Разобрать соответствующий game recording/decoder участок; не объявлять rendering GPU-bound и не менять controller mapping ради общей задержки. |
| Известный render worker занят CPU | Сузить до Dawn recording/Finish/Submit/driver CPU path. Validation/robustness остаются включены; их отключение не является разрешённым быстрым fix. |
| Длинные frames совпадают с активностью других TID/compile warnings | Проверить compiler/cache interference и ownership этих TID; совпадение не доказывает причинность или повторные builds того же hash. |
| Известные render/main преимущественно ждут или hottest TID не идентифицируется | Выбрать **одну** недостающую границу/API envelope/роль для следующего минимального замера; не приписывать futex GPU и не начинать общий EGL profiler/пересборку Dawn. |
| Меню после settling работает нормально, проблема появляется только при смене сцены | Не чинить steady-menu path; следующим отдельно ограниченным вопросом станет переход/intro compile/load. Intro не обязателен в этом первом run. |

### Подготовка после утверждения — LOCAL PASS, ждём меню

Подготовлен только ignored host collector `build/h700-perf01-rc5/collect.py`:180s/60samples, starttime/TID guard, CPU tick deltas без HZ assumption, инкрементальное чтение existing log с проверкой overlap, no device signals/exec game. Локальные fixtures проверили comm со скобками/пробелами, поля stat, TID/starttime reuse, counter rollback, исключение guest double count, attribution и log overlap; дополнительно разобраны уже сохранённые actual firmware CPU/game stat records. Это проверки collector, не performance/game PASS.

Read-only preflight: установленный executable и configured entry hashes совпали с RC5; game/copier/launcher отсутствуют, lock отсутствует,5firmware baseline hashes unchanged, MemAvailable822512KiB (~803MiB). Никаких deploy/no-init/config/cache changes, game launches или signals не выполнялось. Collector пока не запущен и требует explicit main-menu confirmation; следующий шаг — один manual Ports launch пользователя, затем180s наблюдения по этой карточке. Private evidence — только `build/h700-perf01-rc5/`.

После PERF-01 конкретное production-исправление предъявляется отдельным коротким планом с файлами, regression и сопоставимой проверкой. **Не обещаем заранее единственный виновный компонент, величину ускорения или достижение30FPS.** Завершённые GM-01…17/backend/pins не переоткрываются; safeguards и качество картинки сохраняются.

## FPS-01 — компактный экранный счётчик, УТВЕРЖДЁН

Пользователь предложил вывести FPS на экран и ответил **«Конечно»** на конкретное предложение постоянной FPS/ms-строки с выключателем на основе существующих измерений, без GPU profiler/input capture. Это отдельное разрешение локально реализовать/проверить/собрать UI feature, **не** performance fix, deploy, новый device run или завершение текущего процесса.

| Поле | Ограниченная итерация FPS-01 |
| --- | --- |
| Цель / вопрос | Пользователь не может отличить остановку меню от крайне низкого FPS. Дать читаемый непрерывный readout прямо поверх игры, без навигации по тормозящему PortMenu. |
| Решение / source | Existing main-owned `sActualFps` (та же Presented FPS, что на Video/Frame rate), обновляемая RecordFrame примерно раз в секунду; ms=1000/FPS — средний wall interval, не CPU/GPU execution. Без новых timers. При отсутствии корректного положительного sample — `--`, без деления на0. `aurora_get_fps` с окном1s не используется для этой строки: при<2presents оно даёт0 и неполезно для наблюдавшихся ~1s stalls. |
| UI / объём | Маленькое ImGui-окно справа сверху viewport, FPS/ms, существующие font/presentation pass; NoInputs/NoNav/NoFocusOnAppearing/NoSavedSettings. Вызов вне sVisible guard, поэтому работает при закрытом PortMenu. Checkbox Show FPS counter на Video/Frame rate; existing BoolKey show_fps_counter, defaultON только AURORA_H700, OFF других платформ, saved0 уважается. |
| Файлы | platform/debug_ui.cpp, debug_ui/{debug_ui_internal.h,overlays.cpp,window.cpp,performance.cpp,settings.cpp}, port_settings_index.inc; direct settings fixtures/docs входят в этот же scope. Renderer/clock/cap/simulation/validation/robustness не меняются. |
| Проверка / приёмка local | Game compile/link; actual settings roundtrip, H700 default+override selftest, two-process fixture с сохранённым show_fps_counter=0; settings-index check, compile changed UI на non-H700. Никакой game/GPU runtime в container. Local PASS не подтверждает физическую читаемость/невлияние на pad — это pending будущего отдельно разрешённого run. |
| Безопасность / откат | Никаких SSH/deploy/INI/cache/card изменений текущей установки. Чистая новая сборка имеет новый hash; RC5 сохраняется. Counter выключается checkbox, будущий update только после actual exit. Текущий PERF-01 process по последнему snapshot жив, lock не трогать. |
| Завершение / оценка | До1–2ч активной локальной реализации и проверок, ожидание package build отдельно. Code/tests/docs → commit/push → отдельный package/hash; не обещаем улучшение FPS от UI feature. Для последующих before/after сравнивать одинаковое состояние counter. |

PERF-01 capture уже собрал60samples/180s без критических errors; scene confirmation и normal Exit остаются pending. Предварительно один mali-utility-wo занял23,266% total CPU capacity (4CPUs), main5,767%, render1,030%, FIFO0,727% на отрезке31,23–178,04s; это CPU work/wchan samples, не GPU time и не доказанная compiler role. На этом позднем отрезке0slow-compile warnings/0new-miss log lines. Эти данные не являются разрешением на изменение driver/render path.

### FPS-01 — LOCAL PASS, package готовится

Counter реализован в existing ImGui frame: NoInputs/NoNav/NoFocus/NoSavedSettings, вызов вне закрытого PortMenu guard. DefaultON на H700/OFF elsewhere, persisted show_fps_counter=0 уважается; checkbox и search-index на Video/Frame rate. Нет новых GPU queries, timers, threads или render-owner APIs. При invalid/nonpositive FPS выводится `--`; средний ms interval не объявляется GPU duration.

Full incremental game compile/link PASS;7/7 relevant CPU/no-init/settings/profile/install/index CTest PASS, actual H700 defaults/overrides28keys PASS. Persistence fixture выполняет actual write/read в разных процессах, проверяет show_fps_counter=0 и отсутствие перезаписи old/home sentinels. Пять изменённых platform UI TUs компилируются без AURORA_H700; independent read-only review direct caller/lifecycle/units/input flags/default/load-save blockers не нашёл. Source/CPU checks не заменяют physical visibility/pad check. Evidence — ignored `build/h700-game-gm03/fps01-*`.

Изменённый RC6 получает отдельный immutable package/hash; RC5 не пересобирается и не обновляется на устройстве. Текущий device process/lock по последнему snapshot живы; production UI feature local approval не означает permission на kill/update/run.
