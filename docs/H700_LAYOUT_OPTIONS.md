# H700 — сравнение вариантов устранения GX layout 17 > 16

**Статус:** статическое сравнение по запросу пользователя, 2026-11-15. Это рекомендация для согласования, **не разрешение менять renderer или запускать новый probe**. Production code, dependency pins и устройство не менялись. Исследована новая база U `c872da704adde9a7838492aaa56dac73a32053e4`; исходный обзор двух баз — [H700_PORT_REVIEW.md](H700_PORT_REVIEW.md).

**Goal gate:** корректный запуск новой игры на RG40XX H / muOS 2601 → измеренный отказ выбранного GX layout → проследить bindings, shader consumers и существующий upload/lifetime path → выбрать между специализацией графических ресурсов и разделением доступа к данным по стадиям → закончить на сравнении, без implementation/build/SSH.

## 1. Что уже измерено

[Device test contract-v2](H700_CONTRACT_PROBE.md) создал Dawn/OpenGLES Compatibility device с зеркальным U request. Default policy выбрала texture fetch, no-compute = 0, lightmap = 0. Оба normal/shadow-receiver layouts отклонены: **17 fragment sampled textures при effective limit 16**. Нет timeout или driver crash.

| Final device limit | Значение |
| --- | ---: |
| Sampled textures / shader stage | 16 |
| Compatibility storage buffers / vertex stage | 0 |
| Compatibility storage buffers / fragment stage | 4 |
| Storage buffers / shader stage, общий limit | 8 |
| Max storage buffer binding size | 268435456 bytes |
| Max uniform buffer binding size | 16384 bytes |

Текущий group 2 без lightmap: **8 GX + environment cube + 5 ambient volumes + BRDF + froxel/shadow = 16 textures**. Group 0 добавляет texture-backed `abuf`, видимый Vertex и Fragment. Источники: [gx.cpp:866–973](../extern/aurora/lib/gx/gx.cpp), [frame.cpp:822–852](../extern/aurora/lib/gfx/frame.cpp).

Нулевая доступность vertex SSBO не запрещает fragment SSBO. Но reported limits не доказывают компиляцию и корректность mixed shader workload.

## 2. Вариант 1 — фиксированный base/non-PBR resource profile

### Конкретный минимальный состав

Сохранить 8 GX texture/sampler pairs (bindings 0–15), а также shared texture/sampler slots 24/25 с прежними normal и shadow типами. Исключить из этого профиля cube, общий PBR sampler, 5 ambient volumes, BRDF и lightmap. Другим устройствам/профилям расширенный путь оставить.

**Budget: 9 group-2 textures + 1 fragment fetch texture = 10**, то есть запас 6 относительно limit 16. Сохранённые group-2 bindings не обязаны быть последовательными: нужен компактный массив entries с исходными binding numbers, не просто уменьшение `entryCount` существующего массива.

### Почему не следует удалять всё «не-PBR» вслепую

- Обычный GX fog использует uniform и при range adjustment читает `abuf`; это не volumetric fog. Его путь сохраняется: [shader.cpp:5162–5198](../extern/aurora/lib/gx/shader.cpp).
- Volfog может применяться к **non-PBR** draws; его froxels используют 24/25: [shader_info.cpp:411–425](../extern/aurora/lib/gx/shader_info.cpp), [shader.cpp:4916–4937](../extern/aurora/lib/gx/shader.cpp).
- Non-PBR actor shadow decal использует depth texture/comparison sampler в тех же 24/25; normal/shadow layouts нельзя смешивать: [gx.cpp:669–675,784–789](../extern/aurora/lib/gx/gx.cpp).
- Shell scene copy использует GX slot 2, shell textures — 0/1; удаление PBR cube/volumes их не убирает: [gx.cpp:831–845](../extern/aurora/lib/gx/gx.cpp), [shader.cpp:5143–5148](../extern/aurora/lib/gx/shader.cpp).

Сохранение этих bindings не означает, что перечисленные расширенные эффекты проверены на H700; лишь не требуется вырезать их ради текущего sampled-texture budget.

### Необходимые изменения и ограничения

1. Зафиксировать resource profile до `gx::initialize()` и загрузки pipeline cache.
2. Согласованно собрать оба group-2 layouts, обычные bind groups и empty bind group: [gx.cpp:753–862,866–1079](../extern/aurora/lib/gx/gx.cpp).
3. Ограничить effective выбор материалов/ресурсов так, чтобы PBR shaders не попадали в этот профиль. Одного UI setting недостаточно. `PortPBRAllowed` проверяет режим рисования, не GPU capabilities; `icy` и inventory имеют дополнительные пути выбора PBR: [CCubeMaterial.cpp:1080–1083](../src/Kyoto/Graphics/CCubeMaterial.cpp), [CCubeModel.cpp:1013–1046](../src/Kyoto/Graphics/CCubeModel.cpp).
4. Не подменять отказ capabilities молчаливым пропуском draw или поздним `config.pbr = 0`: TEV fallback не эквивалентен всем mod materials. Например, frost shell намеренно не рисуется в неподходящем fallback из-за отсутствия dissolve: [CCubeModel.cpp:967–980](../src/Kyoto/Graphics/CCubeModel.cpp).
5. Проверять совместимость configs на runtime и cache/prewarm путях. `pipelineCacheSkipPbr` пропускает только prewarm и не запрещает runtime PBR: [pipeline_cache.cpp:1454–1474](../extern/aurora/lib/gfx/pipeline_cache.cpp).

Для разрешённого non-PBR пути генератор WGSL уже условно исключает PBR declarations; менять алгоритм vertex/fragment data fetch не нужно. Но исходные игровые assets и запрет неподдержанных replacements должны быть частью явного профиля, не предположением.

**Удалить только cube или BRDF** арифметически достаточно (17 → 16), но PBR source обращается к ним. Это не корректный самостоятельный фикс для сохраняемого PBR: [shader.cpp:5092–5109](../extern/aurora/lib/gx/shader.cpp). Динамическая специализация под каждый ShaderInfo возможна, но сложнее фиксированного профиля: меняются layout selection, keys и правила rebinding. Для текущей цели она не нужна по умолчанию.

## 3. Вариант 3 — texture fetch в vertex, storage buffer во fragment

### Конкретная схема

Предлагаемый group 0 для mixed profile:

| Binding | Ресурс | Visibility |
| --- | --- | --- |
| 0 | существующая vertex-fetch R32Uint texture | Vertex |
| 1 | существующая storage-fetch R32Uint texture | Vertex |
| 2, новый | существующий `storageBuffer`, read-only storage | Fragment |

Число bind groups остаётся прежним. Group 2, его normal/shadow layouts и выбор материалов не меняются ради устранения этого барьера. Lightmap на H700 по-прежнему выключен.

**Fragment budget: 16 sampled textures + 1 storage buffer**, в пределах измеренных 16 textures / 4 fragment storage buffers. Это расчёт, **не выполненный validation/render test нового layout**.

### Почему не нужен второй upload всей копии данных

`storageBuffer` уже создаётся с `Storage | CopyDst | CopySrc`, даже при texture fetch: [frame.cpp:92–104](../extern/aurora/lib/gfx/frame.cpp). Текущий путь сначала копирует staging/spill/resident данные в него, затем его обновлённые строки — в fetch texture: [encoding.cpp:468–515](../extern/aurora/lib/gfx/encoding.cpp).

Mixed profile может привязать **тот же buffer** для fragment reads. Не требуется вторая CPU-загрузка или ещё один полноразмерный data buffer. Но обе fetch textures всё ещё нужны vertex stage; их текущие copies не исчезают. Уменьшение RAM/upload cost или прирост FPS этим не доказаны.

### Что меняется в shaders

Сейчас один WGSL module содержит vertex и fragment entry points, а `apply_texture_vertex_fetch()` глобально заменяет storage declarations и helpers на texture access: [shader.cpp:3876–3920,5658–5689,5784–5788](../extern/aurora/lib/gx/shader.cpp).

Нужно явно разделить доступы:

- vertex attribute fetch и PBR bind-position access продолжают читать texture-backed `abuf`;
- fragment fog-range lookup читает новый fragment-only storage binding: [shader.cpp:5186–5188](../extern/aurora/lib/gx/shader.cpp);
- PBR room-light records также читают этот binding: [shader.cpp:3033–3040](../extern/aurora/lib/gx/shader.cpp).

Нельзя просто изменить visibility прежнего binding или оставить глобальную замену для нового fragment buffer. Нужны раздельные имена/генерация доступа и проверка, что vertex entry points, включая shadow, не обращаются к SSBO. Сохраняются word offsets, bitcast/endian semantics и корректные границы доступа; непроверенные данные/NaN нельзя объявлять безвредными.

### Policy, lifecycle и cache

- Сначала проверить adapter fragment storage capacity и размер binding; final device должен предоставить минимум 1 fragment storage buffer. Явно отразить mixed profile в required limits и `needStorageBuffers`, не полагаться на случайное улучшение defaults. Сейчас policy задаёт fragment request 0 при texture fetch: [gpu.cpp:1145–1234](../extern/aurora/lib/webgpu/gpu.cpp).
- Расширить static layout и `create_static_bind_group()` существующим buffer binding. Growth уже копирует старый buffer в новый, пересоздаёт fetch texture и static bind group; этот механизм нужно сохранить и проверить для всех трёх entries: [frame.cpp:1000–1043,1102–1154](../extern/aurora/lib/gfx/frame.cpp).
- Новая запись в buffer должна предшествовать draw и texture copy; не добавлять raw unsynchronized mappings или обход Dawn resource tracking.
- Policy фиксируется на lifetime device; cached configs должны порождать WGSL/layout текущего профиля. Проверить schema/version/isolation там, где это необходимо, без автоматического удаления пользовательских caches. Pipeline SQLite хранит configs, не готовое доказательство shader compatibility: [pipeline.hpp:21–48](../extern/aurora/lib/gx/pipeline.hpp), [pipeline_cache.cpp:1440–1474](../extern/aurora/lib/gfx/pipeline_cache.cpp).

Главная новая неизвестность — реальный fragment SSBO read и компиляция mixed WGSL на pinned Dawn/Mali. Успешный RequestDevice с reported capacity этого не проверял.

## 4. Сравнение

| Критерий | 1: base profile | 3: mixed fetch |
| --- | --- | --- |
| Fragment sampled textures | 10 | 16 |
| Fragment SSBO | 0 для GX group 0 | 1 |
| Резерв sampled textures | 6 | 0, но measured limit не превышен |
| Vertex data path | Без изменений | Без изменений |
| Group 2 | Перестройка layouts и всех bind groups | Без изменений |
| Shader data access | Не меняется для разрешённых non-PBR configs | Явное разделение fragment/vertex доступа |
| Материалы и asset policy | Нужен согласованный non-PBR профиль и guards | Не требуется ограничивать материалы ради этого лимита |
| Upload/memory | Без изменений data-fetch path | Можно использовать существующий buffer; обе fetch textures сохраняются |
| Дополнительный driver workload | Новый тип чтения не добавляется | Fragment SSBO пока не проверен |
| Риск | Несогласованность capabilities, материалов, cache и layouts | Ошибка stage separation, адресации, binding lifetime либо shader compiler |
| Сохранение расширений | За пределами base profile; другим профилям оставить | Bindings сохраняются; работоспособность самих эффектов не доказана |
| FPS | Не измерен | Не измерен |

## 5. Рекомендация и минимальный следующий шаг

**Предпочтительный кандидат для текущего барьера — вариант 3.** По коду это более локальное исправление слишком общего fallback: отсутствие vertex SSBO не требует переводить fragment reads в texture. Оно сохраняет group 2 и не вносит новую политику материалов только ради 17 > 16. Вариант 1 оставить резервным решением для базового original-assets профиля; его запас по ресурсам полезен, но сам по себе не оправдывает более широкий объём сейчас.

Это рекомендация по структуре и рискам, не заявление, что вариант 3 уже работает или быстрее. Два варианта не нужно реализовывать параллельно.

**Следующий предлагаемый шаг после согласования:** отдельная небольшая итерация probe, не production-перенос:

1. Зафиксировать mixed contract, adapter/final-device guards и source-reference guards. Сохранить старый 17 > 16 как явный regression diagnostic, не менять его на PASS.
2. Проверить mixed normal/shadow layouts на budget 16 и маленькую asset-free shader/resource нагрузку: vertex texture fetch + fragment storage read из одних данных, fog-like word indexing, граница строки 4095/4096 и повторное обновление данных. Приёмка — реальные контрольные pixels и validation, не только создание layout.
3. Проверить замену source buffer и bind group в ограниченном сценарии, необходимом для такого lifetime; не расширять это до EGLImage recreation или полной threading matrix.
4. Сначала подготовка/build/ABI по существующему builder без смены Dawn. Один ограниченный H700 run — только в отдельно согласованном окне с прежней изоляцией/timeout/cleanup. Null — только вспомогательная проверка допустимого бюджета 16, не повод развивать матрицу 17.

При отказе записать точный shader/layout/resource failure и снова выбрать подход. При успехе — согласовать production-правку, её cache/lifecycle проверки и включение в план переноса. В обоих случаях завершить вопрос этого data path, не начинать попутно PBR/FPS/новый backend.

## 6. Роль внешних заметок Mali-G31

[Halo / MALI-G31-NOTES.md](https://github.com/kirklandsig/halo-ce-anbernic-rg35xx/blob/main/docs/MALI-G31-NOTES.md) — практический вторичный источник по H700/Mali с похожей строкой драйвера, но другой прошивкой Knulli. Полезен предупреждениями об uploads, buffer synchronization и NaN, а позже — стоимостью program switches/target copies. Он **не доказывает mixed Dawn path**, не разрешает менять clocks или убирать correctness waits. Сам вариант 3 следует из наших limits и кода, не из чужих FPS.

**В итерации сравнения выполнено:** чтение исходников и существующего device log, независимый read-only разбор варианта 1, сопоставление budget и lifecycle; без builds/tests/SSH. Затем пользователь разрешил подготовить и отдельно запустить [mixed-fetch демку](H700_MIXED_FETCH_PROBE.md): пакет прошёл CPU/ABI/Null gates; на Mali RequestDevice и оба mixed layouts — PASS. Render pipeline setup остановлен недопустимым interpolation sampling `First` в synthetic shader демки, до первого workload draw. Вариант 3 принят по layout budget, но execution/pixels/updates ещё не доказаны. Затем по разрешению пользователя harness исправлен на явный `flat, either` в `mixed-v2`, без изменения oracle; два shader pipelines прошли Null Compatibility validation, package/build/ABI/CPU gates прошли. Это не утверждение production-адаптации и не GPU execution; повторного Mali run нет.
