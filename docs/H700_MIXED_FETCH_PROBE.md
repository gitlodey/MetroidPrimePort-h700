# H700 — демка варианта 3: mixed vertex texture / fragment storage fetch

## Разрешение и цель

Пользователь после [сравнения вариантов 1 и 3](H700_LAYOUT_OPTIONS.md) разрешил попробовать отдельную демку варианта 3. Это разрешение на подготовку asset-free probe, **не production-реализацию и не новый SSH/deploy/device run**. Для устройства нужно отдельно согласованное свободное окно. Донор, game/platform sources, Dawn/SDL pins и builder не меняются.

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

**Подготовка завершена; package готов к отдельно согласованному H700 run.** Новый `mixed-fetch` на Mali не запускался. План был зафиксирован до изменения probe-кода; production sources не менялись.

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
| Mali shader/pixel/resource workload | **Не выполнялся**, не считается PASS по компиляции C++ или Null layouts |

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

Следующий шаг — отдельно согласовать свободную приставку и один `--mode mixed-fetch` run по описанной выше изоляции. Окно может быть очищено: режим проверяет offscreen pixels и не импортирует EGLImage/не показывает узор. Внешний timeout 35 s, затем TERM и через 5 s KILL только собственной process group; никаких clocks/services/firmware/save changes. При смене текущей Mali policy или недостаточных reported capacities — явный отказ, не другой backend.

Успех требует обеих original negative controls, обоих mixed layouts и всех 8 draws / **32768 pixels** плюс source-buffer readbacks, updates и bounded growth. Финальный marker только после API cleanup/проверки diagnostics:

```text
PASS mixed-fetch layouts=2 draws=8 pixels=32768 updates/growth/teardown checked (not game/FPS)
```

Это не проверка production shader generator, всех PBR effects, EGLImage alias lifetime/recreation, concurrent contexts или полной игры. На этом этапе проверить нужно выбранный data path, а не универсальный renderer.
