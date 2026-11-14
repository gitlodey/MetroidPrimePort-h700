# H700 — отдельная демка Dawn / SDL2 / EGLImage

## Текущий статус

**Демка собрана в новом ARM64 Docker image; ENV-01/02 прошли 2026-11-14.** Прежний link blocker Bookworm устранён без изменения Dawn/firmware. 43 unit tests, compile/link, ELF/ABI gate и безопасные контейнерные modes — PASS. На RG40XX H / muOS 2601.0 Jacaranda прошли `--help`, CPU self-test, offscreen (2 кадра) и bridge (120 кадров), включая pixel/order и dedicated-context teardown assertions. Визуальное подтверждение пользователя пока не получено. Результаты — раздел 8. Подробности окружения и его EOL-ограничения — [ENV-01/02](H700_BUILD_ENVIRONMENT.md).

Код игры, root CMake, dependency pins и донор не изменялись. Первоначальная подготовка демки не включала deploy/graphics; затем пользователь отдельно предоставил свободную приставку и разрешил тест. Использовалась временная установка в `/tmp`, после теста удалённая. Исходный read-only SSH inventory описан в [runtime/ABI](H700_RUNTIME_ABI.md). Любой следующий запуск требует актуального разрешения на окно; успешный тест не разрешает начать перенос.

## 1. Что реализовано

Исходники — [tools/h700_graphics_probe/](../tools/h700_graphics_probe/):

- `probe.cpp` — два явных графических режима; без `--mode` графика не запускается.
- `pattern.hpp` — ожидаемые pixels и синтетический WGSL shader: четыре цветных квадранта и меняющийся белый/пурпурный маркер.
- `CMakeLists.txt` — изолированная Linux ARM64 сборка, без подключения game sources.
- `build.py` — offline Docker build, проверка inputs, отдельный output, ELF/ABI gate и asset-free staging.
- `test_build.py` — tests проверок архива, безопасных путей, публикации stage и разбора ELF/ABI.
- `Dockerfile.abi238`, `.dockerignore`, `verify_environment.py` — отдельный narrow-context builder и gate его runtime.

### Режимы

| Режим | Что должен проверить на устройстве |
| --- | --- |
| `--mode offscreen` | SDL2/Mali bootstrap, аппаратный Dawn OpenGLES Compatibility device, WGSL render и CPU-проверка GPU readback |
| `--mode bridge` | Постоянный presenter EGLImage → native Dawn wrapper → GPU draw → ожидание completion → чтение контрольных pixels и GPU blit/swap |
| `--help`, `--self-test` | Без SDL/EGL/Dawn initialization; второй проверяет CPU pattern/arguments/extension token logic |

**Оба графических режима создают SDL video window и могут занять экран**, включая offscreen: окно нужно для проверенного firmware bootstrap. Нельзя считать offscreen пассивным inventory.

Выбор adapter ограничен OpenGLES/Mali, проверяются 4096 texture dimension и 64-byte immediates baseline, ошибки validation/device loss не отключаются. Эти два limits не заменяют проверку полного набора требований U. Фильтрация по Mali-G31 намеренно ограничивает поддерживаемое устройство.

## 2. Границы эксперимента и lifetime

- Один управляющий поток, одна импортированная RGBA8 texture на всё время жизни отдельного Dawn device. Нет resize/reimport и дополнительной многопоточной topology U.
- `Submit` не считается завершением: используется queue work-done и `WaitAny`; перед повторной записью завершается чтение presenter.
- GL pixels считываются только для диагностики; вывод не делает CPU readback/upload всего кадра. Разница физического порядка строк фиксируется и учитывается явно при blit, а не скрывается исправлением исходных pixels.
- SDL TLS не узнаёт о прямых переключениях context внутри Dawn. Presenter восстанавливается через `eglMakeCurrent` с исходными draw/read surfaces и проверкой действительного current context, не через потенциально short-circuit `SDL_GL_MakeCurrent`.
- Известный legacy-import alias не освобождается как отдельный GL object публичным API Dawn. Для ограниченной демки освобождаются все WebGPU references и всё отдельное device/instance; проверяется уничтожение наблюдавшегося Dawn EGLContext, затем освобождаются ресурсы presenter и SDL.
- Это **не решение production per-import lifetime**: сохранение/пересоздание resources при живом device по-прежнему требует отдельного решения в плане переноса.
- Для GPU work задан timeout, последовательность ограничена по времени и числу кадров. Это не гарантирует возврат из зависшего firmware driver call; перед device run нужен согласованный внешний timeout и процедура завершения только демки.

## 3. Сборка и защита inputs

Проверенный image — `metroid-h700-probe:abi238-v1`; helper фиксирует его immutable image reference. Старый `metroid-muos-build:bookworm-20261104` сохранён, но не используется для этой демки. Source project/cache mounted read-only, отдельный output writable, сеть отключена, автоматические pull/install отсутствуют.

SDL пересобирается из того же source snapshot `6057d79baf8321bf190479a699655f06cc2a962f`, старый `sdl-build` binary не используется. Cached SDL source tree проверяется побайтно против archive; его SHA-256 — наблюдённый digest локального input, не независимая аутентификация GitHub provenance. Dawn archive проверяется против SHA-256 из pin U и сверяется с extracted tree.

Используется штатный `SDL_UNIX_CONSOLE_BUILD=ON`: для firmware SDL2/Mali намеренно не нужны X11/Wayland. Их отсутствие не исправляется установкой desktop stack.

Безопасные команды из корня проекта:

```sh
python3 -B tools/h700_graphics_probe/build.py --help
python3 -B tools/h700_graphics_probe/build.py --check-inputs
python3 -B tools/h700_graphics_probe/test_build.py
```

Успешно проверенный вызов build helper (image предварительно создаётся по [инструкции окружения](H700_BUILD_ENVIRONMENT.md)):

```sh
python3 -B tools/h700_graphics_probe/build.py --output build/h700-graphics-probe-abi238-final --jobs 2
```

Output должен быть новым `build/h700-graphics-probe` либо отдельным sibling с суффиксом; чужой cache/package не перезаписывается. Неудавшиеся попытки сохранены отдельно, не маскируются как готовая сборка.

Проверки и сборка выполняются в `package.partial`; только после всех gates, штатного `tools/collect_licenses.sh` и записи manifest stage атомарно переименовывается в `package`. Итог: executable, своя SDL3, manifest, licenses и metadata builder. Firmware EGL/GLES/SDL2 и container libc/libstdc++ не копируются. Проверяются AArch64, loader, реальные `NEEDED`, version nodes каждого provider и пять выбранных exact imports против firmware snapshot; остальные symbols целиком не проверены. Container `ldd` не заменяет runtime на muOS и SDL2 `dlopen`.

## 4. Реальные результаты подготовки

| Проверка | Результат |
| --- | --- |
| Python build/helper tests | **43/43 PASS** |
| Source/package inputs | SHA-256 archives и exact tree matches проверены |
| C++ compilation | Clang16 C++20 `-Wall -Wextra -Wpedantic -Werror -fsyntax-only` — PASS; объект probe также собран в Ninja |
| SDL build | Shared SDL fork собрана из исходников для ARM64 |
| Финальная линковка | **PASS** в ABI-matched Mantic builder; прежняя Bookworm-ошибка сохранена ниже |
| ELF / firmware metadata gate | **PASS** для executable и SDL: NEEDED, version nodes и пять exact imports; не полная проверка всех symbols |
| Executable `--help` / `--self-test` | **PASS** в контейнере, без SDL/GPU initialization |
| Container `ldd` | **PASS**, без неразрешённых dependencies; это не firmware loader test |
| Manifest hashes / licenses | **PASS**; package проверен, штатный collector выполнен |
| Device runtime / offscreen / bridge | **PASS в ограниченном сценарии**, см. раздел 8; визуальная оценка пока pending |

Исправлены две локальные ошибки реализации демки: repeatable callback использует typed userdata с достаточным lifetime; восстановление presenter не полагается на устаревший SDL TLS cache. Дополнительное ревью выявило двойное удаление presenter context в pinned SDL2 backend: теперь после GL cleanup выполняется проверяемое отсоединение context, а единственное удаление остаётся за `SDL_DestroyWindow`. SDL snapshot не изменён. Публикация stage перенесена после всех gates; добавлены regression tests. Эти исправления не меняют выбранную библиотеку или архитектуру эксперимента.

Попытки:

1. `build/h700-graphics-probe`: configure остановлен desktop guard SDL; добавлена штатная console-build опция.
2. `build/h700-graphics-probe-try2`: выявлен неверный include отдельного proc-dispatch API, которого нет в данном monolithic package. Удалён ненужный вызов; используется публичный WebGPU API, как в U.
3. `build/h700-graphics-probe-try3`: SDL и probe object собраны; последний link step не прошёл.

Существенные ошибки **исторической Bookworm-попытки**:

```text
libwebgpu_dawn.a(PhysicalDeviceGL.cpp.o): undefined reference to __isoc23_strtoul
libwebgpu_dawn.a(...): undefined reference to __isoc23_strtol / strtoll / strtoull
libwebgpu_dawn.a(...): undefined reference to std::__cxx11::basic_string<...>::_M_replace_cold(...)
```

Это доказательство несовместимости готовой библиотеки с текущим Bookworm build runtime. **Последующий read-only SSH подтвердил все пять конкретных symbols на firmware**: C23 parsing — `GLIBC_2.38`, string helper — `GLIBCXX_3.4.31`. Container предоставляет glibc 2.36 / `GLIBCXX_3.4.30`, firmware — glibc 2.38 / `GLIBCXX_3.4.32`. См. [фактический runtime/ABI](H700_RUNTIME_ABI.md).

Обновлять firmware для этих symbols не требуется. В новом builder executable слинкован и прошёл описанный metadata gate, а затем loader и оба graphics modes прошли на firmware в ограниченном сценарии раздела 8. Полный аудит всех imports и всех возможностей Dawn/игры не выполнен; наличие пяти functions само по себе не является универсальной гарантией. Пользователь затем согласовал новое build environment; изменение выполнено отдельно по [ENV-01/02](H700_BUILD_ENVIRONMENT.md), без смены Dawn или firmware.

## 5. Обоснование согласованного изменения окружения

После link failure работа была остановлена. Рассматривались варианты:

| Вариант | Плюсы | Минусы / неизвестные |
| --- | --- | --- |
| Пересобрать **тот же Dawn ref** под согласованный baseline | Контролируем требования и flags, сохраняем исходную версию; не зависим от более нового окружения автора prebuilt | Отдельная сборка крупной зависимости, downloads/disk/time; требования compiler и её dependencies нужно проверить заранее |
| Подготовить совместимый toolchain/sysroot для использования prebuilt | Можно сохранить готовый архив и сократить собственную сборку Dawn | Новый build profile и provenance, строгая проверка не только glibc, но C++/transitive ABI; простой переход на более новую distro может сделать artifact несовместимым с firmware |

Не добавляются локальные подмены C/C++ runtime functions, не копируются новые системные библиотеки на приставку и не меняется версия Dawn ради зелёного link. После SSH-проверки firmware пользователь разрешил новое build-окружение: выбрано отдельное native ARM64 окружение с glibc 2.38 и GCC13 runtime. Детали и EOL-компромисс Mantic builder записаны в [решении ENV-01/02](H700_BUILD_ENVIRONMENT.md) до изменения build-кода.

## 6. Готовый локальный artifact

Актуальный stage: `build/h700-graphics-probe-abi238-final/package/`. Первая Mantic-сборка `build/h700-graphics-probe-abi238` предшествует финальным cleanup/staging fixes и **не используется** для device test. Старые outputs не перезаписывались.

| Artifact | Размер, bytes | SHA-256 |
| --- | --- | --- |
| `h700_graphics_probe` | 13578016 | `ea1891ec2a41ea1dbb88b7adbe9dab1fbad8f53443b07c8d060f78ed262f3674` |
| `libSDL3.so.0` | 2771560 | `ffb36e54c7c25f17f788f74550fcfd7da7136a66fa1e24771497b0a18f134e70` |
| `manifest.json` | — | `21b5ef4d82a8cd76de8f74e6c86f028e87c1783e0c490703ca4722615c0c04db` |

Executable требует максимум `GLIBC_2.38`, `GLIBCXX_3.4.32`, `CXXABI_1.3.8`; SDL — `GLIBC_2.38`. Loader: `/lib/ld-linux-aarch64.so.1`, RUNPATH: `$ORIGIN`. Прямые `NEEDED` executable: `libSDL3.so.0`, `libEGL.so.1`, `libGLESv2.so.2`, `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libc.so.6`; у SDL — только `libm.so.6`, `libc.so.6`. Firmware SDL2/Mali runtime проверен в graphics-сценарии раздела 8; audio/input backends и другие динамически открываемые paths этим не проверены.

## 7. Device test — согласованный сценарий

Пользователь предоставил свободную приставку и явно разрешил тестирование. Используется только финальный artifact из раздела 6; игра, renderer и firmware не меняются.

1. Verified SSH с обязательной проверкой host key; read-only проверка архитектуры, доступной RAM, `/tmp` и отсутствия запущенной игры/демки.
2. Новый уникальный test directory в `/tmp`, без установки в Ports. Передача полного asset-free package, проверка manifest и каждого SHA-256 на устройстве. HOME/config/cache/tmp демки изолированы внутри test directory; shell environment и пользовательские настройки не меняются.
3. На firmware выполнить `--help` и CPU `--self-test` с `LD_LIBRARY_PATH`, указывающим только на bundled SDL3. Это loader smoke check, не graphics pass.
4. `SDL_VIDEODRIVER=sdl2`, `SDL3SHIM_SDL2_VIDEODRIVER=mali`; запуск `--mode offscreen --frames 2` с внешним timeout 35 секунд. При ошибке bridge не запускать, сначала зафиксировать причину.
5. Только при успешном offscreen — `--mode bridge --frames 120`: около 12 секунд смены маркера, не FPS benchmark. Отдельный timeout 35 секунд. Pixel/order/teardown assertions обязательны; визуальная оценка пользователя — отдельное свидетельство.
6. Runner запускает демку в отдельной process group. По timeout посылает TERM, через 5 секунд KILL только этой группе; не завершает menu/system services/игру. Зависший kernel driver call этим не гарантированно исправляется; при такой ситуации тесты прекращаются.
7. После теста проверить отсутствие процесса демки, сохранить очищенный результат в документации. Cleanup ограничен созданным test directory, без удаления чужих файлов, caches/settings/saves. Никаких core dumps или raw environment в Git.

Сценарий выполнен; результаты ниже. Ни сборка, ни CPU self-test сами по себе не доказывают графическую совместимость, корректность firmware cleanup или FPS игры.

## 8. Результаты на RG40XX H / Jacaranda — 2026-11-14

**Проверен именно artifact из раздела 6**, без пересборки или изменения dependencies. На устройстве перепроверены manifest и SHA-256 всех его файлов: 49 files вместе с manifest — PASS. SSH host-key verification не отключалась. До запуска игра/другая демка не обнаружены; доступно около 779 MiB RAM, swap отсутствует — разовое наблюдение, не memory budget игры.

| Проверка | Результат | Wall time всего процесса |
| --- | --- | --- |
| `--help` | exit 0, firmware loader загрузил executable/SDL3 | 0.050 s |
| `--self-test` | exit 0, CPU pattern/arguments/extensions — PASS | 0.051 s |
| `--mode offscreen --frames 2` | exit 0, hardware adapter/device, WGSL full-pattern readback и teardown — PASS | 0.751 s |
| `--mode bridge --frames 120` | exit 0, EGLImage alias, семь diagnostic pixels на кадр, порядок обновлений, GL blit/swap и teardown — PASS | 13.059 s |

Timeout не сработал; TERM/KILL не потребовались. Core dumps были запрещены через `RLIMIT_CORE=0`. Применялись только per-process `LD_LIBRARY_PATH`, два SDL driver settings и изолированные HOME/XDG/TMPDIR; firmware/system settings не менялись.

Наблюдённые параметры:

- SDL runtime `3005000` (3.5.0), SDL3 backend `sdl2`, нижний firmware driver `mali`.
- Renderer и Dawn adapter: **Mali-G31**, OpenGLES Compatibility, не CPU/software backend.
- Firmware GLES: **OpenGL ES 3.2**, driver `v1.r20p0-01rel0.8ec337de4ddab3365a32cff58063a9da`; EGL **1.4**, `Bifrost-"r20p0-01rel0"`.
- Presenter drawable **640×480**. Сам синтетический render target — **64×64**, затем GPU blit в drawable; это не проверка игровой нагрузки при полном разрешении.
- Adapter сообщает `maxTextureDimension2D=8192`, `maxImmediateSize=64`, `maxComputeInvocationsPerWorkgroup=128`; device creation с запрошенными 4096/64 прошёл. Полный набор U limits/features не запрашивался, compute workload и immediates shader этой демкой не проверены.
- Dawn предупредил о снижении `maxDynamicUniformBuffersPerPipelineLayout` **72 → 16** и `maxDynamicStorageBuffersPerPipelineLayout` **70 → 16** из-за dynamic-offset allocation limit. Это warning, не отключённая validation error и не доказательство совместимости всех layouts игры.
- Bridge установил: **GL row 0 соответствует первой строке WebGPU**. Явный переворот при display blit выбран по diagnostic pixels; физическую ориентацию и видимость экрана должен подтвердить пользователь.

Существенный очищенный вывод:

```text
PASS adapter/device (not a full U capability or game test)
PASS offscreen frames=2 pixels/ordering and dedicated-context teardown checked
BRIDGE GL row 0 corresponds to WebGPU first row; display blit orientation is explicit
PASS bridge frames=120 pixels/ordering and dedicated-context teardown checked
```

Оба запуска прошли assertions отсутствия неожиданных Dawn validation/device-loss errors и GL errors, завершения queue work и уничтожения наблюдавшегося отдельного Dawn EGLContext после release. Это **bounded teardown witness**, не замер driver memory leaks и не решение удаления каждого alias при живом device.

После запуска процессов игры/демки не обнаружено. Контрольные суммы firmware libc, libstdc++, libmali (EGL/GLES) и SDL2 совпали с исходным ABI snapshot. В изолированных HOME/config/cache/data/state/tmp files не появилось. Созданный test directory удалён после проверки его manifest; пользовательские данные, Ports installation и сохранения не затрагивались. Состояние изображения меню после закрытия удалённо не оценивалось.

**Вывод:** конкретный путь **firmware SDL2/Mali → внешний EGLDisplay → Dawn OpenGLES → постоянный EGLImage → presenter blit/swap** прошёл программные проверки на целевом устройстве. Визуальная приёмка остаётся pending. Это аргумент для последующего выбора новой графики, не автоматический выбор renderer, не проверка полной игры и не основание обещать 30 FPS. В bridge намеренно есть sleep 100 ms/кадр и последовательные waits/readbacks; длительность процесса не является FPS benchmark.

Остаточные вопросы: все обязательные features/limits U, production resource recreation/lifetime, многопоточная topology/синхронизация, реальные GX/retail effects, UI/audio/input/save-load и производительность. PF-02 по MusyX/nod/game-facing SDL API также не закрыт этой демкой.
