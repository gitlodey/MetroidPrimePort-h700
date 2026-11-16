# H700 — заключение по трём оставшимся «КАК»

**Редакция 1, 2026-11-15.** База U: `c872da704adde9a7838492aaa56dac73a32053e4`, Metroid Prime Unbound `v0.46.0`. Цель — корректно работающая игра на RG40XX H / muOS 2601.0 Jacaranda; 30 FPS — следующий этап.

**Статус: исследование завершено в доступном без устройства объёме; ниже конкретная рекомендуемая архитектура, не утверждённый план реализации.** Production-код не менялся, новых обращений к приставке не было. Этот документ уточняет прежний [перечень integration gaps](H700_DAWN_INTEGRATION_GAPS.md), а не объявляет всю интеграцию проверенной.

> **Уточнение 2026-11-16:** пользователь согласовал подход раннего integration slice на настоящем коде. Подготовлен [подробный план IG-01…IG-14](H700_INTEGRATION_PLAN.md), пока ожидающий утверждения; device window отдельно не разрешён. План уточняет реальные callback semantics (`after_present` — статистика, GX draw-done и internal afterSubmit сохраняются), normal drain и предлагает вместо live surface-only recreation проверять EFB resize + два полных поколения. Последнее — явно предложенное изменение приёмки, не автоматически отменённый/пройденный прежний критерий. Результаты исследования и открытый Mali runtime-допуск ниже сохраняются.

## 1. Единый вывод

Рекомендуется **сохранить renderer U и закреплённый Dawn**, добавить H700 platform path через SDL3-over-SDL2 → firmware SDL2 `mali` → EGL/GLES. Старый raw renderer целиком не переносить, firmware не заменять.

| Вопрос | Конкретное решение | Что действительно установлено |
| --- | --- | --- |
| Как совместить потоки и main-thread presenter? | Сохранить main/game, GX FIFO worker, render worker и одного pipeline compiler. Main резервирует presentation slot **до recording**; worker никогда не ждёт main/swap. Main обеспечивает ограниченный progress во всех зависимых waits | Разобраны реальные waits, locks и callbacks U; протокол определён. Совместная работа на Mali **не проверена** |
| Как ограничить lifetime EGLImage без утечек на каждый resize/кадр? | **Два постоянных импортированных native slots на lifetime device**, без reimport на живой группе. Освобождение aliases — через фактическое уничтожение **всей Dawn share group** | Контракт следует из pinned implementation и GLES/EGL lifetime rules. Штатный многопоточный teardown U **не проверен** |
| Как собрать новую базу с доступным ABI? | Закреплённый ARM64 builder/Clang16, проверенный SDL fork, Dawn package, **nod release package/static**, source-built pinned zlib-ng/zstd; offline build из проверенных inputs | Реальные compile/link checks пройдены, недостающие архивы получены и проверены. Полная игра и kernel/runtime совместимость **не проверены** |

[Mixed-fetch](H700_MIXED_FETCH_PROBE.md) остаётся решением исходного layout-барьера: vertex-only R32Uint fetch textures и fragment-only исходный SSBO, с сохранением 16 material textures. Его локальный device workload уже прошёл. Это не заменяет перенос настоящего shader generator, consumers и cache identity.

**Нет основания начинать новую серию независимых демок.** Есть один ещё не проверенный архитектурный допуск: pinned Dawn + firmware Mali выдерживают выбранную совместную context/execution/lifetime схему. Предлагаемый конечный способ проверки — один ранний интеграционный gate (§5), предпочтительно на минимальном production slice. Перенос этого допуска в первую реализацию требует **явного согласования**, поскольку действующие правила требуют закрывать значимые неопределённости до production-кода. Правила этим документом не изменены.

## 2. Потоки, передача кадра и отсутствие цикла ожиданий

### 2.1. Владельцы и порядок

- **Main:** SDL/window/events, presenter EGL context/surfaces, blit/swap, presentation mailbox и recycle.
- **FIFO:** штатное ordered consumption и recording. Draw-sync tokens сохраняют CPU lifetime contract, не становятся GPU fences.
- **Render worker:** encoding, полная final composition (resampler и включённые UI layers), submit и publication. Не выполняет SDL swap и **не приобретает занятый presentation slot**.
- **Compiler:** отдельный поток U для OpenGLES; не переносится в очередь, которую blocking pipeline lookup сам же ожидает.
- Resource calls U с main/FIFO не маскируются под single-thread device. Сохраняются предусмотренные Dawn multi-thread synchronization/scoped-current mechanisms; не создаём собственные compiler contexts.

Перед `gfx::begin_frame()` main приобретает свободный native slot, обслуживая progress. В packet передаётся immutable descriptor `generation/frameId/slotId`; abort до Submit явно возвращает reservation. Всего два credits, FIFO mailbox bounded 2, без перезаписи «последним кадром».

```text
Free → Reserved → Submitted → Ready → Reading → Retiring → Free
                         при недостоверном completion → Quarantined
```

1. Worker записывает только зарезервированный slot, делает Submit, регистрирует `OnSubmittedWorkDone(WaitAnyOnly)` и публикует descriptor/future. Публикация не означает готовность pixels.
2. CPU packet освобождается по своему CPU/submit контракту. Ожидание presentation не удерживает worker. Callbacks, требующие именно показа, завершаются отдельно.
3. Main неблокирующим `WaitAny` получает forward completion; лишь затем читает image.
4. Перед raw GL main делает **настоящий `eglMakeCurrent`** с presenter context и surfaces. Одного SDL wrapper недостаточно из-за TLS fast path.
5. Blit → reverse `EGL_SYNC_FENCE_KHR` → `glFlush` → swap. После успешного swap фиксируется результат для `after_present()`; его внешний callback доставляется только в безопасной точке main (§2.2). Ни публикация mailbox, ни возврат swap не доказывают физический scanout.
6. Slot возвращается в Free только после reverse completion и завершения swap. Неуспех/timeout не разрешает повторную запись; slot карантинируется, запускается error policy.

Главное отличие от опасной схемы: **нет ребра render-worker → main-presenter**. Main может ждать CPU worker, но worker не ждёт показа кадра main-потоком. Ограничение в два credits сохраняет overlap и не является serial fallback.

### 2.2. Progress во время ожиданий

Один нерекурсивный `main_progress_once()` обрабатывает terminal flags, напрямую обслуживает Dawn events/completions, готовый presentation и reverse fences. Его нельзя ставить в заполненную worker queue. Он не вызывает game callbacks, новый recording, `window::poll_events()`, resize или shutdown.

Для этого нужен явный **двухфазный dispatch**, а не предположение, что `ProcessEvents`/`WaitAny` не вызовут callback:

- Зарегистрированные в Dawn callbacks исполняют только внутренние completion/error/state transitions и будят waiters. Внутренние packet/map/credit completions, необходимые для progress, не откладываются до game callback.
- Game/user callbacks, включая внешний `after_present`, превращаются в owned result records и доставляются FIFO только в верхнеуровневой безопасной точке main: вне recording/progress и locks, до следующего recording. `after_present` success возможен только после фактического успешного swap. Payload не ссылается на уже переиспользованный CPU packet/slot; нужные данные копируются либо отдельно удерживаются с явным lifetime.
- Отложенный внешний dispatch **не удерживает presentation credits и не нужен worker для завершения**. В Draining внутренние completions продолжаются, а обязательные внешние success/abort результаты доставляются в отдельной lifecycle-safe точке main, пока их consumers ещё живы, до destruction game/UI owners. Это не рекурсивный вызов callbacks из ожидания или error callback.

Progress вызывается **без queue/sync/pipeline/mailbox/FIFO locks**, с освобождением lock перед повторной попыткой. Нужные места адаптации уже определены, это не будущий поиск:

| Реальный путь U | Изменение контракта в будущем плане |
| --- | --- |
| [render_worker.cpp](../extern/aurora/lib/gfx/render_worker.cpp):88–98, 254–266 | Main-aware bounded queue push и CPU synchronize marker wait с progress/cancellation; ёмкость 256 сама проблему не решает |
| [frame.cpp](../extern/aurora/lib/gfx/frame.cpp):950–990 | Frame/staging acquisition и map waits обслуживают presenter, не только GPU progress |
| `frame.cpp`:1043–1156; [fifo.cpp](../extern/aurora/lib/gx/fifo.cpp):51–66, 154–170, 239–274 | Growth/reservation и FIFO drain; при main growth lock — try-lock/progress, не presenter под `sBufferMutex`, который FIFO держит при enqueue |
| [pipeline_cache.cpp](../extern/aurora/lib/gfx/pipeline_cache.cpp):719–735, 1321–1383 | Blocking lookup не держит cache lock во время progress; compiler остаётся независимо исполняемым |
| Shutdown FIFO/render/compiler | Сначала stop-admission/drain/cancellation и завершение необходимых работ, потом join; не join при невыполнимом ожидании main |

Terminal state: `Running → Draining/Failed → Stopped`. При Draining запрещены новые admissions/reservations, но **результаты уже принятых jobs продолжают доставляться**: submitted descriptor/future доходит main для retirement либо явно передаётся teardown-владельцу. Нельзя закрыть mailbox так, чтобы потерять slot/completion. Error callbacks только записывают первую ошибку и будят waiters. Для отменяемых jobs нужен явный result/abort contract: [gfx/types.hpp](../extern/aurora/lib/gfx/types.hpp):39 и [public gfx.hpp](../extern/aurora/include/aurora/gfx.hpp):114–118. Не вызывать success-only completion после несостоявшегося Submit. Не выполнять job inline на произвольном вызывающем потоке после остановки worker и не detach незавершённые потоки.

Это статическое решение нашего wait graph. Оно **не доказывает** отсутствие скрытых ожиданий внутри libMali/Dawn.

## 3. EGLImage, recreation и окончательное освобождение

### 3.1. Почему не требуется патч importer прямо сейчас

В pinned Dawn import создаёт alias GLuint и возвращает `OwnsHandle::No`; публичного per-import release нет. Поэтому wrapper release и `eglDestroyImageKHR` не удаляют alias. Но это не означает необходимость пересобрать Dawn при заранее ограниченном числе импортов.

Выбранный для предложения контракт:

- Два export storage **640×480 GL_RGBA8**, 1 mip, без MSAA; один import каждого на поколение device. Сами pixels двух slots — 2 457 600 bytes, **не полный driver memory budget**.
- Native output slots независимы от EFB scale/depth/MSAA/resampler. Итоговая композиция идёт в них с явно согласованной color-space conversion; не копировать только EFB, теряя UI.
- Per-frame import, respecification и замена slots при живой Dawn share group запрещены.
- Surface-only recreation сохраняет slots **только** при сохранении presenter context/display и валидного storage. Смена context/display/формата требует полного завершения старого поколения.
- Import failure завершает поколение без retry на том же device: у pinned importer есть ранние error paths после `GenTextures` без общего cleanup guard.

Нормативная опора — **GLES 3.2 §5.1.1**: после destruction последнего context share group реализация освобождает shared objects/resources «as soon as possible». Общий display не объединяет presenter и Dawn в одну share group; дополнительные compiler contexts Dawn входят в его группу. Нельзя наблюдать уничтожение только основного context и считать всё освобождённым.

### 3.2. Shutdown — часть реализации, не process-exit workaround

1. Stop **новых admissions/acquire/producers**, не доставки результатов принятой работы. Submitted descriptors/futures и terminal callbacks продолжают поступать main/teardown-владельцу до retirement; mailbox закрывается только после этого. Завершить либо явно отменить CPU jobs и callbacks по двухфазному dispatch §2.2; на здоровом GPU получить forward/reverse completion. При loss нельзя обещать успешный GPU drain.
2. Завершить calls/scopes/tasks. Каждый управляемый приложением поток обеспечивает **unbind своего current Dawn context** до выхода и до destruction device на другом потоке; main не может отвязать context чужого потока. Затем joins, с сохранением нужного progress до quiescence.
3. Освободить все удерживающие Dawn ссылки: packets, commands, views/bindgroups, caches, UI/effects, callback captures и static holders. Завершить readbacks/maps, а не просто забыть callbacks.
4. `device.Destroy()`, завершение lifecycle callbacks, release queue/device/adapter/instance и оставшихся references. **`device.Destroy()` сам EGL contexts не уничтожает**; они уходят при окончательном destruction native device.
5. Подтвердить фактическое destruction **всех** Dawn shared contexts, включая compiler contexts, без оставшихся current bindings.
6. При ещё живом presenter удалить FBO → EGLImage → source textures; затем presenter/window/display. Proc resolver и библиотеки живут до конца EGL teardown. Dawn заимствует display и сам не делает `eglTerminate`.

**Найден конкретный обязательный lifecycle fix для будущего плана:** [vis_query.cpp](../extern/aurora/lib/gfx/vis_query.cpp):38–51 удерживает static query/buffer slots и pipeline map; explicit shutdown/reset отсутствует и [gfx shutdown](../extern/aurora/lib/gfx/frame.cpp):885–948 его не вызывает. Нужен корректный reset после завершения callbacks, не отключение subsystem. Сейчас код не исправлялся.

Предлагаемая fatal policy: при fatal EGL/device loss прекратить работу, завершить/отменить ожидания, выполнить доступный best-effort teardown и выйти с диагностикой, **без live recovery**. Это ограничение требует согласования в плане. Оно не обещает сохранение несохранённого прогресса или возврат из зависшего driver call. Если destruction группы не подтверждено, нельзя объявлять cleanup успешным или создавать следующее поколение.

### 3.3. Первичные основания

Dawn ref во всех ссылках — `118436b3f8f68583eb86e811ca605dc41637e8cc`:

- [DeviceGL.cpp:421–470](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/DeviceGL.cpp#L421-L470) — alias/import/error paths; [TextureGL.cpp:373–393](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/TextureGL.cpp#L373-L393) — ownership deletion.
- [OpenGLBackend.h:53–85](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/include/dawn/native/OpenGLBackend.h#L53-L85) — публичный import, не private handle cleanup API.
- [PhysicalDeviceGL.cpp:660–694](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/PhysicalDeviceGL.cpp#L660-L694) — main и до четырёх shared compiler contexts при multi-thread toggle; один compiler U не означает один дополнительный context Dawn.
- [DeviceGL.h:343–349](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/DeviceGL.h#L343-L349), [Device.cpp:548–647](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/Device.cpp#L548-L647) — владение contexts и отличие Destroy от окончательного release.
- [ContextEGL.cpp:106–119](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/ContextEGL.cpp#L106-L119), [310–423](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/ContextEGL.cpp#L310-L423) — destructor, legacy make-current с игнорируемой ошибкой и scoped restoration. Toggle не является доказательством правильности всех transitions на Mali.
- [QueueGL.cpp:191–225](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/QueueGL.cpp#L191-L225) — backend GPU completion; [DisplayEGL.cpp:76–84](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/DisplayEGL.cpp#L76-L84) — borrowed display.
- [GLES 3.2 §5.1.1](https://registry.khronos.org/OpenGL/specs/es/3.2/es_spec_3.2.pdf), [EGL_KHR_image_base §2.5.2](https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_image_base.txt), [eglDestroyContext](https://registry.khronos.org/EGL/sdk/docs/man/html/eglDestroyContext.xhtml), [eglCreateContext](https://registry.khronos.org/EGL/sdk/docs/man/html/eglCreateContext.xhtml) — shared-object destruction, siblings, delayed destruction current context и явный share_context. Прочитаны при исследовании 2026-11-15.

## 4. Build-цепочка: реальная компиляция и линковка

### 4.1. Фактически выполнено

Exact image: `metroid-h700-probe:abi238-v1`, ID `sha256:2db4439763783aa796da4268dbd91847ee2c13deef4c8ce2439e0f6bdf926962`. CMake 3.27.4, Ninja 1.11.1, Clang 16.0.6 / libstdc++ GCC13.2. Проверки с Clang16, `RelWithDebInfo`, `-mcpu=cortex-a53`, game-facing `-fsigned-char`.

Контейнеры: `--pull=never --network=none --read-only`, source `/src:ro`, только новый output `/out` writable, ограниченные timeout. Архивы отдельно получались на host по exact URLs с SHA verification. Образ/зависимости проекта не менялись, пакеты не устанавливались, ARM executables не запускались.

| Проверка | Результат |
| --- | --- |
| SDL headers fork `6057d79baf8321bf190479a699655f06cc2a962f` + настоящий SDL DSO | Compile/link references на адреса **252** функций из callers, объявленных в Linux headers; отсутствующих exports **0**. Аргументы всех игровых call sites этим probe не проверены |
| Реальные SDL consumers | Скомпилированы `platform/ai_dma.cpp`, Linux HID, Aurora input, ImGui SDL3/SDLRenderer3 |
| C++20 | Compile/link с настоящим Aurora `thread.cpp`, jthread/concepts/semaphore/filesystem |
| MusyX U | Все **33 C translation units**, `-std=gnu2x`, `-fsigned-char`; whole-archive link без stubs |
| nod первоначальный cached `.a` | 19 API references с headers alpha.12, API link PASS; этого было недостаточно для provenance/rebuild |
| nod **release package alpha.12** | Архив получен, hash совпал; неизменённый `AuroraNodProvider.cmake` выбрал `package/static` → `nod::nod_static`; те же 19 API references слинкованы |
| zlib-ng 2.3.3 | Собран static `libz.a`, `ZLIB_COMPAT=ON`; API compile/link PASS |
| zstd 1.5.7 | Собран static `libzstd.a`, compression/decompression/dictionary/multithreading/legacy сохранены; API compile/link PASS; tools/tests не собирались |
| ELF metadata | Все probe executables AArch64, `/lib/ld-linux-aarch64.so.1`; required provider/version nodes присутствуют в firmware snapshot либо в проверенном bundled SDL DSO |

В начальных checks максимумы: SDL DSO/MusyX **GLIBC_2.38**, C++20 **GLIBCXX_3.4.31**. У дополнительных zlib/zstd/nod probes максимум **GLIBC_2.34**, nod — до **GCC_4.2.0**. Firmware maxima: GLIBC 2.38, GLIBCXX 3.4.32, CXXABI 1.3.14. Это provider/version-node gate; snapshot **не содержит полного набора точных exports для всех символов** и не доказывает runtime на Linux 4.9.170.

Release nod probe имеет `NEEDED`: `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libc.so.6`, `ld-linux-aarch64.so.1`. Внешних compression DSO (`libz`, `libzstd`, `liblzma`, `libbz2`) нет; `libnod.so` не подключался. C++ link driver сам добавляет libstdc++, это не доказательство зависимости nod от C++ runtime.

### 4.2. Закрытые недостающие inputs

Это pins существующего проекта, **не обновление версий**:

| Input / exact URL | SHA256 |
| --- | --- |
| [zlib-ng 2.3.3](https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz) | `f9c65aa9c852eb8255b636fd9f07ce1c406f061ec19a2e7d508b318ca0c907d1` |
| [zstd 1.5.7](https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz) | `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3` |
| [nod v2.0.0-alpha.12 Linux AArch64](https://github.com/encounter/nod/releases/download/v2.0.0-alpha.12/libnod-linux-aarch64.tar.gz) | `6872dd3d45d9e688012db30f0a75b10702b8520b5b550ea6b50e9f6d458b05b3` |

Источники pins: [extern CMake](../extern/aurora/extern/CMakeLists.txt):146–154, 333–340; [dependency versions](../extern/aurora/cmake/AuroraDependencyVersions.cmake):22, 48; [nod provider](../extern/aurora/cmake/AuroraNodProvider.cmake):104–165. Release static `libnod.a` SHA256: `6a907b78fa997e251b431bb26e56813d472169e3fc755a6b4e14943d09b4ee48` — это **не** прежний cached `.a` с SHA `54795d75c05971072e9a7a9425cd502a69a1f4b1152e71228ce9457a1d56ce8a`.

Сохранены/сверены cached inputs absl 20240722.0, fmt 12.1.0, xxHash 0.8.3, PNG 1.6.58, FreeType 2.14.3, ImGui 1.91.9b-docking, SQLite 3.51.3, Tracy 0.14.1, GTest 1.17.0; MusyX/ASTC vendored. **Все библиотеки и полная игра заново не собирались.** Наличие кэша не является самостоятельным рецептом clean build.

### 4.3. Конкретный предлагаемый build profile

- Сохранить exact image/Clang16 для первого full build **только после согласования EOL-компромисса для игры**. Ubuntu 23.10 EOL уже принята для демки, не автоматически для полной игры; [ограничения и recipe](H700_BUILD_ENVIRONMENT.md) сохраняются.
- Native Linux AArch64 / Cortex-A53, Ninja `RelWithDebInfo`, bundled dependencies и embedded resources. GX/DVD/CARD/THP и `AURORA_CACHE_USE_ZSTD=ON` сохранить.
- Dawn ref `118436b3f8f68583eb86e811ca605dc41637e8cc`, package `v20261029.114630`, без patches. SDL fork pin выше, не upstream-default SDL3 3.4.10; headers и DSO из одного дерева.
- nod: `AURORA_NOD_PROVIDER=package`, `AURORA_NOD_LINKAGE=static`, exact release archive + SHA. Rust/Cargo/Corrosion для этого пути не нужны. Vendor Rust не выбирать молча: в image Rust нет, nod требует Rust≥1.85/edition2024, Corrosion pin 0.6.1 не совпадает с cached 0.6.
- zlib/zstd — исходные pinned archives в отдельном writable staging/build. Existing `FETCHCONTENT_SOURCE_DIR_ZLIB`/`ZSTD` указывают source roots; SHA проверяется **до** этих overrides, которые обходят download/patch step. Zlib patch относится к clang-cl ARM, не Linux Clang16. nod local `AURORA_NOD_PACKAGE_URL=file:///out/libnod-linux-aarch64.tar.gz` + SHA прошёл штатный provider. Глобальный fully-disconnected до первого populate не включать: он может пропустить распаковку.
- Clean build должен иметь явный input manifest/bootstrap с hashes, включая SDL/Dawn и остальные providers; source overrides не должны ссылаться на случайный старый build. Компиляция offline; git-fetch при отсутствующем git не считается рабочим fallback. BuildRevision имеет source-snapshot fallback.
- SDL audio/joystick передаются firmware SDL2: container ALSA/PipeWire/PulseAudio dev packages не требуются выбранному shim profile. RmlUi остаётся исходно OFF; OpenSSL — optional desktop/WSS, ffmpeg — importer tool, не обязательная runtime link dependency игры.
- На устройство не поставляются container libc/libstdc++/EGL/GLES. Проверять каждый конечный game artifact, bundled SDL DSO и runtime chain, не переносить ABI PASS probes автоматически.

У nod release найдены weak `statx@GLIBC_2.28`, `pidfd_spawnp`, `pidfd_getpid`, а также `getrandom`, `copy_file_range`, `syscall`. Наличие новых syscall paths **не доказывает ошибку**, weak import не делает вызов безопасным само по себе. Fallback на kernel 4.9 не проверен. Это явный runtime criterion для первого чтения пользовательского game image на H700 и loader check итогового artifact, не основание заранее устанавливать Rust или менять nod. В этой сессии runtime не запускался.

### 4.4. Локальные доказательства

Ignored каталог `build/h700-build-readiness/`: `commands.json`, `finish_commands.json`, build/configure logs, `provider_abi_audit.json`, `artifact_summary.json`, `sdl_import_verification.json`, `verified_input_trees.json`, `cached_archives.json`. Дополнение `input-closure/`: `downloads.json`, `container-command.json`, `{zlib,zstd,nod}.commands.json`, `results.json`, ELF/import reports и сами три архива. Это локальные artifacts, **в Git не входят**; существенные pins, результаты и ограничения сохранены выше.

Загрузка трёх архивов заняла около 5 с, дополнительная extraction/configure/build/audit — 12,5 с. Это не оценка времени полной сборки. Был технический сбой первого Python ABI-аудита (`ModuleNotFoundError: verify_environment`); повтор с явным PYTHONPATH прошёл. Это не compiler failure.

## 5. Один конечный gate вместо серии демок

**Зачем:** static wait graph и спецификация lifetime не доказывают driver concurrency и реальный teardown. Отрицательный результат может поменять execution/context policy — поэтому это единственный оставшийся архитектурный runtime-допуск, а не мелочь, которую можно спрятать до финального запуска игры.

**Рекомендация для согласования:** проверить на первом ограниченном integration slice с настоящими FIFO/render/compiler и lifecycle hooks. Не переносить сначала всю игру и не писать параллельный disposable renderer. Сам slice разбивается в будущем плане на итерации 1–2 часа; до PASS нельзя разворачивать остальной renderer-перенос. Если такой перенос проверки в production не согласован, допуск остаётся предплановым блокером; новая демка автоматически не начинается.

**Один bounded сценарий, два поколения device в одном процессе:**

1. Два native slots, различимые frame IDs/pixels и настоящая final composition. Подтверждённое перекрытие resource creation, render и pipeline compile с blocking lookup; контролируемый backpressure. Проверить progress нужных waits без превращения каждого wait в отдельный device experiment.
2. Один surface-only recreate при сохранённых context/display: число imports не растёт. Нормально завершить первое поколение; наблюдением через публичный EGL proc resolver подтвердить share relations, unbind и уничтожение всех Dawn contexts до export resources/display.
3. Создать второе поколение и запросить завершение при pending render/compile/presentation work. Все jobs/callbacks получают completion либо явный abort, потоки завершаются, holders освобождаются. Проверка относится и к реально созданным static holders, включая `vis_query`; отсутствие его объектов в slice не разрешает забыть reset в плане.

**PASS:** pixels/order верны; read только после forward completion, reuse только после reverse; ровно два imports на поколение; нет validation/EGL/GL errors, зависших callbacks/joins и внешнего timeout; старые contexts действительно уничтожены. Это не доказательство отсутствия всех внутренних driver leaks в многочасовой игре и не FPS benchmark.

**FAIL:** ошибка собственной state machine/progress/cancellation исправляется в пределах утверждённого контракта и повторяется тот же gate. Если при соблюдённых invariants отказ находится в Dawn/Mali concurrency/context teardown — **остановка переноса и согласование изменения архитектуры**. Не включать тихо serial fallback, не менять Dawn/firmware, не писать в занятой image и не объявлять невыясненный timeout PASS.

## 6. Граница готовности плана

**Уже можно составлять конкретный проект плана:** швы, владельцы, порядок ожиданий/освобождения, build dependencies и необходимые изменения названы. Нет причины повторять проверенные mixed-fetch draws или расширять Null backend.

**Нельзя утверждать «все факты уже доказаны» и безусловно разрешать полный перенос:** общий runtime-допуск из §5 открыт. Для исполнимого плана нужно согласовать саму архитектуру, EOL build baseline для игры, fatal-exit policy и место этого одного gate. Выбор gate на раннем production slice — явное ограниченное разрешение, не уже полученное согласие.

Полная сборка, game-facing graphics/UI/effects, input/audio, чтение образа, безопасная установка и реальный save/load остаются проверками самого переноса. Их не заменяют API probes, но и превращать каждую в отдельную предплановую демку не требуется. После согласования границ следующий результат — подробный план итераций, не новый произвольный список исследований.
