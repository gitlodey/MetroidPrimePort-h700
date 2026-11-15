# H700 — что осталось между демкой Dawn и renderer игры

**Исходное исследование:** 2026-11-14, после успешного [device test демки](H700_GRAPHICS_PROBE.md). В той сессии выполнены только разрешённое чтение исходников, сравнение Git и получение первичных Dawn sources по закреплённому ref, без новых сборок/probes/SSH. **Последующее обновление:** отдельно разрешённый [capability test H700](H700_CONTRACT_PROBE.md) подтвердил texture-fetch layout mismatch на Mali; результаты отмечены ниже. Production code и dependency pins не менялись.

**Результат:** минимальный interop уже работает; перенос не сводится к замене одного `Present()`. Ниже разделены доказанные несогласованности исходников, непроверенные аппаратные возможности и ещё не выбранные решения. Это материал для согласования, **не утверждённая архитектура и не разрешение реализации**.

## 1. Источники и границы доказательности

- Новая база U: `c872da704adde9a7838492aaa56dac73a32053e4`, `v0.46.0`. Проверено: diff `extern/aurora`, `platform`, `src`, `include` относительно U пуст; tree Aurora у U и исследованного HEAD одинаков — `f20f931e0b052b30c354cc7bbd893bbcd38aa1f7`.
- Dawn: существующий ref `118436b3f8f68583eb86e811ca605dc41637e8cc`, зеркало `Timdeuces/dawn`, package `v20261029.114630`. Изменения pin или пересборки Dawn нет.
- Предыдущая демка: Mali-G31, EGL 1.4 / GLES 3.2, synthetic RGBA8 target 64×64, drawable 640×480; 2 offscreen / 120 bridge кадров и ограниченное визуальное подтверждение пользователя. Последующий `contract-v2` измерил adapter/device limits/features и создал device с U request; normal/shadow GX layouts отклонены **17 > 16**. Это не запуск U renderer или его shader workload.
- Источники Dawn получены по commit-pinned raw URLs; локальные копии сохранены только в ignored `build/h700-dawn-contract-review/`. Эти исходники не подменяют cached prebuilt и не доказывают его воспроизводимую сборку.

Ключевые первичные файлы:

| Источник | Что подтверждает |
| --- | --- |
| [OpenGLBackend.h](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/include/dawn/native/OpenGLBackend.h) | Публичный external-display/import API, без публичного удаления GL alias |
| [DeviceGL.cpp:421–470](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/DeviceGL.cpp#L421-L470) | Создание alias через `GenTextures`, затем `OwnsHandle::No` |
| [TextureGL.cpp:373–393](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/TextureGL.cpp#L373-L393) | Удаление GL handle только при `OwnsHandle::Yes` |
| [PhysicalDeviceGL.cpp:642–699](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/PhysicalDeviceGL.cpp#L642-L699) | Context topology и Compatibility-only backend |
| [ContextEGL.cpp:372–423](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/ContextEGL.cpp#L372-L423), [DeviceGL.h:185–232](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/DeviceGL.h#L185-L232) | Scoped context locking/restoration и отдельные compiler contexts |
| [Limits.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/Limits.cpp), [BindingInfo.cpp:207–224](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/BindingInfo.cpp#L207-L224), [PipelineLayout.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/PipelineLayout.cpp) | Device defaults и суммарные per-stage binding budgets |
| [Format.cpp:488–490](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/Format.cpp#L488-L490), [BindGroup.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/BindGroup.cpp) | Float sample types и validation texture bindings |

SHA-256 полученных ключевых sources: `DeviceGL.cpp` — `96aa30326943e93cfdced2f13280ddeaecff28d8b3fd533dc2960b2a6556a107`; `TextureGL.cpp` — `f48030fce2d2a4ff23ab861bf29fdc5a95998754c7009320d59bcc6fa0fc4a2f`; `PhysicalDeviceGL.cpp` — `00e1e33c59b29d63364f3aa49298c9fb31ad7b68c4dd8319f6e43f24b6cda0da`.

## 2. Настоящий device contract U

Основной код — [webgpu/gpu.cpp](../extern/aurora/lib/webgpu/gpu.cpp):989–1063, 1145–1454. Прежняя pattern-демка запрашивала только 4096/64 и использовала один uniform на 16 bytes. Последующий capability harness проверил зеркальный default U request: **device создан**, включены ASTC, texture-component-swizzle и implicit-device-synchronization; Core, BC, dual-source и timestamps adapter не объявил. Полные результаты и отличия harness от U — [отчёт device test](H700_CONTRACT_PROBE.md).

### Adapter и features

- Instance: `TimedWaitAny`, Dawn toggle `allow_unsafe_apis`.
- Adapter: `FeatureLevel::Compatibility`, выбранный backend OpenGLES, power preference и `compatibleSurface`. Существующая Linux surface binding знает X11/Wayland, но не firmware SDL2/Mali.
- По исходникам pinned GL backend `CoreFeaturesAndLimits` не объявляется: backend поддерживает **только Compatibility**, а [базовая инициализация features](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/PhysicalDevice.cpp) включает Core только при `SupportsFeatureLevel(Core)`; GL-specific initialization отдельно его не включает. Это source-level вывод для этого pin/backend, не полный feature query Mali. Сам по себе запрос Compatibility на другом backend не означает отсутствия Core; строка GLES 3.2 также не доказывает его наличие.
- Device features запрашиваются только если объявлены adapter: `CoreFeaturesAndLimits`, BC, ASTC, texture-component swizzle, dual-source blending, timestamp queries, а при `WEBGPU_DAWN` — `ImplicitDeviceSynchronization`. Core и BC имеют существующие env opt-outs.
- Необязательность feature для создания device не означает эквивалентный fallback: например, без dual-source blending часть destination-alpha modes выбирает `DstAlphaMode::None` ([gx.cpp](../extern/aurora/lib/gx/gx.cpp):367–385).
- `Float32Filterable` U не запрашивает. BC не обязателен для retail CMPR: существует CPU decode в RGBA8 ([texture_convert.cpp](../extern/aurora/lib/gfx/texture_convert.cpp):713–725).

### Все явно запрашиваемые limits

`S` ниже — reported adapter value; `T` — выбран texture vertex fetch; `N` — no-compute path. Код — `gpu.cpp:1204–1234`.

| Поле | Значение U |
| --- | --- |
| Dimensions 1D/2D/3D, array layers | Полное `S`, если ненулевое, иначе undefined |
| `maxSampledTexturesPerShaderStage` | 17 при `S >= 17`, иначе undefined |
| `maxStorageBuffersPerShaderStage` | 2 при `!T || !N`, иначе undefined |
| `maxStorageBufferBindingSize` | Полное ненулевое `S`, если storage нужен, иначе undefined |
| `maxBufferSize` | Полное ненулевое `S`, иначе undefined |
| `minUniformBufferOffsetAlignment` | `max(S, 64)` |
| `minStorageBufferOffsetAlignment` | `max(S, 16)` |
| `maxImmediateSize` | 64 bytes: `sizeof(DrawImmediateData)` |
| Compatibility vertex / fragment storage counts | 0 / 0 при T, иначе 2 / 2 |

Например, известное adapter dimension 8192 приведёт в U к запросу **8192**, а не 4096 демки. Это limit, не команда выделить текстуру 8192². Undefined и маленькие requested counts нельзя принимать за окончательные device values: pinned `ReifyDefaultLimits` подставляет default, если он лучше запроса. Для sampled textures Compatibility default — **16**. Нужны `GetLimits` итогового device и полный feature query, не только adapter log.

U включает `gl_allow_context_on_multi_threads`, а Release дополнительно `skip_validation` и `disable_robustness` (`gpu.cpp:1366–1383`). Предплановые проверки должны сохранять validation/robustness: отключение ошибки не является подтверждением корректности.

### Что означали warnings демки

Снижение dynamic uniform **72 → 16** и dynamic storage **70 → 16** относится к нормализации reported adapter capacity внутренними пределами Dawn, не к запросу игры использовать столько buffers. U не повышает эти limits: GX использует один dynamic uniform, RmlUi — максимум два; рассмотренные layouts не используют dynamic storage. Compatibility defaults 8/4 это покрывают. **Сами warnings не установили blocker U.**

## 3. Подтверждённая несогласованность texture-fetch layout

**Теперь подтверждено на H700, не только статически:** Compatibility vertex storage buffers Mali = **0**, поэтому default U policy выбирает texture fetch, no-compute = 0, lightmap = 0. Adapter и созданный device имеют sampled-texture limit **16**. Normal и shadow-receiver layouts требуют **17** и отклонены validation; общий exit 1 без timeout. Это layout-барьер неизменённого U, не crash драйвера и не доказательство невозможности Dawn.

Texture fetch выбирается при reported vertex storage count < 2 либо `AURORA_VERTEX_FETCH_TEXTURE=1` (`gpu.cpp:1153–1165`). Он создаёт две `R32Uint` textures шириной **4096**, по 16,384 bytes на строку; данные читаются через integer `textureLoad` из vertex и fragment stages ([frame.cpp](../extern/aurora/lib/gfx/frame.cpp):107–144, 822–846).

Суммарный fragment budget:

| Часть pipeline layout | Sampled textures |
| --- | ---: |
| Group 2: 8 GX + environment cube + 5 ambient volumes + BRDF LUT + froxel/shadow | 16 |
| Group 2 lightmap, если `g_lightmapBinding` | +1 |
| Group 0 storage-data texture с visibility Vertex **и Fragment**, только при texture fetch | +1 |

Источники: [gx.hpp](../extern/aurora/lib/gx/gx.hpp):52–66; [probe.hpp](../extern/aurora/lib/gfx/probe.hpp):45–48; [gx.cpp](../extern/aurora/lib/gx/gx.cpp):866–973, 1040–1052; `frame.cpp:844–846`.

| Fetch / lightmap | Требуется fragment textures | Поле request U | Эффективный limit по pinned defaults/request |
| --- | ---: | --- | ---: |
| Storage / нет | 16 | undefined | 16 |
| Storage / есть | 17 | 17 | 17 |
| Texture / нет | **17** | undefined | **16** |
| Texture / есть | **18** | 17 | **17** |

Последний столбец — статический вывод из `ReifyDefaultLimits`. На Mali затем измерена именно строка **Texture / нет: 17 требуется, 16 доступно**; остальные profiles этим device run не проверялись.

Dawn суммирует visibility **всех entries explicit pipeline layout**, даже если конкретный shader не читает PBR textures. Простое выключение PBR setting не убирает эти bindings; «попросить 18» не решает adapter с пределом 16.

**До соответствующей реализации нужно согласовать resource/layout profile:** специализацию состава/visibility и допустимость lightmap либо другой обоснованный вариант. Это локальный вопрос U fallback, не доказательство необходимости менять весь renderer. Код не исправлялся. Existing [gx_pipeline_compatibility_test.cpp](../extern/aurora/tests/gx_pipeline_compatibility_test.cpp) проверяет lightmap policy, но не этот cross-group total.

## 4. Форматы, depth и дополнительные эффекты

### Минимум, который tiny pattern не проверил

- Реальная комбинация color + `Depth32Float`, depth test/write, blend и EFB copies.
- `R16Sint` palette source → RGBA8, `R32Float` depth snapshot target.
- При texture fetch — 4096-wide `R32Uint`, uploads и reads по обе стороны границы строки 4095/4096.
- Cube / 3D / 2D-array views и sampled RGBA16Float placeholders: некоторые создаются/биндятся независимо от использования PBR конкретным shader.
- RmlUi `Stencil8` clipping; uniform window **8192 bytes**, реальные dynamic offsets и shader-read **64-byte immediates**. Демка запросила immediate limit, но не исполняла такой shader.

Источники: [gpu.cpp](../extern/aurora/lib/webgpu/gpu.cpp):556–570, 1531–1533; [texture_convert.hpp](../extern/aurora/lib/gfx/texture_convert.hpp):25–38; [frame.cpp](../extern/aurora/lib/gfx/frame.cpp):856–875; [WebGPURenderInterface.hpp](../extern/aurora/lib/rmlui/WebGPURenderInterface.hpp):92.

### Depth peek — не то же самое, что EFB depth conversion

[depth_peek.cpp](../extern/aurora/lib/gfx/depth_peek.cpp):439–449 **сразу отключает весь subsystem без Core**, до выбора compute/render fallback. Поэтому наличие fallback без compute не означает working `GXPeekZ` на Compatibility GL. В [GXCpu2Efb.cpp](../extern/aurora/lib/dolphin/gx/GXCpu2Efb.cpp):9–19 без snapshot возвращается 0.

**Но game-facing потребитель не найден:** поиск `GXPeekZ`/depth snapshot request в `src`, `include`, `platform` не выявил вызовов. Например, [CVisorFlare.cpp](../src/MetroidPrime/CVisorFlare.cpp):74–86 использует CPU collision ray, не peek. Это подтверждённый **GX API coverage gap без доказанного retail-влияния**, не повод объявлять сломанными визоры или делать его обязательным первым исправлением.

Отдельный [tex_copy_conv.cpp](../extern/aurora/lib/gfx/tex_copy_conv.cpp):330–380, 769–779 действительно имеет Compatibility single-sample depth reconstruction через comparison sampling и бинарный поиск float bits. Его нельзя смешивать с выключенным `GXPeekZ`. Compatibility MSAA depth readers не создаются; unsupported depth copy может быть пропущена с сохранением старого destination ([encoding.cpp](../extern/aurora/lib/gfx/encoding.cpp):277–287). Это не эквивалент корректного эффекта.

### Опциональные paths не считать автоматически готовыми

- Setter U нормализует MSAA к **1 либо 4**, но не проводит per-format negotiation (`gpu.cpp:1695–1697`). В pinned `Format.cpp` RGBA16Float multisample capability зависит от Core; HDR RGBA16Float 4× не следует объявлять поддержанным Compatibility path.
- No-compute volumetric fog: state format `RGBA32Float`, explicit binding 5 — `TextureSampleType::Float`, но U не включает `Float32Filterable`. Pinned Dawn предоставляет этому формату `UnfilterableFloat` без feature; `textureLoad` вместо sampler не отменяет bind-group validation. Это **статический mismatch опционального render fallback**, не доказанный blocker retail/compute-пути. Источники: [volfog.cpp](../extern/aurora/lib/gfx/volfog.cpp):44, 615–618, 813–821; Dawn `Format.cpp:488–490, 609–611`.
- Compute shaders depth peek / volfog имеют workgroup **8×8×1 = 64**; positive adapter invocations 128 не проверяет остальные dimensions/storage formats и исполнение. Автовыбор no-compute в U проверяет только нулевые counts, не достаточность конкретной нагрузки.
- Необязательный post X-ray не приравнивается ко всем штатным retail visor effects. Ограничения MSAA/дополнительных эффектов нужно привязывать к реальному принимаемому profile, не отключать штатные эффекты заранее.

## 5. Потоки U и границы завершения

| Участник | Работа и источники |
| --- | --- |
| Main/game | SDL/events, GX production, `begin_frame`, FIFO drain и конец recording, UI freeze; [aurora.cpp](../extern/aurora/lib/aurora.cpp):260–343, 419–496 |
| GX FIFO worker | Обработка FIFO и recording draw/pass; [fifo.cpp](../extern/aurora/lib/gx/fifo.cpp):27–105, 239–274 |
| Render worker | Encoder, sealed operations, final composition, `Submit` и сейчас `Surface::Present`; [frame.cpp](../extern/aurora/lib/gfx/frame.cpp):1212–1264, `aurora.cpp:494–648` |
| Pipeline compiler | Синхронные pipeline-create callbacks на отдельном потоке; для OpenGLES U использует **одного compiler worker**; [pipeline_cache.cpp](../extern/aurora/lib/gfx/pipeline_cache.cpp):1321–1383 |

**Не все Dawn calls принадлежат render worker:** resources создаются также на main/FIFO recording paths ([recording.cpp](../extern/aurora/lib/gfx/recording.cpp):189–240, 497–548, 917–975). Поэтому оставить «один graphics worker» ещё не означает оставить один поток обращения к device.

Нельзя смешивать четыре события:

1. **FIFO draw-sync token** — ordered CPU consumption, важный для lifetime skinning workspace; не GPU completion.
2. **`gfx::gpu_synchronize()`** — `render_worker::synchronize()` и ожидание CPU queue marker, не GPU fence (`frame.cpp:1274–1276`, [render_worker.cpp](../extern/aurora/lib/gfx/render_worker.cpp):254–267).
3. **After-submit callback** — после отправки команд/попытки present, не после окончания GPU work (`aurora.cpp:643–648`).
4. **Image ownership** — Dawn закончил запись → presenter может читать; presenter закончил чтение → Dawn может писать повторно. Это отдельный двусторонний contract. Демка обеспечивала его work-done/WaitAny и `glFinish`.

Два `FramePacket` U — CPU packet slots, **не два готовых безопасных presentation images**. Пять staging buffers защищаются mapping lifecycle; освобождение CPU packet после callback не разрешает перезаписать image, который ещё читает presenter.

### Main-thread presenter и риск взаимного ожидания

Если render worker ждёт освобождения presentation slot main-потоком, main уже может ждать frame/staging slot, FIFO drain или `render_worker::synchronize()`. Worker queue имеет capacity **256**, `push()` блокируется (`render_worker.cpp:13, 88–98`). Slot waits обслуживают Dawn progress, но не SDL presenter (`frame.cpp:950–990`).

**Установлен риск при будущей интеграции, не существующий воспроизведённый deadlock U.** Hook только в event/update loop недостаточен: нужен согласованный progress/cancellation protocol во всех зависимых waits, без вызова swap под неподходящими locks.

Отключение render worker не даёт single-main-thread режим: его fallback выполняет work inline на вызывающем потоке, включая FIFO (`render_worker.cpp:61–76`). Перенос pipeline compilation на worker тоже не бесплатен: blocking lookups могут ждать compiler из render callback; job нельзя поставить за этим же callback и ждать его завершения.

### Что меняет Dawn thread toggle

U уже запрашивает `gl_allow_context_on_multi_threads`, которого не было в tiny demo. В pinned Dawn ветка **`GLAllowContextOnMultiThreads && !GLDefer`** создаёт до четырёх shared pipeline contexts сверх основного Dawn context; первый обязателен. Последующий capability test создал device с этим toggle, но фактически выбранная ветка, число contexts и их совместная работа на Mali не проверены. Один compiler worker U не означает создание только одного дополнительного context. `ScopedMakeCurrent` имеет mutex и восстановление предыдущего EGL state; native `ImplicitDeviceSynchronization` объявляется базовым `PhysicalDevice` и U запрашивает feature при наличии.

Это обоснование поддерживаемого механизма, **не проверка его работы на firmware Mali**. Есть также legacy `DeprecatedMakeCurrent`, ошибки которого игнорируются; нельзя объявить toggle доказательством полного отсутствия гонок. Raw EGL presenter restoration из демки сохраняет значение из-за SDL TLS cache.

## 6. Ресурсный lifetime: что именно не закрывает демка

На успешном импортном path `DeviceGL.cpp` создаёт **новый GLuint** через `GenTextures`, связывает его с EGLImage и возвращает wrapper с `OwnsHandle::No`. `Texture::DestroyImpl` удаляет только `OwnsHandle::Yes`. Публичный `OpenGLBackend.h` не отдаёт созданный alias и не предоставляет отдельного release API.

Следствия:

- `wgpu::Texture::Destroy()`/release wrapper и уничтожение исходного EGLImage не являются доказанным удалением этого alias.
- Reimport при каждом кадре/resize на живом device нельзя планировать как bounded resource policy.
- Проверенное в демке полное освобождение отдельного device/instance и уничтожение наблюдавшегося context — ограниченное свидетельство, не production per-import cleanup и не measurement отсутствия driver leaks.
- При shared compiler contexts надо учитывать весь **Dawn share group**, включая pipeline contexts и references, не только один наблюдённый context. Presenter не включён в эту группу автоматически: общий EGLDisplay не означает общую share group.

### Варианты для согласования, ничего не выбрано

| Вариант | Плюсы | Цена / ограничения |
| --- | --- | --- |
| Фиксированный малый набор imported slots на lifetime device; output native 640×480 независим от EFB scale | Можно сохранить текущий prebuilt; число импортов заранее ограничено | Нужно отдельно решить surface-only recreation с сохранением допустимого storage и полный device/display reset. Нет per-import cleanup при живом device; bounded число импортов не доказывает teardown с U context topology |
| Исправить importer ownership в том же Dawn ref и собрать проверенный patched package | Прямое исправление ownership причины; позволяет обоснованно retire/recreate slots | Это изменение dependency artifact и отдельный build/ABI/test процесс. Одной замены `No → Yes` недостаточно для всех ранних error paths; нужны cleanup, validation и regression tests. Patch не решает GPU ordering/retirement pending presenter work — эти требования сохраняются. Источники/зависимости/toolchain пересборки ещё не проверены |
| Другая документированная native interop архитектура | Потенциально может убрать данный legacy wrapper | Не доказано наличие подходящего firmware route. STM EGLImage здесь не реализован; ANGLE GL-texture sharing не считать Mali fallback. Нельзя выбирать без отдельного исследования |

Получение private GLuint через backend internals/случайное GL binding state и ручной `glDeleteTextures` — не готовый публичный lifetime contract. Такой обход не предлагается как решение.

## 7. Конкретные интеграционные швы

Если после исследований сохраняется U renderer, адаптации потребуют:

1. **Bootstrap/readiness:** platform-specific SDL2/Mali context/display до Dawn; текущие surface creation/compatibleSurface/capabilities заменяются только в изолированном path, без fake success API.
2. **Final composition target:** bridge должен принять итог с resampler, RmlUi и ImGui, не только EFB (`aurora.cpp:533–574`). Формат/linear/sRGB semantics должны согласоваться с RGBA8 import и pipeline targets.
3. **Publish/consume/recycle:** bounded image slots, настоящий GPU completion в обе стороны, main progress при backpressure; `after_present` привязывается к успешному выполнению presenter swap, не публикации mailbox. Возврат swap не доказывает физический scanout и не заменяет reverse GPU completion.
4. **Resize/recreation/shutdown:** EFB scale отдельно от native present storage; stop-acquire → drain/retire → release; поколения ресурсов, инвалидация stale presentation requests и завершение/отмена их ожиданий при loss. Существующие after-submit/readback/map completions нельзя безусловно отбрасывать: их lifetime должен корректно завершиться, в том числе по aborted/error path. Linux `SurfaceLock` сейчас пустой, не защита EGL ([window.cpp](../extern/aurora/lib/window.cpp):649–658).
5. **Thread affinity:** явный lifecycle SDL presenter и внешних EGL/GL resources на согласованном потоке. Для calls к Dawn с main/FIFO/render/compiler — проверенный механизм synchronization/scoped-current самого Dawn, а не создание приложением своих compiler contexts или необоснованная замена его mutex/scopes. Shutdown должен завершить recorder/compiler/present jobs до destruction borrowed presenter/display.

Сохранение render worker + main presenter потенциально сохраняет CPU overlap, но требует доказанного mailbox/progress protocol. Serial adaptation меняет execution policy, не является бесплатным fallback и может уменьшить overlap. **Ни один вариант не выбран.** Старый raw renderer остаётся альтернативой из [аудита](H700_PORT_REVIEW.md), но его большой объём адаптации и correctness risks не исчезли из-за найденных локальных проблем U.

## 8. Предлагаемые следующие исследовательские шаги

Это **список потенциальных проверок, не обязательная исчерпывающая матрица** и не план всего переноса. Перед каждым пунктом заново пройти [goal gate](H700_WORKFLOW.md): подтвердить необходимость для конкретного H700-пути, минимальный объём и решение по результату. Иначе пункт не начинать; техническая возможность исследования не равна его необходимости. Новые реализации согласуются отдельно, большие необходимые шаги делятся на сессии 1–2 часа. Device-запуск — только в новом согласованном окне.

| ID | Зачем / ограниченный результат | Проверка и граница |
| --- | --- | --- |
| GAP-A | Получить точный U capability/request contract, не ещё один узор | Расширение отдельной демки: полный adapter + final device limits/features, настоящий U RequestDevice с validation. Включить Compatibility vertex/fragment storage, texture/sampler budgets, buffer limits/alignment, varying/compute/attachment limits. Если запрос не проходит — точная причина, без отключения validation |
| GAP-B | Разрешить layout budget до применения texture fetch | Статически согласовать budget/visibility/lightmap policy; маленький validation test реального GX pipeline layout из трёх bind groups для storage и texture paths, с ожидаемым отказом несогласованного исходного варианта. Проверять случаи, применимость которых к целевому пути установлена; покрытие обоих 16/>=17 на Null не является самостоятельной целью. Тест не должен тайно исправить production layout |
| GAP-C | Проверить реальную типовую нагрузку GX вместо одного uniform | Asset-free shader/resource harness: 64-byte immediates, 8192-byte dynamic uniform offsets, выбранный fetch path, color/depth/palette/EFB copy/stencil. Критерий — pixels плюс validation. Форматы/operations разделить на отдельные небольшие проверки, не полный renderer |
| GAP-D | Подтвердить U-like threading и отсутствие цикла ожиданий | Отдельные main presenter, recorder-like resource thread, render worker и compiler; включённый U thread toggle. Сначала context/API smoke case; отдельными сессиями — реальный compile/render overlap, normal/blocking lookup, backpressure и progress во время frame/staging acquisition, FIFO drain и render synchronize. Shutdown с pending frame/compile — отдельный case. Проверить pixels/all joins; не FPS или live-device reimport |
| GAP-E | Закрыть production ownership/recreation | Сначала выбрать lifetime-вариант из раздела 6. Затем bounded repeated lifecycle/failure test выбранного контракта, включая полный share group. Не начинать с reimport leak в цикле на текущем package |

**Первоначально предлагались GAP-A и статический/validation объём GAP-B; это не разрешает добиваться полноты Null matrix.** Затем GAP-A измерил default U request/limits/features и автоматический выбор texture fetch на Mali; device часть GAP-B воспроизвела отказ выбранных layouts, но **не устранила его**. После этого предложено сравнить локальную специализацию resource/layout profile и mixed fetch; последующие результаты приведены ниже. Исполнение shader/resource path, GAP-C/D/E — только после соответствующих решений; их нельзя слить в «перенести renderer и проверить».

В стороне остаются PF-02 для MusyX/nod/game-facing SDL API, build profile полной игры, input/audio/save-load и итоговая приёмка. Это исследование их не закрывает и не разрешает production-перенос.

### Последующее сравнение решений layout-барьера

После device failure пользователь запросил сравнение двух локальных решений: [фиксированный base profile и mixed vertex-texture / fragment-storage fetch](H700_LAYOUT_OPTIONS.md). Разбор рекомендует второй как более локальный кандидат: device сообщает 4 fragment SSBO при 0 vertex SSBO, а buffer уже существует и заполняется до fetch-texture copies. Это не разрешение менять production code и не результат runtime mixed-path test. Base profile остаётся альтернативой; пересборка Dawn и замена firmware не требуются самим обнаруженным budget mismatch. Позднее по отдельному разрешению выполнен [mixed-fetch run](H700_MIXED_FETCH_PROBE.md): device и оба mixed layouts приняты на Mali, но setup synthetic render pipeline остановлен недопустимым Compatibility interpolation sampling `First`. Это была ошибка shader демки до первого workload draw, не отказ SSBO. После явной правки `flat, either` и отдельного разрешения **mixed-v2 прошёл на Mali**: оба layouts/pipelines, 8 draws / 32768 pixels, updates и bounded buffer growth — PASS, exit 0 без timeout. Runtime feasibility этого data path подтверждена; вопрос не требует новой Null matrix или смены backend. Cleanup завершён, firmware hashes неизменны; production code не менялся, его consumers/cache/lifecycle и остальные интеграционные вопросы требуют согласованного плана.

### Последующая подготовка GAP-A/B

Первая попытка расширения остановилась на Null maximum 16; первоначальные восемь cases GAP-B не пройдены. Это признано уходом от цели, не блокером H700. Затем пользователь разрешил закончить демку с новыми вводными: [готовый `contract-v2` package](H700_CONTRACT_PROBE.md) прошёл compile/link, ABI, 62 helper tests, CPU checks и четыре реальных Null controls бюджета 16. Профиль device-budget-17 явно не покрыт, validation не ослаблена. Новый `capabilities` проверяет default U request и **только фактически выбранные** GX layouts на Mali; принудительные альтернативы не запускаются. Затем пользователь предоставил приставку: один `capabilities` run создал device и воспроизвёл **17 > 16** для обоих выбранных layouts. Cleanup завершён, firmware hashes неизменны. Dawn/backend/production code не менялись; последующий mixed-fetch эксперимент описан выше, production-адаптация не начиналась.
