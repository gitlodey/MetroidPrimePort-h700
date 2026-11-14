# H700 / RG40XX H: обзор донорских изменений и их пригодности для переноса

Дата исследования: **2026-11-14**. Статус: **технический обзор до начала переноса**, не готовый план реализации и не отчёт о работоспособности новой сборки.

## 1. Цель, границы и краткий вывод

Целевая установка — **ANBERNIC RG40XX H, muOS 2601.0 Jacaranda**. Первый поддерживаемый способ сборки — **Linux ARM64 Docker**, установки — вручную в Ports. Цель: оригинальная игра с оригинальными ресурсами и штатными эффектами, корректные управление, звук, сохранение/загрузка; ориентир производительности — **стабильные 30 FPS при 640×480**. Снижение разрешения и отключение штатных эффектов не считать эквивалентным выполнением цели.

Обзор отвечает на вопросы:

1. Что именно изменили два донорских коммита и какую задачу это решало?
2. Какие изменения нужны именно платформе, а какие — общие исправления, оптимизации, настройки или регрессии?
3. Насколько решения корректны и проверены?
4. Что уже иначе реализовано в новой базе?
5. Что переносить, адаптировать как идею или исключить?

**Основной вывод:** переносить оба коммита целиком нельзя. R36S-коммит заменяет значительную часть Aurora, одновременно откатывая ряд уже существовавших проверок и функций. H700-коммит делает этот вариант реально запускаемым на целевой muOS и устраняет несколько регрессий, но не восстанавливает весь исходный контракт Aurora. Новая база уже содержит часть исправлений и более развитые управление ресурсами, GX semantics и ввод.

Наиболее ценный донорский материал — **firmware SDL2/Mali bootstrap, EGL context ownership, EGLImage presentation bridge, проверенный build/runtime profile и конкретные regression cases**. Самостоятельный GLES renderer — возможный источник реализации, но не готовая безопасная замена новому graphics subtree.

**Общая оценка качества:** R — результативный, но сильно связанный с частным окружением прототип: полезные низкоуровневые решения смешаны с незавершёнными paths и откатами safety. H — существенно более аккуратный bring-up: source-pinned shim, невмешивающийся в firmware launcher, root-cause fixes и focused tests. Однако H наследует большинство архитектурных долгов R; ни один коммит не является готовой production-поддержкой H700.

В документе используются решения:

- **Перенести** — сохранить конкретное решение после сверки с новой инфраструктурой; это не означает cherry-pick всего файла.
- **Адаптировать** — перенести инвариант, тест или идею, реализацию встроить заново.
- **Уже есть** — не дублировать и не заменять старой версией.
- **Исключить** — не переносить без отдельного доказательства необходимости.
- **Проверить** — есть существенная неопределённость; перенос пока не обоснован.

## 2. Ревизии и метод исследования

| Обозначение | Репозиторий / ревизия | Назначение |
| --- | --- | --- |
| P | `MetroidPrimePort`, `fd0f23e2c46153600214b2cd5d9f4b98a2cff518` | Родитель R, исходная точка для атрибуции изменений |
| R | `MetroidPrimePort`, `aaf8349b144c3419714d489a017483ce7ef3edb0` | R36S/RK3326, 223 файла, +16131/−24992 строк |
| H | `MetroidPrimePort`, `3f31d19f6956e0c0ada7ed56b48703b9e0c00cd6` | Прямой потомок R, H700 bring-up; 26 файлов, +2299/−234 строки |
| U | `MetroidPrimePort-h700`, `c872da704adde9a7838492aaa56dac73a32053e4` | Целевая база, release `0.46.0` |

Статистика — по Git, не показатель полезности изменений. P присутствует в истории U; R и H не следует считать уже включёнными в U. Названия remote и доступность GitHub не использовались как доказательство происхождения конкретной строки: исследовались локальные объекты Git.

Сравнения различаются:

- **P → R**: что сделал именно R36S-коммит;
- **R → H**: что добавил именно H700-коммит;
- **H ↔ U**: семантическая применимость к новой базе, не простой конфликт строк.

Это существенно: например, ненулевой serial GCI-карты был уже в P, R заменил его нулём, H исправил регрессию другой моделью identity, а U сохраняет path-based serial. Неверно описывать это как впервые реализованную в H поддержку serial.

### 2.1. Уровни доказательности

- **Код**: вывод следует из diff, реализации и call sites. Для статического дефекта отдельно указаны условия проявления; это не доказательство частоты дефекта в игре.
- **Исторический device-test**: результат записан в донорском `MUOS_BRINGUP.md`. Старые captures, JSON и бинарники не были повторно проверены в этой сессии.
- **Свежий read-only SSH**: проверено на устройстве при подготовке этого обзора, без запуска игры, EGL probes и изменения конфигурации.
- **Гипотеза/кандидат**: архитектурное или performance-предположение, требующее отдельного опыта.

Изучены source diffs, активные CMake source lists, соответствующие реализации U и regression tests. Сами C++ tests, Docker build и новая игра **в этой сессии не запускались**. Успешная проверка синтаксиса скрипта не равна проверке его безопасности или runtime.

## 3. Ограничения целевой платформы

Подробные исходные справочники: [RG40XX_H_HARDWARE.md](../../MetroidPrimePort/docs/RG40XX_H_HARDWARE.md), [MUOS_PLATFORM.md](../../MetroidPrimePort/docs/MUOS_PLATFORM.md), [исходный inventory](../../MetroidPrimePort/muos-diagnostics.txt).

### 3.1. Что это означает для кода

| Условие | Следствие для переноса |
| --- | --- |
| 4× Cortex-A53, AArch64 | ARM64 Linux ELF; `-mcpu=cortex-a53` уместен. Не переносить RK3326 sysroot или бинарные библиотеки |
| Mali-G31, общая память около 1 GB | Учитывать CPU heap, driver allocations, textures, buffers и caches совместно; паспортная RAM не является texture budget |
| Панель 640×480, 4:3 | Базовый EFB и вывод 640×480; widescreen не является целевой оптимизацией |
| `/dev/fb0`, `/dev/mali0`, нет `/dev/dri` | Firmware SDL2 `mali` — проверенный путь; KMSDRM/GBM нельзя выбирать по аналогии с R36S |
| Firmware `libmali` предоставляет EGL/GLES | Не подменять её desktop/Mesa/RK3326 blobs |
| Исторически ES 3.2 / EGL 1.4 | Проверять конкретные limits/extensions и context interop, а не только version strings |
| Нет swap | Memory spikes, бесконтрольный program/texture cache и буферные резервы могут завершить процесс |
| Системная SDL2, не подтверждённая native SDL3 | SDL3-over-SDL2 — отдельная поставляемая библиотека; её headers, pin и `dlopen` chain важны |
| ALSA route через PipeWire, запуск из frontend | SSH и Ports могут иметь разное окружение; ошибка SSH audio не доказывает отсутствие звука |

### 3.2. Свежая пассивная проверка 2026-11-14

Через SSH выполнено только чтение OS/kernel, `/proc/meminfo`, library symlinks, sysfs и ELF version metadata. Игра, графические probes и clocks/configuration не запускались/не менялись. Адрес подключения и пользовательские данные в документ не включены.

| Проверка | Результат |
| --- | --- |
| OS / build | `2601.0 (JACARANDA)`, `BUILD_ID=bc38efa0` |
| Kernel / arch | `4.9.170`, `aarch64` |
| glibc | `2.38` |
| RAM | `MemTotal=996632 kB`; `MemAvailable=888740 kB` в одном снимке |
| Swap | `0 kB` |
| Display/GPU nodes | `/dev/fb0`, `/dev/mali0`; `/dev/dri` отсутствует |
| SDL2 symlink | `libSDL2-2.0.so.0 → libSDL2-2.0.so.0.2800.5` |
| EGL/GLES symlinks | `libEGL.so.1`, `libGLESv2.so.2 → libmali.so` |
| C++ runtime | `libstdc++.so.6 → libstdc++.so.6.0.32` |
| Наибольшие числовые version nodes C++ runtime | `GLIBCXX_3.4.32`, `CXXABI_1.3.14`, по `readelf --version-info` |
| CPU governor / разовая частота CPU0 | `ondemand`, `1512000 kHz` |
| GPU devfreq | `/sys/class/devfreq/gpu`; `simple_ondemand`, `420000000 Hz` в снимке |
| GPU available frequencies | 420, 456, 504, 552, 600, 648 MHz |
| Gamepad DB | `/usr/lib/gamecontrollerdb.txt → /opt/muos/share/info/gamecontrollerdb/retro.txt` |
| Найденные tools | `readelf`, `python3`; `eglinfo`/`es2_info` не найдены через `command -v` |

Первый предположенный путь `sunxi-gpu/.../cur_freq` отсутствовал; корректный путь найден через `/sys/class/devfreq`. Это ещё одна причина не вшивать чужую sysfs topology.

Снимок свободной RAM и частот **не является** игровым memory budget или оценкой throttling. Максимальные version nodes не заменяют проверку каждого требуемого ELF symbol. EGL/GL extensions, panel refresh rate и устойчивость нескольких contexts свежим опросом не проверялись: это потребовало бы активного graphics probe.

## 4. Карта областей изменений

Это группировка существенных изменений, а не заявление, что каждая строка из 223 файлов необходима H700.

| Область | R | H | Вывод для U |
| --- | --- | --- | --- |
| CMake/backend | Удаление Dawn, подключение GL; изменение dependency targets | Настройка shim из исходников | Backend-specific интеграция, не замена всего build system |
| Toolchain | Cross preset без полноценного sysroot, пути автора | Явный sysroot, отдельный native ARM64 preset | Основной путь — native Docker; cross отдельно |
| SDL | Imported binary по абсолютному пути | Pinned SDL3-over-SDL2 source fork | Адаптировать с provenance и hashes |
| GPU/window | Raw GLES/EGL, shared contexts, EGLImage slots | Подтверждение runtime и probes | Основной платформенный донор |
| Graphics core | `gfx/common.cpp` вместо модульного WebGPU core | Локальные correctness fixes | Не переносить целиком |
| GX/shaders | Native attrs, GLSL, caches, synchronous FIFO | Draw-sync drain и XF fix | Сохранить U semantics, оценить GL реализацию |
| Texture pipeline | Упрощение formats, streaming и replacements | Реальное allocation encoded replacements | Исправление нужно донору; U имеет иной pipeline |
| Card/IO | Снятие проверок/atomic semantics, serial=0 | Persistent sidecar serial | Не откатывать U card subsystem |
| Input | Handheld fallbacks и упрощение mappings | Firmware DB в launcher | Сохранить новый input layer U |
| Audio | Gain 0.75, меньшая громкость, settings overrides | PipeWire runtime environment | Разделить routing и пользовательскую громкость |
| UI | GL ImGui/RmlUi, stubs ряда APIs | Компактный FPS overlay | Адаптация к выбранному renderer и новому UI |
| SDK/runtime | ARAM/time/THP/DVD compatibility изменения | Небольшой набор fixes | Откаты safety не нужны H700 |
| Packaging | R36S launcher, metadata, deploy/debug scripts | Standalone-safe launcher, staging | Launcher/package адаптировать; deploy R исключить |
| Tests/diagnostics | GL adaptation, удаления части тестов | XF/texture/card tests, probes/profiler | Переносить инварианты, не GL stubs поверх Dawn tests |

## 5. Сборка, зависимости и ABI

### 5.1. R: cross-build как частное окружение автора

Файлы: `CMakePresets.json`, `cmake/toolchain-aarch64-linux-gnu.cmake`, `cmake/BuildRevision.cmake`, `extern/aurora/cmake/*`, `extern/aurora/extern/CMakeLists.txt`.

Preset `linux-aarch64-gles` задаёт Clang target `aarch64-linux-gnu`, Ninja, `RelWithDebInfo`, vendor SDL/nod, отключённые X11/Wayland и tests. Однако исходный toolchain не задавал `CMAKE_SYSROOT`, а использовал `/usr/aarch64-linux-gnu` и `$HOME/foxhollow-sysroot/usr` в root search paths. Это не гарантирует согласованные headers, crt, libc, C++ runtime и linker.

Дополнительно `AuroraSDL3Provider.cmake` подставлял `/home/jbc/sdl3-shim/libSDL3.so`, если файл найден. Такой ранний imported target обходил выбранный provider/linkage, а headers ожидались в build `_deps/sdl-src/include`. Получалась зависимость от случайного бинарника и потенциально других headers. **Исключить.**

`AURORA_USE_GLES=ON` в preset не является доказательством выбора backend: в исследованном доноре этот cache key не управляет реализацией; GL sources подключены непосредственно изменёнными CMake-файлами.

R также принудительно устанавливает `dirty=1` в `BuildRevision.cmake`. Даже чистая сборка маркируется dirty, теряется различимость tested commit и local patch set. Это не compatibility fix. **Не переносить; U уже определяет dirty штатно.**

### 5.2. H: native ARM64 Docker — разумный baseline

[Dockerfile.muos](../../MetroidPrimePort/Dockerfile.muos) и preset `muos-linux-arm64` дают:

- Linux/arm64 Debian Bookworm; не macOS executable и не Jacaranda SDK;
- base image по digest и apt snapshot `20261104T212339Z`;
- Clang 16, Rust 1.90.0, Rustup 1.28.2;
- `Release`, `-mcpu=cortex-a53`;
- shared SDL3 fork, vendor/static nod, без test targets в game build;
- SDL2/EGL/GLES development packages для сборки и probes.

Это существенно лучше R: source provenance, подходящая архитектура и более старая, чем firmware, glibc 2.36. Исторически game build прошёл 1297 Ninja steps, container/device `--version` и on-device dependency resolution были успешны. Это результат старого донора, не U.

**Оценка:** пригодная основа повторяемого build environment, но не hermetic или bit-for-bit build. Rustup checksum скачивается рядом с bootstrap, fork archive не имеет фиксированного `URL_HASH`, nod берётся Git tag, не все транзитивные inputs закреплены. В Dockerfile установлен LLD, но его использование не следует из одного наличия пакета.

Команды должны явно выбирать `--platform linux/arm64`; сам preset не делает среду ARM64. Для source context `.` нужен продуманный `.dockerignore`: `.gitignore` не является защитой от передачи private ISO/saves/build outputs удалённому builder. Наличие Dockerfile без `COPY` не стоит превращать в универсальную гарантию приватности build context.

### 5.3. H: SDL fork как отдельный dependency profile

`AuroraSDL3Provider.cmake` вводит `AURORA_SDL3_SDL2_BACKEND` и требует vendor provider. Pin:

```text
bmdhacks/SDL, sdl2-backend
6057d79baf8321bf190479a699655f06cc2a962f
```

Библиотека и headers теперь собираются вместе. Fork headers сообщают SDL 3.5.0, обычный donor/U pin — 3.4.10. В shim-profile отключаются SDL GPU, X11, Wayland, KMSDRM: игра рисует GLES самостоятельно, системная SDL2 обслуживает display/input/audio.

**Перенести концепцию profile, адаптировать provider:** использовать U download hashes/patch infrastructure, не потерять имеющиеся SDL patches и не смешивать cached upstream/fork build directories. Уже созданный SDL target или source override может обойти fetch: проверять фактически собранный source pin, а не только значение опции.

### 5.4. H: исправление cross-toolchain не завершает его поддержку

Теперь `MP_TARGET_SYSROOT` обязателен, задаётся `CMAKE_SYSROOT`, удалены пути автора. Но не проверены полнота sysroot, target pkg-config, Rust linker, compiler/GCC runtime provenance. Пользовательская переменная не добавлена в `CMAKE_TRY_COMPILE_PLATFORM_VARIABLES`: вложенный configure может её не получить; это **риск по коду, не воспроизведённая configure-ошибка**.

Остаются legacy `RK3326` label, глобальный `$ORIGIN/libs.aarch64` RPATH и preset без включения нужного SDL2 fork. Для согласованной цели cross-build **не переносить как обязательный рабочий путь**. Поддерживать его можно позднее отдельно.

### 5.5. Dependency churn R — не требование H700

R откатывает nod `alpha.12 → alpha.10`, меняет Tracy, namespaced targets `xxHash::xxhash`/`Tracy::TracyClient`, часть pkg-config fallbacks и default shared linkage. Включение host pkg-config при cross-compiling создаёт риск host `.pc` files. Сокращается набор nod package platforms. Обоснования обязательности этих изменений для H700 не найдено.

**Сохранить инфраструктуру U**: bundled-dependency policy, namespaced targets, SHA-256 downloads, поддерживаемые desktop/mobile платформы и новые dependency versions. `MP_BUNDLED_DEPS` не означает полностью static executable: OpenSSL/zlib/zstd/SDL и транзитивные DSO проверяются отдельно.

Не удалять Dawn provider из общего проекта ради одного H700 profile. Это одновременно затронет другие платформы и сделает будущие upstream merges значительно сложнее.

### 5.6. libc compatibility и signed char

R добавляет AArch64 binding `hypot@GLIBC_2.17` вместо x86 baseline `GLIBC_2.2.5`; hidden wrapper предотвращает неверное разрешение symbol reference. **Уже есть в U** в [platform/glibc_compat.c](../platform/glibc_compat.c).

Также U уже применяет `-fsigned-char` к game, MusyX и executable. Для ARM это важно: `-1` sentinels не должны превращаться в 255. Это исходное условие корректности, не новая оптимизация H.

R расширяет forced include `platform/compat.h` на platform sources и вручную объявляет `CARDFormatAsync`. C/C++ guard помогает компиляции C, но blanket forced include скрывает реальные header dependencies. В U `CARDFormatAsync` уже объявлен SDK header. **Не переносить расширение автоматически.**

Исторические ABI требования донора: до `GLIBC_2.34`, `GLIBCXX_3.4.30`, `CXXABI_1.3.13`. Свежая firmware имеет более новые соответствующие version nodes, но U artifacts ещё не построены. Проверять executable, bundled SDL3, interpreter, `NEEDED`, transitive libraries и firmware SDL2 через `dlopen`. `--version` не проверяет video/input/audio.

## 6. R: архитектура прямого GLES renderer

### 6.1. Это замена, а не дополнительный backend

R удаляет Dawn bindings/provider и `webgpu/gpu.cpp`, `gpu_cache.cpp`, `wgpu.hpp`. Добавляет `extern/aurora/lib/gl/`: context, loader, device, buffers/textures, pass/state, FBO, program/binary caches.

Исторический namespace `aurora::webgpu` местами остаётся, но за ним работает GLES. По одному имени файла/namespace нельзя заключать, что WebGPU всё ещё участвует.

Активный graphics core собирается из нового `gfx/common.cpp`. Старые `frame.cpp`, `recording.cpp`, `encoding.cpp`, `gx/texture.cpp` остаются в дереве, но исключены из активного `aurora_gx.cmake`; `regs`/`resident` и часть resource/effects кода удалены либо исключены. Не оценивать runtime по оставшемуся, но не собираемому файлу.

API `gfx.hpp` становится GL-specific: integer handles, `gl::PassEncoder`, GL tasks вместо WebGPU encoder/tasks, сокращённый render-target contract. За validation, capabilities, lifetime и synchronization теперь отвечает Aurora, а не Dawn.

**Оценка:** потенциально меньше driver/API overhead и подходящий firmware integration, но масштаб новой ответственности велик. Для U это не drop-in replacement.

### 6.2. Вывод на H700 уже решён; открыт способ интеграции с U

**Неопределённость не в том, умеем ли мы выводить изображение на H700.** R создал direct GLES/SDL2/EGL механизм, а H довёл этот вариант до запуска и проверил на целевой muOS. Повторно искать display path с нуля не требуется. Открыты способ и объём его интеграции с новой архитектурой: адаптировать старый renderer к новой игре либо сохранить renderer U и подключить его к проверенному платформенному пути. При любом выборе остаются проверки корректности и отдельная задача достижения 30 FPS.

U переводит `BACKEND_OPENGLES` в Dawn `wgpu::BackendType::OpenGLES`; это не независимый renderer R. Linux surface binding U ориентирован на X11/Wayland properties. Проверенный firmware SDL2 borrowed-EGL/EGLImage path там отсутствует.

При этом U уже запрашивает `wgpu::FeatureLevel::Compatibility` и адаптирует требования к adapter limits:

- texture vertex fetch заменяет vertex-stage storage buffers;
- depth peek и volumetric fog имеют render-pass fallbacks без compute;
- BC/ASTC и другие optional features запрашиваются по наличию у adapter.

Поэтому отсутствие vertex SSBO или compute само по себе не обосновывает отказ от U renderer. Эти fallbacks **не доказывают** совместимость Dawn с firmware Mali, но их надо отделять от проблемы отсутствующего display/context integration. Конкретный код — `webgpu/gpu.cpp`, adapter selection и блок required limits/features.

Отсюда два предмета будущего решения:

- адаптация direct GLES R с сохранением необходимых GX/resource contracts U;
- исследование возможности интеграции современной графики с firmware EGL/display path.

Этот обзор **не доказывает** невозможность второго варианта и не требует реализовать оба. Нужно выбрать менее затратную и более надёжную интеграцию, а не заново доказывать возможность вывода на устройстве. Для выбора сравнить конкретные требования Dawn, контекстов, GLES limits и surface ownership; «ES 3.2 есть» недостаточно.

### 6.3. Что удалено или ослаблено

| Подсистема | Поведение R/H | Оценка |
| --- | --- | --- |
| Enhanced effects | Удалены реализации bloom/volfog/shadow/probe/vfx/water | Допустимо не включать Remastered, но нельзя автоматически считать сохранёнными все штатные GX effects |
| PBR/MRT/HDR | Удалён расширенный shader/API path | Для первого этапа необязательно; не уничтожать общую U-инфраструктуру |
| MSAA | Single-sample path | Для 640×480/30 FPS разумная база; setting должен честно отражать отсутствие MSAA |
| Depth peek/snapshots | Отключены/заглушены части depth paths | Проверить реальные игровые consumers; не объявлять аппаратно невозможным |
| Неподдержанный vertex layout | Draw может отбрасываться | Неприемлемая скрытая потеря оригинальной геометрии |
| Lines/points | Hardware primitives вместо прежней expansion | Проверить width, offsets, near clipping и UBO layout |
| Replacement streaming | Удалены части async/mip-tail loading | Возможны blocking loads и spikes даже без HD packs |
| GPU timing/selftest | Stubs/no-op | Старый API не даёт реального измерения/проверки |
| Screenshot | Platform `request_screenshot()` stub | Наличие API не означает capture support |
| Texture/debug/stat APIs | Часть no-op | Output arguments могут остаться незаполненными |
| Window lifecycle | Удалены части surface-changing/release handshake Android | Не нужно H700, но регрессия других платформ при замене общего кода |
| RmlUi | GL texture/pass adaptation, удаление части filters | Нужна проверка clipping/stencil/composition, не только успешная компиляция |

Удаление enhanced `water.cpp` или `shadow.cpp` **не доказывает** исчезновение всей воды/теней retail. Часть оригинальной картинки выражается обычными GX draws/TEV/EFB copies. Но противоположное утверждение — «все штатные эффекты точно сохранены» — тоже не подтверждено.

## 7. EGL/SDL2 presentation bridge

Основные донорские файлы: `lib/gl/context.cpp`, `lib/gl/device.cpp`, [webgpu/sdl2shim_present.cpp](../../MetroidPrimePort/extern/aurora/lib/webgpu/sdl2shim_present.cpp), `lib/aurora.cpp`.

### 7.1. Context ownership и путь кадра

1. `SDL_VIDEODRIVER=sdl2` выбирает backend внутри SDL3 shim.
2. `SDL3SHIM_SDL2_VIDEODRIVER=mali` выбирает firmware SDL2 driver.
3. `SDL_GL_CreateContext` выполняет bootstrap firmware EGL context.
4. Main thread сохраняет borrowed display/present context.
5. Raw EGL создаёт shared render/compiler contexts, с pbuffer surfaces.
6. Создаются два RGBA8 slot textures и EGLImages; render worker импортирует storage в aliases/FBO.
7. Worker композитит кадр в slot; main thread делает blit в default framebuffer и swap.
8. Forward/reverse fences должны исключать чтение незаконченного кадра и повторную запись занятого slot.

Это GPU image interop, **не CPU readback**, но и не прямой scanout scene texture: есть composite/blit work.

Fork не публикует ожидаемые Aurora properties `SDL.window.sdl2_backend.{egl_display,egl_context,gl_get_proc,window,gl_swap_window}`. Донорский путь предусматривает fallback через SDL proc lookup/current EGL display/context; исторический H probe подтвердил этот вариант. Нельзя проектировать перенос так, будто все properties у выбранного fork существуют.

### 7.2. Сильные стороны

- Ограниченная очередь из двух slots вместо бесконтрольного накопления кадров.
- Разделение render и main-thread present соответствует firmware SDL display ownership.
- EGLImage избегает CPU copy всей картинки.
- Fences в обе стороны и flush учитывают межконтекстную GPU работу.
- FBO cache invalidation при удалении textures.
- `gpu_synchronize()` учитывает pending present slot, уменьшая риск взаимного ожидания.

Это **основной кандидат на адаптацию**.

### 7.3. Незаконченные условия корректности

**Fence availability.** Инициализация проверяет не весь необходимый contract. При отсутствии create/wait entrypoints либо неудаче создания fence код может продолжить публикацию без гарантированного ordering. EGL 1.4 не доказывает наличие нужных KHR extensions. Нужны либо fail-fast, либо явно безопасный fallback.

**Timeout-and-reuse.** При reverse-fence timeout/error worker логирует проблему и продолжает запись. Это не доказанно безопасное восстановление; presenter/GPU ещё может читать тот же image. Аналогичный компромисс есть при reuse persistent buffer slots. Нельзя описывать его как гарантированно безвредный один torn frame.

**Resize.** `sdl2shim_present::resize()` есть, но связного вызова из исследованного R device-resize path не найдено. EFB/present dimensions могут обновиться без slots. На фиксированной панели это может быть скрыто до surface recreation.

**Vsync/frame pacing.** Context setup задаёт swap interval 0; `aurora_enable_vsync()` меняет сохранённый mode, но не видно применения нового interval; pacing call в `common.cpp` закомментирован. Фактический swap может блокироваться firmware независимо от этого. UI setting не считать выполненным API contract.

**Compiler context fallback.** Без share context обещана inline compilation на render worker, но callbacks/`end_pipeline_frame()` могут выполняться на recording thread. Неудача `make_share_current()` также только логируется. Для fallback нужно гарантировать context-owning thread, а не продолжать GL calls без соответствующего context.

**Что ещё проверить активно:** multi-thread current contexts, resize/recreate/shutdown, sustained fences, format compatibility, error paths. Старый single-thread probe с pbuffer не покрывает всю эту топологию.

## 8. R: оптимизации CPU/GPU и их цена

### 8.1. Native vertex attributes

`gx/command_processor.cpp::build_native_layout` распаковывает GX attributes в CPU canonical layout. Matrix indices передаются integer attributes; transforms остаются в shader. Это заменяет shader-side storage-buffer fetch, полезно при слабом/недоступном vertex SSBO path.

Потенциальный выигрыш — меньше сложных vertex loads и повторных GPU uploads. Цена — CPU expansion, hashing, storage и invalidation. Ограничение native attributes не является доказательством отсутствия compute или всех SSBO возможностей у Mali-G31.

В U уже есть texture vertex fetch fallback для adapters без vertex storage buffers и resident geometry. Поэтому альтернатива не «R optimization против полного отсутствия оптимизаций», а разные механизмы с разными costs.

### 8.2. Geometry cache и batching

R добавляет content-addressed native geometry cache: FIFO/layout/array hashes, recurrence gate, GPU vertex/index storage, generations, reset. Соседние совместимые draws объединяются; triangle strips сохраняются с primitive restart.

Часть batching/DL/resident механизмов уже существовала в P. Нельзя приписывать R всё уменьшение draw overhead только потому, что соответствующий файл сильно изменился.

**Риск:** cache key должен учитывать каждый mutation source. В `native_array_content_hash()` поле `memo.invalGen` присваивается, но условие пересчёта сравнивает только frame/size/new entry. После переписывания array по прежнему адресу/размеру и `GXInvalidateVtxCache` внутри одного кадра hash может остаться старым. Видимая stale geometry требует hit подходящей cache entry; при miss expansion может прочитать актуальные данные.

Это **статический дефект высокой уверенности в invalidation contract**, но не воспроизведённый источник конкретного игрового артефакта. H его не исправляет. В U другой mechanism очищает frame-array cache при invalidate/clear; переносить R memo буквально нельзя.

### 8.3. State versions и uniform reuse

Разделены pipeline/texture/uniform versions, повторно используются pipeline/bind/uniform data. Для unlit layouts убрана normal palette: десять `Mat3x4<float>`, то есть 480 байт до alignment на соответствующий block.

Это хорошие идеи, но выгода зависит от hit rate. Каждый setter должен обновлять правильную generation/version; cache без полного mutation audit — источник тихих ошибок. В U normals могут требоваться PBR/emboss/другим paths, поэтому условие R нельзя переносить глобально.

### 8.4. Persistent coherent buffers

При наличии buffer-storage extension используются persistent coherent mappings, иначе `glBufferSubData`. Это сокращает некоторые driver-side copies, но CPU recording buffers и `memcpy` остаются. Два sets и fences нужны до reuse.

Availability и реальная выгода `GL_EXT_buffer_storage` на firmware H700 свежим probe не проверены. Не делать extension обязательным без fallback. Наличие `GL_DYNAMIC_STORAGE_BIT` важно: после failed map `BufferSubData` остаётся допустимым для immutable storage.

### 8.5. Program sharing и binary cache

GLSL programs дедуплицируются отдельно от fixed-function pipeline state. Disk key учитывает source и fingerprint driver/vendor/renderer/version/GLSL/binary formats. Rejected binary возвращает source compile; есть writer thread и crash sentinel.

**Оценка:** полезно для shader stutter. Но startup cap 48 MiB не является жёстким runtime bound: `binary_cache_store()` добавляет entries, linked programs живут долго. Нужны runtime eviction/budget и разделение cache по driver/source revision. Перенос U pipeline-cache API в GL должен сохранять смысл, не просто имена.

### 8.6. Удаление FIFO worker

R заменяет отдельный FIFO-processing thread накоплением и синхронным `drain()`; decoder work переносится на game/recording thread. Может уменьшить synchronization overhead, но теряет overlap, создаёт end-frame spikes и усиливает lifetime requirements.

На четырёх Cortex-A53 это **измеряемый trade-off, не очевидная оптимизация**. U уже имеет FIFO ordering/concurrency semantics и tests. Не удалять поток без сравнительного профиля main/worker wall time, CPU time и waits.

## 9. Память: донор не является автоматически малопамятным

### 9.1. Расчёт по константам, не измерение RSS

`gfx/common.hpp` и `common.cpp` задают на packet 5 MiB vertices + 24 MiB uniforms + 2 MiB indices = 31 MiB. Для двух slots:

- CPU reserves — около 62 MiB;
- соответствующие GPU buffers — ещё около 62 MiB;
- native geometry cache 24+4 MiB — ещё 28 MiB.

Итого порядка **152 MiB предусмотренного storage/reserves** до texture uploads, EFB/depth/UI/present textures, decoded textures, game heap, driver programs и registry overhead. На UMA это конкурирует за общую физическую RAM, но reserve/virtual allocation не следует выдавать за уже измеренную resident memory.

Replacement cache nominal budget R/H — **4 GiB**, что не защищает устройство с 1 GB. Age-based texture sweep полезен против старых object IDs, но не заменяет byte budget всех holders.

U уже имеет `MP_TEXTURE_MAX_DIM`, `MP_TEXTURE_CACHE_MB`, pressure-aware policy и учёт новых published textures. Эти механизмы лучше сохранить, даже если на первом этапе нет HD packs: replacements используются и для port prompt icons.

### 9.2. Опасный overflow contract

CPU `ByteBuffer` способен вырасти через `realloc`, а upload за пределами GPU capacity в активном R core обрезается. Draw offsets остаются прежними. Получается не контролируемая ошибка allocation, а чтение неполного/stale range.

Отдельно texture-upload buffer может расти после mid-frame submission, когда worker уже читает его storage. Без lifetime guarantee такой рост требует проверки на гонку/reallocation. В U есть bounds/spill/growth handling; его нельзя потерять при переносе.

**Решение:** не переносить fixed capacities и silent truncation как рабочую memory policy. Нужны измеряемые high-water marks и явные failure paths; конкретный бюджет нельзя вывести из одной строки `MemTotal`.

## 10. Graphics correctness: дефекты, не устранённые H

Это findings по source paths, **не новые on-device воспроизведения**. Высокая уверенность в ошибке конкретной ветки не означает, что она часто используется retail MP.

| Finding | Причина и условия | Решение |
| --- | --- | --- |
| Lines/points UBO layout | `build_uniform()` вставляет 16 байт line fields перед projection при `lineMode != 0`, GLSL block их не объявляет. Native draw сохраняет lineMode | Проверить shared layout/offsets и generated shader; U WGSL содержит нужные поля |
| TEV `RGB8_GT` color | Emitter генерирует `>` между `vec3`; обычный GLSL relational operator не поддерживает vector operands | Vector relational + component-wise selection; нужен generated-shader compile test |
| TEV `RGB8_EQ` color | Vector `==` даёт один bool, GX требует покомпонентный RGB выбор | Проверить разные результаты по каналам. Scalar alpha-ветка этим finding не затронута |
| PC R8/RG8 swizzle | Capability объявлена true, direct upload включён, texture-object swizzle не задаётся | Для `GX_TF_R8_PC/RG8_PC` нужны `(R,R,R,R)` / `(R,R,R,G)` либо эквивалент shader/CPU conversion |
| BGRA | Mapping BGRA storage/upload к RGBA без явной перестановки | Проверить encoded DDS color channels после allocation fix |
| Unsupported native layout | NBT/NBT3, invalid arrays, слишком много attrs и др. могут приводить к drop draw, storage fallback выключен | Не скрывать geometry loss; inventory реальных layouts и корректный fallback |
| Array invalidation | Generation не участвует в условии пересчёта memo | Перенести инвариант из U, не ошибочный cache |
| Allocation/FBO errors | Integer handle/FBO cache entry может сохраняться после GL allocation/incomplete FBO error | Явная ошибка и fallback, не только log |
| Custom callbacks | Raw GL state changes могут обойти shadow cache | Encoder-only contract либо полная нужная invalidation |
| Stencil | Независимый stencil clear не полностью выражен вне depth-clear branch | Проверить UI masks/clipping и render-pass semantics |

R8/RG8 finding относится к **PC direct-upload formats**, не автоматически ко всем original I8/IA8 textures. BGRA и некоторые compressed variants могут не использоваться выбранными original assets — reachability нужно измерить, а не объявлять каждый такой дефект release blocker до проверки.

Также BC support нельзя сводить к одному S3TC flag: BC4/5/7 требуют других capabilities. ES3.2 string не подтверждает каждый compressed format. U сохраняет более широкий decode/format path; не откатывать его по одной hardware family label.

## 11. H: исправления rendering и regression tests

### 11.1. Draw-sync и skinning workspace

[GXManage.cpp](../../MetroidPrimePort/extern/aurora/lib/dolphin/gx/GXManage.cpp), `GXSetDrawSync()`; потребитель — `DolphinCSkinnedModel::TickAllocations/PostDrawFunc`.

**Регрессия R:** token возвращался немедленно, хотя FIFO ещё ссылался на CPU workspace. Игра считала allocation свободной, перезаписывала её до decode — возможна взорванная skinned geometry.

**H:** перед публикацией token вызывает `fifo::drain()`. В синхронной архитектуре донора это корректный CPU lifetime barrier: decoder читает/копирует vertices до reuse. Это **не GPU completion fence**.

Исторически shape inventory Samus исправился. Отдельного автоматического test «draw со ссылкой → token → mutation workspace → проверка copied geometry» коммит не добавляет. Возможная цена — частые drains; режим display-list recording и межпоточная публикация static token остаются слабее полноценной FIFO semantics.

**U:** `GXSetDrawSync` кодирует `GX_AURORA_SET_DRAW_SYNC` в FIFO, token публикуется при обработке, atomic release/acquire. Есть concurrency test `DrawSyncSurvivesConcurrentAppendAndBufferGrowth`. **Старый drain/static token не переносить.** Полезно добавить lifetime scenario к новым tests, если потребуется GL/FIFO adaptation.

### 11.2. XF viewport/projection: групповые регистры

[command_processor.cpp](../../MetroidPrimePort/extern/aurora/lib/gx/command_processor.cpp), обработка XF.

**Регрессия R:** scalar deduplication могла пропустить первое неизменившееся слово блока. Если viewport width/`sx` тот же, а depth parameters изменились, эффективный viewport не пересобирался. Аналогично projection при неизменном `p0`. Был также неверный offset при чтении группы внутри большего packet.

**H:** обновляет shadow всех изменённых слов; изменение любого слова отмечает группу; viewport/projection пересобираются из валидного полного shadow. Поддерживаются partial updates после инициализации и offset packets.

Пять новых tests проверяют:

- последовательные viewport с одинаковым первым словом;
- projection с одинаковым `p0`;
- partial viewport depth write;
- partial projection parameter/type write;
- groups со смещением, включая неизменившийся packet.

Состояние между submissions не сбрасывается — это принципиально для воспроизведения dedupe bug. Исторически corridor/deck geometry заметно исправилась; не утверждается полная scene parity.

**Ограничения:** CPU harness использует фиксированный render target и тождественный viewport mapping, не GPU depth/occlusion. `GX_AURORA_LOAD_PROJECTION_FULL` в доноре не инвалидирует XF shadow: sequence A → full B → identical XF A может сохранить B; это остаточный статический риск, не выполненный здесь test.

**U:** logic вынесена в [gx/regs.cpp](../extern/aurora/lib/gx/regs.cpp). Полный viewport применяется с учётом текущего target, projection дедуплицируется по всему блоку, packet offset корректен, full-projection handler инвалидирует shadow. Но partial blocks `0x1A…0x26` явно игнорируются. **Переносить tests/инварианты, не старый handler целиком**; необходимость partial-write support решать по игровым consumers и полноте GX contract.

### 11.3. Encoded replacement texture: реальное GPU storage

[texture_replacement.cpp](../../MetroidPrimePort/extern/aurora/lib/gfx/texture_replacement.cpp), `create_converted_texture_handle()`.

**R:** создавал непустой `TextureRef` с GL texture `id=0`; replacement считался загруженным, но replay пропускал upload. Это объясняет чёрный rectangle prompt, а не ошибку controller mapping.

**H:** вызывает `gfx::create_gl_texture()` до `write_texture()`, использует существующий worker-owned allocator, возвращает empty handle при нулевом ID. Формат, размеры, mip count сохраняются.

Три CPU tests проверяют DDS decode/mip uploads, allocation failure без публикации handle и alpha bytes RGBA icon. Allocator/upload queue заглушены: реальный GL sampling/FBO не проверяется. Исторический capture и пользователь подтвердили исправленный LB icon.

**Оценка:** необходимое завершение GL path, не Mali-specific workaround. `id != 0` не доказывает успех `TexStorage2D`; некоторые failure paths могут повторять decode/allocation, не попадая в failed-ID cache.

**U:** уже создаёт реальную WebGPU texture/view, имеет streaming/mip-tail tests. **Не переносить старый texture subsystem; сохранить инвариант resource-before-publication при любом новом GL backend.** Original assets не исключают эту задачу: prompt replacements используются без HD pack.

### 11.4. Index buffers и VAO state

[gl/buffers.cpp](../../MetroidPrimePort/extern/aurora/lib/gl/buffers.cpp), create/upload/destroy.

**R:** storage operations bind index buffer через `GL_ELEMENT_ARRAY_BUFFER`, меняя EBO binding текущего VAO. Cached `VaoEntry::ibuf` этого не знает, следующий draw может пропустить rebind.

**H:** выполняет storage/upload/unmap через scratch `GL_ARRAY_BUFFER`, сохраняя logical buffer target. Это корректно: buffer storage не обязан выделяться через draw target. Побочный ARRAY binding надо учитывать в callers, но VAO element binding больше не повреждается.

Это общий GL state bug; исторический A/B **не исправил** corridor artifact сам по себе. Нет отдельного VAO/EBO regression test. В U такого GL helper нет. **Перенести, если используется этот direct GL layer**, не внедрять в unrelated Dawn resource code.

## 12. Карты памяти, IO и SDK: наиболее нежелательные откаты R

### 12.1. GCI-folder backend

`card/CardGciFolder.cpp`, `openFile/createFile/deleteFile/fileRead/fileWrite/seek/open`.

R снимает identity lookup по `(game, maker, filename)`, проверки длины/alignment/duplicates/limits, sanitization disk names и payload bounds. Загрузка перестаёт собирать временное валидированное состояние. Повторный `open()` может дополнить старый список.

`deleteFile()` сначала открывает файл с truncation, затем удаляет без полноценной проверки; стирание entry из vector сдвигает номера остальных handles. При неуспешном удалении содержимое уже может быть потеряно. `seek(End)` смешивает GCI header и payload.

**Почему не переносить:** это не обязательное ускорение для H700. Цена ошибок — повреждение saves, а не только visual glitch. U сохраняет identity-aware lookup, stable slots/tombstones, limits и безопасные filenames. H эти откаты не устраняет.

### 12.2. Raw cards и atomic updates

R ослабляет block-chain/range checks, допускает округление и раннее сужение размеров, удаляет destination при rename вместо `EXIST`, заменяет atomic data+metadata update последовательными writes. `format/commit` теряют возвращаемые ошибки, dirty state может очищаться после failed write.

`card/FileIO.cpp::fileWrite` меняется с atomic `UpdateExisting` на `r+b` с fallback `w+b`; исчезнувший destination может быть создан заново неполным. В IO API удаляется `AtomicFileMode::UpdateExisting`. При этом atomic writer целиком **не удалён**: H использует оставшийся helper для sidecar.

`dolphin/card.cpp` допускает безусловный READY GCI, слабую mount/commit propagation и автоформатирование Slot A, если никакая карта не загрузилась, вместо условия «карта действительно отсутствует». Ошибка чтения существующей raw card не должна превращаться в разрешение форматировать её.

**Исключить эти изменения.** Сохранить U error model, atomic transactions и management APIs. Atomic rename всё равно не доказывает power-loss durability: flush/directory sync и recovery проверяются отдельно.

### 12.3. H: persistent serial sidecar

`CardGciFolder::loadOrCreateSerial/getSerial`, файл `.aurora-card-serial`:

- ровно 16 байт: magic `AURGCIS1` + big-endian ненулевой 64-bit serial;
- случайная identity создаётся через atomic writer;
- переживает reopen, rename и path aliases при сохранённом sidecar;
- исключена из GCI enumeration;
- malformed/zero/oversized metadata и dangling sidecar не заменяются молча.

**Причина:** R вернул `serial=0`, а Save Station после лечения считала карту отсутствующей и не предлагала запись. H устраняет регрессию в корне, не обходит игровой guard.

Исторические tests: 11/11 в ARM64 Docker, 9/9 non-symlink на SD1; сохранность synthetic GCI, identity, aliases, malformed files, create/write/reopen/read. Позднее подтверждён реальный цикл station prompt → write → relaunch → file select → gameplay load.

**Качество решения:** сильнее path-hash по стабильности identity при переносе каталога. Но `format()` игнорирует результат создания serial, backend `getError()`/CARD readiness остаются упрощёнными; нет полной интеграции failure в игровой error UI. Два процесса могут создать разные serial, atomic rename не даёт create-once identity. Потеря sidecar меняет identity, копия с sidecar клонирует identity.

**U:** ненулевой FNV-1a от пути уже устраняет исходный save blocker; новая error model лучше. Sidecar — **отдельное улучшение**, не обязательное повторное исправление. Если переносить, встроить в U errors и определить backup/migration policy, не заменять U `open/format/commit` старыми методами.

### 12.4. ARAM, DVD и THP

| Изменение R | Оценка и положение U |
| --- | --- |
| `ARAlloc` guards заменены `ASSERTMSGLINE`, удалены guards `ARFree`/DMA ranges | В release часть проверок исчезает; debug expressions используют несовпадающие `__AR_*` names. U сохраняет реальные bounds/direction checks. Исключить |
| Отложенные ARQ callbacks | Уже были в P; это не новое достижение R. Нужны игре, сохраняются в U |
| `aurora_dvd_base_*`, read-error callback удалены/заглушены | Потеря overlay/import/base-image APIs и error reporting. Для H700 не требуется. Сохранить U |
| THP локальный unbounded `ByteReader` shim | R заменяет общий abstraction, но P уже вызывал unbounded decode: нельзя приписывать R первое появление всех truncated-input рисков |
| Bounded THP U | Есть bounded API и movie-player call sites, размеры input/output planes проверяются. Legacy unbounded API не становится безопасным автоматически. Не откатывать U |
| POSIX fallback времени | `gmtime_r/timegm/mktime` вместо недоступных частей C++20 calendar/timezone; полезно старому toolchain, но DST ambiguity и portability остаются. Адаптировать только при фактической compile-проблеме U |
| `fs_helper.hpp` и мелкие include/capture rewrites | Build compatibility, не ускорение H700; не переносить уже ненужные локальные shims поверх новых IO helpers |

## 13. Управление, UI, звук и platform changes

### 13.1. Input R: handheld fallback ценой потери контрактов

`input.cpp`, `input.hpp`, `dolphin/pad/pad.cpp`, `include/dolphin/pad.h`.

R добавляет software-assigned player index и fallback «первый controller = player 0», что помогает shim/device, не предоставляющему ожидаемый SDL player index. Но одновременно удаляется часть real-controller preference, virtual-controller policy и безопасного чтения mapping files.

`PAD_NATIVE_BUTTON_TRIGGER_LEFT/RIGHT` меняют numeric IDs; multi-slot keyboard API превращается в wrappers, игнорирующие slot, хотя `PAD_KEY_SLOT_COUNT` объявлен 4. Это не эквивалентная реализация нескольких bindings. Удалены части аналогового trigger-as-button handling; mapping loader теряет exact-read/RAII checks и может частично применить повреждённые данные. Добавление left shoulder к L может быть полезно digital-trigger handheld, но mapping конкретной модели не следует задавать глобально для всех pads.

**Вывод:** взять требование корректного первого controller на muOS, не заменять input U. В U уже есть controller preference и современный action/mapping layer, precise deadzones и tests. GUID/оси/deadzones/rumble встроенного `muOS-Keys` проверять отдельно на устройстве; имя не является полной сертификацией.

### 13.2. Launcher mapping и конфликт выхода

R вшивает GO-Super mappings, H заменяет их firmware `/usr/lib/gamecontrollerdb.txt`. Это разумно для данной muOS; SDL2 raw mapping, SDL3 shim semantics и game action bindings — три разных слоя.

R выходит по Back+Start/Guide/Escape. В U **Back+Start уже открывает PortMenu**, поддерживает оба порядка нажатия и подавляет обычный Start. Слепой перенос donor exit handler отнимет меню. Для RG40XX H предпочтительно сохранить menu → Quit, но окончательную exit policy согласовать при реализации.

`metroid_prime.gptk` не является действующим решением: bindings преимущественно пусты, запуск gptokeyb не включён, H staging файл не копирует. Нельзя обещать exit combo на основании наличия `.gptk`.

### 13.3. Звук R: attenuation не равно исправлению backend

Изменения:

- MusyX и AI DMA streams получают `SDL_SetAudioStreamGain(..., 0.75f)`;
- game defaults SFX/music снижаются с 127 до 90;
- startup меняет system/default volume scale;
- `sfx_volume/music_volume` в port settings переопределяют значения saves;
- movie audio учитывает port override.

Вероятное намерение — уменьшить громкость/перегруз и сохранять настройки вне game save. Однако это несколько уровней attenuation одновременно, меняется баланс/поведение настроек; часть default changes сделана даже вне `TARGET_PC` guards. Нет измерения clipping, подтверждающего необходимость именно коэффициентов 0.75/90.

**Исключить как обязательный H700 fix.** U уже имеет управляемые music/SFX boosts и другую audio settings policy. Сначала сохранить баланс и routing, затем проверять реальные шум/клиппинг/latency. Исправление H `PIPEWIRE_RUNTIME_DIR` относится к доступу к audio service, а не к громкости.

### 13.4. Остальные platform changes R

- `SDL_HINT_NO_SIGNAL_HANDLERS=1` после установки собственного crash handler — разумное требование ownership сигналов, но проверить текущее поведение U/SDL fork, не переносить порядок вслепую.
- AArch64 LR/SP/FP dump помогает symbolication. Но ручное разыменование FP chain внутри signal handler может повторно fault; не гарантированы frame pointers и валидность стека. Guard `__aarch64__` сам по себе не различает Linux и Darwin layout `ucontext`. Это debug code, не release prerequisite.
- `fprintf` в CMainFlow/audio startup — диагностические вставки, не оптимизации. Если нужны, использовать текущую logging policy.
- Изменения lambda capture/structured binding и добавленные includes — compiler compatibility, проверять необходимость на выбранном Clang16 и новом source, не переносить механически.
- `GXShims.h`, `sdk_stubs.cpp` вводят no-op PBR/lighting/resident/depth/VFX/water/screenshot/card management APIs. Они позволяют линковку, но не выполняют contract. Не превращать успешную сборку в доказательство поддержки функции.

## 14. Launcher и упаковка

### 14.1. R launcher: что не подходит H700

[Metroid Prime.sh](../../MetroidPrimePort/portmaster/Metroid%20Prime.sh) в ревизии R использует `/roms/ports`, `sudo killall -9`, fixed `/tmp` Mali directory, подбор blob и symlinks EGL/GLES/GBM, KMSDRM, performance governors CPU/GPU/DMC, ALSA route и unlimited core dumps.

Для заданной muOS это неверные или опасные assumptions: нет DRI, другой Mali stack, root не требует sudo, global kill обходит cleanup, governor не восстанавливается, blob selection по имени не проверяет ABI. **Не переносить.** Не нужны также старые deploy/debug credentials и отключённая SSH host verification.

### 14.2. H launcher: хорошая минимальная основа

H определяет `metroid_prime/` относительно launcher, допускает standalone mode без полного PortMaster, проверяет executable и bundled SDL3, не меняет firmware/clocks, сохраняет exit code и использует:

| Переменная | Назначение |
| --- | --- |
| `SDL_VIDEODRIVER=sdl2` | Внешний SDL3 backend |
| `SDL3SHIM_SDL2_VIDEODRIVER=mali` | Внутренняя firmware SDL2; override сохранён |
| `LD_LIBRARY_PATH` | Game directory перед inherited path, без нового пустого компонента |
| `PIPEWIRE_RUNTIME_DIR=/run` | Только при существующем socket и незаданном значении |
| `SDL_GAMECONTROLLERCONFIG_FILE` | Firmware DB при наличии |
| `MP_ASPECT=4:3` | Целевой экран |
| `MP_OPENGLES=1` | Default, с override |
| `MP_UPDATE_CHECK=0` | Default без необязательной сети |

**Адаптировать для U.** Учитывать, что `MP_OPENGLES` в двух архитектурах выбирает не один и тот же implementation.

Ограничения H launcher:

- два readable PortMaster-файла ещё не гарантируют successful sourcing/functions; signal cleanup trap отсутствует;
- inherited `LD_PRELOAD/LD_LIBRARY_PATH` сохранены; старые `.so` рядом с игрой способны изменить запуск;
- overrides непоследовательны: aspect/video driver принудительные, GL choice изменяемый;
- `-f` disc path не означает readability, первый glob не означает корректный поддерживаемый образ;
- secondary path case/layout задан конкретно; relative `MP_DISC` трактуется после `cd`;
- `tee log.txt` перезаписывает лог; private paths могут попасть в него;
- standalone layout — договорённость нашего launcher, не универсальное API muOS.

Это не повод усложнять первый launcher autodetection всех прошивок. Нужно явно обозначить supported profile и errors, без скрытых глобальных исправлений ОС.

### 14.3. H staging и metadata

[package_muos.sh](../../MetroidPrimePort/tools/package_muos.sh) создаёт новый staging, копирует allowlist executable/SDL3/notices, проверяет AArch64, пишет ELF report, отказывается перезаписывать staging. Не копирует ISO/saves и не выполняет deploy. Это хорошие ограничения.

Но это **не release gate**: version-info не снимается со всей dependency closure, `ldd` выполняется в container, ошибка частично подавляется, SDL2 `dlopen` не покрыт. `port.json/.gptk` не включены в staging; package manifest/digests и полный комплект licenses не обеспечены.

U имеет [tools/collect_licenses.sh](../tools/collect_licenses.sh); использовать его инфраструктуру вместо трёх donor notices как якобы полного списка.

R `port.json` обещал glibc 2.31 без ABI доказательства, H правильно убирает это поле. Однако old disc metadata/checker ограничивают образ `GM8E01`, disc 0, revision 0. U принимает USA 1.00/1.01/1.02 и PAL experimental. **Различать upstream support и H700 tested matrix**, не урезать acceptance U из-за одного исторического test image.

### 14.4. Старые deploy/debug scripts

`deploy_rk3326.sh`, `read_core.sh`, `run_core_dump.sh`, `run_gdb_catch.sh`, `run_gdb_remote.sh` не стали H700-safe после H. В них есть private credentials/host defaults, `sshpass`, отключённая host-key verification, hardcoded paths, KMSDRM/Mali injection, core deletion/collection и даже автоматическая отправка private ISO. Значения credentials здесь намеренно не воспроизводятся.

**Исключить целиком.** Если позже понадобится deploy, определить новый allowlist artifact flow, explicit host/path, host verification, сохранение `user/`, checksums/rollback, без игровых assets по умолчанию. Для первого ручного Ports install эти scripts не нужны.

## 15. H: диагностика и качество тестового покрытия

| Инструмент/тест | Что даёт | Чего не доказывает |
| --- | --- | --- |
| `muos_diagnose.sh` | Kernel/libc/RAM/nodes/library/input inventory, optional timed tools | Не capabilities dump без context; сам создаёт файл. Output creation не атомарно относительно symlink/race |
| `muos_graphics_probe.c` | Firmware SDL2 backend, ES3 context, flags, clear/swap | Не multi-context game renderer, не sustained correctness |
| `muos_sdl3_probe.c` | Shim bootstrap, shared pbuffer, EGLImage alias, complete FBO | Contexts переключаются в одном потоке; нет полноценного cross-thread fence/pixel visibility stress |
| `muos_profile.py` | Bounded `/proc`/sysfs CPU/thread/RSS/IO/GPU-estimate sampling, PID-reuse check | Не GPU frametime, continuous FPS или исчерпывающий throttling test |
| `check_disc_header.py` | Первые 8 байт raw ISO/GCM | Не hash/integrity/RVZ decoding; old revision whitelist не соответствует всей U support matrix |
| `MP_FPS_OVERLAY` | Видимый software-presentation rolling FPS без F1/клавиатуры | Не physical scanout, simulation TPS или interval average |
| XF tests | Group updates, dedupe/partial/offset cases | Не GL depth/occlusion и target resize |
| Texture allocation tests | Resource-before-upload, mips/alpha/failure | Не real GPU allocation/sampling |
| GCI tests | Identity/metadata/preservation/filesystem round trips | Не power loss, disk full, concurrency и полная CARD error UI |

`muos_profile.py`: 100% CPU означает одно ядро, не весь SoC; auto-PID — эвристика. GPU debugfs counter — driver estimate «since last show», его чтение может менять интервал. Отсутствие IO counters нельзя безусловно называть нулевым IO. При преждевременном выходе процесса возможна потеря незаписанных samples. Privacy review нужен даже для inventory: hostname/mount metadata и crash dumps не становятся безопасными от слова «read-only».

Test CMake/harness R/H адаптированы к GL types и stubbed allocator/buffer paths. **Не переносить GL stubs поверх Dawn tests U**: иначе можно получить зелёные tests, переставшие исполнять интересующую логику.

Исторические результаты H: пять focused XF tests проходили, полный соответствующий suite был **177/181** с четырьмя записанными ранее существовавшими failures; texture tests 3/3; card tests 11/11 в ARM64 и 9/9 non-symlink на SD1. Это не свежий прогон и не утверждение о зелёном полном suite.

## 16. Что реально известно о производительности

[MUOS_BRINGUP.md](../../MetroidPrimePort/docs/MUOS_BRINGUP.md) содержит разные этапы; ранние startup/menu observations нельзя распространять на весь game.

### 16.1. Исторические наблюдения

- Существовали живые sparse overlay captures gameplay с показаниями 33.8–39.2 FPS; отдельные scan/transition кадры отличаются. Это не time-weighted average/1% low.
- В boss sample за 20.01 s process CPU был 172% одного ядра, два busiest game threads 69.7% и 58.8%, Mali utility thread 29.1%.
- Driver GPU utilization 38–65%, median 42%; GPU 420 MHz; CPU 1008–1416 MHz, median 1104 MHz; температура 64.0–67.4 °C.
- Minimum available RAM 392.6 MiB, major faults 0, process storage bytes 0/0 в том измерении.
- После sample screenshot в Scan Visor показывал 11.3 FPS. Сам sample был смешанным: примерно 15 s Combat, 5 s Scan. Нельзя объявлять 11.3 средним FPS всего sample или scan-only profile.
- При scale 0.5, то есть 320×240 EFB с 640×480 output, GPU utilization снизилась; post-sample combat screenshot показал 13.7 FPS. Пользователь не заметил практического улучшения за примерно пять минут. Scenes/visors/temperature различались; точный speedup из этих снимков не следует.

**Вывод:** цель 30 FPS пока не достигнута доказанно, особенно для тяжёлого боя. Снижение pixel count не стало достаточным решением. Вероятны существенные CPU submission/driver/synchronization/geometry costs, но один виновник не установлен. Неполная загрузка GPU не исключает stalls или bandwidth. Отсутствие major faults не исключает все memory-related bottlenecks.

### 16.2. Значение для переноса

Не начинать с безусловного повышения частот или повторного снижения resolution. Не складывать overlapping main/worker wall times как последовательные затраты. Нужны отдельные simulation/record/decode/replay/present/wait measurements и continuous frametime series в повторяемой сцене.

Ориентир 30 FPS соответствует примерно 33.33 ms presentation interval. Должны быть определены p95/p99/1% low, допустимые загрузочные stutters и длительность run; одно среднее FPS не описывает стабильность. Simulation retail остаётся 60 Hz: **30 FPS output не означает автоматически `sim_rate=30`**.

## 17. Какие улучшения стоит сохранить доступными

Оценки ниже — по структуре кода U, **не benchmarks H700**. Низкая стоимость не означает отсутствие изменения gameplay. Не переносить все новые graphics features в первый GL backend только ради наличия опции.

| Возможность U | Предполагаемая цена | Рекомендация |
| --- | --- | --- |
| Remapping, twin-stick, раздельная скорость осей | Небольшая CPU input logic, без дополнительных graphics passes | Сохранить, проверить удобство и mapping |
| Precise stick deadzones | Вычисления на осях, без GPU cost | Хороший кандидат для встроенных стиков; calibrate по drift, не ставить ноль вслепую |
| Toggle lock/grapple/spider, target cycle, Aim Lock-On | Логика ввода/выбора цели | Вероятно посильно; делать опционально как изменение управления |
| Lock-on free aim | Ввод/направление выстрела, возможное изменение auto-aim | Опциональный gameplay QoL, не требование retail correctness |
| Skip cutscenes/lifts, hints/map QoL | Обычно небольшая постоянная цена | Оценивать отдельно; не спутать с корректностью исходных сцен |
| Fast Morph / Spring Ball | В основном gameplay logic | Вероятно недорого, но меняет механику — не включать молча |
| Anisotropy 2× | Texture filtering/bandwidth; без нового world pass | Первый кандидат на визуальный A/B после базы; сохранять original mip semantics |
| Per-pixel lighting | Дополнительная fragment work, меняет shading | Отложить до запаса по FPS; это не бесплатное smoothing геометрии |
| Sharpening | Дополнительные samples в resample | При 640×480 → 640×480 U bypass делает его ненужным; актуален только при upscale |
| Больший FOV / widescreen | Может увеличить видимую геометрию | Widescreen не нужен панели 4:3; FOV не считать бесплатным |
| Frame/pose interpolation | History/blending CPU state; предназначена для output выше 60 | Не ускоряет цель 30 FPS; в baseline выключить |
| MSAA, sharp scan window | Multisample storage/bandwidth; scan window может повторно рисовать мир | Не включать в исходную конфигурацию |
| Дополнительные sun shadows | Каскады/проходы и geometry work | Выключить до доказанного запаса |
| Remastered/PBR/HD packs | Иная geometry/material/texture/memory нагрузка | Вне первой цели; возможные дешёвые части изучать отдельно |

U `wgpu_aniso()` не принуждает все textures к выбранной степени: request/mips/arbitrary-mip semantics сохраняются. Поэтому `anisotropy=2` не означает глобальные 2× на каждом sample. Generic «model smoothing» setting не найден; pose interpolation и per-pixel lighting — разные функции, не subdivision mesh. Найденная LUT color grading связана с Remastered room environments, а не с готовым универсальным retail color preset.

### 17.1. Почему `Original Experience` не готовый H700 preset

[platform/debug_ui/options.cpp](../platform/debug_ui/options.cpp), effective getters и input/runtime hooks:

- задаёт 1×, 4:3, FOV 55°, MSAA1, anisotropy1, отключает dynamic res/sharpen/per-pixel/interpolation и многие mods/gameplay changes;
- оставляет часть альтернативного ввода, включая twin-stick; не равен полному сбросу управления;
- принудительно включает **60 FPS cap**, даже при сохранённом `fps_cap=30`;
- не выключает `SunShadows()`; дополнительные sun shadows могут работать и с retail room lighting.

Следовательно, для профиля «оригинальные ресурсы/эффекты + выбранный controls QoL + 30 FPS» нужны явные настройки, а не один checkbox.

Кандидат baseline **для U, не готовый конфиг старого донора**: `original_experience=0`, 4:3, render scale 1, dynamic resolution off, MSAA1, anisotropy1, per-pixel lighting/sun shadows/sharp scan window/interpolation off; original helmet/visor effects сохранить. Для имеющихся U getters output cap30 выражается `frame_limit=0`, `fps_cap=30`, `sim_rate=60`, `sim_adaptive=0`. Это лишь смысл настроек по коду; конфиг на приставку не устанавливался, runtime pacing не проверен.

Штатные Thermal/X-Ray, EFB copies, прозрачность, отражения и visor effects **не переводить в категорию необязательных улучшений**. Их сохранность входит в принятую цель.

## 18. Сводная матрица решений

| Изменение / идея | Решение | Условие |
| --- | --- | --- |
| Linux ARM64 Docker / Cortex-A53 profile | Перенести | Совместить с dependencies U, pin/hash и ABI reports |
| SDL3-over-SDL2 pinned source | Адаптировать | Headers из того же source, проверить API U, patches и фактический pin |
| Firmware `sdl2 → mali` | Перенести в platform profile | Не делать глобальным default всех Linux |
| Borrowed EGL / EGLImage two-slot present | Адаптировать | Исправить extension/fence/error/lifecycle contract |
| Raw GL backend R целиком | Не переносить целиком | Архитектурное решение отдельно; сохранить необходимые U semantics |
| Native attrs / program sharing / persistent uploads | Взять идеи | Limits, correctness tests, memory budget и benchmarks |
| Content-hash geometry cache | Переосмыслить | Исправить invalidation, сравнить с U residency, eviction |
| Отказ от FIFO worker | Проверить | Только по overlap/wait profile; не откатывать ordering |
| H draw-sync drain | Уже есть лучшее в U | Сохранить FIFO command и atomic token; добавить lifetime test при необходимости |
| H XF handler | Перенести scenarios/идею | U full-block уже исправлен; partial support и invalidation отдельно |
| H encoded texture allocation | Сохранить инвариант | В U уже есть resource creation; применимо к новому GL path |
| H EBO scratch target | Перенести при direct GL | Нет соответствующего raw GL helper в U |
| H serial sidecar | Опциональная адаптация | U nonzero serial уже есть; ошибки/migration/backup policy обязательны |
| R card/IO/ARAM safety rollbacks | Исключить | Сохранить U guards/transactions/errors |
| R audio attenuation/default overrides | Исключить из обязательного переноса | Только при измеренном clipping с объяснимым коэффициентом |
| R controller-first fallback | Взять требование | Реализовать без отката input U и virtual/multi-pad policy |
| Back+Start immediate exit | Не переносить вслепую | U использует chord для PortMenu |
| H launcher и asset-free staging | Адаптировать | U paths/disc support/licenses, понятные error/env policies |
| R governors/Mali injection/KMSDRM | Исключить | Не соответствуют target; не маскировать ими проблемы |
| R deploy/debug scripts | Исключить | Credentials, assets, host verification, destructive/global operations |
| H probes/profiler/FPS overlay | Адаптировать как optional tools | Чёткие метрики и privacy; не runtime dependency игры |
| Удаление Dawn/desktop/mobile инфраструктуры | Исключить | H700 opt-in profile вместо общепроектного отката |
| Лёгкие input/QoL и anisotropy2× | Сохранить возможность | Отдельный correctness/perf A/B, без обещания результата |

## 19. Вопросы, которые обзор оставляет для планирования

Это критерии будущего решения, не автоматически утверждённый порядок работ:

1. Какая граница backend интеграции минимальна: platform/display bridge отдельно или новый direct GLES backend? Проверить реальные Dawn/EGL requirements до выбора.
2. Какие GX layouts/effects retail MP реально задействует в representative scenes, и где R currently drops/stubs работу?
3. Как сохранить U FIFO/resource/IO contracts без переноса большого пласта старого `common.cpp`?
4. Какой budget допустим по измеренным CPU/GPU allocations, peak RSS и OS headroom? Предусмотреть bounded program/geometry/texture caches.
5. Какая policy GCI identity и миграции? Первый тест новой сборки — отдельная установка с отдельным `user/`, не эксперименты на единственной карте сохранений.
6. Какие сцены фиксируют acceptance 30 FPS: тяжёлый бой, scan, Thermal/X-Ray, Morph Ball, карта, переходы/streaming и долгий run? Какие отклонения допустимы?
7. Какой compact overlay нужен: presented FPS, frametime и simulation TPS; old FPS-only label недостаточен для диагностики slow simulation.
8. Какая exit/environment policy нужна manual Ports launcher с новым input: меню, safe Quit, inherited preload/library paths, external overrides?

При миграции data paths учесть U `CardFolder()`: writable executable directory может выбирать portable `<exe>/user/` независимо от ожиданий по `MP_USER_PATH`. Одной переменной недостаточно обещать полную изоляцию card data. Ничего из существующих saves/settings при подготовке обзора не менялось.

## 20. Источники и воспроизведение аудита

### Локальные первоисточники

- [Hardware](../../MetroidPrimePort/docs/RG40XX_H_HARDWARE.md), [muOS platform](../../MetroidPrimePort/docs/MUOS_PLATFORM.md), [inventory](../../MetroidPrimePort/muos-diagnostics.txt).
- [Исторический bring-up journal](../../MetroidPrimePort/docs/MUOS_BRINGUP.md): особенно разделы graphics fixes, FPS/boss profiling и zero card serial. Начальные pending статусы не отменяют поздних подтверждений.
- [Donor presets](../../MetroidPrimePort/CMakePresets.json), [Dockerfile](../../MetroidPrimePort/Dockerfile.muos), [SDL provider](../../MetroidPrimePort/extern/aurora/cmake/AuroraSDL3Provider.cmake).
- [Donor GL device](../../MetroidPrimePort/extern/aurora/lib/gl/device.cpp), [EGL context](../../MetroidPrimePort/extern/aurora/lib/gl/context.cpp), [presentation bridge](../../MetroidPrimePort/extern/aurora/lib/webgpu/sdl2shim_present.cpp).
- [Donor active graphics core](../../MetroidPrimePort/extern/aurora/lib/gfx/common.cpp), [GLSL emitter](../../MetroidPrimePort/extern/aurora/lib/gx/shader.cpp), [uniform builder](../../MetroidPrimePort/extern/aurora/lib/gx/shader_info.cpp), [GX processor](../../MetroidPrimePort/extern/aurora/lib/gx/command_processor.cpp).
- [Donor GX tests](../../MetroidPrimePort/extern/aurora/tests/gx_fifo_test.cpp), [texture tests](../../MetroidPrimePort/extern/aurora/tests/texture_replacement_allocation_test.cpp), [card tests](../../MetroidPrimePort/extern/aurora/tests/card_gci_folder_test.cpp).
- [U renderer](../extern/aurora/lib/webgpu/gpu.cpp), [U XF registers](../extern/aurora/lib/gx/regs.cpp), [U GCI](../extern/aurora/lib/card/CardGciFolder.cpp), [U FIFO draw-sync](../extern/aurora/lib/dolphin/gx/GXManage.cpp).
- [U input](../platform/input/port_input_devices.cpp), [settings](SETTINGS.md), [frame interpolation](FRAME_INTERPOLATION.md), [port notes](../PORT_NOTES.md).

Ссылки на донор ведут к рабочему H checkout. Если утверждение относится к R до исправлений H, читать blob именно R, а не текущий файл. Оба checkout должны сохраняться рядом; Git object IDs выше фиксируют **версионируемые исходники**, но не все локальные справочные inputs.

`RG40XX_H_HARDWARE.md` и `MUOS_PLATFORM.md` на дату аудита untracked в доноре; `muos-diagnostics.txt` не входит в дерево H. Их существенные обезличенные сведения отражены в разделе 3, а использованные локальные версии идентифицируются SHA-256:

| Локальный input в доноре | SHA-256 |
| --- | --- |
| `docs/RG40XX_H_HARDWARE.md` | `853fa7135aee300f0d8e617d0707420f3feec6b77a4a709b4d9c3eb5f50af997` |
| `docs/MUOS_PLATFORM.md` | `1a3699f3ca71c3a68282b7ebb6c1691b8869ffbf14613b52ca2cce6786601e25` |
| `muos-diagnostics.txt` | `06c24c8e17e5e3951e034104bdb111e9777e921037ef9a50673f2eaa0c68575e` |

Один checkout R/H не восстанавливает эти три файла; hashes фиксируют использованные bytes, но не заменяют хранение источников. Свежий SSH-опрос — отдельное наблюдение, его результаты приведены в разделе 3.2, а не приписаны старому inventory.

Примеры команд из корня донорского репозитория:

```sh
git --no-pager show --stat aaf8349b144c3419714d489a017483ce7ef3edb0
git --no-pager diff aaf8349b144c3419714d489a017483ce7ef3edb0^ aaf8349b144c3419714d489a017483ce7ef3edb0 -- extern/aurora/lib/card/CardGciFolder.cpp
git --no-pager diff aaf8349b144c3419714d489a017483ce7ef3edb0 3f31d19f6956e0c0ada7ed56b48703b9e0c00cd6 -- extern/aurora/lib/gx/command_processor.cpp
git --no-pager show aaf8349b144c3419714d489a017483ce7ef3edb0:extern/aurora/cmake/aurora_gx.cmake
```

### Итоговая граница проверки

При подготовке документа исходный код, конфиги, saves, firmware и clocks не изменялись. На приставке были только пассивные SSH-чтения. **Новая ARM64 сборка, игра, graphics probes, C++ tests и performance experiments не выполнялись.** Документ обосновывает материал для следующего плана, но не утверждает, что U уже запускается на H700 или достигает 30 FPS.
