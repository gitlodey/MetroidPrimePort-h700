# muOS — спецификация целевой платформы RG40XX H

**Целевая установка: MustardOS 2601.0 Jacaranda на ANBERNIC RG40XX H.**
Документ сверён **2026-11-14** по имеющейся диагностике, журналу проекта и
официальной документации. Новый SSH-опрос, probes и изменения устройства
при подготовке не выполнялись. Аппаратная спецификация —
[RG40XX_H_HARDWARE.md](RG40XX_H_HARDWARE.md).

Это профиль **конкретной установки**, а не обещание одинакового ABI/SDL/GPU
стека у всех версий muOS и всех поддерживаемых ею устройств.

## 1. Релиз, ядро и память

Первичный источник — [muos-diagnostics.txt][inventory], строки 4–25.

| Параметр | Подтверждённое значение |
| --- | --- |
| Название ОС | `MustardOS` / muOS |
| `ID` | `muos` |
| `VERSION_ID` | `2601.0` |
| Версия | `2601.0 (JACARANDA)` |
| `BUILD_ID` | `bc38efa0` |
| Архитектура | `aarch64` |
| Platform ID | `sun50iw9` |
| Ядро | Linux `4.9.170`, `SMP PREEMPT` |
| C runtime | glibc `2.38` |
| Память Linux | `MemTotal: 996632 kB`, около 973,3 MiB |
| Доступная память при старом inventory | `MemAvailable: 877960 kB`; разовое наблюдение, не лимит приложения |
| Swap при inventory | `SwapTotal: 0 kB`, `SwapFree: 0 kB` |

На дату сверки [сайт muOS][devices] предлагает **2606.0 Andromeda** как текущий
релиз. Это **не установленная целевая версия**. Профиль ниже не предполагает
обновления до 2606.0 и не переносит её возможности/библиотеки на Jacaranda.
Наличие glibc не означает, что firmware — Debian; Debian используется в
контейнере сборки проекта, а не как подтверждённый sysroot устройства.

## 2. Графический runtime

Наличие файлов — из [inventory][inventory], строки 27–61.
Runtime capabilities — из исторического [bring-up журнала][bringup],
разделов о SDL2 probe и SDL3 shim/EGLImage bridge.

| Компонент | Подтверждённый статус | Ограничение утверждения |
| --- | --- | --- |
| Display node | `/dev/fb0` | Не установлен desktop window-system protocol |
| GPU node | `/dev/mali0` | Не определяет полный набор extensions |
| DRM nodes | `/dev/dri` отсутствовал | Не выбирать KMS/DRM/GBM как заведомо рабочий путь |
| GPU userspace | Firmware `libmali.so`; EGL/GLES symlinks ведут к firmware Mali library по журналу | Точная driver revision не зафиксирована |
| GL renderer | Mali-G31 | GPU устройства, не программный llvmpipe |
| OpenGL ES | В probe получен OpenGL ES 3.2 | Не desktop OpenGL и не доказательство Vulkan |
| EGL | В probe получен EGL 1.4 | Не обещание EGL 1.5 |
| SDL2 video backend | `mali` | Не `kmsdrm`, X11 или Wayland по умолчанию |
| SDL2 swap | Probe выполнил один swap, GL error 0 | Не длительный stress test |
| Shared context | Создан shared ES3 context с pbuffer | Подтверждена конкретная схема probe, не произвольная context topology |
| EGLImage bridge | Texture alias через EGLImage; FBO complete | Не проверка всех formats/MSAA/resize/shutdown сценариев |
| Реальная игра-донор | Инициализация 640 × 480, два EFB slots, меню/gameplay | Не подтверждение работоспособности переносимой v0.46.0 |

`libGLESv2.so.2` — имя shared library, **не ограничение до OpenGL ES 2.0**.
Для данного проекта проверен прямой GLES через firmware SDL2/Mali. Vulkan,
desktop GL, X11/Wayland и DRM/GBM runtime здесь не подтверждены.
Подменять H700 `libmali` библиотекой R36S/RK3326 нельзя.

Probe-код проверяет, в частности, EGLImage и fence extension flags;
[SDL3 bridge probe][shim-probe] реально вызывает `eglCreateImageKHR` и
`glEGLImageTargetTexture2DOES`. Само наличие проверки extension в исходнике
не является положительным результатом её выполнения. Полный extension dump,
GL limits и отдельный результат для surfaceless contexts здесь не зафиксированы.

## 3. SDL: firmware и библиотека проекта — разные слои

| Слой | Источник / версия | Как используется |
| --- | --- | --- |
| Системная SDL2 | В inventory `/usr/lib/libSDL2-2.0.so.0.2800.5`; SONAME `libSDL2-2.0.so.0` | Firmware runtime; имя файла не заменяет вызов SDL_GetVersion |
| SDL2 дополнения | Inventory перечисляет SDL2_image, SDL2_mixer, SDL2_ttf, SDL2_net, SDL2_gfx | Наличие не означает, что игра использует каждую библиотеку |
| SDL3 shim проекта | [bmdhacks/SDL, sdl2-backend][sdl-fork], commit `6057d79baf8321bf190479a699655f06cc2a962f` | Отдельно собранная и поставляемая с портом `libSDL3.so.0`, не системная SDL3 muOS |
| Headers shim | Версия 3.5.0 по журналу сборки | Headers брать из того же fork/commit, не смешивать с другим SDL3 pin |
| Связь shim → firmware | SDL2 загружается через `dlopen` | Одного ELF `NEEDED`/`ldd` для проверки этого слоя недостаточно |

Рабочий выбор драйверов состоит из **двух** переменных:

```sh
export SDL_VIDEODRIVER=sdl2
export SDL3SHIM_SDL2_VIDEODRIVER=mali
```

Первая выбирает backend внутри SDL3 shim, вторая — backend системной SDL2.
Без второй SDL2 может унаследовать `sdl2` и завершиться с
`SDL_Init(VIDEO): sdl2 not available`.

По журналу fork не публикует Aurora-specific window properties
`SDL.window.sdl2_backend.{egl_display,egl_context,gl_get_proc,window,gl_swap_window}`.
Проверенный fallback получает context/display через EGL и proc lookup.
Это свойство выбранного shim, не требование реализовывать новый SDL backend.

## 4. ABI и shared dependencies

- Для native executable нужен **AArch64 Linux ELF**. В журнале подтверждён
  loader `/lib/ld-linux-aarch64.so.1` у рабочего эталона.
- glibc устройства — **2.38**. По отдельной on-device проверке, записанной в
  bring-up журнале, `libstdc++.so.6` разрешался в **6.0.32**. Это наблюдение
  имени библиотеки, не полный список поддерживаемых `GLIBCXX_*` symbols.
- Историческая baseline-сборка потребовала до `GLIBC_2.34`,
  `GLIBCXX_3.4.30`, `CXXABI_1.3.13`; её `ldd` и `--version` прошли на устройстве.
  Эти значения **не являются измерением новой v0.46.0 сборки**.
- Журнал отмечает успешно разрешённые зависимости старого game binary:
  OpenSSL 3, SQLite3, zstd, Freetype, libpng16, GLESv2 и C++ runtime.
  Точные версии всех firmware packages не установлены.
- ARM64 Debian Bookworm container с glibc 2.36 — **build environment**, не
  Jacaranda SDK. Проверять `readelf -d`, `readelf --version-info` и фактический
  runtime каждого нового binary, включая transitive dependencies и `dlopen` SDL2.
- Не копировать container/desktop libraries поверх системных библиотек,
  чтобы скрыть ABI error. Само `--version` также не проверяет graphics/input/audio.

## 5. Аудио и ввод

| Подсистема | Подтверждено в проекте | Что не установлено |
| --- | --- | --- |
| Audio | Игра сообщала SDL audio driver `alsa`; firmware default ALSA route использует PipeWire | Полная ALSA configuration, latency, supported sample formats |
| Runtime socket | Launcher проверяет `/run/pipewire-0` | Наличие сокета при каждом будущем запуске не гарантировано |
| Audio environment | При существующем сокете launcher задаёт `PIPEWIRE_RUNTIME_DIR=/run`, если значение ещё не задано | Это не универсальный audio setting всех muOS-релизов |
| Проверка звука | В запуске из Ports пользователь слышал звук; ранний SSH-run дал `ALSA: Host is down` | SSH-ошибка не доказывает отсутствия audio support |
| Gamepad | `muOS-Keys`, в game log controller port 0 | Полная раскладка/оси/deadzones/rumble не сертифицированы |
| Другие input names | `axp2202-pek`, `dierct-keys-polled` — точное написание inventory | Их роли не определять только по имени |
| Gamepad DB | `/usr/lib/gamecontrollerdb.txt`; launcher использует `SDL_GAMECONTROLLERCONFIG_FILE` | Конкретный GUID/mapping в этом документе не фиксируется |

Наличие Bluetooth 4.2 в железе не подтверждает Bluetooth audio/controllers
в этой установке. Работа правого стика зависит также от игрового режима;
не диагностировать mapping только по отсутствию прицеливания в default mode.

## 6. Файловая система, запуск и PortMaster

- Inventory подтверждает `/roms/ports` и `/mnt/mmc/ROMS/PORTS`.
  Журнал также описывает `/mnt/sdcard` и фактические **rw** mounts
  `/mnt/mmc`/`/mnt/sdcard`, несмотря на старый login banner.
- `/roms` может участвовать в firmware bind-mount схеме. Не объявлять его
  физическим SD-slot или symlink без проверки mounts конкретной установки.
  Регистр `ports`/`PORTS` сохранять как в реальном filesystem.
- [Текущая инструкция muOS/PortMaster][portmaster] выбирает SD2, если она
  доступна, иначе SD1; game data размещаются в `ports/<game>`, launcher в
  `roms/PORTS/<game>.sh`. Это текущая инструкция сайта, не гарантия layout
  исторической ручной установки нашего порта на Jacaranda.
- [Проверенный launcher проекта][launcher] ожидает `metroid_prime/` рядом
  с `.sh`, добавляет каталог игры в `LD_LIBRARY_PATH`, запускает game binary
  и пишет локальный `log.txt`. Это **схема нашего launcher**, не API muOS.
- В исторической установке `/roms/ports/PortMaster` содержал `control.txt`,
  но не `funcs.txt`. Поэтому launcher поддерживает standalone mode и не
  вызывает отсутствующие PortMaster helpers. Это состояние установки,
  не утверждение о неполноте PortMaster во всех Jacaranda images.
- SSH в bring-up использовался от root, без `sudo`. Адреса, credentials и
  enablement сетевых сервисов не являются частью этой спецификации.
- Bind/union layers могут менять фактический backing storage. Права записи
  и свободное место проверяются по реальному пути, а не по banner или tmpfs.

### Рабочее окружение проекта, не глобальные настройки ОС

Источник — [Metroid Prime.sh][launcher]. Значения ниже описывают уже
использованный запуск; это не инструкция менять firmware или делать deploy.

| Переменная | Значение / условие |
| --- | --- |
| `SDL_VIDEODRIVER` | `sdl2` |
| `SDL3SHIM_SDL2_VIDEODRIVER` | По умолчанию `mali`, внешний override сохранён |
| `LD_LIBRARY_PATH` | Каталог игры добавлен перед прежним значением |
| `PIPEWIRE_RUNTIME_DIR` | По умолчанию `/run`, только если существует PipeWire socket |
| `SDL_GAMECONTROLLERCONFIG_FILE` | По умолчанию `/usr/lib/gamecontrollerdb.txt`, если файл читается |
| `MP_ASPECT` | `4:3` |
| `MP_OPENGLES` | По умолчанию `1` |
| `MP_UPDATE_CHECK` | По умолчанию `0` |

## 7. Значимые особенности Jacaranda из release notes

[Архивная страница 2601.0][jacaranda] подтверждает:
- ожидание готовности PipeWire перед frontend load;
- SDL/GL overlay stage injection, впоследствии сделанную opt-in;
- исправления обработки незаданного `LD_PRELOAD`;
- bind map с реальным backing storage и исправления free-space checks;
- joystick calibration tool, изменения controller selector;
- обновления H700 boot packages и device tree information.

Это причины различать запуск из Ports и SSH. Release notes **не доказывают**,
что overlay/colour filter/preload сейчас включён на нашем устройстве.
Не добавлять или убирать preload автоматически только по этому списку.

## 8. Что остаётся неизвестным

- Точная Mali driver revision, полный EGL/GL extension list, texture/UBO/FBO
  limits, MSAA counts, compressed formats и anisotropy на данном runtime.
- Измеренные GPU memory budget, частоты/governor/temperature и frame pacing.
- Наличие и работоспособность Vulkan, X11/Wayland, HDMI modes, Bluetooth
  profiles, hotplug и suspend/resume для этого порта.
- Полный sysroot/package manifest и maximum exported GLIBCXX/CXXABI versions.
- Текущие mounts, свободная RAM/SD, enabled overlays и сетевые службы:
  исторический inventory не является live monitoring.

Эти неизвестные не заполняются характеристиками другой muOS-версии или
R36S. Документ фиксирует факты, а не добавляет задачи разработки renderer.

## Источники

1. [Read-only inventory устройства][inventory] — kernel/OS/libc/RAM/nodes/libs/input/paths.
2. [Исторический bring-up журнал][bringup] — actual probes, ABI, audio и запуск.
3. [Действующий donor launcher][launcher] — конкретные env/path/fallback решения.
4. [SDL3 shim probe проекта][shim-probe] — что именно проверяет EGLImage test.
5. [MustardOS 2601.0 Jacaranda — архивный релиз][jacaranda].
6. [muOS — PortMaster installation][portmaster], текущая инструкция, изменена 2026-10-24.
7. [muOS — Supported Devices / current release][devices].
8. [bmdhacks/SDL, sdl2-backend][sdl-fork] — provenance shim, pin указан выше.

Внешние страницы ANBERNIC/muOS прочитаны 2026-11-14. Факты о runtime получены
из локальных первоисточников, **не из повторного опроса устройства**.
Ссылки на локальный донор требуют соседнего checkout `MetroidPrimePort`.

[inventory]: ../../MetroidPrimePort/muos-diagnostics.txt
[bringup]: ../../MetroidPrimePort/docs/MUOS_BRINGUP.md
[launcher]: ../../MetroidPrimePort/portmaster/Metroid%20Prime.sh
[shim-probe]: ../../MetroidPrimePort/tools/muos_sdl3_probe.c
[jacaranda]: https://muos.dev/release/archive/2601_0
[portmaster]: https://muos.dev/installation/portmaster
[devices]: https://muos.dev/devices
[sdl-fork]: https://github.com/bmdhacks/SDL/tree/sdl2-backend
