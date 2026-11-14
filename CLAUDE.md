# Контекст проекта и инструкции для coding agents

## Назначение

Этот репозиторий — перенос Metroid Prime Unbound на **ANBERNIC RG40XX H / H700 под muOS 2601.0 Jacaranda**. Это нативный порт на основе PrimeDecomp, не эмулятор.

Отвечай пользователю по-русски. Код, identifiers и существующий стиль комментариев сохраняй согласно окружающему файлу. Работай точечно: задача — адаптировать новую базу, а не заменить её старым форком.

## Сначала прочитай

1. [README.md](README.md) — цель и актуальный статус.
2. [docs/H700_PORT_REVIEW.md](docs/H700_PORT_REVIEW.md) — подробный аудит донорских изменений, матрица решений и границы доказательности.
3. [docs/NATIVE_PORT.md](docs/NATIVE_PORT.md) — сборка базового порта и раздел `Where port code goes`.
4. При работе с подсистемой — её код, tests и документацию: [docs/SETTINGS.md](docs/SETTINGS.md), [docs/DEBUGGING.md](docs/DEBUGGING.md), [PORT_NOTES.md](PORT_NOTES.md), [tools/README.md](tools/README.md).

Не считай исторические pending-статусы из начала bring-up журнала последним результатом. Различай анализ исходников, старые device-tests и собственные свежие проверки.

## Цель и приоритеты

- Оригинальные ресурсы, штатные эффекты, корректные управление, звук и сохранение/загрузка.
- Ориентир — **стабильные 30 FPS при 640×480, 4:3**. Это пока не подтверждённый результат.
- Основной целевой build path — **Linux ARM64 Docker**; установка вручную в **muOS Ports**.
- Сначала correctness и воспроизводимые измерения, затем оптимизация.
- Remastered/PBR/HD packs не обязательны для первого этапа. Дешёвые input/QoL и визуальные улучшения можно сохранять опционально после проверки.
- Не удаляй upstream-возможности и поддержку других платформ только потому, что они не нужны исходному H700-профилю. Предпочитай изолированные platform/backend options.

## Состояние на момент создания инструкции

На 2026-11-14 новая база исследована на `c872da704adde9a7838492aaa56dac73a32053e4` (`v0.46.0`). Обзор подготовлен; код переноса ещё не реализован, план реализации не утверждён.

В этом checkout пока нет перенесённых `Dockerfile.muos`, H700 preset или Ports launcher. Не предлагай `cmake --preset muos-linux-arm64` как уже существующий рабочий интерфейс здесь. Донорские build instructions относятся к другому коду.

Этот раздел — снимок, не вечный запрет разработки. Сверяй его с текущими файлами, Git и последним запросом пользователя. По мере реализации обновляй статус и добавляй только реально проверенные команды.

## Две рабочие папки и ревизии

Ожидаемая локальная структура:

```text
projects/
  MetroidPrimePort/       # старый донор R36S + H700 bring-up
  MetroidPrimePort-h700/  # новая база; изменения делаем здесь
```

| Ревизия донора | Значение |
| --- | --- |
| `fd0f23e2c46153600214b2cd5d9f4b98a2cff518` | Родитель R36S-коммита: необходим для точной атрибуции |
| `aaf8349b144c3419714d489a017483ce7ef3edb0` | R36S/RK3326: большая замена графики и сопутствующие изменения |
| `3f31d19f6956e0c0ada7ed56b48703b9e0c00cd6` | H700/muOS bring-up и исправления регрессий R36S |

Донор — источник для чтения и сравнения. Не изменяй его без отдельной задачи пользователя. Не делай cherry-pick обоих коммитов целиком: вместе с полезным GLES-кодом они откатывают безопасность IO/карт и содержат no-op APIs.

Дополнительные локальные источники в доноре:

- [docs/RG40XX_H_HARDWARE.md](../MetroidPrimePort/docs/RG40XX_H_HARDWARE.md);
- [docs/MUOS_PLATFORM.md](../MetroidPrimePort/docs/MUOS_PLATFORM.md);
- [docs/MUOS_BRINGUP.md](../MetroidPrimePort/docs/MUOS_BRINGUP.md);
- [muos-diagnostics.txt](../MetroidPrimePort/muos-diagnostics.txt).

Они требуют соседнего checkout; не все входят в его Git-дерево. В обзоре сохранены существенные сведения, provenance и hashes локальных inputs. Недоступность старого GitHub upstream не заменяет проверку локальных объектов Git.

## Целевая платформа: не путать с R36S

- **H700: 4× Cortex-A53, AArch64, Mali-G31, около 1 GB общей RAM**, панель 640×480.
- muOS **2601.0 Jacaranda**, Linux 4.9.170, glibc 2.38; swap в проверенной установке отсутствует.
- `/dev/fb0` и `/dev/mali0` есть, `/dev/dri` отсутствует. Не предполагай рабочий KMSDRM/GBM/X11/Wayland.
- Firmware `libEGL.so.1` / `libGLESv2.so.2` разрешаются в `libmali.so`. Не подменяй их R36S/RK3326, Mesa или container-библиотеками.
- Старый порт проверял **SDL3-over-SDL2 → firmware SDL2 `mali` → EGL/GLES**. Shim и headers должны происходить из одного source snapshot.
- Для этого донорского пути нужны обе переменные: `SDL_VIDEODRIVER=sdl2` и `SDL3SHIM_SDL2_VIDEODRIVER=mali`. Это platform-profile settings, не глобальный default всех Linux.
- Исторические ES 3.2 / EGL 1.4 не доказывают все extensions, formats, limits или корректность нескольких contexts.
- Audio в доноре работал через ALSA/PipeWire; условный `PIPEWIRE_RUNTIME_DIR=/run` решал routing, а не громкость.
- Встроенный controller обнаруживался как `muOS-Keys`; firmware DB — `/usr/lib/gamecontrollerdb.txt`. Полная mapping/deadzone/rumble проверка всё равно нужна.
- Debian Bookworm container — build environment, **не Jacaranda sysroot**. ABI проверяется по каждому новому artifact и его dependency chain.

Наблюдения свободной RAM, частот и CPU/GPU utilisation — разовые измерения, не гарантии бюджета/производительности. Подробности и датированные результаты — в обзоре.

## Карта исходников

| Путь | Назначение |
| --- | --- |
| `platform/`, `platform/include/` | Host/platform layer, input, paths, настройки, UI, audio bridge, tooling hooks |
| `src/`, `include/` | Декомпилированные game/Kyoto sources; изменения минимальные, game-facing hooks — по существующему стилю |
| `extern/aurora/` | GX/SDK compatibility, графика, SDL, dependency providers и собственные tests |
| `extern/musyx/` | Audio engine |
| `CMakeLists.txt`, `cmake/`, `files.cmake` | Native build и manifest game sources |
| `tests/` | Port regression tests |
| `extern/aurora/tests/` | Aurora/GX/FIFO и другие tests |
| `tools/`, `packaging/` | Build/test/package scripts и licenses |
| `decomp/` | Отдельная исходная GameCube build-система, не основной native build |
| `build/` | Локальные artifacts; не место для versioned исходников |

## Инварианты при переносе

1. **Не путай два GLES-пути и уже решённую задачу с задачей интеграции.** R36S-коммит создал механизм вывода, H700-коммит довёл его вариант до запуска на целевой muOS. Вывод на H700 не нужно искать с нуля. В новой базе OpenGLES — backend Dawn/WebGPU; у донора — самостоятельный raw GL renderer. Открыт выбор: адаптировать старый renderer к новой игре либо подключить новый renderer к проверенному платформенному пути. Не объявляй один вариант выбранным и не начинай оба без согласования; достижимость 30 FPS проверяется отдельно.
2. U уже имеет compatibility-mode, texture vertex fetch и render-pass fallbacks без compute. Не отвергай текущий renderer только из-за отсутствия vertex SSBO/compute; отдельно проверяй display/context integration.
3. Сохраняй `-fsigned-char` для game-facing ARM code, `glibc_compat` и корректную ABI policy.
4. Сохраняй FIFO-ordered draw-sync tokens и lifetime skinning workspaces. Старый immediate token/static echo недостаточен; drain донора не лучше текущего FIFO command.
5. Не откатывай bounds, error propagation, atomic writes и identity checks карт памяти. Ненулевой serial уже есть в новой базе; sidecar — отдельное изменение с migration/backup policy.
6. Успешный compile/link с no-op shim не означает поддержку feature. Не прячь отсутствие rendering/screenshot/card API за пустыми функциями.
7. Размеры upload buffers, caches и textures должны иметь проверяемые пределы и failure paths. Не переносить silent truncation или nominal texture cache 4 GiB на устройство с 1 GB.
8. `Original Experience` не является готовым low-cost/30-FPS preset: он задаёт cap60 и не выключает все дополнительные эффекты. Output FPS и simulation TPS — разные величины.
9. Back+Start в новой базе открывает PortMenu. Не заменяй его немедленным exit без согласованного решения.
10. Штатные visors, EFB copies, прозрачность и другие retail effects входят в correctness; их отключение не выдавай за безусловно допустимую оптимизацию.

## Сборка и проверка

Перед командами проверь актуальные CMake options, наличие compiler/dependencies и выбранную платформу. Не смешивай host, Docker ARM64, разные SDL forks и разные toolchains в одном build directory. Не перестраивай чужую существующую конфигурацию без понимания её назначения.

Для **поддерживаемого native host**, не как H700-инструкция, исходная документация задаёт:

```sh
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAURORA_ENABLE_TESTS=ON -DMP_BUILD_TESTS=ON
cmake --build build/native -j 2
ctest --test-dir build/native -L port --output-on-failure
```

Фильтр `-L port` не означает прогон всех Aurora tests. После конфигурации используй `ctest --test-dir build/native -N` для списка и выбирай относящиеся к задаче tests; для полного suite запускай CTest без label-фильтра. `AURORA_ENABLE_TESTS` — имя опции новой базы, не старое donor `AURORA_BUILD_TESTS`.

Требования compiler/library сверяй с `docs/NATIVE_PORT.md`: C++20 и C23 support для MusyX. Успешный donor build на Clang16 не подтверждает совместимость всего нового upstream с тем же toolchain. Configure может скачивать зависимости и требовать сеть/место на диске.

Проверяй от узких tests к broader build. Для новых ARM64 artifacts проверяй архитектуру, ELF interpreter, `NEEDED`, `GLIBC_*`, `GLIBCXX_*`, `CXXABI_*`, bundled SDL3 и `dlopen` firmware SDL2. `--version` не проверяет graphics/audio/input. Host `ldd` в container не заменяет device runtime.

Для performance используй повторяемые сцены, continuous frametimes и отдельные CPU/wall/wait measurements. Не называй один screenshot средним FPS боя и не складывай перекрывающиеся main/worker times. Не обещай 30 FPS до измерения.

В итоговом сообщении указывай изменённые файлы, реально выполненные проверки, их результат и что осталось непроверенным. Для docs-only изменений достаточно проверки ссылок/форматирования; не запускай полный build без необходимости.

## Приставка, сохранения и приватность

- Не сохраняй в репозитории IP/credentials, SSH keys, private paths, ISO, извлечённые game assets, saves, core dumps или неочищенные логи. Адрес устройства получай из текущего разрешённого контекста либо запрашивай у пользователя.
- Разрешение на пассивный SSH inventory не считай разрешением запускать fullscreen probes, игру, deploy, менять настройки или завершать текущую сессию. Активные действия согласовывай отдельно.
- Не меняй firmware libraries, governors, clocks, swap, audio route или system services без отдельного обоснования и разрешения.
- Не запускай старые `deploy_rk3326.sh`/remote debug scripts: там чужие paths/credentials, небезопасные defaults и операции с игровыми данными.
- Используй отдельную test installation; сохраняй и проверяй backups перед migration. Не удаляй caches/settings/save files как первый способ «починить запуск».
- `MP_USER_PATH` не гарантирует изоляцию cards: проверь текущий `CardFolder()` и portable `user/` рядом с executable.
- Проверяй SSH host identity; не отключай host-key verification ради удобства. Не публикуй raw core/`bt full`/environment без privacy review.

## Рабочий процесс

- Начни с `git status`; сохрани чужие local/untracked changes. Не коммить и не создавай ветки без запроса.
- Для атрибуции используй P→R, R→H и сравнение с текущей базой; чтение только donor HEAD скрывает, какой коммит ввёл регрессию.
- Исправляй причину; добавляй regression case там, где это разумно. Не отключай tests или meaningful code ради зелёного результата.
- Не копируй старые dependency providers/версии/абсолютные пути вместо адаптации к текущему build system.
- Если решение архитектурно значимо, сначала представь варианты/риски и согласуй границы. Не превращай документирование в несогласованный перенос renderer.
- По мере реализации обновляй README, проверенные команды и статус; подробные findings сохраняй в обзоре/отдельных целевых документах, не раздувай эту инструкцию журналом каждой сессии.

## Лицензии

[LICENSE](LICENSE) и [NOTICE](NOTICE) — обязательные источники лицензионных границ. MIT не распространяется автоматически на декомпилированный game/SDK code и игровые assets. Упаковка не должна включать disc images или извлечённые игровые данные; bundled dependencies должны сопровождаться своими notices/licenses. Используй существующую инфраструктуру `tools/collect_licenses.sh`, не сокращай её до произвольных двух-трёх файлов.
