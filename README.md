# Metroid Prime Unbound — порт для H700

Адаптация нативного порта **Metroid Prime** для **ANBERNIC RG40XX H под muOS 2601.0 Jacaranda**. Это нативная C/C++-игра на основе декомпиляции, не запуск через эмулятор.

> **Статус: подготовка плана переноса.** Разобраны изменения старого R36S/H700-порта и подготовлен технический обзор. Подробный план реализации ещё не утверждён; платформенный код, Docker-профиль и launcher не перенесены. Запуск новой базы на RG40XX H и достижение целевых FPS **пока не подтверждены**. Краткий текущий статус — **[прогресс переноса](docs/H700_PROGRESS.md)**.

## Цель H700

- Оригинальная игра с оригинальными ресурсами и штатными эффектами.
- Корректные управление встроенным контроллером, звук, сохранение и загрузка.
- Сначала воспроизводимая сборка и корректно работающая игра на приставке; затем отдельный этап оптимизации к **стабильным 30 FPS при 640×480, 4:3**. Целевой FPS пока не достигнут доказанно.
- Сборка в **Linux ARM64 Docker**, ручная установка в **muOS Ports**.
- Лёгкие улучшения управления и качества изображения можно оставить опциональными после проверки. Remastered/PBR, HD-паки и другие тяжёлые расширения не входят в первый этап.

Работа пока ограничена **RG40XX H + Jacaranda 2601.0**. Совместимость с другими H700-устройствами и прошивками не заявлена; публикация PortMaster-пакета — отдельная задача.

**Игровые данные не включены.** Для запуска нужна собственная копия игры. Не добавляйте в репозиторий или распространяемые пакеты образы дисков, извлечённые игровые ресурсы и личные сохранения. Лицензионные границы описаны в [LICENSE](LICENSE) и [NOTICE](NOTICE).

## Откуда переносим

| Источник | Роль |
| --- | --- |
| Этот репозиторий | Новая база upstream, исследованная на `c872da704` (`v0.46.0`) |
| [jbcgames/MetroidPrimePort, ветка r36s](https://github.com/jbcgames/MetroidPrimePort/tree/r36s) | Донор R36S-адаптации: `aaf8349b144c3419714d489a017483ce7ef3edb0` |
| Соседний локальный checkout `MetroidPrimePort` | R36S-донор с H700 bring-up: `3f31d19f6956e0c0ada7ed56b48703b9e0c00cd6` |

Обе линии происходят из проекта `Odrannnn/MetroidPrimePort`; доступность прежних GitHub-ссылок не гарантируется. Для аудита используются локальные Git-ревизии.

В старом checkout игра уже запускалась на целевой приставке, проверялись отдельные графические исправления и сохранение/загрузка. Это **не подтверждение работоспособности новой базы**: там другой renderer и другая реализация ряда подсистем.

## С чего начать

1. Начать с **[короткой версии на 3–5 минут](docs/H700_PORT_SUMMARY.md)** — основные выводы без геймдев-терминологии. Обоснования и детали — в **[полном обзоре переноса](docs/H700_PORT_REVIEW.md)**.
2. Для разработки — **[CLAUDE.md](CLAUDE.md)**: устройство репозитория, ограничения и правила работы.
3. Для существующих upstream-команд — [docs/NATIVE_PORT.md](docs/NATIVE_PORT.md) и [tools/README.md](tools/README.md). Это не готовая инструкция сборки H700.

## Как работаем

До кода утверждаем подробный план и закрываем известные технические вопросы. Дальше — итерации примерно по 1–2 часа: небольшой объём, проверка, обновление документов, commit и push. При необходимости изменить план работа останавливается до согласования с пользователем.

- **[Правила переноса](docs/H700_WORKFLOW.md)** — требования к плану, итерациям, проверкам и изменениям подхода.
- **[Прогресс](docs/H700_PROGRESS.md)** — что готово, что блокирует и какой следующий шаг.
- **[Находки для оптимизации](docs/H700_OPTIMIZATION_NOTES.md)** — идеи будущего ускорения, отдельно от реализации переноса.

Дополнительные источники пока находятся в соседнем донорском checkout:

- [RG40XX H: аппаратная платформа](../MetroidPrimePort/docs/RG40XX_H_HARDWARE.md);
- [muOS Jacaranda: программная платформа](../MetroidPrimePort/docs/MUOS_PLATFORM.md);
- [исторический журнал bring-up](../MetroidPrimePort/docs/MUOS_BRINGUP.md).

Эти три ссылки требуют соседнего каталога `MetroidPrimePort`; автономная сводка ограничений и результатов есть в обзоре внутри этого репозитория. Статус и контрольные суммы локальных исходных справочников приведены в его разделе об источниках.

## Сборка и запуск на приставке

**В этом checkout пока нет перенесённых `Dockerfile.muos`, H700 CMake preset и Ports launcher.** Команды из донорского журнала относятся к старому коду и не должны выдаваться за рабочие команды новой версии.

**Вывод изображения на H700 уже решён и проверен в старом порте:** R36S-коммит создал механизм, H700-коммит довёл этот вариант до запуска на нашей muOS. Путь: **SDL3-over-SDL2 → firmware SDL2 `mali` → EGL/GLES**. Открыт не вопрос вывода с нуля, а способ интеграции с новой архитектурой: адаптировать старую отрисовку к новой игре либо подключить новую отрисовку к проверенному платформенному пути. Ни один вариант пока не реализован здесь; стабильные 30 FPS остаются отдельной целью.

В целевой muOS есть `/dev/fb0` и `/dev/mali0`, но отсутствует `/dev/dri`. Наличие OpenGL ES в upstream само по себе не даёт готовой интеграции с этим display stack.

Не переносите донорские коммиты целиком: вместе с GLES-кодом они содержат удаления функций и откаты проверок безопасности. Не подменяйте firmware `libmali`, не включайте KMSDRM по аналогии с R36S и не запускайте старые `deploy_rk3326.sh`/debug-скрипты. Для новых тестов используйте отдельную установку и резервную копию сохранений.

До плана реализации подготовлена [отдельная graphics demo](docs/H700_GRAPHICS_PROBE.md): [ABI-matched Docker builder](docs/H700_BUILD_ENVIRONMENT.md), compile/link и ELF gate прошли. По отдельному разрешению loader/CPU checks, Dawn offscreen (2 кадра) и EGLImage bridge (120 кадров) прошли программные pixel/order/teardown проверки на RG40XX H. Пользователь подтвердил видимость квадрантов и маркера при кратком наблюдении; полная визуальная проверка и возврат меню не подтверждены. Это не сборка игры и не доказательство всех requirements/FPS. Следующий [статический разбор](docs/H700_DAWN_INTEGRATION_GAPS.md) выделил конкретные layout/feature и lifecycle/threading вопросы; новые проверки только предложены для согласования. Далее нужно закрыть их, выбрать интеграцию и утвердить план переноса.

---

## О базовом порте (upstream)

Ниже сохранена справка о возможностях и происхождении базового **Metroid Prime Unbound**. Перечень платформ, форматов и функций относится к upstream, **не к проверенной H700-сборке**. Ссылки на прежний upstream могут быть недоступны.

> **Disclaimer:** this is an independent project, not affiliated with or endorsed
> by [PrimeDecomp](https://github.com/PrimeDecomp/prime). It builds on their
> decompilation, but they don't maintain or support this port: please report
> problems here, not to them.

**Looking for the sequel? See the [Metroid Prime 2: Echoes native port](https://github.com/Odrannnn/MetroidPrime2Port).**

**Make sure to check out the F1 (Or start+back on controller) menu for all the additional features and QoL!**

**You can load prime remastered models and textures in the game (check F1 > mods), you must provide your
personal Metroid Prime Remastered dumped image, no assets are provided in this repo**

The upstream project provides a native build of **Metroid Prime** for Linux, Windows, Android, macOS and iPhone/iPad, compiled
from the [PrimeDecomp](https://github.com/PrimeDecomp/prime) decompilation and
rendered through Aurora (SDL3/WebGPU). There's no emulator involved. Supported
discs: USA 1.00, 1.01 and 1.02, PAL and Trilogy, both USA and PAL.

**No game content is included.** You need your own copy of the game: the port
reads it from a disc image and never ships any of its assets. This repository
ships no disc image, and neither does any package or release built from it.

## Upstream features (not yet validated on H700)

- **Modern controls:** Mouse & Keyboard, Dual stick camera, Gyro controls, Native touch controls for mobile!
- **Remastered assets:**  (Experimental) import models, effects and lighting from your own Metroid Prime
  Remastered dump (F1 > Remastered)
- **Metroid Prime Trilogy:** run Trilogy's Metroid Prime from a Wii disc, with Trilogy's HUD,
  scan visor and radial beam and visor menus
- **Metroid Dread Samus:** import Samus's suits from your own Metroid Dread dump, first-person
  arm cannon included
- **Third-person camera** with an aiming cannon arm, and **suit colour customisation**
- **Widescreen:** with an edge-to-edge HUD, and **uncapped frame rate**
- **Graphics:** FOV, MSAA, anisotropic filtering, resolution scale and dynamic
  resolution, HUD scale, arm cannon offset, toggles to hide the helmet and visor
  effects, and a choice of renderer, OpenGL included
- **Controls:** mouse and twin-stick aim, gyro, lock-on free aim, full rebinding
  and presets, separate Morph Ball binds
- **Optional tweaks:** Fast Morph, Spring Ball, Toggle Lock-On, Sticky Charge... And so many more!
- **Skippable cutscenes**
- **Save states** and **quick resume** after quitting
- **Saves:** import from and export to USA/PAL GameCube, Wii Trilogy and Dolphin
- **Photo mode** with free-cam and high-resolution capture
- **Map and tracker:** reveal the map, plus item, scan and room tracking
- **Dolphin texture packs**, including HD texture replacements
- **Mods:** drop-in mod folders that replace disc data, plus [community mods](docs/MODS.md)
- **Discord Rich Presence** on desktop, and a **touch overlay** with a layout editor on Android and iOS
- The **F1 overlay** has every setting
- And more..!

## Randomizer and Archipelago Multiworld

- **Randomizer** - Generate your own randomizer games
- **Archipelago multiworld** - Generate a game in archipelago with the [Metroid Prime APWorld](https://github.com/UltiNaruto/MetroidAPrime) and host it. Open F1 > Archipelago, enter the server (`host:port`), your
  slot name and the password if any, and connect; no seed file is needed. Each seed and slot gets its own separate save, checks made
  offline are sent on the next connect, chat with command support, DeathLink.
  
## Downloads and upstream disc support

This H700 adaptation does not yet provide a validated RG40XX H package. The
[former upstream releases page](https://github.com/Odrannnn/MetroidPrimePort/releases)
is a historical reference; its availability is not guaranteed, and its builds
must not be assumed to work on muOS.

The upstream disc support includes GameCube USA 1.00, 1.01 and 1.02 (`GM8E01`),
PAL (`GM8P01`) and Trilogy (USA `R3ME01`, PAL `R3MP01`); see
[the native-port reference](docs/NATIVE_PORT.md#run) for formats and experimental
support details. This is not the H700 test matrix. Every build needs your own
disc image; no game assets may be packaged with it.

The upstream settings menu opens with **F1** or **Start+Back** on a controller.

## Building the upstream port (not the H700 Docker profile)

CMake 3.25+, Ninja, a C++20 compiler/library and a C compiler with C23 support
for MusyX; a normal clone is enough. The requirements, the configure, build and test commands for Linux, Windows and
macOS, the CMake options and what CI runs are in
[`docs/NATIVE_PORT.md`](docs/NATIVE_PORT.md#build); the Android build is in
[`docs/ANDROID.md`](docs/ANDROID.md). Before changing the game code, read
[where port code goes](docs/NATIVE_PORT.md#where-port-code-goes).

## Running a native host build (not a muOS launcher)

```sh
./build/native/metroid_prime_port "/path/to/Metroid Prime (USA) (v1.00).iso"
```

## Troubleshooting

- **Where are the logs?** `metroid_prime_port.log` in the user folder (F1 >
  System shows its path); the previous run's is `metroid_prime_port.old.log`. On
  Android a copy goes to `Documents/MetroidPrime/`; on iOS it is in Files > On
  My iPhone (or iPad) > Metroid Prime. Attach it to a bug report.
- **"unsupported disc"**: the image is not one of the revisions above (see the
  log for the disc id it found). Re-dump it, or check you picked the GameCube disc.
- **Settings in a bad state**: F1 > System > Settings > **Reset configuration...**
  backs up and resets every setting and mapping; saves are not touched.

## Credits

This port would not exist without the work of these projects. Thank you.

**Upstream and H700 work**

- `Odrannnn/MetroidPrimePort`: the Metroid Prime Unbound upstream code and features.
- [jbcgames/MetroidPrimePort](https://github.com/jbcgames/MetroidPrimePort/tree/r36s): the R36S/GLES donor.
- gitlodey: the RG40XX H / muOS bring-up and this migration effort.
- [bmdhacks/SDL, sdl2-backend](https://github.com/bmdhacks/SDL/tree/sdl2-backend):
  the SDL3-over-SDL2 fork used by the donor; integration into this checkout is pending.

**Foundation**

- [PrimeDecomp](https://github.com/PrimeDecomp/prime): the decompilation the game
  code is built from, and the decompiled Dolphin SDK.
- [Aurora](https://github.com/encounter/aurora): the GameCube graphics, input and
  platform layer over SDL3 and WebGPU.
- [MusyX](https://github.com/AxioDL/musyx): the audio mixer.
- [nod](https://github.com/encounter/nod): the disc image reader.

**Randomizer and Archipelago**

- [randomprime](https://github.com/aprilwade/randomprime): the room patches for
  skippable cutscenes and Archipelago pickups, the room and elevator tables, and
  the custom pickup and door assets are generated from it.
- The [Metroid Prime APWorld](https://github.com/UltiNaruto/MetroidAPrime): the
  door table, and the game this client connects to.
- The Metroid Prime AP PopTracker pack: the Archipelago logic rules.
- [Archipelago](https://archipelago.gg): the multiworld itself.

**Remastered import**

- [astc-encoder](https://github.com/ARM-software/astc-encoder): the ASTC texture encoder.
- tegra_swizzle, bcdec_rs and astc-decode, ported to C++ for the texture importer.

**Platform and input**

- [libadrenotools](https://github.com/bylaws/libadrenotools): custom Vulkan drivers on Android.
- [GamepadMotionHelpers](https://github.com/JibbSmart/GamepadMotionHelpers): gyro
  calibration and aiming spaces.
- [Kenney Input Prompts](https://kenney.nl/assets/input-prompts) (CC0): the
  controller button icons.
- [Xelu's Free Controllers & Keyboard Prompts](https://thoseawesomeguys.com/prompts/) (CC0): the
  Steam Deck button icons.
- [Dolphin](https://dolphin-emu.org): the texture pack and save formats the port reads and writes.

**Libraries**

[SDL3](https://libsdl.org), [Dawn](https://dawn.googlesource.com/dawn),
[Dear ImGui](https://github.com/ocornut/imgui), [RmlUi](https://github.com/mikke89/RmlUi),
[fmt](https://github.com/fmtlib/fmt), [Abseil](https://abseil.io), [FreeType](https://freetype.org),
libpng, zlib-ng, [zstd](https://github.com/facebook/zstd), [xxHash](https://github.com/Cyan4973/xxHash),
[SQLite](https://sqlite.org) and OpenSSL.

## Licensing

Every third-party component keeps its own licence; the full list, with where each
licence text lives, is in [`NOTICE`](NOTICE), and the texts are copied into every
release package.

`LICENSE` covers this project's own work — the platform layer, the build and
packaging scripts, and the Android and Flatpak integration. It does **not** cover
the game code under `src/` and `include/`, which comes from the decompilation
and remains Nintendo's. See `NOTICE`.

Metroid Prime is a trademark of Nintendo. This project is unaffiliated with
Nintendo and Retro Studios, and ships no game data.

## Documentation

| Document | What it covers |
|---|---|
| [docs/H700_PORT_REVIEW.md](docs/H700_PORT_REVIEW.md) | Подробный аудит R36S/H700-изменений и применимость к новой базе |
| [CLAUDE.md](CLAUDE.md) | Контекст H700 и правила для coding agents |
| `docs/SETTINGS.md` | Every control, setting and environment variable |
| `docs/NATIVE_PORT.md` | The detailed reference: build options, smoke-test hooks, platform notes |
| `docs/MODS.md` | HD texture packs and the mods folder |
| `docs/MOD_FORMATS.md` | Mod file formats, for mod authors |
| `docs/DREAD_SAMUS.md` | The Metroid Dread Samus model import |
| `docs/RANDOMIZER.md` | The built-in randomizer |
| `docs/ARCHIPELAGO.md` | The Archipelago multiworld client |
| `docs/CONSOLE.md` | The debug console (`MP_CONSOLE`) |
| `docs/DEBUGGING.md` | Headless debugging with `tools/mprig.py` |
| `tools/README.md` | What each script in `tools/` does and what runs it |
| `docs/RELEASING.md` | What a release contains and must carry |
| `docs/ANDROID.md` | The Android port's status and build |
| `docs/IOS.md` | The experimental iPhone/iPad build: sideloading, the disc, where files are |
| `docs/REMASTERED_EFFECTS.md` | Importing Remastered particle effects |
| `docs/FRAME_INTERPOLATION.md`, `docs/HIGH_FPS_AUDIT.md` | High frame rate design notes |
| `decomp/` | The original decompilation's GameCube build and notes (not used by the port) |
| `PORT_NOTES.md` | Open follow-ups and non-obvious facts |

## AI assistance

The upstream project reports using AI coding assistants under its maintainer's
oversight. AI assistance is also used for this H700 migration; source review,
validation results and unverified assumptions must be reported separately.
