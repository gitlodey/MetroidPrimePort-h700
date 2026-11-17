# H700 — исследование производительности и настроек графики

Исследование по запросу пользователя, 2026-11-17; продолжение PERF-03 — 2026-11-18. Цель — сделать Metroid Prime Unbound v0.46.0 играбельной на RG40XX H / H700 / muOS 2601.0, а не выбрать оптимизацию по одному подозрительному участку. **Разделы1–7 описывают исходное read-only исследование. Актуальная реализация/проверки/блокер PERF-03 — в разделе8; первоначальные ограничения approval заменены разрешением автономной работы.**

**Жёсткое уточнение пользователя:** разрешение не менять ни при каких условиях, включая диагностику. Сохранять 1×/640×480, dynamic resolution Off. Предложение scale A/B отменено. Пользователь сообщил о 60 FPS меню в старом порте; следующий research — сравнение целого активного OLD/NEW пути того же меню, не урезание картинки.

## Краткий вывод

1. Настройки качества есть, но любые изменения render scale/разрешения и dynamic resolution исключены прямым запретом пользователя. Сравнение и оптимизации выполняются только при базовых 1×/640×480.
2. В измеренном запуске MSAA, дополнительное освещение, солнечные тени, интерполяция, моды и пользовательский texture pack уже были выключены. Повторно выключать их бессмысленно.
3. Обычный front-end рисует видеофон и GUI, а не игровой мир с его тенями. Настройки теней не объясняют автоматически 8–10 FPS меню.
4. Дорогой участок локализован до driver-facing rendering, но конкретный draw/upload/wait не измерен. Native vertex input остаётся кандидатом, **не выбранным первым исправлением**. Предыдущее предложение его немедленного переноса пользователь не утвердил и запросил это исследование.
5. Следующий выбор должен опираться на сравнение старого и нового активного пути меню, число проходов, draws и реальный объём загрузок. Уже существующие диагностические logs предпочтительнее ещё одного profiler framework или пересборки Dawn.

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
| `render_scale` | `1` | Не менять: 1×/640×480 — жёсткое пользовательское ограничение, включая диагностические опыты. |
| `dynamic_resolution`, `dynamic_res_min`, `dynamic_res_target` | `0`, `1`, `0` | Не включать: динамическое изменение разрешения также запрещено пользователем. |
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

**Справочная деталь UI, не рекомендация к изменению:** slider `Resolution scale` сейчас ограничен 1–8×, но INI parser принимает конечные значения от 0 до 8; 0 означает Auto. Несмотря на наличие этих controls, пользователь запретил менять базовые 1×/640×480, в том числе через INI. `Original experience=0` в capture, поэтому этот режим не принуждал scale обратно к 1×. Текущий `upscale_sharpen=dynamic` также остаётся без изменений; опыт со снижением scale отменён.

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
| Пиксельная работа / EFB passes | Full-resolution movie+GUI, scale 1×; стоимость отдельного fragment/pass неизвестна | Сравнение активных OLD/NEW shader/pass/copy paths при неизменных 1×/640×480 | Устранить доказанно лишний pass/copy или специализировать shader с сохранением изображения. Разрешение не менять. |
| Video texture streaming / YUV conversion | Movie quad и повторный Y/U/V setup реально присутствуют | Сопоставить OLD/NEW movie resource/upload paths и draw/pass counts при 1×; при необходимости выделить один movie-upload/API envelope | Переиспользование ресурсов с корректным ring lifetime, обновление только нового movie frame, специализированный YUV path. Не отключать видео навсегда ради «зелёного FPS». |
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

## 5. Следующий шаг — активный старый путь как контроль

**Предложение scale A/B отменено прямым запретом пользователя.** На момент исходного исследования новые device actions/production correction ещё не были разрешены; затем пользователь дал [PERF-03 autonomy approval](H700_PERFORMANCE_PLAN.md#perf-03--автономное-устранение-неиграбельности-утверждено).

Сравнить OLD и NEW полный путь кадра того же меню: THP decode/repeat, GX texture identity/cache/retirement, реальные CPU/GPU uploads, shader, момент исполнения draws и waits. В OLD читать активные `gfx/common.cpp`, `gfx/texture.cpp`, `gx/gx.cpp` и `gl/*`, не оставшиеся вне target Dawn files. Сообщённые пользователем старые 60 FPS — контрольный ориентир; нового matched device benchmark пока нет.

При необходимости дальнейшего замера в binary уже есть `MP_PASS_LOG=N` и `AURORA_FRAME_PEAKS=1`: число/размеры passes и четыре high-water значения (vertex/uniform/index/storage), не полный счётчик texture/copy bytes. Исходный launcher с `env -i` их не пропускал; в PERF-03 добавлен явный passthrough diagnostics и bench-only flags, не generic renderer overrides. Разрешение при этом остаётся неизменным.

Выбирать адресный перенос полезного принципа OLD с сохранением NEW correctness/эффектов. Не копировать donor draw dropping/cache invalidation defect и не начинать full backend rewrite по одному предположению.

## 6. Источники и границы исследования

- Собственные PERF-01/PERF-02 measurements и сохранённый effective settings dump; результаты не заменены предположением по defaults.
- Текущий code path v0.46.0, [SETTINGS.md](SETTINGS.md), [разбор native attributes донора и invalidation defect](H700_PORT_REVIEW.md#81-native-vertex-attributes).
- Точный pinned Dawn implementation и [provenance source ref](../packaging/h700-notices/provenance.json). Это источник поведения backend, не профилирование live Mali и не реконструкция всех compiler flags prebuilt library.
- [ARM: Thread Synchronisation](https://arm-software.github.io/opengl-es-sdk-for-android/thread_sync.html) — первичное описание цены лишней синхронизации/частой смены context и необходимости fences при shared resources. Это общие GLES-принципы, не доказательство конкретного H700 bottleneck и не предложение снять наши guards.
- ARM Best Practices и Khronos Buffer Object Streaming страницы вернули HTTP 403; их содержимое не использовалось как якобы прочитанный источник. Среда/зависимости ради обхода не расширялись.

Результат исследования — список реальных настроек, сцено-зависимых ограничений и проверяемых оптимизаций. **Ни прирост FPS, ни достижение 30/60 FPS этим исследованием не заявлены.**

## 7. Сравнение целого активного OLD/NEW пути меню

Контроль OLD — donor HEAD `3f31d19f6956e0c0ada7ed56b48703b9e0c00cd6`, tracked worktree чистый. Пользователь сообщил о 60 FPS его меню; новый matched device benchmark не выполнялся. В OLD проверены активные `gfx/common.cpp`, `gfx/texture.cpp`, `gx/gx.cpp`, `gl/*`: оставшиеся `gfx/frame.cpp`, `gfx/encoding.cpp`, `gx/texture.cpp` не входят в старый target и не использовались как его runtime implementation.

| Этап того же movie+GUI кадра | OLD | NEW |
| --- | --- | --- |
| Movie update/decode | Три CPU Y/U/V slots, не более двух подготовленных кадров; одиночное продвижение кадра, не catch-up loop | Та же схема advance/repeat; decoder получил reservoir/Huffman lookup и bounded input. Нового полного decode pass не найдено |
| Movie texture objects/формат | На draw три локальных Init/Load/Destroy GXTexObj; I8 разворачивается в RGBA8 | То же создание GX identities и тот же RGBA8. Сам Init не является GPU allocation; «в OLD movie textures были persistent» — неверно |
| Texture resolver/retention | При отсутствии replacement — fresh texture/upload; общего pixel-content LRU этого пути нет. Delete откладывается до EOF worker item | Source guard/mincore, content hash и LRU 128 MiB; hit может избежать upload, miss идёт через Dawn staging. Bind-group cache дополнительно удерживает references 32 frames. Это дополнительная работа/retention, но также возможная польза от dedup |
| Frame data и GPU upload | Стабильные CPU ByteBuffers; точные dirty V/U/I ranges через persistent memcpy либо glBufferSubData в отдельный buffer set каждого frame slot | Полный MapAsync/Unmap source staging; использованные GPU buffer copies в один общий destination set, затем V/S buffer→R32Uint mirror copies. GX copies инкрементальны, не весь allocation на каждый pass |
| Момент исполнения draws | GL-команды выдаются worker-ом по мере обработки passes; GPU может начать работу ещё во время CPU recording | Worker записывает encoder commands, фактический GL replay этого frame command buffer — при EOF Submit. CPU recording и encoding перекрываются, но это не раннее исполнение draws |
| Reuse и completion | Перед повторным использованием slot ждёт его buffer fence; presenter ставит forward GPU server-wait | Есть также writable-staging MapAsync gate; presenter требует наблюдаемого forward work-done completion. Снять эти guards ради скорости нельзя |
| Vertex/shader | Native attributes и hand-written GLSL emitter | R32Uint shader fetch и WGSL/Tint→GLSL. Высокоуровневые YUV 4–5 TEV stages и ADD/SUB формулы присутствуют в обеих версиях; отдельного OLD movie shader с вырезанным этим расчётом не найдено. Сравнения финального driver machine code нет |

### Проверенные цепочки

```text
OLD: CPU-owned frame data → dirty range upload в fenced per-slot buffers
     → GL draw/copy этого pass → следующие operations → EOF composite/fences
NEW: CPU writes mapped staging → encoder staging copies + fetch-mirror copies
     → record draws → EOF Unmap → Finish/Submit/GL replay → remap/completion
```

Главный вывод сравнения — не «добавить один native flag», а исследовать/адаптировать **модель загрузки и исполнения старого рендера**. При этом нельзя копировать отключённые unsupported draws и дефект native cache invalidation. Они не являются оправданием игнорировать старые 60 FPS: базовый movie quad поддерживается обычным native input, а источник его видео и формат planes не стали проще в OLD.

Адресный сравнительный кандидат внутри существующего Dawn: CPU-owned bounded recording data → точные `Queue.WriteBuffer` в `CopyDst | CopySrc` source buffers → прежние per-operation GPU copies. Pinned `QueueGL.cpp:93–106` реализует WriteBuffer через точный BufferSubData. Это может убрать MapWrite/Unmap путь, но **не** убирает автоматически shared destinations, fetch mirrors или EOF replay и не обещает возврат 60 FPS.

Это не однострочная подмена: source-slot ownership/reuse должны подтверждаться реальным completion, а не фиктивным map success. Root admission/retirement, cancellation/fail-stop, memory bounds, initialization, native forward/reverse и validation/robustness сохраняются. Даже такой ограниченный production patch требует конкретного scope и сравнения при неизменных 1×/640×480. В PERF-03 пользователь разрешил автономный выбор исправлений; source-owner rewrite пока не реализован: сначала выбран меньший transport-кандидат из раздела8.

Подтверждения OLD: `gfx/common.cpp:367–381,1496–1501,1641–1662,1720–1740`, `gl/buffers.cpp:25–59`, `gl/pass.cpp:296–343`, `gx/gx.cpp:164–197`, `gx/shader.cpp:866–955`; NEW: `gfx/frame.cpp:381–394,1296–1310,1361–1386`, `gfx/encoding.cpp:468–539`, `gx/texture.cpp:811–875`, `gx/shader.cpp:426–479,4684–4715`. Пути здесь от `extern/aurora/lib/` соответствующего checkout. Полные source references и цепочки movie/upload изучены локально; в исходном read-only исследовании code/build/tests/device changes не выполнялись.

## 8. PERF-03 — draw-константы через UBO: LOCAL PASS, runtime BLOCKED

### Почему этот кандидат следует из сравнения OLD/NEW

В NEW `gx::render` передаёт `DrawImmediateData` через `SetImmediates` для каждого GX draw. Это **64bytes/16u32**, не16bytes. Точный pinned Dawn понижает используемые диапазоны в `glUniform1uiv` default-uniform array: `CommandBufferGL.cpp:787–805`; `ImmediatesTracker.h:81–96` повторно помечает данные dirty при смене pipeline, а изменение user data помечает user block. OLD direct-GL transport этого GX immediate block не имел. Это конкретное отличие, но **не доказательство**, что именно оно объясняет69ms Submit или вызывает внутреннюю Mali shader specialization.

Меньший opt-in candidate вместо переписывания source ownership:

- Только H700 и exact `MP_H700_DRAW_UBO=1` вместе с `MP_H700_BENCH=1|menu`; default OFF. В обычном launcher запуске без bench флаг UBO не передаётся.
- Обычный noncaster/non-PBR/LDR GX draw сохраняет полный immutable snapshot через existing bounded `push_uniform()` **после окончательного `fogRangeBase`**. Material uniform не перезаписывается; merge сохраняет snapshot первого draw.
- Group1 binding0 material + binding1 draw constants, два dynamic offsets в тот же uniform buffer; WGSL меняет только declaration `imm` с `var<immediate>` на `var<uniform>`. Для этого draw не вызывается GX `SetImmediates`; внутренние Dawn immediates могут остаться.
- Специальные shaders/passes/caster copies оставляют прежний transport. Spare bit config различает pipeline/cache identity без изменения старых default keys. Bind-group aliases следуют существующим growth/held-reservation/retirement boundaries; overflow/failure paths и owner guards сохранены.
- Для inline draw payload≤128bytes поля переупорядочены; instance count хранится какu16, поскольку actual producer получает GX u16 vertex count, при release-active bounds assertion. Vertex/index counts merged draws остаютсяu32.

Код: `extern/aurora/lib/gx/{gx.hpp,gx.cpp,command_processor.cpp,pipeline.hpp,pipeline.cpp,shader.cpp}` и `gfx/{resources.hpp,frame.cpp,shadow.cpp,actor_shadow.cpp}`. Это ограниченный эксперимент внутри прежнего renderer, не перенос donor draw dropping и не отключение retail effects/validation/robustness.

### Как будет различаться реальная цена transport/upload

Opt-in `MP_H700_GL_TIMING=1` измеряет wall реальных GL driver calls: map/unmap, buffer allocation/upload/copy, texture upload/storage, draw variants, blit/flush/GetError, Uniform1uiv/UseProgram/BindBufferRange и actual `eglMakeCurrent`. Fixed atomic aggregates без history/GPU queries/новых threads; отчёт рядом с existing2s frame-wall. Default OFF возвращает raw GL pointers, без per-call instrumentation. Перекрывающиеся thread wall суммы **не GPU time**; границы concurrent windows приблизительны, cumulative calls/bytes/time не теряются.

`MP_H700_BENCH=menu MP_FAST_BOOT=1` использует existing FastBoot до FileSelect, но не начинает New Game; phase/screen/movie readiness логируются только в bench. Для matched capture нужен actual FileSelect (`phase4/screen3/next3/movie_ready1`), проверка картинки, warm-up и одинаковые прочие profiling flags. Off/on должен выполняться на **одном binary**, из отдельных копий одинаковых user/cache, с неизменными1×/640×480/dynamic Off/cap30. Не включать candidate по умолчанию до измеренного выигрыша и проверки изображения/раннего gameplay.

### Что действительно выполнено

- Independent static review обоих scopes без выявленных blockers; это не Mali execution proof.
- Existing compatible incremental ARM64 tree `build/h700-game-gm03/build`: game + presentation/shader/mixed/FIFO/profile targets собраны. Первая попытка остановилась на missing Abseil header нового test include; добавлены уже существующие `absl::flat_hash_map`/`absl::btree` dependencies test target, ни один test/assert не удалён.
- **57/57 scoped CTest,269/269 FIFO и16/16 host launcher tests PASS.** Проверены env gate, payload/cache identity, transport-equivalent WGSL, special fallback, dynamic range bounds, alias lifecycle, snapshot/merge/caster source contracts, profiler totals/max/reset/concurrent sampling. Это CPU/no-init tests, не GPU/Null execution.
- Отдельный incremental benchmark stage `build/h700-perf03-ubo/package`, текущий launcher включён.17source/ELF/ABI/provider/notices/content/hash/no-init/syntax gates PASS; SDL/dependencies переиспользованы без пересборки. Fresh build/RC-архив не создавались; реальные user/cache/cards не менялись.
- Executable229350184bytes, SHA256 `026d1241dcbb3a578c91f1f225ede3207ab3b696ca2e97c8fd833d9f8a6847eb`; source3345files, SHA256 `661acf5e68eab74a70a0aba1bdde720192595339147849d3eca028c3c4fa3bb7`. Before/after source snapshots совпали; manifest честно содержит dirty source inputs родительской ревизии. Private commands/results — `build/h700-perf03-ubo/`.

### Физический blocker и условие продолжения

SSH после пользовательского reboot остаётся timeout. Bounded LAN discovery не нашёл узел с pinned SSH key приставки; USB network отсутствует. Host identity не ослаблялась, к соседним устройствам не выполнялась аутентификация. Изменять router/firmware/services/clocks либо бесконечно сканировать сеть не требуется.

Подготовлен штатный muOS content handoff в отдельный performance clone, но installed marker/dispatch semantics и сам clone **ещё не подтверждены**.32offline protocol checks не заменяют live attestation. Entry явно задаёт одинаковые menu/profile flags и UBO0/1; guard делает максимум один TERM только identity-bound own game и сохраняет launcher drain/lock contract. Bare SDL поверх frontend, frontend stop/restart и KILL запрещены.

**PERF-03 не завершён:** новый candidate не установлен/не запускался, прирост FPS и визуальная корректность UBO на Mali не измерены. Нужна физически доступная приставка с Wi-Fi/SSH, не новое approval. После восстановления связи: installed-route/isolation check → baseline/UBO-on одного меню → решение по actual FPS/API/errors/RAM. Если UBO не помогает, не делать его default; выбрать следующий upload/draw path по измерению, а не по числу локальных зелёных tests.
