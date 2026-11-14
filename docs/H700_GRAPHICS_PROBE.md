# H700 — отдельная демка Dawn / SDL2 / EGLImage

## Текущий статус

**Демка собрана в новом ARM64 Docker image; ENV-01/02 прошли 2026-11-14.** Прежний link blocker Bookworm устранён без изменения Dawn/firmware. 43 unit tests, compile/link, ELF/ABI gate и безопасные контейнерные modes — PASS. На приставке демка ещё не запускалась. Подробности окружения и его EOL-ограничения — [ENV-01/02](H700_BUILD_ENVIRONMENT.md).

Код игры, root CMake, dependency pins и донор не изменялись. В рамках подготовки демки deploy и graphics runtime не выполнялись; отдельный разрешённый read-only SSH inventory описан в [runtime/ABI](H700_RUNTIME_ABI.md). Разрешение подготовить демку не считается разрешением занять экран.

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
| Device runtime / offscreen / bridge | Не выполнялись |

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

Обновлять firmware для этих symbols не требуется. В новом builder executable слинкован и прошёл описанный metadata gate, но остальные exact imports и firmware runtime не проверены; наличие пяти functions не означает доказанную полную совместимость Dawn. Пользователь затем согласовал новое build environment; изменение выполняется отдельно по [ENV-01/02](H700_BUILD_ENVIRONMENT.md), без смены Dawn или firmware.

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

Executable требует максимум `GLIBC_2.38`, `GLIBCXX_3.4.32`, `CXXABI_1.3.8`; SDL — `GLIBC_2.38`. Loader: `/lib/ld-linux-aarch64.so.1`, RUNPATH: `$ORIGIN`. Прямые `NEEDED` executable: `libSDL3.so.0`, `libEGL.so.1`, `libGLESv2.so.2`, `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libc.so.6`; у SDL — только `libm.so.6`, `libc.so.6`. Динамически открываемые backend libraries требуют отдельной runtime-проверки.

Следующий шаг — согласовать свободное окно, точную процедуру запуска/timeout/завершения **только демки** и отдельное разрешение на deploy. Только после этого проверять offscreen, затем bridge. Ни сборка, ни CPU self-test не доказывают графическую совместимость, корректность firmware cleanup или FPS игры.
