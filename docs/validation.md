# Локальная проверка — 2026-11-14

Проверено на Linux x86_64, GCC 13.3.0.

| Проверка | Результат |
|---|---|
| Release, Qt 6.8.3 + HIDAPI, warnings as errors | Сборка успешна |
| CTest Release 0.1.0 | 60/60, включая проверку `--version` |
| Debug, ASan + UBSan, `halt_on_error=1` | 60/60, ошибок не обнаружено |
| Debug с GCC coverage | 59/59 |
| GoogleTest | 57 отдельных тестов: цвет, область, VIA, OpenRGB |
| Python release tools | 12 тестов, вызываются одним CTest-тестом |
| GUI Preview | Отдельный CTest smoke-test, без доступа к клавиатуре |
| Portable Release-сборка | `dist/linux-x86_64/`, smoke-test без Qt SDK путей успешен |
| actionlint 1.7.12 | Все workflows прошли проверку |
| `git diff --check` | Ошибок нет |

gcovr 8.8, production-модули `src/core.cpp` и `src/backends.cpp`:

- Строки: **93,2%** — 409/439.
- Функции: **100%** — 40/40.
- Ветви: **64,3%** — 436/678 (включая ветвления исключений C++).

Порог CI — 80% строк этих двух модулей. `src/main.cpp` и платформенный capture
не включены в этот процент. Отчёты создаются в `coverage/index.html` и
`coverage/coverage.xml`, логи/JUnit — в build-каталогах. Они не коммитятся.

GoogleTest проверяет VIA с поддельным HIDAPI и OpenRGB с локальным TCP-сервером.
Это не повторная аппаратная проверка клавиатуры. Реальная клавиатура тестами
не переключается. Поведение повреждённых пакетов, ошибок доступа/инициализации,
неполного снимка настроек и восстановления проверяется без оборудования.

Реальная работа Windows 11 будет проверена пользователем. CI выпускает portable-
архивы Windows x64, Linux x64 и macOS ARM64; результаты runner-ов видны в Actions.
