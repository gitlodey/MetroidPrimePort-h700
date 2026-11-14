# H700 — новое окружение сборки демки

## Решение и границы

Пользователь явно согласовал новое окружение сборки после [проверки firmware ABI](H700_RUNTIME_ABI.md). Меняем **только Docker builder**, не macOS, firmware, Dawn pin или архитектуру graphics demo. Старый Bookworm image сохраняется.

Причина: prebuilt Dawn использует C23 parsing functions `GLIBC_2.38` и C++ string helper `GLIBCXX_3.4.31`. Firmware их предоставляет, прежний builder glibc 2.36 / GLIBCXX 3.4.30 — нет. Поэтому на этом шаге сохраняем проверенный Dawn archive и используем совместимый build runtime вместо его пересборки.

**Выбран минимальный native Linux ARM64 builder на Ubuntu 23.10 Mantic.** Это build-only compatibility baseline для отдельной демки, не Jacaranda sysroot и не автоматически утверждённый profile полной игры. Он подходит по проверенным runtime functions; конечный artifact всё равно проверяется отдельно.

## Компромисс EOL

Ubuntu 23.10 завершила поддержку 2024-07-11 — [официальное объявление](https://lists.ubuntu.com/archives/ubuntu-announce/2024-July/000303.html). Наличие архивного `mantic-security` не означает продолжающуюся security-поддержку.

Принимаемые ограничения для этого isolated builder:

- official base фиксируется по ARM64 manifest digest;
- apt использует только официальный `old-releases.ubuntu.com`, сохраняет signature verification и дополнительно проверяет SHA-256 подписанных `InRelease`;
- runtime/compiler верхнего уровня закреплены точными package versions; полный установленный package manifest сохраняется;
- build context ограничен recipe, `.dockerignore` и отдельным `verify_environment.py`; только verifier копируется в image, repository/assets/secrets не передаются;
- компиляция демки выполняется без сети, с read-only source mount и отдельным writable output;
- контейнерные libc/libstdc++/EGL/GLES не поставляются на приставку;
- долгосрочный поддерживаемый builder с matched sysroot — возможное последующее решение, а не параллельно начатая реализация.

Base manifest проверен через Docker registry:

```text
ubuntu:23.10
linux/arm64/v8 manifest: sha256:72c9c923b9807a752ba9c8ba41432e62bb88312de9d15b536caeaf3ea5965ad5
multi-platform index: sha256:fd7fe639db24c4e005643921beea92bc449aac4f4d40d60cd9ad9ab6456aec01
```

## Закрепляемые inputs

| Input | Значение |
| --- | --- |
| libc6 / libc6-dev | `2.38-1ubuntu6.3` |
| libstdc++6 / libstdc++-13-dev / libgcc-s1 | `13.2.0-4ubuntu3` |
| Clang | `1:16.0.6-15` |
| CMake | `3.27.4-1` |
| Ninja | `1.11.1-2` |
| python3 default package | `3.11.4-5` |
| pkg-config | `1.8.1-2` |
| libegl-dev / libgles-dev | `1.6.0-1` |

Точные versions проверены в официальных ARM64 Packages indexes. Runtime `.deb` также проверены по SHA-256 из индексов: C23 symbols определены с GLIBC_2.38, нужный string helper — GLIBCXX_3.4.31, libstdc++ maxima — GLIBCXX_3.4.32 / CXXABI_1.3.14. Измерение установленного image остаётся обязательным.

Подписанные archive indexes, проверенные 2026-11-14:

| Pocket | InRelease SHA-256 |
| --- | --- |
| [mantic](https://old-releases.ubuntu.com/ubuntu/dists/mantic/InRelease) | `d5539f456b74f789f8977eeee5f3c2399225de8a424b4cfff66843b78697a311` |
| [mantic-updates](https://old-releases.ubuntu.com/ubuntu/dists/mantic-updates/InRelease) | `22d7500cff13df9ee4be385d661d63c964c72ff81487e737adc05b3d6e575520` |
| [mantic-security](https://old-releases.ubuntu.com/ubuntu/dists/mantic-security/InRelease) | `5fca3465e4e80bccdd0eba53c90a6575a908c3b467fd293f2be58da42afd445e` |

`Valid-Until` в этих files отсутствует. Отключать GPG authentication или подменять источники пакетов не требуется. При изменении index/digest build должен остановиться, а не незаметно получить другие inputs.

## Утверждённый объём следующей работы

### ENV-01 — отдельный image и проверка runtime

- **Зачем:** устранить доказанное несоответствие link runtime без подмены firmware.
- **Объём:** отдельный Dockerfile и минимальный build context в `tools/h700_graphics_probe/`, новый tag `metroid-h700-probe:abi238-v1`; прежний image не перезаписывать.
- **Проверки:** Linux AArch64, glibc 2.38, конкретные C23 exports, GLIBCXX_3.4.32 / CXXABI_1.3.14 и `_M_replace_cold@@GLIBCXX_3.4.31`; package/tool manifest.
- **Результат:** зафиксированный image ID с подходящими runtime functions. До проверки не объявлять image готовым.
- **Оценка:** до 1–2 часов активной работы; downloads/provisioning отдельно.
- **Откат:** вернуть выбор старого builder; ни одна библиотека на host/приставке не заменяется. Автоматический Docker prune не выполняется.

### ENV-02 — демка, ABI gate и безопасные smoke modes

- **Зачем:** подтвердить, что причиной link failure был builder, а не требование сменить Dawn/firmware.
- **Объём:** переключить isolated build helper на новый image, сохранить source/package verification и fresh output policy; не менять game sources.
- **Проверки:** host tests helper, compile/link, artifact ELF/required symbols/version nodes и NEEDED против измеренного firmware snapshot; bundled SDL; container `--help` и CPU `--self-test`.
- **Результат:** stage для будущей device-проверки, с manifest, notices и явной отметкой «на устройстве не проверено». Не копировать container EGL/GLES/libc/libstdc++ в package.
- **Оценка:** до 1–2 часов активной работы; cold SDL build отдельно.
- **Откат:** сохранить неудачный output как diagnostic, не трогая предыдущие builds; при новом ABI blocker остановиться и согласовать изменение.

Graphics modes, SSH deploy и fullscreen probes в этот объём **не входят**. Нужны отдельное разрешение на окно и готовый ABI-проверенный artifact.

## Статус выполнения

**ENV-01 и ENV-02: проверки выполнены 2026-11-14.** Прежний link blocker устранён без пересборки/смены Dawn и без изменений firmware. Device graphics остаётся отдельным непроверенным этапом.

Проверенные команды из корня репозитория:

```sh
docker build --platform linux/arm64 --progress=plain \
  -f tools/h700_graphics_probe/Dockerfile.abi238 \
  -t metroid-h700-probe:abi238-v1 tools/h700_graphics_probe
python3 -B tools/h700_graphics_probe/test_build.py
python3 -B tools/h700_graphics_probe/build.py --check-inputs
python3 -B tools/h700_graphics_probe/build.py \
  --output build/h700-graphics-probe-abi238-final --jobs 2
```

Последняя команда требует **новый** output; для повторения выбрать другой суффикс, не удалять предыдущий build. Helper не скачивает зависимости: нужны проверенные cached SDL/Dawn inputs, описанные в [демке](H700_GRAPHICS_PROBE.md).

| Проверка | Фактический результат |
| --- | --- |
| Docker build / signed archive hashes | PASS; narrow context около 6 KB |
| Image ABI verifier | PASS; AArch64, glibc 2.38, все пять exact exports, GLIBCXX_3.4.32 / CXXABI_1.3.14 |
| Tools | Clang 16.0.6, CMake 3.27.4, Ninja 1.11.1, binutils 2.41, Python 3.11.6 |
| Package inventory | 231 installed package entries; полный `packages.tsv` сохранён |
| Host unit tests | **43/43 PASS**, включая ABI/provider/export gates и безопасную публикацию stage |
| Offline compile/link | PASS; probe C++20 с `-Wall -Wextra -Wpedantic -Werror`, SDL пересобрана из pinned source |
| ELF gate | PASS; executable и SDL AArch64, loader корректен, NEEDED и все требуемые version nodes найдены в firmware snapshot / bundled SDL; пять selected imports совпали с firmware exports |
| Container `ldd`, `--help`, CPU `--self-test` | PASS; SDL/EGL/Dawn initialization не выполнялась |
| Package integrity / licenses | Все manifest SHA-256 перепроверены; штатный license collector выполнен |
| SSH / deploy / graphics | Не выполнялись в ENV-01/02 |

Локальный image reference, полученный через `docker image inspect` и используемый helper:

```text
sha256:2db4439763783aa796da4268dbd91847ee2c13deef4c8ce2439e0f6bdf926962
```

Это зафиксированная identity данной локальной сборки, не обещание bit-for-bit одинакового image ID при новом Docker build. Config digest этой сборки: `sha256:9e8f2a0cab9b7666a0083c0fe38c82f3e4835d10ded30cb6ff94cb323a7c36e9`.

Package находится в `build/h700-graphics-probe-abi238-final/package/`; metadata `abi.json` / `packages.tsv` и `manifest.json` содержат результаты, tool versions и input/artifact hashes. В image дополнительно сохранены подписанные `InRelease` в `/opt/h700-toolchain/`.

**Ограничение ABI gate:** сверяются реальные provider/version nodes и пять конкретных exports, но не каждый импортируемый symbol. Контейнерный loader не заменяет muOS loader, `dlopen` firmware SDL2 и работу Mali. Kernel 4.9 и runtime/driver behaviour также требуют согласованной device-проверки. Достигнута готовность artifact к такой проверке, не доказана полная совместимость игры.

После завершения ENV-01/02 пользователь отдельно разрешил активную проверку: firmware loader/CPU smoke tests, offscreen 2 кадра и bridge 120 кадров прошли программные pixel/order/teardown assertions на RG40XX H. Визуальная оценка пока pending; это не полный ABI/feature audit игры. Сценарий, artifact hashes, результаты и cleanup — [H700_GRAPHICS_PROBE.md](H700_GRAPHICS_PROBE.md). Границы исходной ENV-итерации и её результаты выше сохранены как история.

Текущий статус — [H700_PROGRESS.md](H700_PROGRESS.md).
