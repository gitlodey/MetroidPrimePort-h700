# H700 — план оставшейся работы

**Редакция3, 2026-11-17, УТВЕРЖДЕНА пользователем. Локальные GM-12…17 выполняем с commit/push; device-окна отдельно.** [Подробный необходимый остаток](H700_GAME_REMAINING_PLAN.md), [исходный план и результаты](H700_GAME_PLAN.md), [прогресс](H700_PROGRESS.md). Неутверждённый план редакции2 с16подшагами заменён, его дополнительные требования не действуют.

## Главное

Нужна **Metroid Prime Unbound v0.46.0 на RG40XX H / H700 под muOS 2601.0 Jacaranda**, а не новый framework или идеальная тестовая матрица. Графика/UI, pad, слышимый звук, CARD save/load обязательны.30FPS — потом.

GM-01…GM-17 завершены локально. Две fresh build и comparison прошли; выбран [rc3](H700_GAME_RC.md). В отдельно разрешённом A header ISO подтвердил USA1.00; RC3 запустился со звуком, затем застыл в меню/fail-stop, normal exit не достигнут. Пользователь сам перезагрузил приставку. Подготовлен отдельный RC4 diagnostics, не runtime fix; read-only post-reboot SSH отдельно разрешён и выполнен, update/повтор ещё не разрешены. GM-12 включил root handshake, checked sizing/progress-aware waits и замкнутых consumers: full ARM64 game build,38/38CPU/no-init/DVD CTest и изменённые non-H700 game TUs PASS. Подробности трёх итераций — в полном остатке. Настоящий игровой executable собирается, backend и lifecycle механизмы есть. **Первый запуск FAIL, полная приёмка игры не завершена.** Железо/ОС/build facts берём из документов, заново не исследуем.

## Остаток — от известного дефекта к реальной игре

| Шаг | Необходимое действие | Проверяемый результат |
| --- | --- | --- |
| **1. GM-12 — LOCAL PASS** | Корректные sizes/token waits и передача отказа через реальные model/shadow/boid consumers до root; использовать готовый GM-11b cleanup | Нет неправильных workspace/draw/free. Running jobs держат свои данные, Unknown — fail-stop. Ring32MiB и scheduler не меняются |
| **2. GM-13 — LOCAL PASS** | ARM64 game build, относящиеся CPU tests, non-H700 core/GX и изменённые game TUs | Замкнутый production status/lifetime path, а не только helper tests. Без GPU в container и без новой backend-матрицы |
| **3. GM-14/15 — LOCAL PASS** | Свои executable/user/card/cache, чистые начальные настройки; тонкий Ports launcher с явным disc argument, правильным SDL/audio env и lock/exit handling | Старый порт/данные не используются. Настройки/карта сохраняются. Нет demo watchdog, автоматического kill или ложного exit0 |
| **4. GM-16/17 — LOCAL PASS** | Адаптировать существующие package/audit helpers к игре; local SDL loader path, actual ABI/notices/source/input/hash; повторить fresh build того же source state | Один идентифицированный RC, воспроизводимый рецепт, install/rollback инструкции. Без system-library replacement и нового build framework |
| **5. GM-18/19/20 — реальная приёмка** | A: gameplay/display/pad/audio/Exit. B: rooms/RAM → station save → Exit → новый процесс/CARD load → затем reset. C: оставшиеся materials/visors/EFB/input/audio | Пользователь видит/слышит результат; настоящая своя карта загружается новым процессом. Missing scene не становится PASS |
| **6. GM-21 — завершить перенос** | Зафиксировать ровно испытанный пакет, checklist/ограничения и инструкции | Принятая игра на указанной приставке/ОС; без непроверенной «финальной» пересборки |

Skinning выполняется безопасными рабочими commits: сначала готовность root/consumers, затем включение новых отказов. Доказанный прямой caller/header/test — часть исправления, не повод снова спрашивать одобрение. Отдельный policy-framework для этого не нужен.

## Что убрано из обязательной работы

- Всеобщая prevalidation INI и блокировка небазовых UI-настроек. Для цели достаточно собственной чистой установки и согласованного retail-профиля; произвольные ручные изменения не входят в эту приёмку. INI не перезаписывается молча.
- Новый ready/health/final protocol log copier. Оставляем существующее логирование/redaction/RAM diagnostics с узким безопасным byte cap и продолжением drain; потеря лога не означает отсутствие ошибок.
- Новый inventory железа/firmware, полная symbol/platform matrix, повтор graphics demo и смена pins/backend.
- Подготовка поздних visors **до первого запуска**. Они нужны перед C и до полной приёмки, но не блокируют A.
- 16самостоятельных local gates и оценка срока умножением их числа на часы. Нужны законченные рабочие итерации, а не раздувание структуры.

## Условия выполнения

**Локально:** прежние exact image/14pins/backend/профиль. Каждая завершённая безопасная итерация — проверка → документация → commit/push. Крупный skinning block не выдаётся за одну часовую правку;1–2ч — ориентир рабочей итерации, не причина снова переписывать план из-за каждого caller.

**На устройстве:** A до20мин, B до30мин, C до30мин — каждое отдельно разрешается, с конечной последовательностью процессов. SSH/deploy/log observation только по явному разрешению. Разрешение A уже использовано, run завершился FAIL; read-only post-reboot SSH уже отдельно разрешён/выполнен; снятие stale lock/update/no-init и новый diagnostic menu run до10мин требуют нового permission. B/C пока не разрешены. До A выбираем disc/container/private path и раннюю сцену; station route нужен к B, late-scene access — к C. Прохождение отдельно.

**Safety:** sampled96MiB остаётся начальной policy, не измеренной гарантией. При известной ошибке/недостатке памяти прекращаем нагрузку и выходим безопасно; CARD не прерываем. Зависание/Unknown — FAIL/PARTIAL, не kill/retry permission. Старые данные/firmware/services/clocks/swap не трогаем.

**Если обнаружится реальный runtime-дефект:** минимальный local fix того же решения + regression + новый hash/RC, с теми же image/pins; повторяем failing/зависимые проверки. Для окончательного результата обязательные пункты должны быть подтверждены на принятом RC, старый PASS новому binary самовольно не переносится. Новые device-процессы согласуются отдельно. Архитектурная смена/ослабление safety/критериев — остановка, обычная техническая правка — нет.

**Текущее выполнение:** после восстановления Docker выполнены два новых successful fresh package runs:125/125CPU +269/269FIFO +39/39shared +34/34game helpers PASS в каждом, staged ABI/provider/notices/archive gates PASS. Выбран [RC3 и его immutable hash](H700_GAME_RC.md). Source/input/providers/compile commands совпали; executable отличается только CARD build-time string и GNU build-id (24bytes), поэтому bit-identical не заявлен. Это local evidence RC3, не device PASS: разрешённый A дал запуск/звук, затем menu hang и incomplete Dawn release; normal exit не достигнут, пользователь сам перезагрузил приставку. Для установления причины добавлен только failure-path output без смены guards/профиля; RC4 diagnostics fresh package прошёл126/126CPU +269/269FIFO +39/39shared +34/34game helpers и package gates. [Новый hash и scope diagnostic repeat](H700_GAME_RC.md#rc4-diagnostics--local-package-pass-device-permission-pending); он ещё не установлен и не является исправлением/приёмкой. Read-only post-reboot preflight отдельно разрешён и завершён: игра/copier не запущены,5firmware hashes совпали, оставшийся lock не удалён. Update/remove-lock/no-init и один menu-only run до10мин отдельно согласуются; B/C pending, late scenes нужны к C.
