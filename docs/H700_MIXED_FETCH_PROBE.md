# H700 — демка варианта 3: mixed vertex texture / fragment storage fetch

## Разрешение и цель

Пользователь после [сравнения вариантов 1 и 3](H700_LAYOUT_OPTIONS.md) разрешил подготовку asset-free probe, без production-реализации и device run. Затем явно подтвердил свободную приставку и разрешил **один `--mode mixed-fetch` run** по сценарию ниже. Разрешение включает verified SSH/preflight, временный package, loader/CPU smoke checks и cleanup, не дополнительные graphics modes или production-адаптацию. Донор, game/platform sources, Dawn/SDL pins и builder не меняются.

**Goal gate:** корректный запуск новой игры на H700 → выбранный U layout отклонён 17 > 16 → проверить разделение texture fetch в vertex и storage buffer read во fragment → успех позволит планировать локальную адаптацию U, отказ даст конкретную причину для сравнения с base profile → остановиться на проверенном пакете, без автоматического запуска на Mali.

## Зафиксированный объём подготовки

- Новый явный device mode `--mode mixed-fetch`, без `--frames`: ограниченная последовательность, не benchmark. Как остальные device modes, создаёт SDL2/Mali window для firmware bootstrap; fullscreen graphics не запускаются в контейнерных smoke tests.
- Mixed group 0: binding 0 — vertex R32Uint texture, Vertex; binding 1 — storage mirror R32Uint texture, Vertex; binding 2 — тот же source storage buffer, ReadOnlyStorage/Fragment. Group 1 — прежний dynamic uniform; group 2 — исходные 16 sampled textures без lightmap, normal/shadow варианты. Immediate size — 64 bytes.
- Adapter и final device guards: texture dimension >=4096, sampled textures >=16, fragment storage >=1, достаточно storage binding/buffer size для ограниченного replacement. Request явно сохраняет 16 sampled textures, vertex storage request 0, fragment storage request >=1. Native U feature candidates и thread toggle сохраняются, validation/robustness включены; это всё ещё external EGLDisplay harness без application overrides/cache/compatibleSurface.
- Исходные U layouts не исправляются: оба 17 > 16 rejections проверяются конкретной причиной, не принимается произвольная validation error. Старый `--mode capabilities` остаётся nonzero при отказе selected U layout.
- Оба mixed layouts должны пройти scoped validation. Полноразмерный game renderer и lightmap не включаются.
- Asset-free shader module содержит vertex и fragment entry points с раздельными ресурсами. Vertex реально читает обе fetch textures, fragment — storage buffer и все 16 графических textures (в shadow-варианте последний ресурс — depth array/comparison sampler). Контрольные RGBA8 pixels сравниваются с независимым CPU расчётом; не только declarations/layout success.
- Данные проходят staging → storage buffer → fetch texture в одном упорядоченном command stream. Проверяются fog-like bitcast/word reads, переход 4095/4096 и повторное обновление. Source buffer одновременно остаётся источником texture mirror и fragment binding, второй CPU upload только ради fragment не добавляется.
- Четыре состояния ресурсов × normal/shadow = 8 draws: начальные данные; обновление существующего buffer; ограниченный рост buffer с CopyBufferToBuffer и пересозданием mirror/bind group с сохранением старых данных; обновление выросшего buffer, включая данные за старой границей. Ожидания GPU completion и readback сохраняются. Это не тест всех production growth/spill/resident scenarios.
- Новый отдельный `--mixed-layout-self-test`: только Null budget 16, два mixed-positive и два исходных texture-negative layouts; без SDL/EGL/GPU, без shader/pixel execution. Прежний четырёх-case `--layout-self-test` сохраняется. Ни профиля Null 17, ни нового backend не требуется.
- CPU `--self-test` дополнен policy/layout/pattern assertions. Build helper сохраняет отдельный Null mixed report и точные coverage/markers в manifest; отсутствие/подмена marker, wrong-budget, nonzero, drift источников проваливают gate. Свежий output, предыдущие artifacts не перезаписываются.

## Проверки и приёмка

1. Python helper regression tests, integrity архивов/источников и guards U reference files.
2. ARM64 compile/link с прежним `-Wall -Wextra -Wpedantic -Werror`, ELF/ABI/provider gate, container ldd.
3. Только safe modes в контейнере: `--help`, `--self-test`, `--layout-self-test`, `--mixed-layout-self-test`. Null не доказывает shader/Mali execution.
4. Licenses, metadata, все package hashes и manifest. Новый package явно **не device-validated**.
5. После отдельного разрешения устройства: verified SSH/preflight, unique `/tmp` package, manifest verification, isolated HOME/XDG/TMPDIR, core dumps off, внешний timeout/process group и cleanup по прежним safeguards. Один mixed-fetch run. Это не разрешено автоматически подготовкой.

**Откат:** не использовать новый package; прежние outputs сохранены. Production files не меняются. Ошибки не скрываются и приёмка не ослабляется; если нужна смена зависимости/архитектуры/объёма, остановка и согласование по [workflow](H700_WORKFLOW.md).

Подготовка разбита на контракт/CLI/Null controls и shader-resource workload/build, ориентир по 1–2 часа активной работы на часть, ожидание сборки отдельно. Runtime-проверка и production-адаптация — отдельные последующие решения.

## Текущий статус

**Подготовка прошла; отдельно разрешённый H700 run выполнен с частичным результатом, overall FAIL.** Device и оба mixed layouts созданы, но `CreateRenderPipeline` отклонил синтетический shader демки из-за неподдерживаемой в Compatibility mode flat interpolation sampling `First`. До mixed draws/readbacks дело не дошло; buffer execution не подтверждено. Это дефект совместимости тестового shader, не доказанный отказ fragment SSBO. Production sources не менялись; повторного запуска/исправления после этого результата нет.

Из корня репозитория выполнены:

```sh
python3 -B tools/h700_graphics_probe/test_build.py
python3 -B tools/h700_graphics_probe/build.py --check-inputs
python3 -B tools/h700_graphics_probe/build.py --output build/h700-graphics-probe-mixed-v1 --jobs 2
```

| Проверка | Результат |
| --- | --- |
| Python helper tests | **70/70 PASS**, включая прежние gates и новые mixed marker/input/drift failures |
| Cached SDL/Dawn inputs и U references | **PASS**, прежние pins; 8 SHA-256 guards (исходные 5 + shader.cpp/resources.hpp/encoding.cpp) |
| ARM64 compile/link | **PASS**, Clang16, C++20, прежние warning/error flags |
| ELF/ABI/provider gate и container ldd | **PASS**, все container dependencies разрешены; это не loader test нового binary на muOS |
| `--help` и CPU `--self-test` | **PASS**, включая mixed golden pixels, границы, update/preserve/growth и проверки отказов; без SDL/EGL/Dawn initialization |
| Старый `--layout-self-test` | **4/4 PASS**: прежние storage-positive и texture-negative normal/shadow controls бюджета 16 |
| Новый `--mixed-layout-self-test` | **4/4 PASS**: mixed-positive и original texture-negative normal/shadow controls бюджета 16; только Dawn Null layouts |
| Package | **PASS**, licenses/metadata/manifest, 51 файл вместе с manifest; все artifact/source hashes перепроверены, `package.partial` отсутствует |
| Mali shader/pixel/resource workload при подготовке | Не запускался. Последующий отдельно разрешённый run описан ниже; overall FAIL до первого workload draw |

Дополнительно выполнен независимый read-only review stages/layouts, oracle обновления/роста и failure propagation: подтверждённых статических блокеров не найдено. Package hashes проверены отдельно родительской задачей. Это не заменяет исполнение shaders на Mali.

Реализация: [contract.hpp/.cpp](../tools/h700_graphics_probe/contract.hpp) — исходный U mirror и отдельная proposed mixed schema; [mixed.cpp](../tools/h700_graphics_probe/mixed.cpp) — ограниченная нагрузка и CPU oracle; [probe.cpp](../tools/h700_graphics_probe/probe.cpp) — explicit CLI, request/diagnostics/cleanup; [build.py](../tools/h700_graphics_probe/build.py) — safe modes/metadata/integrity. В исходной selected-U проверке отказ 17 > 16 по-прежнему остаётся overall failure.

### Проверенный artifact

`build/h700-graphics-probe-mixed-v1/package/`

| Файл | Размер, bytes | SHA-256 |
| --- | ---: | --- |
| `h700_graphics_probe` | 13665992 | `cf9fcf548460f186ec5ee598eec251c4307f43388ade5c520990c981ad6400e8` |
| `libSDL3.so.0` | 2771560 | `ffb36e54c7c25f17f788f74550fcfd7da7136a66fa1e24771497b0a18f134e70` |
| `manifest.json` | 22159 | `90dac3db2dcefc5af0fc8c8a4d17617a217843766b2098daefb33fee58647225` |

Builder identity: `sha256:2db4439763783aa796da4268dbd91847ee2c13deef4c8ce2439e0f6bdf926962`, прежний demo-only `metroid-h700-probe:abi238-v1` с документированным EOL-компромиссом. Executable: AArch64, loader `/lib/ld-linux-aarch64.so.1`, максимум GLIBC_2.38 / GLIBCXX_3.4.32 / CXXABI_1.3.13. Bundled SDL3 неизменна; container libc/libstdc++/EGL/GLES/SDL2 не упакованы. Существующие outputs не удалены.

Отдельные metadata reports содержат точные финальные markers:

```text
PASS layout-self-test cases=4 budget=16 (Dawn Null validation only; no SDL/EGL/GPU)
PASS mixed-layout-self-test cases=4 budget=16 (Dawn Null validation only; no SDL/EGL/GPU)
```

Manifest отдельно указывает `mixed_layout_validation_scope` и непроверенный GPU/Mali workload. Reference/source hashes совпадают с текущими файлами. Для новой сборки нужен новый output suffix, не перезапись этого package.

### Граница device-проверки

Первоначально следующий шаг требовал отдельного разрешения на один `--mode mixed-fetch` run; пользователь затем явно подтвердил свободную приставку и разрешил его. Условия выполненного запуска: Окно может быть очищено: режим проверяет offscreen pixels и не импортирует EGLImage/не показывает узор. Внешний timeout 35 s, затем TERM и через 5 s KILL только собственной process group; никаких clocks/services/firmware/save changes. При смене текущей Mali policy или недостаточных reported capacities — явный отказ, не другой backend.

Успех требует обеих original negative controls, обоих mixed layouts и всех 8 draws / **32768 pixels** плюс source-buffer readbacks, updates и bounded growth. Финальный marker только после API cleanup/проверки diagnostics:

```text
PASS mixed-fetch layouts=2 draws=8 pixels=32768 updates/growth/teardown checked (not game/FPS)
```

Это не проверка production shader generator, всех PBR effects, EGLImage alias lifetime/recreation, concurrent contexts или полной игры. На этом этапе проверить нужно выбранный data path, а не универсальный renderer.

## Фактический H700 run — частичный результат, overall FAIL

Пользователь подтвердил свободное устройство. Цепочка этой сессии: H700 layout-барьер → один mixed-fetch run с прежним artifact → сохранить конкретный layout/shader/resource результат → cleanup и остановка, без повторных graphics runs.

Verified SSH и preflight: AArch64, Linux 4.9.170, процессов игры/демки нет; шесть проверенных firmware loader/libc/libstdc++/EGL/GLES/SDL2 hashes совпали с исходным snapshot. MemAvailable **824968 kB**, swap отсутствует, `/tmp` free **510255104 bytes**. Эти числа — разовый снимок, не бюджет игры.

Проверены все **51 package files** и локальные source hashes. Пакет передан в новый уникальный `/tmp` directory и повторно проверен на устройстве по manifest/hashes. Runtime изолирован: HOME/XDG config/cache/data/state/runtime/TMPDIR, `LD_LIBRARY_PATH` только package, per-process SDL3-over-SDL2 → SDL2 `mali`, core dumps off. Внешний timeout 35 s, TERM + 5 s до KILL только собственной process group; timeout/сигналы не понадобились.

| Команда | Exit | Timeout | Wall time, s |
| --- | ---: | --- | ---: |
| `--help` | 0 | нет | 0.059 |
| `--self-test` | 0 | нет | 0.169 |
| `--mode mixed-fetch` | **1** | нет | **0.471** |

Фактически выбран аппаратный **Mali-G31**, EGL 1.4 / GLES 3.2, drawable 640×480. Policy: texture fetch = 1, no-compute = 0, lightmap = 0. Mixed request явно содержит sampled textures = 16, vertex storage = 0, fragment storage = 1. Итоговый device: **16 sampled textures, 0 vertex storage buffers, 4 fragment storage buffers**; alignment uniform/storage 64/16, storage binding limit 268435456 bytes. ASTC, texture-component-swizzle и implicit-device-synchronization включены; thread toggle включён, validation/robustness сохранены.

| Часть проверки | Фактический результат |
| --- | --- |
| RequestDevice mixed profile | **PASS** |
| Исходные U layouts normal/shadow | Оба отклонены именно **17 > 16**, отрицательные controls прошли |
| Mixed layouts normal/shadow | **Оба PASS на Mali**, 16 textures + 1 fragment SSBO, 0 vertex SSBO |
| Synthetic render pipeline setup | **FAIL**, первая сохранённая validation diagnostic — normal pipeline / interpolation sampling `First` |
| 8 workload draws / 32768 pixels / updates / buffer growth | **Не выполнены**, PASS case markers: 0 |
| Финальная device-приёмка | **FAIL**, success marker отсутствует; отрицательные controls и layout PASS не маскируют отказ |

Существенный output:

```text
PASS mixed layout shadow=0 fragment_textures=16 fragment_storage=1 vertex_storage=0
PASS mixed layout shadow=1 fragment_textures=16 fragment_storage=1 vertex_storage=0
FAIL Mixed setup: The interpolation sampling (First) of the vertex output at location 0 is not supported in compatibility mode
 - While calling [Device].CreateRenderPipeline([RenderPipelineDescriptor ""Mixed normal GX-shaped pipeline""]).
```

### Причина и границы вывода

Синтетический shader в [mixed.cpp:235–245](../tools/h700_graphics_probe/mixed.cpp) передаёт собственную контрольную сумму как `@location(0) @interpolate(flat) signature: u32`. На этом Compatibility device Dawn отверг получившийся sampling `First`. Это **ошибка совместимости введённого harness shader**, не driver crash и не evidence поломки SSBO/readback. В целевом [GX shader generator](../extern/aurora/lib/gx/shader.cpp) литерал `@interpolate(flat` не найден; именно этот varying добавлен демкой.

Расчётный resource budget теперь подтверждён real-device layout validation. Но совместное shader execution texture/SSBO, pixels и buffer growth **ещё не доказаны**. Основания менять firmware/Dawn, переходить к варианту 1 или объявлять вариант 3 рабочим по этому отказу нет.

Следующий предлагаемый шаг — точечная compatibility-правка передачи контрольной суммы в shader демки с сохранением oracle/всех 8 cases и validation; затем новый artifact/build gates и отдельно согласованный повторный device run. Не менять production shader, не ослаблять проверки и не расширять backend matrix. Исправление и повторный запуск в этой сессии не выполнялись.

### Cleanup

Собственный test directory удалён после проверки ownership/manifest marker. Процессов игры/демки нет, все шесть firmware hashes неизменны. Во всех семи изолированных runtime directories — **0 файлов**. Игра, saves, Ports, clocks, settings и services не менялись. Визуальная проверка/возврат меню не подтверждались: mixed mode не показывает узор.

Полный raw result сохранён только локально в ignored `build/h700-graphics-probe-mixed-v1/device-mixed-fetch.json`. Приватный SSH target и уникальный session path в Git не включены; локальная session orchestration остаётся в ignored build directory, не становится новым project framework.
