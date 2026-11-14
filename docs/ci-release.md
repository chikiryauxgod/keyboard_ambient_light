# Проверки и выпуск

## CI на изменения

`.github/workflows/ci.yml` запускается на каждый push в ветку, pull request,
merge queue (`merge_group`) и вручную через Actions. Используется `pull_request`,
без выполнения кода PR с повышенными правами через `pull_request_target`.

- Linux x86_64, Windows x86_64, macOS arm64: Release-сборка Qt 6.8.3 + HIDAPI,
  GoogleTest и тестовый запуск GUI, установка runtime в отдельный каталог,
  запуск установленной программы, архивирование с метаданными исходного коммита.
- Linux ASan + UBSan: Debug-сборка и весь набор CTest. Для Qt offscreen отключена
  только проверка утечек GUI-процесса; остальные тесты работают с обычными настройками ASan.
- Linux coverage: GCC/gcovr, отчёты HTML и Cobertura XML, минимум 80% строк в
  `src/core.cpp` и `src/backends.cpp`. GUI и платформенный захват не входят в этот
  процент; их тестовый запуск и геометрия области проверяются отдельно.
- Проверка workflow-файлов через actionlint и тесты Python-инструментов релиза.

Все C++ юнит-тесты используют **GoogleTest**. Он находится через CMake или
загружается как зафиксированный архив v1.18.0 с проверкой SHA-256. CTest показывает
отдельные тесты и создаёт JUnit-отчёт. VIA использует тестовый HID-транспорт без
линковки с настоящим HIDAPI; команды реальной клавиатуре не отправляются.
OpenRGB тестируется локальным TCP-сервером, в том числе на повреждённых ответах.

Результаты тестов, логи, coverage и переносимые архивы доступны в Artifacts
соответствующего запуска Actions. Итоговый статус **CI required** успешен только
после всех проверок. Его можно назначить обязательным в GitHub Settings → Rules /
Branch protection. Сам workflow не меняет настройки защиты веток.

## Запуск релиза с выбранным тегом

После первого push файлов workflow в default branch:

1. GitHub → **Actions → Release → Run workflow**.
2. Выбрать ветку запуска и ввести `tag`, например `v0.1.0`.
3. При необходимости включить `draft` и/или `prerelease`.
4. Нажать Run workflow.

Через GitHub CLI тот же запуск:

```bash
gh workflow run release.yml --ref main -f tag=v0.1.0 -f draft=false -f prerelease=false
```

Новый тег указывает на точный коммит выбранного запуска. Если тег существует,
собирается его коммит, включая корректное разрешение аннотированного тега. Коммит
должен содержать актуальные CMake/test/package-инструменты. Ветки не используются
как подвижная ссылка во время матричной сборки: в неё передаётся полный SHA.

Релизный тег должен соответствовать версии из `VERSION`: сейчас `v0.1.0` или
`0.1.0`. Допустимы SemVer prerelease/build суффиксы, например `v0.1.0-rc.1`.
Версия проверяется в точном коммите, из которого собирается релиз.

## Что публикуется

- `KeyboardAmbient-<tag>-linux-x86_64.tar.gz`
- `KeyboardAmbient-<tag>-windows-x86_64.zip`
- `KeyboardAmbient-<tag>-macos-arm64.tar.gz`
- `SHA256SUMS`

Portable-архивы содержат исполняемое приложение, runtime-библиотеки и `BUILD-INFO.json`
с тегом, полным SHA коммита и платформой. Unix-архивы сохраняют исполняемые права
и симлинки `.app`/framework. Сборка с отсутствующим HIDAPI завершается ошибкой,
вместо выпуска урезанного приложения без VIA.

Публикация разрешена отдельному job с `contents: write` только после успешных
сборок и тестов всех трёх ОС. Перед публикацией проверяются полнота набора,
контрольные суммы, соответствие тегу и SHA. Затем проверяется, что существующий
Git-тег не изменился за время сборки. Новый тег создаётся без force push.
Релиз сначала создаётся черновиком и становится опубликованным после загрузки
всех файлов, если `draft=false`. Релиз с тем же тегом не перезаписывается.

Если публикация прервалась после создания draft-релиза, проверить его в Releases.
Перед новым запуском тот же черновик нужно удалить вручную или завершить публикацию
вручную после проверки файлов. Автоматической перезаписи нет. Отключённые Actions,
ограничения политики репозитория или недостаточные права GitHub Token могут помешать
публикации. Отдельный PAT в штатном случае не нужен.

Ни локальная подготовка workflow, ни локальный actionlint не подтверждают запуск
runner-ов Windows/macOS. Их результаты будут видны после первого push/запуска Actions.
Подпись Windows и macOS notarization пока не настроены.

## Локальные проверки

```bash
cmake --preset dev
cmake --build --preset dev --parallel 3
ctest --preset dev --output-junit test-results.xml
python3 -W error::ResourceWarning -m unittest discover -s tests -p test_release_tools.py -v
```

Первой сборке тестов нужен интернет для GoogleTest, если пакет не установлен
локально. Для offline-сборки можно передать `FETCHCONTENT_SOURCE_DIR_GOOGLETEST`.

Санитайзеры:

```bash
cmake -S . -B build-sanitizers -DCMAKE_BUILD_TYPE=Debug -DAMBIENT_ENABLE_SANITIZERS=ON -DAMBIENT_WARNINGS_AS_ERRORS=ON
cmake --build build-sanitizers --parallel 3
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build-sanitizers --output-on-failure
```

Coverage:

```bash
cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DAMBIENT_ENABLE_COVERAGE=ON
cmake --build build-coverage --parallel 3
ctest --test-dir build-coverage --output-on-failure
python3 -m pip install gcovr==8.8
mkdir -p coverage
gcovr --root . --filter 'src/(core|backends)\.cpp$' --merge-mode-functions merge-use-line-min --xml coverage/coverage.xml --html-details coverage/index.html --print-summary --fail-under-line 80
```

CI включает `UBSAN_OPTIONS=halt_on_error=1`, поэтому диагностика UB не остаётся
просто сообщением в успешном тесте. `AMBIENT_WARNINGS_AS_ERRORS=ON` применяется
к исходникам проекта; библиотеки GoogleTest и Qt не пересобираются с этим флагом.

## Portable-структура dist

Готовая сборка не требует установки приложения или службы. Архив распаковывается целиком.

```text
dist/
  linux-x86_64/       # bin/keyboard-ambient + библиотеки
  windows-x86_64/     # bin/keyboard-ambient.exe + Qt/HIDAPI/MSVC DLL
  macos-arm64/        # keyboard-ambient.app
  artifacts/         # архивы и контрольные суммы
```

`cmake --install --prefix <абсолютный путь>/dist/<система>` используется только как операция
сборки файлов в локальный каталог: в системные каталоги ничего не устанавливается.
Windows runtime размещается рядом с exe; запуск отдельного VC Redistributable
инсталлятора не требуется. Windows 11 будет проверена пользователем; CI проверяет
сборку, тесты и запуск portable-пакета на Windows runner.
