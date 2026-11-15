# GAP-A/B — расширение отдельной демки

## Приоритет цели и исправление ухода в сторону

**Лимит Null 16 не является установленным препятствием запуску игры на H700.** Попытка закрыть собственную восьми-case matrix через новый Dawn/backend была неоправданным расширением задачи. Пользователь потребовал проверять связь **любого действия, исследования и гипотезы с целью до начала работы**; обязательный фильтр — [H700_WORKFLOW.md](H700_WORKFLOW.md).

Этот документ сохраняет первоначальный замысел и честный частичный результат, **не предписывает заканчивать Null matrix**. Другой backend/пересборка Dawn ради неё не готовятся; продолжать отладку вспомогательного пути без установленной необходимости для RG40XX H нельзя. Нужные проверки корректности остаются обязательными, неудавшиеся случаи не превращаются в PASS.

## Разрешение и границы

Пользователь согласовал подготовку расширения после [статического исследования](H700_DAWN_INTEGRATION_GAPS.md). Эта итерация реализует только отдельный asset-free harness; game/platform sources, Dawn/SDL pins и firmware не меняются. Deploy, SSH и graphics/device run в разрешение **не входят**.

## Первоначальный объём (не завершён, не самодостаточное основание продолжать)

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

## Статус

**Подготовка расширения осталась незавершённой; Null-матрица не считается блокером порта или целью дальнейшей работы.** Код расширения написан и скомпилирован локально; на Mali новый `capabilities` не запускался. Восемь cases не объявлены пройденными, критерии не ослаблены. Новый технический шаг выбирается только после goal gate для фактического H700-пути, не из желания закончить этот harness.

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

**Исправленный вывод:** не навязывать выбор способов обхода Null limit. Сначала установить, какие limits/fetch/layout действительно нужны Mali и какое решение изменит минимальная проверка. Для следующей реализации нужны обоснованный целевой объём и соответствующее согласование; этот запрос исправления правил сам по себе её не разрешает. Код расширения остаётся локальным частичным экспериментом, не готовым package; ни новый backend, ни новый Dawn build, ни сокращение/отключение тестов не реализованы. Документирование failure и правил не означает успешное завершение расширения.
