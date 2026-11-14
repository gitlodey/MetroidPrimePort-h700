# RG40XX H / muOS 2601 — фактический runtime и ABI

**Снимок:** 2026-11-14, 18:06 UTC. **Устройство:** ANBERNIC RG40XX H, muOS 2601.0 Jacaranda, AArch64. Собран по явно разрешённому read-only SSH inventory. При сборе снимка firmware, библиотеки, настройки и сохранения не изменялись; демка и игра не запускались. Последующий отдельно разрешённый device test демки описан в [H700_GRAPHICS_PROBE.md](H700_GRAPHICS_PROBE.md); исходный JSON inventory не изменён.

## 1. Короткий ответ по блокеру Dawn

**Ошибка возникла внутри Docker build image, не в macOS и не на приставке.** Обновление библиотек Mac не меняет runtime Linux-контейнера. Все пять конкретных функций из последней ошибки линковки Dawn присутствуют в системных библиотеках приставки.

| Runtime | Прежний Docker Bookworm image | Приставка |
| --- | --- | --- |
| glibc | **2.36** | **2.38** |
| Наибольший числовой GLIBCXX node | **GLIBCXX_3.4.30** | **GLIBCXX_3.4.32** |
| Наибольший числовой CXXABI node | **CXXABI_1.3.13** | **CXXABI_1.3.14** |
| C23 parsing functions из ошибки | Linker их не разрешает | Экспортируются с **GLIBC_2.38** |
| String `_M_replace_cold` из ошибки | Linker её не разрешает | Экспортируется с **GLIBCXX_3.4.31** |

**Ограничение проекта:** системные C/C++/graphics библиотеки приставки не обновляем; build artifacts должны быть совместимы с установленной firmware.

**Вывод:** эти ошибки не требуют обновления firmware. Вариант с подходящим build toolchain/sysroot для готовой Dawn остаётся обоснованным. Пересборка Dawn — другой возможный вариант, но её необходимость нельзя выводить только из старого образа сборки. После этой проверки пользователь согласовал [новое build-окружение](H700_BUILD_ENVIRONMENT.md). ENV-01/02 затем прошли: отдельный image создан, демка слинкована и проверена без графики в контейнере; firmware осталась неизменной.

Это ещё **не подтверждение полной совместимости Dawn**: конечный executable теперь слинкован; его NEEDED, version nodes и пять selected imports сверены со snapshot; затем loader/CPU checks и offscreen/bridge сценарии прошли на firmware. Не каждый exact symbol и не все runtime paths полной игры проверены. Не следует также менять образ на произвольную новую distro: её glibc/libstdc++ могут оказаться новее firmware.

## 2. Проверены именно отсутствовавшие symbols

На host в ARM64 container получены undefined symbols из закреплённого `libwebgpu_dawn.a` через `nm -u`. На приставке прочитаны **определённые экспортируемые symbols**, а не только имена файлов библиотек.

| Symbol из Dawn | Предоставляется на приставке |
| --- | --- |
| `__isoc23_strtol` | `libc.so.6`, `@@GLIBC_2.38` |
| `__isoc23_strtoul` | `libc.so.6`, `@@GLIBC_2.38` |
| `__isoc23_strtoll` | `libc.so.6`, `@@GLIBC_2.38` |
| `__isoc23_strtoull` | `libc.so.6`, `@@GLIBC_2.38` |
| `std::__cxx11::basic_string<char, ...>::_M_replace_cold(...)` | `libstdc++.so.6`, `@@GLIBCXX_3.4.31` |

Последний проверенный mangled name:

```text
_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE15_M_replace_coldEPcmPKcmm
```

Изученный package — pin U `v20261029.114630`, source ref `118436b3f8f68583eb86e811ca605dc41637e8cc`; архив ранее проверен по SHA-256 из конфигурации проекта. Новая версия Dawn не подставлялась.

## 3. Системные границы

- OS: `MustardOS 2601.0 (JACARANDA)`, `BUILD_ID=bc38efa0`.
- Kernel: `4.9.170`; architecture: `aarch64`.
- Loader: `/lib/ld-linux-aarch64.so.1`.
- libc: `/lib/libc.so.6`; определены числовые GLIBC nodes до `GLIBC_2.38`.
- C++ runtime: `/usr/lib/libstdc++.so.6.0.32`; nodes до `GLIBCXX_3.4.32` и `CXXABI_1.3.14`. В прежнем контейнере файл — `libstdc++.so.6.0.30`.
- System EGL/GLES: оба SONAME разрешаются в `/usr/lib/libmali.so`. Graphics context при этом inventory не создавался.

Числовые максимумы — удобные ориентиры, **не универсальная гарантия совместимости**. Нужны конкретные version nodes, символы и транзитивные зависимости каждого artifact. Нечисловые version nodes также сохранены в JSON.

## 4. Проверенные библиотеки

В [машиночитаемом снимке](H700_RUNTIME_ABI.json) найдены **46 SONAME entries / 45 уникальных ELF files**. Для исходных runtime candidates и полной найденной `NEEDED`-цепочки сохранены machine, реальный путь, SONAME, SHA-256, зависимости, определённые/требуемые version nodes и выбранные exports.

Основные записи:

| SONAME | Реальный файл на устройстве | Назначение |
| --- | --- | --- |
| `libc.so.6` | `/lib/libc.so.6` | C runtime |
| `libstdc++.so.6` | `/usr/lib/libstdc++.so.6.0.32` | C++ runtime |
| `libgcc_s.so.1` | `/lib/libgcc_s.so.1` | Compiler runtime/unwind |
| `libm.so.6` | `/lib/libm.so.6` | Math |
| `libdl.so.2` | `/lib/libdl.so.2` | Dynamic loading compatibility |
| `libpthread.so.0` | `/lib/libpthread.so.0` | Threading compatibility |
| `librt.so.1` | `/lib/librt.so.1` | Realtime compatibility |
| `libSDL2-2.0.so.0` | `/usr/lib/libSDL2-2.0.so.0.2800.5` | Firmware SDL2, загружается shim через `dlopen` |
| `libEGL.so.1` | `/usr/lib/libmali.so` | Firmware EGL |
| `libGLESv2.so.2` | `/usr/lib/libmali.so` | Firmware GLES |
| `libasound.so.2` | `/usr/lib/libasound.so.2.0.0` | ALSA candidate |
| `libpulse.so.0` | `/usr/lib/libpulse.so.0` | PulseAudio candidate |
| `libssl.so.3` | `/usr/lib/libssl.so.3` | Full-port TLS candidate |
| `libcrypto.so.3` | `/usr/lib/libcrypto.so.3` | Crypto/TLS candidate |
| `libsqlite3.so.0` | `/usr/lib/libsqlite3.so.0.8.6` | Database candidate |
| `libzstd.so.1` | `/usr/lib/libzstd.so.1.5.5` | Compression candidate |
| `libfreetype.so.6` | `/usr/lib/libfreetype.so.6.20.1` | Fonts candidate |
| `libpng16.so.16` | `/usr/lib/libpng16.so.16.43.0` | PNG candidate |
| `libz.so.1` | `/usr/lib/libz.so.1.3.1` | zlib candidate |

**Имена ELF files не всегда являются semantic versions пакетов.** Например, `libsqlite3.so.0.8.6` не означает SQLite версии 0.8.6. В этой проверке runtime functions библиотек ради получения marketing/package versions не вызывались: для ABI существенны SONAME, exports и symbol versions.

`libsndio.so.7` / `libsndio.so.7.0` не найдены в обследованных системных каталогах. Это optional audio candidates, не доказанные обязательные dependencies демки. Новая SDL3 сейчас имеет в `NEEDED` только `libm.so.6` и `libc.so.6`; отдельные backend libraries могут загружаться динамически. Не устанавливать sndio «на всякий случай» — выбирать/проверять реально используемый audio path.

Все `NEEDED` найденных ELF из этой выборки разрешились в обследованных каталогах. Найденные X-related libraries в транзитивной PulseAudio-цепочке **не доказывают наличие рабочего X11 display** и не меняют выбранный firmware SDL2/Mali path.

## 5. Что проверять для каждого нового artifact

1. Архитектура AArch64, нужный ELF loader и реальные `NEEDED` executable и bundled DSO.
2. Требуемые version nodes и **точные symbols**, не только максимальный `GLIBC_*` или имя файла runtime.
3. Транзитивные зависимости и библиотеки, открываемые через `dlopen`: firmware SDL2, graphics/audio backends, plugins по необходимости.
4. Отсутствие непредусмотренных container/system libraries в package и изменений firmware.
5. После согласования device test — фактическая загрузка/работа. Metadata не доказывает graphics, audio, input, save/load или FPS.

Снимок не содержит окончательного списка dependencies ещё не собранной игры. При новом artifact или смене firmware сравнение нужно повторять. Нельзя обещать отсутствие всех сюрпризов лишь по текущим версиям; этот файл задаёт проверяемую исходную точку и предотвращает конкретный уже найденный класс ошибок.

## 6. Метод и повторение

Сборщик: [tools/h700_runtime_inventory.py](../tools/h700_runtime_inventory.py). Destination задаётся явно при запуске, не вшит в код и не сохраняется в отчёт. Он отправляет фиксированный Python-код через verified SSH stdin, не копирует скрипт на приставку и не создаёт там файлы. Использованы `getconf`, `readelf` и чтение system ELF; `dlopen`, `ldd`, graphics и game startup на устройстве не выполнялись.

Output создаётся эксклюзивно, существующий snapshot не перезаписывается. Перед повторным запуском согласовать актуальный SSH destination и выбрать новый локальный output. Нельзя отключать host-key verification. Полный environment, hostname, IP, credentials, game paths и saves не собираются.

В [H700_RUNTIME_ABI.json](H700_RUNTIME_ABI.json) сохранены исходные обезличенные данные. В [H700_GRAPHICS_PROBE.md](H700_GRAPHICS_PROBE.md) — история устранённой ошибки линковки и результаты device demo; актуальный статус — [H700_PROGRESS.md](H700_PROGRESS.md).
