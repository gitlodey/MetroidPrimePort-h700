# H700 — исследование производительности и настроек графики

Исследование по запросу пользователя, 2026-11-17. Цель — сделать Metroid Prime Unbound v0.46.0 играбельной на RG40XX H / H700 / muOS 2601.0, а не выбрать оптимизацию по одному подозрительному участку. **Ни настройки, ни production code, ни установка не менялись; сборок, новых запусков и SSH в ходе исследования не было.**

## Краткий вывод

1. Настройки качества есть. Самая полезная оставшаяся ручка для первого опыта — внутренний `render_scale`: сравнить 1× и 0.5× на существующем binary. Это уменьшает число пикселей в четыре раза, **не обещает четырёхкратный FPS**.
2. В измеренном запуске MSAA, дополнительное освещение, солнечные тени, интерполяция, моды и пользовательский texture pack уже были выключены. Повторно выключать их бессмысленно.
3. Обычный front-end рисует видеофон и GUI, а не игровой мир с его тенями. Настройки теней не объясняют автоматически 8–10 FPS меню.
4. Дорогой участок локализован до driver-facing rendering, но конкретный draw/upload/wait не измерен. Native vertex input остаётся кандидатом, **не выбранным первым исправлением**. Предыдущее предложение его немедленного переноса пользователь не утвердил и запросил это исследование.
5. После проверки разрешения следующий выбор должен опираться на число проходов, draws и реальный объём загрузок. Уже существующие диагностические logs предпочтительнее ещё одного profiler framework или пересборки Dawn.

## 1. Что действительно измерено

[PERF-02 и идентичность установленного binary](H700_PERFORMANCE_PLAN.md#perf-02--wall-время-отправки-завершения-и-показа-кадра-утверждён): 452 presented frames за 54.4674 s полных report windows — **8.299 FPS**. Пользователь подтвердил неизменность меню всю минуту. Критических ошибок в capture нет, sampled available RAM не ниже 313.6 MiB.

| Wall-интервал | Frame-weighted mean |
| --- | ---: |
| Native acquire → начало Submit, включая подготовку/очередь | 41.668 ms |
| Queue::Submit | 69.037 ms |
| От возврата Submit/registration до наблюдения завершения | 119.928 ms |
| Native presenter | 0.646 ms |
| Из него swap | 0.059 ms |

**Интервалы перекрываются, складывать их нельзя.** Completion observation включает задержку polling/CPU/driver и не является GPU execution time. Активность Mali utility threads — CPU, не GPU utilization и не установленная роль compiler. В выбранном log-окне нет compile/warning lines, что не исключает внутреннюю работу драйвера.

После capture пользователь не смог воспользоваться штатным выходом при таком FPS и сам перезагрузил приставку. Это usability FAIL, но не воспроизведённый defect процедуры Exit. Состояние устройства после reboot не проверялось. Приватные исходные данные и агрегат — только в ignored `build/h700-perf02-wall/`.

## 2. Какие настройки реально есть

Значения ниже взяты **из полного effective settings dump измеренного запуска**, а не только из H700 defaults. После reboot INI заново не читался. Основные controls находятся в **PortMenu → Video → Quality**, интерполяция — в **Video → Frame rate**, packs — в **Mods**.

| Настройка / INI key | В измеренном запуске | Что можно получить и чем заплатить |
| --- | --- | --- |
| `render_scale` | `1` | 0.75× — номинально 480×360, 0.5× — 320×240 при выходе 640×480. Меньше пиксельной работы и EFB storage, более мягкая картинка. Объём исходного видео/геометрии и число draw calls не обязаны уменьшиться. |
| `dynamic_resolution`, `dynamic_res_min`, `dynamic_res_target` | `0`, `1`, `0` | Может автоматически снижать внутреннее разрешение. При minimum=maximum=1 снижать нечего. Для первого сравнения хуже фиксированного scale: меняет нагрузку во времени и может давать resize hitches. |
| `msaa` | `1` — Off | Уже минимум. Включение 4× увеличивает нагрузку и память; это не кандидат ускорения. |
| `anisotropy` | `1` | Уже минимум. Выигрыша от повторной установки 1× не будет. |
| `per_pixel_lighting` | `0` | Дополнительное per-pixel GX lighting уже выключено; обычное освещение сохранено. |
| `sun_shadows` | `0` | Добавленные портом солнечные shadow maps уже выключены. Это не общий выключатель всех теней. |
| `projected_shadows` | `1` | Управляет Remastered actor shadow maps/decals, а не всеми retail-тенями. Нужен активный Remastered room mode; при его отсутствии остаются штатные простые тени. Не средство ускорить обычный front-end. |
| `sharp_scan_window` | `0` | Повторный рендер мира для более чёткого scan window уже выключен. К главному меню не относится. |
| `mods`, `texture_pack` | `0`, `0` | Моды и пользовательский pack выключены. Built-in textures/HD font — отдельные механизмы; нельзя обещать, что этот ключ отключает вообще все увеличенные ресурсы. |
| `smooth_frames` | `0` | Интерполяция уже выключена. Это не frame generation и не ускоритель. Simulation rate ради FPS не понижать. |
| `room_geo_gpu` | `0` | Не универсальное «использовать GPU». Это модовая room geometry; H700 policy задаёт resident pool 0 MiB. Один INI-флаг не создаст работающий resident fast path retail-меню. |
| `remastered_performance` | `1` | Уже включён на уровне флагов; это специализированные компромиссы Remastered, не общий Low preset обычной игры. |

**Важная деталь UI:** slider `Resolution scale` сейчас ограничен 1–8×, но INI parser принимает конечные значения от 0 до 8. Для фиксированных 0.75/0.5 нужен INI своей установки; 0 означает Auto, а не минимальное качество. `Original experience=0` в capture, поэтому этот режим не принуждал scale обратно к 1×. На первом опыте `upscale_sharpen=dynamic` менять не требуется: для фиксированного низкого scale он не включает дополнительный sharpening.

Подтверждения: [H700 defaults](../platform/debug_ui/settings.cpp#L745), [INI parser scale](../platform/debug_ui/settings.cpp#L552), [UI scale](../platform/debug_ui/video.cpp#L611), [применение scale](../platform/debug_ui/options.cpp#L403), [динамическое разрешение](../platform/debug_ui/options.cpp#L243), [shadow activation](../platform/port_actor_shadow.cpp#L26), [retail shadow fallback](../src/MetroidPrime/CActor.cpp#L331), [allocation policy](../platform/include/port_h700_profile.h#L45).

## 3. Почему меню надо разбирать отдельно от игрового мира

Обычный front-end в [CFrontEndUI::Draw](../src/MetroidPrime/CFrontEndUI.cpp#L2796) рисует текущий movie и затем соответствующие GUI screens. Это не вызов рендера комнаты с actor/sun shadow passes.

[CMoviePlayer::DrawFrame](../src/Kyoto/Graphics/CCubeMoviePlayer.cpp#L514) рисует **четыре вершины** movie quad. Его setup использует Y/U/V planes, 4–5 GX TEV stages для цветового преобразования/field filtering и texture invalidation. В [texture setup](../src/Kyoto/Graphics/CCubeMoviePlayer.cpp#L141) каждый draw создаёт и уничтожает три GXTexObj. Это подтверждённая работа; стоимость GPU allocation/upload отдельно не измерена. Остальной GUI, разумеется, добавляет свои draws.

Отсюда два практических вывода:

- отключение игровых теней не является обоснованной первой попыткой исправить это меню;
- комментарий донора «texture vertex fetch медленный на Mali» не доказывает, что перенос всех GX layouts — лучший первый шаг: movie background сам по себе содержит только четыре вершины, а его pixel shading, texture streaming и синхронизация тоже могут стоить дорого.

## 4. Ранжирование направлений проверки и исправлений

Это порядок **получения полезного решения с минимальным риском**, а не уже измеренные доли bottleneck.

| Направление | Что подтверждено | Минимальная различающая проверка | Возможное исправление, если подтвердится |
| --- | --- | --- | --- |
| Пиксельная работа / EFB passes | Full-resolution movie+GUI, scale 1×; стоимость отдельного fragment/pass неизвестна | Фиксированный 1× → 0.5×, та же сцена и остальные настройки | При приемлемой картинке — низкий fixed scale или затем dynamic resolution. Для сохранения качества — устранить доказанно лишний полноразмерный pass/copy или специализировать дорогой shader. |
| Video texture streaming / YUV conversion | Movie quad и повторный Y/U/V setup реально присутствуют | Сопоставить draw/pass counts с изменением scale; при необходимости выделить один movie-upload/API envelope | Переиспользование ресурсов с корректным ring lifetime, обновление только нового movie frame, специализированный YUV path. Не отключать видео навсегда ради «зелёного FPS». |
| Staging, дополнительные copies, driver waits | Весь staging buffer отображается; есть buffer→fetch-texture copies; все их стоимости не измерены | Реальный high-water и число копирований относительно allocations; затем только нужная API boundary | Настроить стартовые staging partitions по данным, сохранить grow/spill/budgets; уменьшить доказанно лишние копии/полные строки без нарушения порядка. |
| Scalar vertex texture fetch | GX shaders загружают слова через R32Uint textures; donor native attributes обходят это | После проверки pixel/upload затрат — ограниченный fast-path A/B одного распространённого layout с подтверждённым использованием | Специализация aligned/vector loads либо native vertex input через Dawn. Сохранить mixed fallback, bounds/endian/matrix-index correctness. Не переносить donor draw dropping и defect cache invalidation. |
| Resource reuse, pass breaks, API/context overhead | Общие destination buffers; EFB resolves прерывают passes; источник Submit включает context scope | Existing pass counts, load/store/copy pattern; адресный замер реального ожидания только при необходимости | Убирать установленную redundant operation, улучшать совместимое batching/reuse. Per-slot ресурсы требуют бюджета RAM/lifetime-плана; это не бесплатный флаг. |

Код: [incremental buffer и mirror uploads](../extern/aurora/lib/gfx/encoding.cpp#L468), [округление fetch uploads до строк](../extern/aurora/lib/gfx/frame.cpp#L467), [покадровый Unmap](../extern/aurora/lib/gfx/frame.cpp#L1361), [GX scalar loads](../extern/aurora/lib/gx/shader.cpp#L3896), [GX pipeline без vertex layout](../extern/aurora/lib/gx/pipeline.cpp#L20), [уже существующий draw merge](../extern/aurora/lib/gx/command_processor.cpp#L989).

**Необоснованные shortcuts:** «в новом коде нет batching» — неверно; «надо отключить validation/robustness» — не разрешённое решение; «прогреть cache и всё станет быстро» — не следует из steady capture без compile warnings. Полная замена Dawn на donor direct GLES — архитектурная альтернатива на случай доказанной необходимости, не первое исправление. Донор быстрее по сообщению пользователя, но matched benchmark одинаковой сцены/полноты effects не проводился.

### Уточнение о 30 МиБ staging: что установлено, а что нет

Aurora создаёт пять staging buffers, каждый `MapWrite | CopySrc`. В capture partitions были 4/8/2/8+8 MiB, то есть **30 MiB на один buffer / 150 MiB allocation pool**, не пять загрузок на каждый кадр. [MapAsync](../extern/aurora/lib/gfx/frame.cpp#L381) отображает весь buffer, а не GX high-water. Малые последующие CopyBufferToBuffer ranges сами по себе не доказывают малую стоимость Map/Unmap.

Проверен implementation **точного source ref релиза Dawn** `118436b3f8f68583eb86e811ca605dc41637e8cc`, не другая версия. [BufferGL.cpp](https://raw.githubusercontent.com/Timdeuces/dawn/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/BufferGL.cpp):

- при `ShadowCopyMapWrite` MapAsync использует CPU staging; Unmap отображает и memcpy-копирует **GetSize целиком**;
- без этого пути используется GL MapBufferRange и затем UnmapBuffer — явного безусловного 30 MiB memcpy в этой ветке нет;
- [описание toggle](https://raw.githubusercontent.com/Timdeuces/dawn/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/Toggles.cpp) прямо предупреждает о performance penalty host-side copy;
- фактическое состояние ShadowCopyMapWrite в игровом device пока не установлено. Его нет в списке явно запрошенных toggles, но это не полный dump resolved defaults. **Нельзя выдавать «игра каждый кадр точно копирует 30 MiB через CPU» за доказанный defect.**

Кроме того, `DeviceGL::EnqueueGL` выполняет работу немедленно при выключенном GLDefer или nested context scope. Нельзя автоматически приписывать Unmap именно измеренным 69 ms Submit. Это проверяемая гипотеза о mapping/driver cost, а не готовое обоснование отключить toggle или пересобрать Dawn.

Exact BufferGL source SHA256: `585de731ab58208ddfacfd88d21645a60e720f491382e5c5a905216af20e89af`; копия и прочие source excerpts сохранены только в ignored `build/h700-dawn-contract-review/`.

## 5. Рекомендуемый следующий практический шаг — без новой сборки

**Предложение, ещё не разрешение на device actions.**

1. После отдельного согласия проверить состояние устройства после user reboot, сохранить собственный INI. Не переисследовать hardware/firmware и не очищать cache/cards.
2. На том же binary изменить только `render_scale=1` → `render_scale=0.5`; оставить dynamic resolution Off, остальные параметры и cap30 прежними. Проверить фактический scale в штатном settings log.
3. Один ручной запуск, тот же экран меню без навигации, короткий bounded capture уже имеющихся FPS/wall metrics. Игровой мир/intro не смешивать с меню. Предыдущие 8.299 FPS — ориентир; небольшой выигрыш требует сопоставимых scene/warm-up условий, а не объявления победы по разным сценам.
4. Существенная чувствительность к scale обосновывает pixel/pass направление и временный low-resolution preset. Если FPS почти не меняется, не продолжать урезать качество наугад: размер исходного movie upload, geometry/vertex work, API calls и waits этим не устранены. Это **не** автоматическое доказательство CPU-bound.
5. Если нужен следующий слой данных, в binary уже есть `MP_PASS_LOG=N` и `AURORA_FRAME_PEAKS=1`: число/размеры passes и четыре high-water значения (vertex/uniform/index/storage). Они **не** дают полного счётчика texture/copy bytes. Текущий `run-game.sh` использует `env -i` и эти variables не пропускает: простого export в SSH недостаточно. Для ограниченного diagnostic launch нужен отдельно согласованный временный passthrough/entry с backup и честными hashes; новой компиляции не требуется.

После scale опыта выбирать **один** подтверждённый путь. Сначала возможная настройка/адресная оптимизация, затем сравнение. Не начинать обязательный перенос всего vertex path, полный новый profiler или backend rewrite до этих данных.

## 6. Источники и границы исследования

- Собственные PERF-01/PERF-02 measurements и сохранённый effective settings dump; результаты не заменены предположением по defaults.
- Текущий code path v0.46.0, [SETTINGS.md](SETTINGS.md), [разбор native attributes донора и invalidation defect](H700_PORT_REVIEW.md#81-native-vertex-attributes).
- Точный pinned Dawn implementation и [provenance source ref](../packaging/h700-notices/provenance.json). Это источник поведения backend, не профилирование live Mali и не реконструкция всех compiler flags prebuilt library.
- [ARM: Thread Synchronisation](https://arm-software.github.io/opengl-es-sdk-for-android/thread_sync.html) — первичное описание цены лишней синхронизации/частой смены context и необходимости fences при shared resources. Это общие GLES-принципы, не доказательство конкретного H700 bottleneck и не предложение снять наши guards.
- ARM Best Practices и Khronos Buffer Object Streaming страницы вернули HTTP 403; их содержимое не использовалось как якобы прочитанный источник. Среда/зависимости ради обхода не расширялись.

Результат исследования — список реальных настроек, сцено-зависимых ограничений и проверяемых оптимизаций. **Ни прирост FPS, ни достижение 30/60 FPS этим исследованием не заявлены.**
