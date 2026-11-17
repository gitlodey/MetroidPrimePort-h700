# H700 — игровой RC и результат повторной fresh build

**Локальный package PASS; окно A / GM-18 FAIL: runtime fail-stop, actual exit не достигнут.** Target: Metroid Prime Unbound v0.46.0 на ANBERNIC RG40XX H / H700, muOS 2601.0 Jacaranda. GM-16 и локальная подготовка GM-17 завершены. Пользователь отдельно разрешил A; RC3 установлен, loader check PASS, выполнен один ручной запуск Ports. Запуск и слышимый звук подтверждены пользователем, затем сообщены тормоза и зависание. B/C не выполнялись. 30 FPS — следующий отдельный этап.

## Выбранный неизменяемый payload

Для выполненного окна A выбран **ровно `rc3`**, без пересборки после проверок:

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| `build/h700-game-rc3/h700-game-rc.tar.gz` | 77 462 023 | `2539152dbbece585732db9aa7b9ad5bbc6f8b70c53667aed7d30579b31ccf18a` |
| `H700Game/metroid_prime_port` | 229 211 416 | `2482a3822fed7972b6b446cff5043c041369f6dbb848e70505b4a1780ae1036d` |
| `H700Game/lib/libSDL3.so.0` | 7 263 800 | `8df6e362b508d44cad5a253b4f7533ec4d8a976a8e49f49320d2baa42846a13b` |

Archive root — `H700Game/`. Game executable — unstripped ELF64/AArch64; interpreter `/lib/ld-linux-aarch64.so.1`, exact local `$ORIGIN/lib` RUNPATH. В payload только game, matched SDL, launcher/template, README, audit/provenance/notices/checksums; нет disc/извлечённых игровых assets/user/INI/cards/cache или container EGL/GLES/SDL2/libmali.

Идентичность RC определяется этими bytes/hashes, а не именем текущей ветки. Последующие documentation commits не пересобирают и не переименовывают executable под прежним hash.

## Recipe и frozen source

Две успешные команды выполнены последовательно, без изменений checkout между ними:

```sh
python3 -B tools/h700_game/build.py --package --output build/h700-game-rc2 --jobs 2
python3 -B tools/h700_game/build.py --package --output build/h700-game-rc3 --jobs 2
```

- Source revision: `e86eac74ce673c1c3e04eeffc17a2b222a21e44d`.
- Build-relevant source SHA256: `6e613d78a3cf855c7841e88e5076e837fb349eaf41d9888e930abb68bb072b99`.
- 3344 source files, dirty-source list пуст; host pre/post source checks passed в обоих runs.
- Exact approved image: `sha256:2db4439763783aa796da4268dbd91847ee2c13deef4c8ce2439e0f6bdf926962`, native ARM64, те же14 hashed inputs.
- Offline/read-only container profile; никаких installs, смены backend/pins, GPU или device runs.
- Fresh compile/link: `rc2` 609,094s, `rc3` 547,067s. Каждый build выполнил весь recipe, включая staging и archive gates.

Прерванный ENOSPC `rc1` не является qualifying build и не выбран. После освобождения места пользователем и восстановления Docker оба новых outputs завершены полностью. Старые outputs/input archives не удалялись.

## Что подтвердилось в каждом успешном run

- Full game и необходимые CPU targets compile/link.
- **125/125 scoped CPU/no-init/lifecycle/DVD/skinning/recording/readback/CARD/atomic/launcher CTest PASS.**
- **269/269 FIFO tests PASS.**
- **39/39 shared helper и34/34 game helper tests PASS.**
- Staged `--version`, H700 defaults и settings selftests PASS; shell syntax PASS.
- Actual game+SDL metadata/provider audit, exact bundled SDL imports, payload/mode/hash inventory и archive contents PASS. Staged binary находит свою local SDL без build-directory RPATH или `LD_LIBRARY_PATH`.

Это не execution game loop, skin jobs/renderer на Mali, actual loader/kernel4.9 acceptance, physical pixels/pad/audio или save-station/load proof. Неполный firmware exact-symbol coverage сохранён в `audit.json`, не объявлен runtime PASS.

## Повторяемость: рецепт воспроизведён, байты не идентичны

| Output | Archive SHA256 | Executable SHA256 |
| --- | --- | --- |
| `h700-game-rc2` | `7a0288843a5b7df79ddd124b2191209dd1d21a7fe4f65d6a8ae393659147ecfd` | `d52f54b3218130d8bc4d195c271b6c013524cc2eecf042e40f5da6abd56c7095` |
| `h700-game-rc3` | `2539152dbbece585732db9aa7b9ad5bbc6f8b70c53667aed7d30579b31ccf18a` | `2482a3822fed7972b6b446cff5043c041369f6dbb848e70505b4a1780ae1036d` |

Полностью совпали `source.json`, `input-manifest.json`, `Inputs.cmake`, `environment.json`, `packages.tsv`, providers, `compile_commands.json` и generated `port_build_info.cpp`. Единственное различие CMakeCache — `SITE:STRING`, hostname контейнера для test-site metadata. После исключения только этой строки cache SHA256 равен `cf8b9c6c1b872c11796d51477a1edd5d1391354d8ff743ff8670fe46865f805e` в обоих runs.

Payload path/mode/size sets совпали; **все payload files, включая SDL/notices/audit/source/launchers, byte-identical кроме executable**. Его размер одинаков, отличаются ровно24bytes:

- 20bytes GNU build-id в `.note.gnu.build-id`: `8642f0207bb124bc1394b986abfb7f866d681c1f` против `e0df785df9f40f34bf64a2f8ff1dca3ceeb792fa`.
- 4bytes в `.rodata` CARD logging timestamp: `Nov 17 2026 14:04:06` против `Nov 17 2026 14:15:23`. Источник — `extern/aurora/lib/dolphin/card.cpp:336`, существующие `__DATE__`, `__TIME__`.
- Все остальные ELF bytes совпадают. Ranges: `[0x29c,0x2b0)`, `[0x193a1a9,0x193a1ab)`, `[0x193a1ac,0x193a1ae)`. Section/build-id identity дополнительно проверена actual `readelf -W -n/-S`.

Manifest/SHA256SUMS/archive hashes закономерно различаются вслед за executable. **Bit-identical build не заявлен.** Production код не менялся ради удаления timestamp. Проверка и подробный локальный отчёт — `build/h700-game-rc3/reproducibility.json`.

## Установка, legal scope и окно A

[Physical layout, launcher, manual install/update/rollback](../packaging/h700/README.md) входят в проверенный payload. Использовать отдельный physical `.metroid-prime-unbound-h700` рядом с Ports entry; сохранять собственные user/cache между запусками. Template `DISC=''` намеренно не запускает игру до приватной настройки одного literal absolute readable disc path. Не искать старые settings/cards и не менять firmware libraries.

Пакет — **private local RC, не разрешение public distribution**. `licenses/game-coverage.json` сохраняет шесть unresolved групп primary grant/revision evidence; отсутствующие grants не заменены портовой MIT. Нет игровых ресурсов или личных данных.

Пользователь подтвердил выбор: **GameCube USA1.00, ISO уже на приставке**, приставка свободна. В разрешённом preflight read-only header32bytes подтвердил GM8E01/disc0/revision0 и GameCube magic; размер1459978240bytes. Full ISO hash не измерялся; образ не переносился/не изменялся, private absolute path в Git не записан.

Маршрут окна A: **New Game → intro → первая управляемая площадка Frigate Orpheon / Exterior Docking Hangar**, движение, прицеливание и стрельба; затем **PortMenu → Game → Exit game → Exit**, actual child exit и возврат в Ports.

Пользователь явно разрешил A до20мин active time: SSH preflight/transfer выбранного RC3, отдельная установка, один no-init `--version` и один ручной запуск из Ports с наблюдением логов/RAM. Это разрешение использовано; нового approval на уже выполненные действия не требуется. После failure повторный запуск, kill/reset/reboot не разрешены автоматически. Старый порт/данные/ISO/firmware не менялись.

A проверяет физические graphics/HUD/fonts/orientation, встроенный pad, слышимые movie/music/SFX, actual disc/nod path и normal Exit; exit0/CPU tests не заменяют пользовательское подтверждение. Station route требуется к B, late Thermal/X-ray scenes — перед C, не перед A. Actual memory sampled96MiB headroom остаётся runtime gate, не измеренной гарантией.

## GM-18 / A — FAIL, пользователь затем перезагрузил устройство

- Изолированная установка RC3 опубликована отдельной записью `Metroid Prime Unbound`;102 package/manifest/checksum files совпали. Wrapper настроен одним literal private disc path, syntax/hash checks PASS. Перед первым запуском собственные user/cache отсутствовали.
- Единственный разрешённый no-init `--version` вернул0 и `0.46.0`. Один full game process запущен вручную пользователем через Ports, не агентом по SSH. Лог подтверждает Mali-G31/OpenGLES, native640×480, validation/robustness ON, disc USA1.00 и распознанный `muOS-Keys`.
- Пользователь подтвердил запуск и слышимый звук, затем тормоза и зависание; уточнение: на экране меню, звука больше нет. Ранний gameplay/move/aim/fire, physical HUD/colors/orientation и pad ещё не подтверждены; station save/load не проверялись.
- Один ранний интервал240frames:14,3FPS, worst310,2ms; отдельные draw stalls280–326ms и pipeline compilation349–496ms. Это baseline отдельных интервалов, не измерение устойчивого gameplay FPS и не установленная причина отказа.
- Наблюдаемая последовательность: `game failure=1` на `loop: no BeginScene, before loader continuation`, `FIFO downstream wait failed`, root terminal owner retirement, `Dawn release incomplete: imports, contexts or current bindings remain`, затем `normal exit fail-stop: status=3 stage=12 (unknown ownership)`. Watchdog сообщает отсутствие кадров60,1s на frame1024. Эти сообщения не устанавливают первопричину; watchdog stage labels не доказывают, что поток всё ещё выполняет старый UpdateTicks/queue submit.
- В последнем pre-reboot snapshot actual child был жив (sleeping), install lock удерживался. Normal Exit/возврат в Ports не достигнуты. Затем пользователь сам сообщил о перезагрузке приставки. Агент не посылал signals/kill/reset/retry, не запускал новых игровых процессов и не очищал lock/log/user/cache; после перезагрузки по SSH не подключался. Post-reboot состояние/lock не проверены, перезагрузка не засчитана штатным exit.
- Последний logged sampled minimum MemAvailable357456KiB (~349MiB), выше96MiB; RSS high-water в process snapshot613096KiB. После частичного teardown MemAvailable598296KiB (~584MiB). В полученном логе нет признака OOM; полную причину отказа это не исключает.
- Все5 baseline firmware hashes совпали до установки, после no-init и после failure при ещё живом процессе. Это не post-exit verification: actual exit пока отсутствует.

Активное окно A остановлено с FAIL. После пользовательской перезагрузки принудительное завершение прежнего процесса не требуется. Любой новый SSH/device run требует отдельного разрешения; B/C pending, перенос не принят.

### Локальная диагностика после A — не исправление зависания

Существующий `PresentationState` сохраняет sticky `first_error`, но RC3 не выводит его при root shutdown. Observer также объединяет trace exhaustion/invalid context/driver failure, а финальный release — недостоверность observer и actual live ownership. Поэтому сохранённого лога недостаточно для выбора исправления renderer. Исчерпание bounded65536-event trace — конкретная гипотеза, не установленная причина этого запуска; лимит/guards не менялись.

Добавлен только failure-path output: однократный уже latched first_error перед draining, разные причины EGL observation failure с counters и summary rejected Dawn-release proof (trustworthy/events/live contexts/Dawn bindings/imports). Нет нового logging protocol, изменения renderer/scheduler/ownership, увеличения лимитов, обхода certification, retry или ослабления fail-stop.

Full incremental game compile/link,44/44 scoped presentation/lifecycle/no-init CTest,54/54 complete render/lifecycle tests и non-H700 `aurora_core` compile PASS. Source regression проверяет wiring новой диагностики; CPU PASS не воспроизводит зависание Mali. Independent read-only diff review blockers не нашёл. Evidence — existing `build/h700-game-gm03/gm18-diagnostics-*` и non-H700 output. Прежний RC3 и его FAIL неизменны. Новый diagnostic artifact подготовлен ниже; на устройство он не переносился.

### RC4 diagnostics — LOCAL package PASS, device permission pending

После diagnostic commit/push `cb06a13e868ae705ef5aab29fabe3e62bf1a6ab4` выполнен существующий fresh recipe, без смены image/14inputs/backend/профиля:

```sh
python3 -B tools/h700_game/build.py --package --output build/h700-game-rc4-diagnostics --jobs 2
```

- Fresh configure18,169s, full compile/link558,337s;126/126scoped CPU/no-init CTest,269/269FIFO,39/39shared +34/34game helpers PASS. Staged ABI/provider/notices/content/hash/archive/no-init gates PASS.
- Source3344files, dirty-source list пуст; source SHA256 `f9f0742f5827573e2a29854ffcd7770a746df4522d04cc5174baa23ec60c04b7`.
- Archive `build/h700-game-rc4-diagnostics/h700-game-rc.tar.gz`,77463802bytes, SHA256 `7308f9a98ba41a4e08405b8073666b38eecc6f614e135740784fdba65e74a64c`.
- Executable SHA256 `bb4e95c4ab11f5398bed03d74a2d70b1fa554eae47516be261025605500ce61b`; SDL тот же `8df6e362b508d44cad5a253b4f7533ec4d8a976a8e49f49320d2baa42846a13b`.
- Это диагностический кандидат, **не исправление зависания, не device PASS и не public distribution clearance**. RC3 не перезаписан; новую сборку после gates не меняли.

Следующий минимальный device-шаг отдельно согласуется: до10мин active time, SSH preflight после пользовательской перезагрузки, проверка отсутствия game/copier данной установки, снятие только собственного stale lock при доказанно завершённом процессе, update только payload/с сохранением собственного user/cache, один no-init loader check нового binary и один manual Ports run до того же меню. Ждать повторения отказа либо до5мин наблюдать responsive menu, затем normal Exit; New Game/station/save/reset не включать. Логи/RAM наблюдает агент, пользователь подтверждает экран/звук/реакцию меню. Нет automatic kill/reset/retry; любой hang/unknown останавливает run. Это диагностический повтор failure path, не автоматически A PASS. До явного permission никаких post-reboot SSH/deploy/run не выполнять.

## Evidence на локальном диске

Оба completed outputs сохраняют `source.json`, `input-manifest.json`, environment/packages/provider/ELF records, command logs, package audit/notices/manifest/SHA256SUMS и `package-result.json`. Выбранный archive и comparison report — в `build/h700-game-rc3/`. Большие artifacts/raw logs в Git не добавлены. Device evidence A, raw logs и приватные transport/session records — только ignored `build/h700-game-a-rc3/`; `window-a-result.json` фиксирует FAIL и удержанный процесс. Текущий краткий статус — [H700_PROGRESS.md](H700_PROGRESS.md).
