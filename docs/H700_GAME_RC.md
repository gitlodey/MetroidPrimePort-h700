# H700 — игровой RC и результат повторной fresh build

**Локальный package PASS, device acceptance PENDING.** Target: Metroid Prime Unbound v0.46.0 на ANBERNIC RG40XX H / H700, muOS 2601.0 Jacaranda. GM-16 и локальная подготовка GM-17 завершены. Пользователь выбрал GameCube USA1.00 в ISO и сообщил, что образ уже на приставке, приставка свободна. Это пользовательские сведения, не fresh device verification; SSH/deploy/run ещё не разрешены. 30 FPS — следующий отдельный этап.

## Выбранный неизменяемый payload

Для будущего окна A выбран **ровно `rc3`**, без пересборки после проверок:

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

## Установка, legal scope и оставшийся шаг к A

[Physical layout, launcher, manual install/update/rollback](../packaging/h700/README.md) входят в проверенный payload. Использовать отдельный physical `.metroid-prime-unbound-h700` рядом с Ports entry; сохранять собственные user/cache между запусками. Template `DISC=''` намеренно не запускает игру до приватной настройки одного literal absolute readable disc path. Не искать старые settings/cards и не менять firmware libraries.

Пакет — **private local RC, не разрешение public distribution**. `licenses/game-coverage.json` сохраняет шесть unresolved групп primary grant/revision evidence; отсутствующие grants не заменены портовой MIT. Нет игровых ресурсов или личных данных.

Пользователь подтвердил выбор: **GameCube USA1.00, ISO уже на приставке**, приставка свободна. Локальная подготовка GM-17 завершена; actual image metadata/path ещё не читались. Существующий ISO не переносим/не изменяем, private absolute path уточняем перед запуском в отдельно разрешённом preflight и не пишем в Git.

Маршрут окна A: **New Game → intro → первая управляемая площадка Frigate Orpheon / Exterior Docking Hangar**, движение, прицеливание и стрельба; затем **PortMenu → Game → Exit game → Exit**, actual child exit и возврат в Ports.

Следующее действие требует отдельного явного разрешения A до20мин active time: SSH preflight/transfer выбранного RC3, отдельная physical установка по existing layout, один no-init `--version` для loader check и один ручной запуск игры из Ports со штатным log copier. Агент наблюдает логи/имеющиеся RAM diagnostics, пользователь подтверждает physical graphics/pad/audio. Не трогаем старый порт/данные/ISO/firmware и не применяем kill/reset/retry при зависании. Никаких SSH/device действий до permission. Сведения о свободной приставке повторно проверяются только как необходимый session-safety preflight, не новый inventory.

A проверяет физические graphics/HUD/fonts/orientation, встроенный pad, слышимые movie/music/SFX, actual disc/nod path и normal Exit; exit0/CPU tests не заменяют пользовательское подтверждение. Station route требуется к B, late Thermal/X-ray scenes — перед C, не перед A. Actual memory sampled96MiB headroom остаётся runtime gate, не измеренной гарантией.

## Evidence на локальном диске

Оба completed outputs сохраняют `source.json`, `input-manifest.json`, environment/packages/provider/ELF records, command logs, package audit/notices/manifest/SHA256SUMS и `package-result.json`. Выбранный archive и comparison report — в `build/h700-game-rc3/`. Большие artifacts/raw logs в Git не добавлены. Текущий краткий статус — [H700_PROGRESS.md](H700_PROGRESS.md).
