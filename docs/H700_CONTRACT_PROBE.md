# GAP-A/B — расширение отдельной демки

## Приоритет цели и исправление ухода в сторону

**Лимит Null 16 не является установленным препятствием запуску игры на H700.** Попытка закрыть собственную восьми-case matrix через новый Dawn/backend была неоправданным расширением задачи. Пользователь потребовал проверять связь **любого действия, исследования и гипотезы с целью до начала работы**; обязательный фильтр — [H700_WORKFLOW.md](H700_WORKFLOW.md).

Этот документ сохраняет первоначальный замысел и честный частичный результат, **не предписывает заканчивать Null matrix**. Другой backend/пересборка Dawn ради неё не готовятся; продолжать отладку вспомогательного пути без установленной необходимости для RG40XX H нельзя. Нужные проверки корректности остаются обязательными, неудавшиеся случаи не превращаются в PASS.

## Разрешение и границы

Пользователь согласовал подготовку расширения после [статического исследования](H700_DAWN_INTEGRATION_GAPS.md). Эта итерация реализует только отдельный asset-free harness; game/platform sources, Dawn/SDL pins и firmware не меняются. Deploy, SSH и graphics/device run в разрешение **не входят**.

## Текущий согласованный объём: закончить демку для H700

Пользователь явно разрешил довести демку с новыми вводными. Цепочка текущего шага: **запуск новой игры на H700 → неизвестен фактический U device/fetch/layout contract на Mali → подготовить один режим его запроса и проверки выбранного layout → по device-результату решить, нужна ли локальная адаптация U → закончить на проверенном пакете, без deploy**.

- `--mode capabilities` запрашивает default U contract с validation, выводит adapter/requested/final-device limits и features, затем проверяет **только выбранный U fetch/lightmap path**, normal и shadow-receiver layouts, создаваемые U. Принудительных альтернативных profiles нет. Отказ выбранного layout остаётся nonzero failure.
- `--layout-self-test` — ограниченный контейнерный контроль, не замена Mali: сохраняются четыре реальные Null cases бюджета 16 (storage-positive / texture-negative × normal/shadow). Null profile 17 больше не является release gate. Его прежний отказ сохранён ниже; positive lightmap=17 / negative total=18 при device budget 17 **не объявляются проверенными**. CPU/source assertions для counts 17/18 остаются.
- В manifest и Null report явно записываются budget 16, четыре случая и границы покрытия. Старый eight-case success marker не принимается за результат новой проверки. Ни validation, ни проверка конкретной причины отрицательного case не ослабляются.
- Приёмка подготовки: helper tests, input/reference hashes, ARM64 compile/link, прежний ABI/provider gate, CPU self-test, четыре Null validation cases, licenses и итоговый manifest проходят. **Готовность пакета к согласованному device test не означает device PASS нового binary.**
- Docker image, Dawn/SDL pins и production code остаются прежними. Fresh output сохраняет предыдущие попытки. `capabilities`, offscreen и bridge не запускаются в контейнерных smoke tests; SSH/deploy/device run отдельно не разрешены.
- При успешном будущем device request/layout — переходим к решению по интеграции и необходимым shader/resource checks; при отказе фиксируем конкретный U/Mali барьер. Не расширяем Null/Dawn ради полноты собственной матрицы.

**Текущий статус: подготовка завершена, пакет готов к отдельно согласованной проверке на H700.** Новый `capabilities` на приставке **не запускался**; device PASS прежней демки к этому binary не переносится. Исторические восемь cases и остановка ниже не являются текущим acceptance.

### Проверенные результаты текущего объёма

Из корня репозитория выполнены:

```sh
python3 -B tools/h700_graphics_probe/test_build.py
python3 -B tools/h700_graphics_probe/build.py --check-inputs
python3 -B tools/h700_graphics_probe/build.py --output build/h700-graphics-probe-contract-v2 --jobs 2
```

| Проверка | Результат |
| --- | --- |
| Python helper regression tests | **62/62 PASS**; исходные проверки сохранены, ошибочные/старые markers и wrong-budget output не принимаются |
| Input integrity / U mirror | SDL/Dawn archive+tree и пять U-reference SHA-256 — PASS |
| Offline ARM64 C++20 compile/link | **PASS**, Clang 16, `-Wall -Wextra -Wpedantic -Werror`; прежний builder image |
| ELF/provider/version/selected-export gate | **PASS** для executable и bundled SDL3; это не полный symbol/runtime audit |
| Container `ldd` | **PASS**, все dependencies разрешены в builder |
| `--help` / CPU `--self-test` | **PASS**, без SDL/EGL/Dawn initialization; policy/undefined-limit/count assertions сохранены |
| `--layout-self-test` | **4/4 PASS** при effective Null budget 16: два storage-positive и два конкретных texture-budget rejections, normal/shadow |
| Package | Опубликован после gates/licenses/manifest, все SHA-256 перепроверены; 50 файлов вместе с manifest, `package.partial` отсутствует |
| Device/graphics run | **Не выполнялся**; режимы capabilities/offscreen/bridge не запускались в контейнере |

Null output явно содержит:

```text
COVERAGE Null budget=16 only; device-budget-17/lightmap cases and Mali remain unvalidated
PASS layout-self-test cases=4 budget=16 (Dawn Null validation only; no SDL/EGL/GPU)
```

### Готовый artifact

`build/h700-graphics-probe-contract-v2/package/` содержит executable, свою SDL3, licenses, metadata и manifest. Runtime libc/libstdc++/EGL/GLES/SDL2 из container туда не копируются. `metadata/layout-self-test.txt` сохраняет реальный Null output; `manifest.json` отдельно записывает `layout_validation_scope` и непроверенные cases.

| Файл | Размер, bytes | SHA-256 |
| --- | --- | --- |
| `h700_graphics_probe` | 13582168 | `b88571f3a7659d0f1ec15a86975483b91e722c6a3ab17a2f56691226b7a5f09d` |
| `libSDL3.so.0` | 2771560 | `ffb36e54c7c25f17f788f74550fcfd7da7136a66fa1e24771497b0a18f134e70` |
| `manifest.json` | — | `76d9c4cc037975a3e94ffa09e5fcb604679d436011f3dfc297b2ebf8694f8bf8` |

Executable требует максимум GLIBC_2.38 / GLIBCXX_3.4.32 / CXXABI_1.3.8, SDL — GLIBC_2.38. Прямые NEEDED executable: SDL3, EGL, GLESv2, libstdc++, libm, libgcc_s и libc; у SDL только libm/libc. Loader `/lib/ld-linux-aarch64.so.1`, RUNPATH `$ORIGIN`. Builder identity: `sha256:2db4439763783aa796da4268dbd91847ee2c13deef4c8ce2439e0f6bdf926962`.

Для повторения build нужен **другой новый output suffix**, существующий каталог не удалять/перезаписывать. Старые `abi238-final` и failed `contract-v1` сохранены, но для новой capability-проверки используется только `contract-v2`.

### Следующий шаг — отдельно разрешённый device test

После разрешения пользователя и проверки актуального SSH/free-screen окна: временный test directory, полная передача package с manifest verification, изолированные HOME/XDG/TMPDIR и core dumps off, loader/CPU smoke checks, затем один запуск `--mode capabilities` с внешним timeout (35 s + 5 s до KILL только process group демки). Не запускать дополнительные graphics modes автоматически.

Per-process settings, как в проверенном bootstrap: `LD_LIBRARY_PATH` указывает только на test package, `SDL_VIDEODRIVER=sdl2`, `SDL3SHIM_SDL2_VIDEODRIVER=mali`. `capabilities` может занять/очистить экран, **цветной узор не рисует**; результат — stdout/request/layout diagnostics и exit status. Изменять игру, firmware, clocks или services не требуется.

Успех даст фактические Mali limits, selected fetch/lightmap policy и validation выбранных U layouts. Отказ RequestDevice или выбранного layout должен остаться видимым nonzero результатом: это вход для конкретного решения по порту, не повод выключать validation. После теста — удалить только собственный test directory и записать очищенный результат. Новый запуск сейчас не выполнен и этим документом не авторизован.

## Первоначальный объём (история первой попытки, не текущая приёмка)

### GAP-A — будущий режим `--mode capabilities`

- **Зачем:** узнать полный native U RequestDevice contract, а не только 4096/64 tiny demo.
- **Решение:** прежний проверенный SDL2/Mali bootstrap и OpenGLES Compatibility adapter, полные adapter/final-device limits с Compatibility chain и все объявленные/включённые features. Зеркало default U policy определяет texture fetch, no-compute и lightmap; env force/opt-out overrides не применяются.
- **Device:** точные required limits и семь conditional feature candidates U. Native thread toggle включён; `skip_validation` и `disable_robustness` явно запрещены. Shader cache, application overrides и WebGPU surface не копируются: это external-display harness, не init всей игры. Отличия от U логируются.
- **Проверка:** будущий device run должен вывести requested и effective budgets отдельно, проверить выбранный реальный GX layout и diagnostic alternative. Несоответствие выбранного U layout — nonzero failure, даже если отрицательный diagnostic case ожидаем. Никакого скрытого уменьшения layout или запроса больших limits.
- **Ограничение:** режим создаёт SDL window и может занять экран. Сейчас только compile/link и проверка CLI; не запускается на host/device. Это не shader/pixel, threading или recreation test.

### GAP-B — `--layout-self-test` без окна

- **Зачем:** проверить реальную Dawn validation для GX pipeline layout из трёх bind groups, а не только арифметику counts.
- **Решение:** явный Dawn Null Compatibility adapter, два отдельных device profiles с effective texture budgets 16 и 17. Все layouts повторяют U, включая normal/shadow variants и 64-byte immediates. Storage path должен пройти, texture-fetch path — получить конкретную sampled-texture/fragment-budget validation error: всего восемь случаев.
- **Отрицательный результат:** неожиданный success, другой тип/причина ошибки, uncaptured error, device loss или timeout проваливают self-test. Нельзя принять произвольную validation error за доказательство budget mismatch.
- **Проверка:** запуск только в контейнере, explicit Null backend без fallback на GPU; success marker появляется лишь после всех восьми проверок. Null не создаёт SDL/EGL contexts и не доказывает Mali support или pixels.

### Общие safeguards и приёмка подготовки

- Зеркало request/layout вынесено в `contract.hpp/.cpp`, без линковки game sources. Исходные пять reference files U проверяются по SHA-256 до configure и фиксируются в manifest. При drift требуется пересмотреть mirror, не менять production files ради прохождения gate.
- CPU `--self-test` дополнен policy/undefined-limit/layout-count assertions; по-прежнему без инициализации Dawn/SDL/EGL.
- CMake сохраняет C++20 / `-Wall -Wextra -Wpedantic -Werror`; builder image и ABI gates прежние.
- Safe build smoke modes: `--help`, `--self-test`, `--layout-self-test`. Только Null-mode и CPU checks; ни `capabilities`, ни offscreen/bridge build helper не запускает.
- Fresh output под `build/`, прежние artifacts не перезаписываются. Пакет публикуется только после tests, ABI, manifest и licenses; новый artifact не наследует device-validation старого binary.
- **Критерий готовности:** host helper tests, native ARM64 compile/link, ELF/provider gate и все восемь Null validation cases проходят; документация содержит команды, hashes и остаточные ограничения.
- **Откат:** не использовать новый отдельный artifact; прежний проверенный package сохранён. На приставке изменений нет.
- **Оценка:** подготовка GAP-A и GAP-B — отдельные небольшие сессии порядка 1–2 часов каждая; ожидание сборки отдельно. U shader workload, фактический multi-context progress и per-import lifetime — последующие согласуемые шаги, не часть этого расширения.

## История первой незавершённой попытки

**На момент этой попытки подготовка расширения осталась незавершённой; Null-матрица не является блокером порта или целью дальнейшей работы.** Код расширения написан и скомпилирован локально; на Mali новый `capabilities` не запускался. Восемь cases не объявлены пройденными, критерии не ослаблены. Новый технический шаг выбирается только после goal gate для фактического H700-пути, не из желания закончить этот harness.

### Фактические проверки

- `python3 -B tools/h700_graphics_probe/test_build.py`: **62/62 PASS** (43 прежних + 19 новых helper regression tests).
- `python3 -B tools/h700_graphics_probe/build.py --check-inputs`: PASS; SDL/Dawn archive/tree и пять U-reference SHA-256 совпали.
- `python3 -B tools/h700_graphics_probe/build.py --output build/h700-graphics-probe-contract-v1 --jobs 2`: **FAIL на Null runtime self-test**, не на compile/link или ABI gate.
- ARM64 C++20 compile/link с warnings-as-errors и ELF/provider gate прошли. `--help` и CPU `--self-test` прошли; Null budget 16 прошёл storage-positive и texture-negative normal/shadow — **4 случая**. Negative cases дали именно sampled-texture fragment-budget error (17 против 16).
- При попытке создать следующий Null device с бюджетом 17 получен отказ: `Required limit (17) is greater than the supported limit (16)`.
- Итоговый `package/` **не опубликован**; сохранены failed output и `package.partial/`. Прежний device-validated artifact не перезаписан. Новый binary не считается готовым для deploy.
- SSH, deploy, EGL/SDL device modes и изменения firmware не выполнялись. Ни validation, ни acceptance marker на восемь случаев не ослаблены.

### Причина отказа теста и история ошибочного расширения scope

В первичном [Null backend этого pin](https://github.com/Timdeuces/dawn/blob/118436b3f8f68583eb86e811ca605dc41637e8cc/src/dawn/native/null/DeviceNull.cpp#L98-L108) `InitializeSupportedLimitsImpl()` берёт стандартные limits и повышает только immediate size; sampled-texture maximum остаётся 16. Backend toggles не задают override. Публичный `Adapter::SetUseTieredLimits(false)` не повышает реальную capability. Это **не результат запроса Mali** и не причина менять renderer.

Ниже сохранены предложенные ранее варианты **как история**, не как актуальный выбор, который требуется от пользователя. Особенно предложение отдельного тестового Dawn/backend без доказанной необходимости для H700 было ошибкой.

| Исторически предложенный вариант | Предполагаемые плюсы | Минусы / влияние |
| --- | --- | --- |
| Сохранить pinned package; принять 4 реальные Null cases бюджета 16, а 17/18 оставить CPU/source checks и будущей проверке на подходящем backend | Нет новых dependency artifacts и ослабления validation; подтверждён реальный 16-budget failure, ближайший шаг небольшой | Меняется критерий GAP-B: positive lightmap=17 / negative total=18 пока не покрыты реальной Dawn validation при бюджете 17. Их нельзя объявлять закрытыми |
| Отдельно подготовить тестовый Dawn/другой backend, честно поддерживающий >=17 | Можно сохранить исходный eight-case acceptance | Новый согласуемый build/dependency/backend scope; наличие подходящего окружения пока не доказано. Изменённый Null test artifact не тождественен исходному prebuilt; повышать лимит через отключение validation недопустимо |

**Исправленный вывод:** не навязывать выбор способов обхода Null limit. Сначала установить, какие limits/fetch/layout действительно нужны Mali и какое решение изменит минимальная проверка. На этом этапе запрос исправления правил сам по себе не разрешал реализацию; код оставался локальным частичным экспериментом, не готовым package. Новый backend и новый Dawn build не создавались, failures не объявлялись пройденными. Последующее явное разрешение закончить целевую демку и уточнённая приёмка записаны выше; документирование исторического failure само по себе не означает успех нового build.
