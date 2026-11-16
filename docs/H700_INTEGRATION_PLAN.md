# H700 — план первого интеграционного этапа

**Редакция 1, 2026-11-16. Статус: IG-01…IG-13 утверждены пользователем для последовательной локальной реализации с commit/push после каждой итерации. IG-14/device window отдельно не разрешён.**

**Для чтения без технических подробностей:** [что делаем в 13 итерациях, что получаем и какая это часть переноса](H700_INTEGRATION_SUMMARY.md). Ниже — рабочая техническая версия того же плана.

Пользователь явно утвердил все **13 локальных итераций**: настоящий участок переноса, diagnostic scene без игровых ресурсов, штатные потоки/renderer U. Разрешено выполнять их последовательно без повторного запроса между итерациями; после каждой — проверки, commit и push. При необходимости изменить план работа останавливается по [workflow](H700_WORKFLOW.md). Приставка/IG-14 остаются отдельным разрешением; остальной перенос игры не разрешён этим этапом.

Основание — [архитектурное заключение](H700_ARCHITECTURE_DECISION.md), база U `c872da704adde9a7838492aaa56dac73a32053e4`, Unbound v0.46.0. Цель этапа — получить пригодное для будущей игры графическое основание **RG40XX H / H700 / muOS 2601.0**, не отдельный альтернативный renderer и не FPS benchmark.

## 1. Результат и границы

**Остаётся в порте:** opt-in H700 backend Aurora, SDL2/Mali bootstrap, mixed-fetch в настоящем GX generator/resources, два постоянных native slots, main presenter, progress/drain и generation teardown. Эти production-файлы затем использует игра без копирования из harness.

**Вспомогательная часть:** постоянный небольшой frontend `tools/h700_integration_driver/`, который вызывает штатные `aurora_initialize/begin_frame/end_frame/shutdown` и GX API. Он линкует реальные `aurora::core/gx/main/vi/mtx/os`, включая FIFO/render worker/compiler, а не компилирует их упрощённые копии. В current CMake нет единого target `aurora`; source manifests остаются штатными. Driver не является продолжением старого `h700_graphics_probe`.

**Не входит:** полная сборка/запуск игры, audio/input acceptance, cards/save-load, game-owner shutdown hooks, skinning waits, все retail effects, PBR/HD-паки, live device-loss recovery и оптимизация FPS. Они остаются задачами следующего плана. Другие платформы и существующие GX callback semantics не удаляются и не подменяются.

**Не раздуваем scope:** не создаём общий callback framework, не переделываем неиспользуемые optional readbacks, не включаем subsystem ради новой проверки. Однако ресурсы, созданные штатным `gfx/gx` bootstrap, подлежат освобождению независимо от того, рисует ли их сцена.

## 2. Решения, входящие в утверждение плана

1. **Build baseline:** использовать прежний exact ARM64 image `sha256:2db4439763783aa796da4268dbd91847ee2c13deef4c8ce2439e0f6bdf926962`, Clang16, Ninja, RelWithDebInfo, Cortex-A53, `-fsigned-char` и проверенные pinned inputs. Это явное принятие [EOL-ограничений](H700_BUILD_ENVIRONMENT.md) **только для данного integration slice**, не автоматическое утверждение builder полной игры. Никаких apt/Rust installs, нового image или Dawn rebuild.
2. **Providers:** Dawn package/static без patches; SDL fork `6057d79baf8321bf190479a699655f06cc2a962f` vendor/shared с matched headers; nod alpha.12 release package/static; source-built pinned zlib-ng/zstd. Компиляция offline из manifest с hashes, fresh output. Providers должны подтвердить ожидаемые targets/paths; неожиданный system target — ошибка конфигурации, не повод менять версию. Existing image уже проверен на отсутствие нужных system dev packages. Не добавлять универсальные provider переделки ради других окружений.
3. **Platform option:** новая opt-in `AURORA_H700`, допустимая только для Linux AArch64. Обычный upstream path по умолчанию неизменён. Реализация в `extern/aurora/lib/h700/`, интеграция через штатные Aurora manifests. Имена новых файлов/API ниже — планируемые, сейчас их нет.
4. **Runtime:** строго аппаратный OpenGLES на firmware display; без CPU/Null/другого backend fallback. `SDL_VIDEODRIVER=sdl2`, `SDL3SHIM_SDL2_VIDEODRIVER=mali`. Native output 640×480, GL_RGBA8 / RGBA8Unorm, 1 mip, без MSAA. Существующий linear-format present путь U сохраняется, дополнительную sRGB encode/decode не добавлять. EFB и native output — разные объекты.
5. **Нагрузка и проверки:** frame/storage scale 1, resident geometry 0, неприкреплённые staging buffers; один compiler U для OpenGLES, штатный multi-thread Dawn toggle. RmlUi остаётся исходно OFF; ImGui входит в настоящую final composition. Не форсировать no-compute. `pipelineCacheNoSeed=true`, отдельный пустой cache, профиль `h700-mixed-v1`. Validation и robustness **включены**, в том числе в RelWithDebInfo: U иначе при NDEBUG включает `skip_validation/disable_robustness`. Driver явно отключает эти toggles и отклоняет конфликтующие runtime overrides/skip-present, а не полагается на чистое окружение.
6. **Recreation — уточнение предлагаемой приёмки:** native presenter context/surface/storage сохраняются до конца поколения; смена EFB-размера их не пересоздаёт. В gate проверяем EFB resize при неизменных native slots, затем **полный teardown и второе поколение**. Это предлагаемая замена surface-only recreation из §5 прежнего заключения: для фиксированного H700 output и fatal-exit policy live recreation не нужно, а SDL-владельцу поверхности нельзя произвольно подменять его EGLSurface. Поддержку live surface recreation не заявляем. Утверждение этого плана согласовало эту замену приёмки; ни EFB resize, ни full teardown ещё не объявляются проверенными.
7. **Failure policy:** fatal EGL/device loss → terminal state, пробуждение ожиданий, доступный best-effort teardown и диагностический exit, без live recovery. Нельзя обещать GPU drain после loss или возврат из зависшего driver call; timeout не PASS. Неизвестный teardown запрещает второе поколение. Firmware/runtime libraries на устройстве не заменяются.

## 3. Неизменные контракты реализации

- Main резервирует native credit **до** `gfx::begin_frame`. Descriptor поколения/frame/slot передаётся worker по значению и живёт независимо от CPU packet. Worker не ждёт swap/main.
- Два imports на device; FIFO mailbox bounded 2. `Free → Reserved → Submitted → Ready → Reading → Retiring → Free`, недостоверное завершение → Quarantined. Forward completion до чтения, reverse EGL fence completion и успешный swap до reuse; не освобождать credit «по timeout».
- Normal Draining закрывает **root admissions** на зафиксированной границе принятой работы. Downstream остаётся открыт, пока accepted FIFO/frame producers могут публиковать continuations. Quiescence означает: эти producers завершились, их queued/executing render/compiler continuations и completion obligations закончены. Одного пустого queue, `is_idle()` или sync marker недостаточно. Нормальный stop не отменяет уже принятые frames; rejected root work, ненужный background backlog и явный pre-submit/failure outcome различаются. Выполняющийся job не теряет lifetime данных только потому, что waiter отменён.
- `main_progress_once()` нерекурсивно обслуживает только внутренние events/completions/presentation, без game/update/event polling/нового recording/resize/shutdown и без неподходящих locks. Не ставится в заполненную render queue.
- **Уточнение терминов заключения:** существующий `gfx::after_present()` (`frame.cpp:1278`) — статистика, не внешний callback. Для H700 статистика обновляется после успешного swap. Новые notifications driver — owned result records, выдаваемые в его main safe-point, без удержания native credits.
- Существующие `EncoderTaskDescriptor::afterSubmit` и GX draw-done не переносятся автоматически в этот dispatcher. Первые могут запускать internal maps на render worker, вторые имеют проверяемую семантику возврата до `fifo::drain()`. Не менять публичный callback ABI только ради данного driver.
- H700 Dawn error/lost callbacks только фиксируют первую terminal ошибку и будят waiters, без прежнего немедленного `FATAL` и без рекурсивного shutdown. Failed wait/lookup прекращает обычный frame/growth/drain путь и возвращает управление driver failure/quiesce; нельзя просто выйти из void wait и продолжить как после success. Публичные здоровые GX semantics и callback ABI сохраняются; для failure propagation нужны status-bearing internal paths.
- После stop/drain/maps retirement — thread-local unbind на владельцах, joins, release всех фактически созданных app holders, Destroy/lifecycle callbacks/final device release. **Вся Dawn share group** должна исчезнуть до export storage/presenter/display; resolver/libraries живут до конца. Process exit не заменяет штатный release первого поколения.

## 4. Итерации реализации

Для всех карточек ниже общие требования: ограниченный собственный diff → относящиеся к нему проверки → запись результатов/ограничений в этот план и [прогресс](H700_PROGRESS.md) → отдельный commit/push. Не трогать чужие `.DS_Store`, донор, прежние builds или image. Rollback — только собственное изменение; сломанный artifact не развертывать, логи сохранить. До IG-14 все проверки локальные, без устройства.

Оценки — **1–2 часа активной работы на карточку**, не обещание точного срока. Cold configure/build и device window — отдельно. Если карточка не помещается либо нужен новый архитектурный выбор, фиксируем частичный результат и согласуем split/изменение, не расширяем работу молча.

### IG-01. Воспроизводимая сборка настоящих Aurora targets

- **Зачем/результат:** исключить случайные старые binary trees и собрать реальные зависимости первого slice.
- **Файлы:** новые `tools/h700_integration_driver/{CMakeLists.txt,main.cpp,build.py,inputs.json}`, `cmake/h700/InitialCache.cmake`; `extern/aurora/CMakeLists.txt` — объявление H700 option. Root game CMake не менять.
- **Решение:** reuse существующего environment/ABI verifier, manifest/staging pinned archives в fresh output; `add_subdirectory(extern/aurora)` и линковка штатных targets с существующей RESCAN policy. CLI пока только help/version и явный отказ ещё неготового graphics режима, без fake successful rendering.
- **Проверка:** hash mismatch/missing input останавливает bootstrap; offline configure выбирает exact package/fork targets, без Rust/Corrosion/vendor Dawn. Compile/link настоящих Aurora libraries и frontend; `--help` не инициализирует GPU.
- **Готово:** inputs/providers/команды и ELF metadata сохранены, нет случайных source/build overrides. Это build PASS, не runtime PASS. **Зависимости:** утверждённый план; уже полученные archives. Ожидание cold build может занять десятки минут, пока не измерено. Для CPU tests — отдельный output `/out/tests-h700` с `AURORA_ENABLE_TESTS=ON`, теми же pins и новой H700 state-test target; explicit build/run перечислены в §5.1.

### IG-02. CPU-контракт двух credits и terminal results

- **Зачем/результат:** владение native slot не смешивается с CPU packet и не теряется при stop.
- **Файлы:** новые `extern/aurora/lib/h700/presentation_state.{hpp,cpp}`, `extern/aurora/tests/h700_presentation_state_test.cpp`, test manifest. H700 records — internal API, без общего external callback framework.
- **Решение:** immutable descriptor/generation, state machine, reservation rollback, non-overwriting FIFO mailbox, accepted result delivery в Draining. Internal notifications queue не удерживает credits; внешняя выдача только в driver safe-point.
- **Проверка/готово:** CPU tests: нельзя reserve третий credit, читать Submitted или reuse Reading; pre-submit abort возвращает reservation; stale descriptor отклоняется; pending result не теряется при stop; duplicate completion не удваивает release. GPU completion пока моделируется только как входное событие.
- **Зависимости:** IG-01; local test target не требует Mali/нового Null profile.

### IG-03. Постоянный EGL/Mali bootstrap и два native imports

- **Зачем/результат:** тот platform/device path, который затем использует игра.
- **Файлы:** новые `extern/aurora/lib/h700/{presentation.hpp,presentation.cpp}`; `lib/window.cpp/.hpp`, `lib/webgpu/gpu.cpp/.hpp`, `lib/aurora.cpp`, `cmake/aurora_core.cmake`, `cmake/aurora_gx.cmake` внутри Aurora.
- **Решение:** штатный SDL shim bootstrap; внешний display/proc API Dawn, без WGPU desktop surface и fake surface capabilities. Фиксированный output format. Два GL_RGBA8 export storage/imports на generation. Resolver отслеживает EGL create/share/make-current/destroy и returns по всему Dawn share group. Использовать только публичный interop; private alias GLuint не извлекать.
- **Проверка/готово:** compile/link H700 path и non-H700 guard; cleanup собственных частично созданных resources структурно определён, import failure прекращает generation без retry. Device claims не ставятся по compile PASS.
- **Зависимости:** IG-01–02. До интеграции drain/final release driver graphics-запуск запрещён.

### IG-04. Mixed-fetch resources/layout и buffer lifecycle

- **Зачем/результат:** убрать доказанный 17>16 барьер в настоящих bindings U, не урезая material textures.
- **Файлы:** Aurora `lib/gfx/resources.hpp`, `frame.cpp`, `encoding.cpp`, `lib/gx/gx.cpp`, `lib/webgpu/gpu.cpp`, нужные resource declarations.
- **Решение:** в H700 mixed profile group0 bindings0/1 — vertex-only R32Uint textures; binding2 — fragment-only исходный storageBuffer. Group2 сохраняет штатные 16 textures без lightmap согласно измеренным limits. Buffer и его texture mirror имеют прежний upload/zeroing/growth contract; при замене buffer обновляются все нужные bind groups. **RequestDevice тоже mixed:** vertex storage 0, fragment storage минимум 1, sampled textures 16; SSBO требуется независимо от no-compute. Проверить final device limits и binding-size limits для фактического storageBuffer U, не размера 64 KiB прежней демки. Несовместимость — явный отказ, не возврат к исходному texture-fetch layout. Остальные profiles неизменны.
- **Проверка/готово:** CPU/source assertions layout visibility и bindings, сборка actual consumers; static resource footprint/profile выводится в diagnostics. Не выдавать эти проверки за actual Mali layout PASS и не запускать прежнюю матрицу 17 на Null.
- **Зависимости:** IG-01; runtime совместно с IG-05, без промежуточного GPU-запуска несовместимых shader/layout.

### IG-05. Mixed-fetch в настоящем shader generator и cache identity

- **Зачем/результат:** реальные GX shaders соответствуют новым resources; не подложить synthetic WGSL вместо generator.
- **Файлы:** Aurora `lib/gx/shader.cpp/.hpp`, при необходимости `shader_info.*`, `lib/gfx/pipeline_cache.cpp`; tests generator в existing Aurora test infrastructure.
- **Решение:** stage-specific data access в одном WGSL module: vertex helpers обращаются к bindings0/1 как textures; fragment fog/TEV/array consumers — к binding2 как SSBO. Разделять helper families/fragment emitters при генерации, не переписывать весь WGSL слепым global replace и не вводить общий WGSL parser. Normal/shadow entrypoints и bounds semantics сохраняются. H700 resource profile входит в production namespace/identity `pipeline_cache.db`, а не только в driver path; старый cache не удалять и не использовать с новым layout. Dawn blob cache не переделывать: в нынешнем OpenGLES пути U его descriptor не подключается. Для применимых flat varyings явно допустимый Compatibility sampling, не blanket изменение interpolation.
- **Проверка/готово:** source tests соответствия normal/shadow stage bindings, fragment helper signatures и cache namespace; non-H700 generated contract не меняется. Generated modules/pipelines реально проверяет только общий IG-14.
- **Зависимости:** IG-04. Если требуется иной shader architecture, остановка и согласование, не незаметная замена решения.

### IG-06. Final composition и двусторонний GPU ownership

- **Зачем/результат:** native slot содержит итог renderer + resampler + ImGui, а main безопасно выводит его.
- **Файлы:** Aurora `lib/aurora.cpp`, `lib/gfx/frame_packet.hpp`, `frame.cpp/.hpp`, `lib/h700/presentation.*`, при необходимости `lib/webgpu/gpu.*` для target access.
- **Решение:** reserve до recording; захват descriptor по значению независимо от очищаемого packet; worker composing/Submit → work-done future publication без ожидания main. Main: forward completion → настоящий `eglMakeCurrent` → blit → reverse EGL fence + flush → swap → recycle после completion. Ни glFinish на каждом кадре, ни CPU readback/upload как транспорт.
- **Проверка/готово:** actual pipeline/format consumers компилируются, CPU state tests подтверждают порядок publication/recycle и abort; callbacks/tasks не удерживают packet до swap. Правильность pixels/fences остаётся IG-14.
- **Зависимости:** IG-02–05. Отдельную последовательную graphics демку не запускать.

### IG-07. Render queue и frame/staging waits

- **Зачем/результат:** main может обслуживать presenter при full queue, sync и staging acquisition без reentrancy/lock-cycle.
- **Файлы:** Aurora `lib/gfx/render_worker.cpp/.hpp`, `frame.cpp/.hpp`, нужные encoding enqueue sites; existing `render_worker_test.cpp`, `gfx_recording_test.cpp`.
- **Решение:** один main-only progress hook; queue capacity/sync/frame/map waits отпускают lock перед progress и различают terminal outcome. Internal map/packet completions выполняются сразу. Partial acquisitions и pre-submit abort не теряют credits. Rejected enqueue после stop не исполняет graphics job inline на произвольном caller. Lifetime уже исполняющихся jobs с захваченными ссылками сохраняется до quiescence.
- **Проверка/готово:** targeted CPU tests full queue/progress, accepted drain, rejected sync wake, отсутствие inline work и rollback частичного acquire; прежние ordering/shutdown tests сохраняются. Stubs recording fixture не являются GPU/threading доказательством.
- **Зависимости:** IG-02, IG-06.

### IG-08. FIFO drain/growth без ожидания под buffer lock

- **Зачем/результат:** настоящий FIFO не замыкает цикл main↔worker при downstream backpressure.
- **Файлы:** Aurora `lib/gx/fifo.cpp/.hpp`, existing `gx_fifo_test.cpp` и относящиеся CPU stubs.
- **Решение:** main drain и growth terminal/progress-aware; try-lock/retry вне `sBufferMutex`. Сохранить accepted FIFO continuations, ordered draw-sync tokens, unpublished tails и draw-done return-before-drain contract. Failure не продвигает фиктивно processed target и не освобождает используемый buffer.
- **Проверка/готово:** существующие FIFO growth/token/draw-done tests и один CPU downstream-backpressure case проходят; progress не исполняется с buffer lock. Это не новый GPU harness.
- **Зависимости:** IG-07. Game skinning polling не входит и не объявляется решённым.

### IG-09. Независимый compiler: blocking lookup и drain

- **Зачем/результат:** pipeline compilation не зависит от очереди/lock, которые ожидает renderer/main.
- **Файлы:** Aurora `lib/gfx/pipeline_cache.cpp/.hpp` и используемый internal wait/outcome helper; существующий CPU test target.
- **Решение:** один независимый compiler OpenGLES; blocking lookup отпускает cache lock до main progress. Normal Draining заканчивает builds, нужные accepted frames; ненужный background backlog может быть явно отменён. Writer/prefetch перестают добавлять root work. Terminal failure будит waiter, но не выдаёт pipeline за готовый. Активный driver compile должен реально вернуться до release device.
- **Проверка/готово:** CPU wait/outcome tests ready/failure/draining без lock; compile реального compiler path. Настоящий overlap и shutdown активной compilation — только IG-14.
- **Зависимости:** IG-07–08. Не переносить compiler jobs в render queue, не отключать compilation ради gate.

### IG-10. Retirement фактических maps и app holders

- **Зачем/результат:** после последнего кадра ресурсы bootstrap/scene освобождаются без требования «ещё одного Submit».
- **Файлы:** Aurora `lib/gfx/frame.cpp`, `recording.cpp`, `vis_query.cpp/.hpp` при наличии его holders, `lib/webgpu/gpu_prof.cpp`, фактические bootstrap/effect cleanup sites. Не перерабатывать все optional readback subsystems.
- **Решение:** фиксированный inventory объектов выбранного init/scene path; internal map retirement при drain; reset query/buffer/pipeline/static refs после settle callbacks. Известный `vis_query` reset включить, если holders созданы; пустое состояние не выдавать за проверку release созданных объектов. Screenshots/selftest/overdraw/bloom специально не активировать. Уже созданные init-ресурсы нельзя исключить этой оговоркой.
- **Проверка/готово:** CPU late/duplicate completion и reset ownership cases для затронутого helper, existing relevant readback tests; maps settled, нет незавершённых callback obligations/неучтённых captures, у каждого созданного holder есть владелец и release/reset point. Постоянные resources ещё могут жить до IG-11; их окончательное освобождение и исчезновение contexts этим не объявляются.
- **Зависимости:** IG-07–09. Любая обнаруженная обязательная крупная перестройка readbacks — остановка для согласования, не расширение всех effects.

### IG-11. Driver quiescence, thread-local unbind и полный release generation

- **Зачем/результат:** настоящий штатный shutdown до возврата из процесса, допускающий второе поколение.
- **Файлы:** Aurora `lib/aurora.cpp`, `lib/gfx/frame.cpp`, `lib/webgpu/gpu.cpp/.hpp`, worker epilogues `fifo.cpp`, `render_worker.cpp`, `pipeline_cache.cpp`, `lib/h700/presentation.*`, driver main.
- **Решение:** driver закрывает root producers, сохраняет progress/accepted continuations и доставляет свои результаты живым consumers; затем thread-owned unbind/acknowledgement и joins. Release holders → Destroy/lifecycle callbacks/final release всей группы → presenter FBO/EGLImage/source textures → window/display, resolver последним. Контекст, вновь сделанный current lifecycle-вызовами на main, снова unbind. Проверяется отсутствие app references, удерживающих старую группу после final release. EGL errors фиксируются, не заметаются; повторный shutdown безопасен.
- **Проверка/готово:** CPU order/idempotence/terminal tests, compile реальных epilogues. Context observer должен различать share groups и не путать Destroy с фактическим context destruction. Без успешного полного teardown следующее поколение запрещено.
- **Зависимости:** IG-03, IG-06–10. Game main/platform shutdown hooks не менять: владельцами сейчас являются driver и Aurora.

### IG-12. Диагностическая сцена через GX и один сценарий

- **Зачем/результат:** проверить постоянный путь на реальной нагрузке, не на clear-only кадре или собственной WebGPU сцене.
- **Файлы:** новые `tools/h700_integration_driver/scene.{hpp,cpp}`, driver main, минимальные opt-in diagnostic observation hooks Aurora.
- **Решение:** конечная последовательность GX geometry с асимметричными цветными областями/frame marker, обновляемыми vertex/array данными и **fog range adjustment** как fragment SSBO consumer (`fogRangeEnabled`, не просто включённый fog); ImGui рисует контрольную фигуру в final composition. Четыре заранее заданных холодных shader variants и диагностические phase barriers создают scheduling scenario из §5.2; отдельный пустой cache каждого поколения. Один controlled backpressure эпизод двух native credits, не перебор всех очередей/эффектов.
- **Проверка/готово:** независимый pixel oracle §5.3 проверяет **final-composed native slot** после forward completion и несколько pixels **после GL blit, до swap**, не только EFB. Readback диагностический, не транспорт. Trace различает периоды artificial barrier wait и действительные интервалы вызовов Dawn API/jobs; stop/pending и overlap нельзя засчитать по одному ожиданию на hook. Параллельное исполнение внутри libMali не заявляется. Hooks не заменяют jobs stubs и не меняют normal runtime по умолчанию.
- **Зависимости:** IG-04–11. Без game assets, disc access, screenshot API и seed cache.

### IG-13. Конечный ARM64 пакет и local readiness

- **Зачем/результат:** готовый к отдельно разрешённому запуску artifact с известными runtime requirements.
- **Файлы:** driver build/staging helper, проверяемая artifact metadata и license collector integration; новый отчёт `docs/H700_INTEGRATION_GATE.md` с pending device status.
- **Решение:** clean build actual target, staged shared SDL с local RPATH, notices/licenses; reuse environment/ELF verifier, audit executable+DSO+runtime chain. Не включать container libc/libstdc++/EGL/GLES, assets/settings/saves. Manifest фиксирует source revision, image/input hashes, toggles, профиль, scenario bounds и artifact hashes.
- **Проверка/готово:** targeted CPU tests из IG-02/07–11, CLI help/version, hashes/ABI/provider audit и package content check. Full GPU gate остаётся **pending**, никакой другой GPU backend не становится заменой Mali. Package не отправляется автоматически.
- **Зависимости:** IG-01–12. Ожидание clean build измерить отдельно; full-game build не нужен.

### IG-14. Единственный общий gate на RG40XX H

- **Зачем/результат:** проверить архитектурный допуск Dawn/Mali на настоящем коде и получить основание для следующего плана.
- **Зависимости:** IG-13 PASS и **отдельное разрешение на device window/deploy/run** с актуальным адресом/host identity. Текущее согласование подхода таким разрешением не является.
- **Объём:** один bounded сценарий из §5, одна test installation, два поколения в одном процессе. Реальные game installation, firmware, настройки и saves не затрагиваются.
- **Готово:** все критерии §5, очищенная краткая запись результата и cleanup. После PASS готовим план остального переноса; сам PASS не разрешает его автоматически. При timeout/unexplained loss/driver concurrency failure — остановка, не silent serial fallback или замена Dawn.
- **Оценка:** до 1–2 часов активной подготовки/приёмки; само штатное выполнение ограничено ниже. Новые попытки — только в пределах согласованного окна и неизменного сценария, не автоматическая серия экспериментов.

## 5. Конечная device-приёмка

**Ни один из критериев ниже не считается выполненным по CPU tests, сборке, Mock/Null или прежней graphics demo.**

1. Первое поколение: **120** конечных кадров, ровно два imports 640×480. Frame IDs/pixels соответствуют ожиданию; реальные resource/render/compiler calls перекрывались, blocking lookup и backpressure завершились с main progress. No software/Null fallback.
2. В середине последовательности EFB меняется с 640×480 на 320×240 при native 640×480; imports/native storage не меняются, final composition и UI остаются правильными. Это не заявка на live EGLSurface recreation.
3. Normal drain первого поколения завершает accepted work/maps/presentations; наблюдение EGL подтверждает unbind и фактическое destruction **всех** Dawn contexts до export cleanup. Никакого process exit между поколениями.
4. Во втором поколении — та же штатная инициализация и свой пустой cache; **normal Draining** запрашивается при подтверждённо принятой pending render/compile/presentation работе, не после полной idle-паузы (§5.2). Принятые frames и необходимые continuations исполняются, submitted presentations проходят retirement. Abort допустим для rejected root work/ненужного background backlog либо явного failure, **не как замена принятой работы просто из-за normal stop**. Начало/конец работы и состояние при stop сохранены в trace; нет callbacks после destruction owners.
5. В обоих поколениях: read только после forward completion, reuse только после reverse completion + swap; ни потерянных slots/futures, ни случайного mailbox overwrite. Validation/robustness включены. Нет validation/EGL/GL errors, зависших waits/joins или неизвестного teardown.
6. App deadline **60 секунд**, внешний watchdog **90 секунд** на весь сценарий; bounded кадры второго поколения — не более **8**. Не успел/не удалось получить нужное pending/overlap состояние — **не PASS**, критерий не переписывается ради зелёного результата. Нет обещания прервать зависший driver call средствами самого приложения.
7. Только отдельный свежий test directory, без auto-discovery ISO/старых user paths и доступа к cards. Перед запуском/после cleanup проверить firmware hashes по ранее принятому набору; временные файлы/процессы убрать. Пользователь отдельно подтверждает правильную ориентацию/видимость UI и возврат меню после закрытия. Этот gate не является измерением FPS и не проверяет управление/звук/save-load.

**Развилка FAIL:** локальный дефект реализации — исправление внутри того же контракта с повтором того же gate в разрешённом окне. Доказанный отказ context/concurrency/lifetime внутри Dawn/Mali — остановка и предложение изменения архитектуры с evidence. Неясная причина остаётся открытым результатом, а не доказательством невозможности Dawn или разрешением ослабить correctness.

### 5.1. Локальные tests — исполнимый маршрут

Тестовый configure driver frontend в отдельном output `/out/tests-h700`: те же dependencies/toolchain, `AURORA_H700=ON`, `AURORA_ENABLE_TESTS=ON`. Build **explicit targets** `h700_presentation_state_tests` (новый IG-02), `render_worker_tests`, `gx_fifo_tests`, `gfx_recording_tests`; `bloom_readback_slots_tests` — только если затронут его helper. `EXCLUDE_FROM_ALL` сам их не соберёт.

Запуск именно этих GPU-free executables через existing GTest entry points с ограничением времени каждой команды; CTest используется для добавленных им suites с явным фильтром, не `-L port` и не произвольный полный suite. Новые cases проверяют production CPU helpers, не альтернативную реализацию. Docker ARM64 execution допустим для этих CPU tests, не считается runtime Mali/kernel 4.9 evidence. GTest 1.17 уже закреплён и cached, установка не нужна.

Для утверждения «non-H700 guards компилируются» — отдельный configure `/out/compile-upstream` с `AURORA_H700=OFF`, сборка **`aurora_core` и `aurora_gx`**, без graphics запуска. Это compile regression check исходной ветки на том же ARM64 toolchain, не тест всех upstream платформ. Не выдавать единственный H700 configure за эту проверку.

### 5.2. Scheduling сценарий, не неопределённый stress loop

- Каждое поколение получает **свой свежий cache directory**. NoSeed не заменяет этого: он не запрещает читать ранее записанную `pipeline_cache.db`.
- В поколении1: четыре фиксированных GX variants подаются через настоящий FIFO/compiler. Перед реальными CreateRenderPipeline/CreateBuffer/CreateTexture вызовами opt-in hook синхронизирует старт через общий phase barrier; releases дают реальным jobs выполняться. В trace отдельно пишутся `barrier_enter/leave` и интервалы Dawn calls после leave. Для overlap нужны пересекающиеся реальные интервалы compiler и resource/render work, не только hooks. Если наблюдения нет в ограниченном сценарии — результат incomplete, не скрытое увеличение числа variants.
- Backpressure: после принятия первых двух frames test hook задерживает только **выдачу уже безопасного native credit**, продолжая отслеживать forward/reverse completion. При третьем main acquisition `main_progress_once` после четырёх проходов снимает задержку. Main не должен вызывать driver notification или держать queue/FIFO lock. Worker не ждёт этой задержки; не подменяются GPU fence results.
- Поколение2: frame0 уже Submitted, его retirement временно удерживается diagnostic hook. Frame1 принят с новым pipeline; compiler hook удерживает **до** фактической compilation, render continuation ожидает настоящий результат. При наблюдении этих трёх pending состояний main запрашивает normal Draining и снимает обе диагностические задержки, чтобы принятая compilation/frame/presentation штатно завершились. Это доказывает shutdown pending accepted work; реальный overlap проверен отдельно поколением1 и не приписывается искусственному ожиданию.
- Все hooks opt-in только у diagnostic driver, фазовые и ограниченные, без нового универсального scheduler. Failure/deadline снимает собственные barriers, чтобы они не создавали искусственный вечный deadlock. Нельзя снять неизвестный GPU ownership как будто он завершён.

### 5.3. Pixel oracle и fragment data path

Diagnostic scene задаёт ортографический linear fog, белую внутреннюю область на чёрный fog color с GX depth `z=1/4`, start/near 0, end/far 1. `GXSetFogRangeAdj` включён; десять значений таблицы одинаковы:64 (`k=1`), затем128 (`k=2`) в другом контрольном кадре. Uniform coefficients соответствуют `a=1,c=0`; неверные coefficients/данные — failure, не подстройка oracle под считанный результат.

Для EFB width `W`, выбранного fragment x и центра viewport `W/2` независимая формула:

```text
offset = 2 * (x + 0.5) / W - 1
range = sqrt(offset*offset + k*k) / k
fog = clamp(0.25 * range, 0, 1)
RGB = UNORM8(1 - fog), alpha = 255
```

Контроль native точек `(80,200),(240,200),(400,200),(560,200)`; при nearest resampler `x = floor((nativeX+0.5)*W/640)`. Geometry/viewport задают данный GX depth с учётом штатного reversed-Z conversion. Fog LUT/expected pixels driver вычисляет независимо, **не вызывает production `build_fog_range_lut` или shader generator для ожидаемых значений**. Fog допускает заранее ограниченный 1 LSB на float/UNORM conversion; первичные цветные corner/frame markers и opaque ImGui marker проверяются точно. Контрольные точки удалены от raster/marker edges. Nearest resampling и sharpness 0 задаются явно, не зависят от default settings.

ImGui opaque cyan marker имеет native rect `(16,16)–(48,48)`, проверяется в `(24,24)` при обоих EFB размерах; остальная frame-ID/corner разметка асимметрична и не перекрывает fog samples. Проверяются final native slot и presenter backbuffer после blit, до swap. Для GL readback координата преобразуется из top-left в bottom-left (`yGL=479-y`); transport не объявляется проверенным по одному native texture. Несколько контрольных readbacks не являются CPU-путём вывода кадра.

## 6. Учёт и последующий переход

IG-01…IG-10 **PASS**; следующий шаг IG-11, IG-11…IG-13 не начаты; IG-14 не разрешён и не начат. Утверждённые локальные итерации выполняются по dependencies и не требуют повторного согласования той же архитектуры на каждом commit. Device window остаётся отдельным разрешением. Новый dependency artifact, execution policy, расширение scope либо изменение критериев — согласование по workflow.

Полезный объём не равен одной короткой правке: здесь **13 локальных итераций и один общий device-рубеж**, ориентир **13–26 часов активной реализации**, плюс отдельно ожидание сборок/окна и device-приёмка. Это оценка по source seams, не обещание срока; дополнительные устранения новых blockers заранее не считаются включёнными. Нельзя честно назвать весь permanent backend + shader/resources + waits/lifetime работой на один вечер.

До подключения игры следующий план обязан отдельно учесть game-owner safe quiesce до уничтожения `archSupport`, реальные skinning waits, runtime nod на kernel 4.9, бюджет памяти/ресурсов игры, остальные эффекты/UI/input/audio/cards и безопасную установку. Их отсутствие в раннем gate не означает готовность всей игры и не маскируется новым набором предплановых демок.

## 7. Выполненные итерации

### IG-01 — PASS, 2026-11-16

- Добавлены opt-in Linux ARM64 `AURORA_H700`, общий `cmake/h700/InitialCache.cmake`, driver CMake/CLI и offline build helper/manifest. Штатные Aurora source manifests не заменены, root game CMake не менялся. Volatile references на initialize/shutdown удерживают настоящий runtime в link, даже когда выполняется только `--help`.
- Все **14** pinned archives проверены до staging, используются новые source trees, не старые binary directories. Источники `/src:ro`, только `/out` writable, exact image, network none. SDL provenance остаётся local digest, не независимая аутентификация ref. Source overrides обходят vendor patches: для Linux SDL2-backend не применяются Android/Windows/X11 и optional GameCube raw-axis изменения; zlib clang-cl patch не применим.
- Реальные providers проверяются после configure: shared SDL из fork, static Dawn/nod packages, source zlib-ng/zstd. Первый configure выявил directory-local imports nod; frontend импортирует тот же проверенный package через `find_package(... GLOBAL ...)`, без изменения provider/artifact. Отдельный output не удалялся.
- **10 helper tests PASS**, missing/hash failure до staging, safe extraction/fresh output. Clean configure **19,3 с**, build **145,8 с**, compile graph содержит **121** Aurora library translation unit. Сборка target и ELF metadata executable/SDL — PASS; `--help` exit0 без SDL/EGL/Dawn initialization. Это не GPU/firmware ABI/device PASS.
- Команды из корня: `python3 -B tools/h700_integration_driver/test_build.py -v`; `python3 -B tools/h700_integration_driver/build.py --check-inputs`; `python3 -B tools/h700_integration_driver/build.py --output build/h700-integration-ig01-r2 --jobs 2`. Output должен быть свежим.
- Локальные evidence: ignored `build/h700-integration-ig01-r2/{commands.json,environment.json,input-manifest.json,elf-metadata.json}`, `build/providers.txt`, compile commands/build/help logs. Executable SHA256 `606d2d1d173faa0de8f0fd6a441ffca65da7fad509fd332251e99167a1afd2ec`, SDL SHA256 `8df6e362b508d44cad5a253b4f7533ec4d8a976a8e49f49320d2baa42846a13b`.
- Приставка, donor, образ и firmware не использовались/не менялись. Graphics execution CLI пока явно отказывает с exit2; это не реализованная сцена. IG-01 зафиксирован и отправлен: `99ddc286b`.

### IG-02 — PASS, 2026-11-16

- Добавлен CPU-only production helper `aurora::h700::PresentationState`: два credits, FIFO2, immutable generation/frame/slot descriptors, отдельные Submit boundary/publication, forward/reverse/swap transitions, Draining/Failed/quarantine. Notifications содержат owned значения, не удерживают native credit. Не добавлялись GL calls или общий callback framework.
- Подключён к `aurora_core` только при `AURORA_H700`; новый `h700_presentation_state_tests` компилирует тот же production source с GTest/Threads, без GPU.
- **17/17 CTest cases PASS** в exact ARM64 image/Clang16. Fresh output: `build/h700-integration-ig02`, configure tests `AURORA_ENABLE_TESTS=ON`, explicit build target `h700_presentation_state_tests`; `ctest --test-dir /out/build -R H700Presentation --output-on-failure --timeout 15`. CTest run около0,05с. Первый узкий фильтр выбрал16fixture cases; расширенный prefix выполнил все17, включая zero-generation case.
- CPU events не являются проверкой настоящих GPU completions. `try_stop()` подтверждает только отсутствие presentation ownership, не quiescence renderer/compiler; failure-only `stop_after_teardown()` требует доказанного внешним wrapper разрушения поколения и не делает quarantine reusable.
- Локальные logs: ignored `build/h700-integration-ig02/{configure-tests.log,build-state-tests.log}`, CTest `build/Testing/Temporary/LastTest.log`. Read-only independent review существенных issues не обнаружил. Устройство не использовалось. IG-02 зафиксирован и отправлен: `1bd6c80af`.

### IG-03 — PASS, 2026-11-16

- Добавлен постоянный guarded H700 SDL2/Mali/EGL bootstrap, public Dawn external-display/getProc path без WGPU surface/capabilities или CPU/Null fallback. Два fixed640×480 RGBA8 exports/imports, raw `eglMakeCurrent` restoration, один IG-02 state на generation; native slot access проверяет весь descriptor, включая frame.
- Observer учитывает create/share/current bindings/destroy/release-thread и delayed context destruction до unbind. Failed create с известным `EGL_NO_CONTEXT` регистрируется без выдуманного владельца; итог bootstrap определяют реальные Dawn outcomes. Exports/window/resolver не освобождаются при неподтверждённом release всей группы.
- Определены partial-init release и будущие IG-11 release APIs. Review обнаружил возврат success после callback failure при создании pipelines/EFB; исправлено terminal-state check и cleanup реально созданных bootstrap holders. Полный app quiescence ещё не реализован и не заявляется.
- **H700 compile/link/help/version/ELF metadata PASS** в exact ARM64 image. Parent отдельно сконфигурировал `AURORA_H700=OFF` и собрал **полные `aurora_core`/`aurora_gx`**, без graphics execution. Первичная обработка ELF повторного build остановилась на существующем log (защита `open(x)`); повтор с новым каталогом logs PASS, correctness gate не отключался.
- Evidence: ignored `build/h700-integration-ig03/`, `parent-ig03.json` (configure/libraries/H700 link exit0), `parent-final/commands.json`, `parent-slot-build.log`; дополнительные агентские compile/CLI reports. Последний parent rebuild после проверки полного slot descriptor — PASS.
- Frame admission и driver graphics execution пока явно закрыты до IG-06/11/12. Mixed request/layout на момент завершения — следующий IG-04; device test не проводился, настоящий GPU/context teardown PASS отсутствует.

### IG-04 — PASS, 2026-11-16

- Постоянный compile-time `gfx::g_mixedFetch` в `lib/gfx/resources.hpp`: true только для `AURORA_H700`, без environment override/fallback. Настоящий group0: b0 vertex-only R32Uint vertex mirror, b1 vertex-only R32Uint array mirror, b2 fragment-only ReadOnlyStorage того же `Resources::storageBuffer` (whole buffer, offset0). Non-H700 layout сохраняет два прежних bindings/visibility; generator/cache не менялись.
- `RequestDevice` запрашивает vertex SSBO0, fragment SSBO1, sampled16. Общий SSBO limit1 без compute /2 с compute; fragment binding обязателен независимо от `g_noCompute`. Сохраняется запрос supported binding-size/buffer limits U, а не 64 KiB probe. Adapter gate требует defined limits, storage binding >=8 MiB, maxBuffer >=63 MiB (полный scale-1 staging ceiling), texture dimension >=4096. После RequestDevice отдельный chained `Device::GetLimits` проверяет реально выданные compatibility/size limits, в том числе requested sizes, и отказывает при несовместимости без retry.
- Group2 normal/shadow receiver сохраняет все16 штатных sampled textures, без lightmap. Проверка в `gx::initialize` считает реальные созданные descriptors вместе с group0: normal vertex3/fragment16, shadow receiver vertex2/fragment16. Diagnostic logs выводят профиль, stage budgets и granted sizes; это ещё не измерение нового production layout на Mali.
- Прежние zero initialization, row alignment, initial full copies, staging/resident updates, whole-buffer growth copy и deferred reserve сохранены. Центральный `create_static_bind_group` при initialization/growth/reserve берёт b2 из текущего storageBuffer; обе vertex mirrors по-прежнему пересоздаются/обновляются. Дополнительный size guard проверяет фактические allocations после sizing/alignment. `encoding.cpp` не потребовал изменений: его actual buffer-copy и mirror-update call sites покрыты source assertions.
- **6/6 CPU/source GTests PASS**: реальные production descriptors/visibility, подсчёт texture-only entries, mixed request с compute и без, отказ на недостаточных/undefined limits (включая 64 KiB), source hooks current buffer / initialization / staging / resident / growth / reserve, normal/shadow budgets и final-device gate. Tests не создают WebGPU instance/device. Первый прогон5/6 выявил ошибку test fixture (`texture={}` означает Undefined, не BindingNotUsed); fixture исправлен на default unused entry, correctness check не ослаблялся.
- **Actual H700 consumers compile/link/help PASS**, включая `gpu.cpp`, `frame.cpp`, `encoding.cpp`, `gx.cpp` и неизменённый `shader.cpp`; полный incremental `aurora_core/aurora_gx` с `AURORA_H700=OFF` — **PASS**. Использованы existing `build/h700-integration-ig03/{build,compile-upstream}`, exact ARM64 image, offline/read-only `/src`, writable только `/out`. IG-01 snapshot, donor, dependencies/toolchain не менялись. H700 compile/link30,1с; non-H70028,9с; CPU CTest0,03с. CLI без аргументов отдельно вернул exit2 до SDL/EGL/Dawn initialization.
- Воспроизводимые команды внутри того же constrained container: configure existing `/out/build` с `-DAURORA_ENABLE_TESTS=ON`; `cmake --build /out/build --target h700_mixed_fetch_tests --parallel 2`; `ctest --test-dir /out/build -R H700MixedFetch --output-on-failure --timeout 10`; `cmake --build /out/build --target h700_integration_driver --parallel 2`; `cmake --build /out/compile-upstream --target aurora_core aurora_gx --parallel 2`. Fresh logs: ignored `build/h700-integration-ig03/ig04-{contract-commands,r2-commands,upstream-commands}.json`, соответствующие `ig04-*.log`, `ig04-cli-refusal.log`.
- Hooks для IG-05: `gfx::g_mixedFetch`, `MixedFragmentStorageBinding=2`, `MixedSampledTextureBudget=16`, `detail::static_bind_group_layout_entries`; contract/source tests — `extern/aurora/tests/h700_mixed_fetch_test.cpp`. WGSL stage helpers и production cache namespace **ещё не адаптированы**: запуск graphics до следующих итераций закрыт. Device/IG-14 и GPU execution в container не проводились; новая runtime/Mali layout приёмка не заявляется. IG-04 зафиксирован и отправлен: `e600cf150`.

### IG-05 — PASS, 2026-11-16

- Настоящий `gx::build_shader_source` генерирует **один** WGSL module с разными helper families: vertex texture bindings0/1 и fragment `fs_*` storage helpers/binding2. Fog range и PBR room-light rows читают исходный storage buffer с bounds/zero return; фактические TEV registers остаются в `ubuf`, их перенос не придуман. Whole-module texture rewrite не используется для mixed-пути.
- Normal/shadow/cut-caster entrypoints и endian/cross-word fetch сохранены. Применимых integer/flat varyings в текущем source нет, blanket interpolation changes не внесены.
- Production namespace — `pipeline_cache.h700-mixed-v1.db`; старый `pipeline_cache.db` не открывается/не удаляется mixed-путём. Unprofiled bundled/embedded seeds не импортируются; Dawn blob cache не менялся.
- **5/5 CPU generator/cache tests +6/6 IG-04 tests PASS**, actual H700 compile/link/help PASS. Полные non-H700 core/GX compile и **2/2 tests PASS**; шесть hashes generated normal/fog/shadow × storage/texture-fallback совпали с исходным generator. Parent повторил обе CTest группы:11/11 и2/2 PASS. Настоящие WGSL/pipeline validation/execution на Mali остаются IG-14.
- Evidence: ignored `build/h700-integration-ig03/ig05-evidence.json`, `ig05-final-*.log`; exact image, offline `/src:ro`. CLI graphics/default по-прежнему exit2 до initialization, устройство не использовалось. Read-only independent review blockers не обнаружил. Следующая итерация — IG-06.

### IG-06 — PASS, 2026-11-16

- Main резервирует один из двух native credits до `gfx::begin_frame`, возвращает reservation при отказе acquisition. Immutable descriptor захвачен по значению в штатном end-frame callback независимо от очищаемого CPU packet. Существующие resampler, conditional RmlUi и ImGui composing используют постоянный native target; worker не ждёт main/swap.
- Worker фиксирует Submit boundary, выполняет настоящий Submit и публикует `WaitAnyOnly` work-done future. Main проверяет actual wait/callback status, восстанавливает raw presenter context, выполняет blit, создаёт reverse EGL fence, flush и swap. Recycle требует reverse completion и successful swap; failures/quarantine сохраняют pending ownership/futures/fences. `after_present()` — только статистика после success.
- Независимый review обнаружил, что закреплённый SDL shim безусловно возвращает true после SDL2 swap. Исправлено внутри того же контракта: `eglSwapBuffers(display, draw)` на неизменной SDL-owned surface с проверкой `EGL_TRUE`; отказ немедленно сохраняет `eglGetError`, quarantine и return до success transitions. Provider, surface ownership и зависимости не менялись; source regression закрепляет эту ветку. Повторный review blocker закрыл.
- **23/23 CPU/source tests PASS** (17 state +6 integration), actual H700 driver compile/link PASS, full non-H700 core/GX compile PASS (повторно16 core +60 GX TU), CLI help/version exit0 и graphics/default refusal exit2. Parent повторил `ctest --test-dir /out/build -R H700Presentation --output-on-failure --timeout 10`:23/23 PASS. `git diff --check` PASS. Exact offline ARM64 image, `/src:ro`, writable только `/out`; GPU/device не запускались.
- Evidence: ignored `build/h700-integration-ig03/ig06-swap-return-{commands,handoff,nonh700-inventory}.json` и соответствующие logs. `pending_native_frames()` даёт bounded inventory для IG-11; `release_exports()` отказывает при pending records. Общие waits IG-07…09 и full quiescence/retirement IG-11 ещё впереди. Правильность pixels/orientation/fences и фактический teardown остаются IG-14; graphics CLI закрыт до lifecycle/scene. Следующий шаг — IG-07.

### IG-07 — PASS, 2026-11-16

- Queue push/sync, frame-slot/staging/map и growth waits теперь status-bearing и используют один main-only direct progress hook вне внутренних locks. `Completed/Rejected/Failed` различаются; mutation paths не продолжаются после failed wait. Отдельные root admission/drain и downstream close сохраняют accepted continuations; остановленный worker не выполняет rejected graphics inline.
- Partial slot acquisition защищён leases. `MapSlotOwnership` переносит release obligation отменённого waiter в internal completion с exactly-once release. Принятые packets/captures сохраняются до завершения jobs; pre-submit abort возвращает native reservation без success callback. Growth получает encoder по ordered packet на worker, не через стековую ссылку или преждевременный handle.
- Hooks следующих итераций: `wait_healthy/progress_wait`, status-bearing `synchronize/try_gpu_synchronize`, `begin_draining/close_admission/is_quiescent`, packet/map inventory и explicit bootstrap reset. `is_quiescent` относится только к joined worker и его obligations, не всему поколению. Public afterSubmit/GX draw-done semantics не заменены.
- **54/54 H700 targeted CPU/source tests PASS** (11 новых cases), **25/25 non-H700 CPU regressions PASS**; actual H700 driver compile/link и full non-H700 core/GX recompile (16+60 TU) PASS. Parent повторил H70054/54; independent read-only review blockers не нашёл. CLI help/version0, graphics/default2, diff check PASS.
- Evidence: ignored `build/h700-integration-ig03/ig07-final-{handoff,commands,nonh700-inventory}.json`, соответствующие logs. Exact offline image, без GPU/device. Fixtures/stubs не доказывают Mali threading/runtime. Actual FIFO/compiler waits и full generation retirement остаются IG-08/09/11. Следующий шаг — IG-08.

### IG-08 — PASS, 2026-11-16

- Настоящие FIFO drain/growth/reset возвращают status; main try-lock/retry вызывает progress вне `sBufferMutex`. Terminal failure запрещает дальнейшие append/patch/publication, не очищает используемый buffer и не выдаёт незавершённый range за processed. Необходимые Aurora/recording/effect callers прекращают зависимый путь при failed drain.
- Draining закрывает следующий frame root, сохраняя accepted frame continuations. Token ordering/unpublished tail и draw-done return-before-drain сохранены; processor публикует watermark только после callback return. `shutdown()` не освобождает inputs до фактического выхода processor, `is_quiescent()` означает только FIFO join. Для IG-11: accepted FIFO producers/tail drain завершаются до downstream close.
- **260/260 FIFO CPU tests PASS в обоих профилях**, включая6 новых cases и настоящий FIFO→BoundedQueue backpressure/progress-outside-lock; H700 wait/source31/31 и non-H700 wait25/25 PASS. H700 driver compile/link, full non-H700 core/GX recompile16+60TU PASS. Parent повторил FIFO260/260; independent read-only review blockers не нашёл. CLI0/0/2 и diff check PASS.
- Evidence: ignored `build/h700-integration-ig03/ig08-reviewed-{handoff,commands,nonh700-inventory}.json`, logs. Exact offline image; GPU/device не запускались. Game skinning не затронут, compiler waits/full teardown — IG-09/11, runtime доказательств пока нет. Следующий шаг — IG-09.

### IG-09 — PASS, 2026-11-16

- H700 blocking pipeline lookup отпускает cache mutex до health/main progress; `Ready/Failed/Cancelled` различаются, failure не выдаёт pipeline за готовый. Сохранён независимый compiler U, один поток для OpenGLES, не render-queue job.
- Normal Draining закрывает writer/prefetch/background roots, отменяет необязательный queued backlog и сохраняет foreground builds/continuations принятых кадров. Demand promotion не обходится memo shortcut. Active compile/captures остаются учтены до фактического возврата; cleanup выполняется после настоящих compiler/writer joins.
- Hooks: `begin_pipeline_cache_draining()` закрывает background roots; `drain_pipeline_cache()` вызывается только после upstream producer quiescence и закрывает continuations. False не означает quiescence; `active_pipeline_compiles()`/`pipeline_cache_threads_joined()` не заменяют release всей generation.
- **8/8 CPU wait/outcome/lifetime/source tests PASS**, H700 wait/recording/generator30/30, non-H70035/35 и FIFO260/260 в обоих профилях PASS. Actual H700 driver link и full non-H70016+60TU core/GX compile PASS; CLI0/0/2, diff check PASS. Parent повторил compiler8/8; independent read-only review blockers не нашёл.
- Evidence: ignored `build/h700-integration-ig03/ig09-final-{handoff,commands,nonh700-inventory}.json`, logs. Exact offline ARM64 image, без GPU/device. Настоящие Mali compilation/overlap/active-call stop остаются IG-14; зависший driver call не считается завершённым по timeout. Следующий шаг — IG-10.

### IG-10 — PASS, 2026-11-16

- Request-owned `MapCompletion` учитывает pending/executing callback, one-shot outcome и retirement. Staging проверяет identity; profiler/vis-query callbacks не захватывают переиспользуемый slot. Cancelled waiter не равен completion. `pending_internal_maps()` покрывает staging/profiler/vis-query выбранного пути.
- `retire_frame_resources_after_quiescence()` main-only после FIFO/render joins обслуживает callbacks без нового Submit, отказывает при pending/executing, затем unmap/reset и освобождает abandoned recorder/packets. Полный recorder reset включает suspended-pass holders. Profiler reset также удаляет QuerySet copies в `PassTimestampWrites`; vis-query reset guarded, lazy queries для теста не включались.
- Source inventory выбранного пути (не live GPU allocation/release evidence):

| Владелец | Создание / release point |
| --- | --- |
| Staging | 5 buffers/maps при init, remaps после кадров; callback settle/unmap/records retirement → buffers в `gfx::shutdown` |
| Packets/recorder | Encoder, ops, targets/captures, snapshot/offscreen pools; normal completion либо reset после joins/maps без finish/enqueue |
| GPU profiler | Conditional TimestampQuery bootstrap: QuerySet/resolve/4 readbacks даже при runtime profiling OFF; guarded shutdown и reset descriptor refs |
| Vis-query | Lazy3 query/resolve/readback slots и pipelines; poll без Submit, guarded shutdown всех holders/results/registration |
| Shared/mixed buffers | 4 shared buffers,2 mirrors/views, groups/layouts; существующий reset в `gfx::shutdown` после retirement |
| Copy conversion | Bootstrap pipelines/layouts/samplers, conditional/lazy families; полный `tex_copy_conv::shutdown` |
| Palette conversion | 3 bootstrap pipeline/layout pairs + sampler; `tex_palette_conv::shutdown` |
| Depth-peek | Conditional core pipeline/layout; `depth_peek::shutdown`; optional snapshot/maps не активированы и вне map inventory |
| GX | Empty texture/group, samplers/layouts и actual texture/copy caches; `gx::shutdown` + texture shutdown |
| Resource/pipeline cache | Demand groups/samplers/pipelines/captures; cache reset после compiler/writer joins |
| WebGPU | Copy/resampler/EFB и worker-target copies; partial-init holder release либо runtime shutdown; final generation order — IG-11 |
| ImGui/native | Backend/font/textures/frozen captures и2 imports; ImGui shutdown после jobs, native release — IG-11 |

- **10/10 новых retirement CPU/source cases PASS**, H700 related50/50, non-H70052/52, FIFO260/260 в обоих профилях; H700 link и full non-H70016+60TU compile PASS. Parent повторил10/10; independent read-only review blockers не нашёл. CLI0/0/2, diff check PASS.
- Evidence: ignored `build/h700-integration-ig03/ig10-final-{handoff,commands,resource-inventory,nonh700-inventory}.json`, logs. Exact offline image, без GPU/device. Screenshots/selftest/overdraw/bloom/optional depth readbacks не включались. Пустые GPU holders в CPU fixture не доказывают release созданных объектов; final share-group release — IG-11, hardware proof — IG-14. Следующий шаг — IG-11.
