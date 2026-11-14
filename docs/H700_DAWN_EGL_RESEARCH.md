# PF-01: Dawn и firmware EGL — исследование исходников

**Дата:** 2026-11-14. **Статус:** найден точный source snapshot, выполнен статический разбор; runtime-совместимость с RG40XX H не проверена. Это материал для [предплановых проверок](H700_PREFLIGHT.md), не утверждённая архитектура переноса. Позднее по разрешению пользователя подготовлена [изолированная демка](H700_GRAPHICS_PROBE.md); прежний Bookworm link blocker снят отдельным [ABI-matched builder](H700_BUILD_ENVIRONMENT.md), compile/link и безопасные container modes прошли. Device graphics остаётся непроверенной.

## 1. Достоверность источника

Пользователь предоставил [Timdeuces/dawn](https://github.com/Timdeuces/dawn). GitHub API подтвердил наличие **точного ref**, заданного в `AuroraDependencyVersions.cmake` новой базы:

- commit: `118436b3f8f68583eb86e811ca605dc41637e8cc`;
- tree: `08778b0fc542428e267e1d77dd8ff8fc781770e7`;
- commit message: `opengl: load libEGL.so.1 before libEGL.so`;
- [проверка commit через API](https://api.github.com/repos/Timdeuces/dawn/commits/118436b3f8f68583eb86e811ca605dc41637e8cc).

Таким образом, получен source ref **существующего pin**, а не предложена замена Dawn другой версией. Ранее проверенный cached ARM64 archive совпадает с package SHA-256 из конфигурации U; это отдельная проверка integrity package, не результат воспроизводимой пересборки и сравнения бинарников.

Исходники прочитаны по HTTP с commit-pinned URLs. Полный repository не клонировался, зависимости не пересобирались, provider URLs в CMake не менялись. Исходные 404 по адресам `Odrannnn/dawn` остаются историческим фактом, но больше не блокируют доступ к необходимым source files.

## 2. Вывод для выбора подхода

**Новый renderer нельзя заранее отвергать из-за EGL 1.4 или отсутствия X11/Wayland.** Dawn этого snapshot имеет штатный native API для внешнего EGLDisplay и импорта EGLImage. Поэтому маленькая проверка новой графики с уже работающим firmware presenter имеет основание.

Однако этот API не означает заимствование SDL context и не делает обмен кадрами автоматически синхронизированным. Статически снят вопрос наличия механизма; доступность firmware extensions, фактическая работа обмена и production lifetime/threading остаются отдельными проверками.

## 3. Подтверждённые контракты

| Вопрос | Ответ по исходникам | Следствие для демки |
| --- | --- | --- |
| Можно передать готовый EGLDisplay? | `RequestAdapterOptionsGetGLProc` принимает `display` и `getProc`; backend передаёт их в `CreateFromProcAndDisplay` | Использовать firmware display/procs, без X11/Wayland surface и незаметного выбора другой EGL library |
| Кто завершает display? | `DisplayEGL` не вызывает `eglTerminate` в деструкторе, но загрузка display procs вызывает `eglInitialize` | Display должен оставаться валиден; отдельно проверить cleanup вместе с SDL |
| Нужен EGL 1.5? | Базовый минимум — **EGL 1.4** | Строка EGL 1.4 не отвергает путь, но extensions/procs всё равно обязательны |
| Нужен ANGLE? | Нет общего обязательного требования; ANGLE-specific paths включаются условно | Не поставлять ANGLE как якобы уже необходимую библиотеку |
| Используется SDL EGLContext? | Dawn создаёт собственные contexts; публичная структура выше context не принимает | Не предполагать общую share group с SDL по умолчанию |
| Нужен surfaceless? | При отсутствии extension создаётся 1×1 pbuffer | Проверить реальные configs; pbuffer path предусмотрен |
| Можно импортировать EGLImage? | Есть native `WrapExternalEGLImage` | Кандидат для фиксированной RGBA8 texture, не универсальный импорт любых formats |
| Работает ли общий STM EGLImage API? | GLES `ImportSharedTextureMemoryImpl` этого snapshot реализует AHardwareBuffer-ветку, а не обычный EGLImage descriptor | Не планировать `SharedTextureMemoryEGLImage` только по наличию общей декларации в headers |
| Можно просто передать GLuint? | `WrapExternalGLTexture` требует ANGLE texture-sharing path | Не считать его запасным способом обмена независимых firmware contexts |

Resolver должен возвращать не только extension functions, но и запрашиваемые core EGL entrypoints. Нельзя без проверки приравнивать этот contract к возможностям голого firmware `eglGetProcAddress`. Время жизни resolver/library не передаётся Dawn автоматически.

### Требования обнаружения adapter/context

- Feature level — **Compatibility**.
- Проверяется context robustness; для EGL 1.4 существенен `EGL_EXT_create_context_robustness`.
- Discovery принимает fence-sync или reusable-sync, но в последующих sync helpers есть различие KHR/core paths. Для первого консервативного опыта на EGL 1.4 требуется проверить **`EGL_KHR_fence_sync`**, не объявлять reusable-only достаточным.
- Фактически созданный GLES context должен быть не ниже 3.1. Для полученного GLES 3.1 код дополнительно проверяет `GL_EXT_color_buffer_float`. Исторический ES 3.2 другого context не заменяет эту проверку.
- Без no-config-context extension выбирается RGBA8 config с window bit даже для offscreen пути, затем при необходимости pbuffer config. Проверить оба условия на firmware.

Это требования конкретного Dawn snapshot, **не свежий extension dump приставки**.

## 4. Минимальный путь импорта и синхронизации

Для исследовательского discriminator кандидат — native `WrapExternalEGLImage`:

1. Presenter создаёт фиксированную RGBA8 texture/EGLImage на том же EGLDisplay.
2. Dawn создаёт alias через `glEGLImageTargetTexture2DOES`.
3. Dawn рисует простой рисунок, ожидается успешное завершение соответствующей queue work.
4. Явно восстанавливается presenter context; проверяются контрольные pixels и выполняется показ.
5. Чтение presenter завершается до следующей записи в image.

Ограничения wrapper: 2D, один mip/layer, sample count 1; проверяются размеры. Соответствие действительного GL format объявленному WebGPU format полностью не валидируется. Для демки ограничиться `GL_RGBA8` / `RGBA8Unorm`, без sRGB/MSAA/reinterpretation и изменения storage.

Legacy wrapper не предоставляет STM Begin/EndAccess с acquire/release fences. Сам импорт `SharedFence` не связывает его с legacy texture. **`Submit` не означает GPU completion.** Для минимального correctness-опыта рассмотреть последовательную передачу: дождаться успешного work-done с обслуживанием callbacks/events и завершить внешнее GL-чтение перед reuse. Это заведомо не окончательная оптимизированная кадровая схема и не измерение 30 FPS.

Внутренние EGLImage tests Dawn показывают clear/sampling/validation, но создают image в контексте самого Dawn. Они не доказывают межконтекстный SDL/Mali обмен.

## 5. Что ещё нельзя считать решённым

1. **Lifetime импортированного alias.** EGLImage import создаёт GLuint, но examined path передаёт `OwnsHandle::No`; `Texture::DestroyImpl` удаляет handle только при `Yes`. Полный безопасный путь освобождения alias для production пока не определён. Не создавать wrapper на каждый кадр и не выдавать выход процесса за готовую политику resource recreation. В подготовленной демке использован один фиксированный импорт на полный lifetime отдельного device/instance с проверкой уничтожения наблюдавшегося Dawn context после освобождения всех ссылок. Этот ограниченный shutdown обоснован статически, но на устройстве ещё не проверен; он не решает освобождение каждого импорта при живом production device.
2. **Context/thread topology.** Toggle, разрешающий использование context с разных потоков, затрагивает scoped current-context behaviour и может создавать дополнительные pipeline contexts. Его нельзя включать как безусловный «фикс SDL sharing». Согласовать минимальную топологию опыта и способ восстановления presenter context.
3. **Firmware capabilities.** Robustness, fence/image extensions, core proc lookup, configs и фактическая видимость storage требуют проверки на устройстве. Ранее работающий donor bridge не доказывает всех требований Dawn.
4. **Package composition.** Отдельная демка теперь слинкована с native display/import APIs этого package и прошла container CPU smoke tests. Это закрывает API link check в её объёме, но не firmware loader/runtime или все требования полной игры.
5. **Покрытие игры.** Успешный узор на экране не доказывает все GX effects, интерфейс, sound/save paths или производительность. Они остаются частью последующего плана переноса.

Блокировка «нет исходников нужной версии» **снята**. PF-01 не следует автоматически отмечать полностью завершённым: нужны окончательная фиксация минимального ownership/sync/cleanup сценария и согласование дальнейших probes. Production-вопросы, не покрываемые маленькой демкой, должны быть явно закрыты/учтены до утверждения плана реализации, а не оставлены как неоговорённые изменения по ходу.

## 6. Первичные source files

Все ссылки фиксируют одну ревизию, независимо от будущего HEAD зеркала:

- [OpenGLBackend.h](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/include/dawn/native/OpenGLBackend.h) — публичный native display/import API.
- [BackendGL.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/BackendGL.cpp) — adapter discovery и обязательные проверки.
- [DisplayEGL.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/DisplayEGL.cpp), [EGLFunctions.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/EGLFunctions.cpp) — display lifetime, версии, configs и resolver contract.
- [ContextEGL.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/ContextEGL.cpp) — создание/уничтожение contexts, pbuffer, scoped current.
- [PhysicalDeviceGL.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/PhysicalDeviceGL.cpp) — GLES requirements и внутренняя context topology.
- [DeviceGL.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/DeviceGL.cpp), [TextureGL.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/TextureGL.cpp) — EGLImage wrapping, ограничения, handle ownership.
- [UtilsEGL.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/UtilsEGL.cpp), [QueueGL.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/QueueGL.cpp), [Queue.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/Queue.cpp) — sync helpers и queue completion.
- [SharedTextureMemoryGL.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/SharedTextureMemoryGL.cpp), [SharedFenceEGL.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/opengl/SharedFenceEGL.cpp) — отличия STM/fence paths от legacy import.
- [EGLImageWrappingTests.cpp](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/tests/white_box/EGLImageWrappingTests.cpp) — доступные tests и границы их доказательства.

Новых graphics probes, сборки или обращений к приставке при этом исследовании не было. Код переноса и dependency pins не изменены.
