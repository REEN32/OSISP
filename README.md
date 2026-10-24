# Лабораторная работа 4 — синхронизация потоков Windows

**Вариант 4: обедающие философы, асимметричный захват Mutex.** STUDENT_ID=027, индивидуальные SEED=3619 и 235 циклов на философа.

## Что готово и что ещё нужно

Реализованы программа Windows API, проверки, журнал, статистика JSON, 48 реальных проверочных запусков portable-backend на macOS, таблицы, два графика, редактируемые схемы и отчёт DOCX/Markdown с полным листингом.

**Windows-сборка и Windows-измерения в текущей среде не выполнялись.** Данные `data/portable` относятся только к macOS и не подтверждают работу системных вызовов Windows. `scripts/windows.ps1` выполняет необходимую Windows-серию и пересобирает отчёт из неё. Удалённый Git не настроен: его адрес и доступ преподавателю должен предоставить студент. Поля ФИО, группы и учреждения на титульном листе оставлены для заполнения. Защита и параметры преподавателя выполняются отдельно.

## Среда и сборка Windows

Windows 10/11, Visual Studio 2022/2026 или Build Tools с компонентом Desktop development with C++, Windows SDK, CMake 3.20+, Python 3.10+. При несовместимости toolset существующего `.vcxproj` используйте CMake, он выберет установленный компилятор.

Из PowerShell в корне репозитория:

```powershell
cmake -S . -B build-windows -A x64
cmake --build build-windows --config Release
ctest --test-dir build-windows -C Release --output-on-failure
.\build-windows\Release\philosophers.exe --help
.\build-windows\Release\philosophers.exe --cycles 3
.\build-windows\Release\philosophers.exe --individual --philosophers 7 --think-ms 15 --quiet --json individual.json
```

Можно открыть существующее решение `LAB4NEW/LAB4NEW.slnx`. Входной `LAB4NEW.cpp` подключает единую реализацию `src/main.cpp`; отдельной расходящейся копии кода нет. Для ARM64 через CMake замените `-A x64` на `-A ARM64` и выберите новую папку сборки.

## Полное воспроизведение в Windows одной командой

```powershell
powershell -ExecutionPolicy Bypass -File scripts/windows.ps1
```

Сценарий собирает Release, запускает проверки, устанавливает Python-зависимости, выполняет **48 запусков** и создаёт:

- `data/windows/*.json`: оригинальные результаты каждого запуска;
- `data/windows/manifest.json`: команды, платформа, SHA-256 данных и исполняемого файла;
- `data/windows/summary.csv`, `workers.csv`: таблицы конфигураций и участников;
- `data/windows/throughput.png`, `waiting.png` и их SVG;
- `report/report.docx`, `report/report.md`: отчёт, пересчитанный из Windows-данных.

Повтор серии: `scripts/windows.ps1 -Output data/windows-repeat2`. Скрипт запрещает перезапись ранее завершённой серии. Для отдельного этапа:

```powershell
python scripts/experiments.py --exe build-windows/Release/philosophers.exe --out data/windows
python scripts/analyze.py --data data/windows
python scripts/build_report.py --data data/windows
```

GitHub Actions `.github/workflows/windows.yml` выполняет тот же сценарий на Windows и сохраняет результаты вместе с EXE как downloadable artifact. Загрузка репозитория и фактический запуск workflow здесь не выполнялись. После изменения кода необходимо провести новую серию: старые данные относятся к SHA-256 записанного в манифесте бинарника.

## Параметры

| Аргумент | По умолчанию | Диапазон и смысл |
|---|---:|---|
| `--philosophers` | 5 | 2..32; столько же вилок |
| `--cycles` | 100 | 1..1000000 на участника |
| `--think-ms` | 2 | 0..10000, номинальное размышление |
| `--eat-ms` | 2 | 0..10000, номинальная еда |
| `--jitter-ms` | 1 | 0..1000, отклонение ±J, минимум 0 |
| `--seed` | 1000 | 0..2147483647 |
| `--timeout-ms` | 1000 | 1..60000, бюджет одной попытки пары вилок |
| `--duration-ms` | 60000 | 1..3600000, длительность ожидания завершения |
| `--mode` | asymmetric | asymmetric или ordered |
| `--individual` | выключен | seed=3619, cycles=235 |
| `--quiet` | выключен | выключить журнал для замеров |
| `--json` | не задан | путь итогового JSON; каталог должен существовать |

Явные `--cycles`/`--seed` переопределяют индивидуальный набор независимо от позиции `--individual`. Для постоянных длительностей используйте `--jitter-ms 0`. Возврат: 0 — успех, 1 — ошибка параметров/инициализации/файла, 2 — неполное выполнение/дедлайн/ошибки/инварианты, 3 — невозможность завершения работников за защитные 5 секунд.

Пример для защиты с новыми параметрами:

```powershell
.\build-windows\Release\philosophers.exe --individual --philosophers 9 --cycles 50 --think-ms 4 --eat-ms 3 --timeout-ms 10 --duration-ms 30000 --quiet --json defense.json
```

## Алгоритм и корректность

Номера 0..N−1. Левая вилка i, правая (i+1)%N. Чётный философ берёт левую, затем правую; нечётный — правую, затем левую. Режим ordered берёт сначала меньший номер для контрольного сравнения. В каждый момент каждая вилка имеет единственного владельца Mutex.

У направленного графа зависимостей вилок кольца существуют рёбра обеих ориентаций, поэтому замкнутого направленного цикла нет (для N=2 оба берут 0→1). Это разрывает круговое ожидание и предотвращает deadlock без опоры на тайм-аут. FIFO и отсутствие голодания не гарантированы Windows Mutex. Три успешных запуска сами по себе не доказывают эти свойства.

Windows: `CreateMutexW`, `ReleaseMutex`, `WaitForMultipleObjects`, ручные `CreateEventW`, `SetEvent`, `WaitForSingleObject`, `CRITICAL_SECTION`, `GetCurrentThreadId`, `QueryPerformanceCounter/Frequency`, `_beginthreadex`, `CloseHandle`. Тайм-аут освобождает первую вилку через RAII, STOP прерывает ожидание вилки и длительные модельные задержки. `WAIT_ABANDONED` приводит к остановке с ошибкой. После join проверяется освобождение ресурсов.

Журнал содержит время относительно начала, номер философа, TID, состояние и RESOURCE при применимости. Записи защищены отдельной критической секцией. Статистика в JSON содержит индивидуальные операции, попытки, тайм-ауты, ожидание и время еды. `wait_success_ms` суммирует ожидание завершённых циклов; `wait_total_ms` также включает прерванное ожидание и служебные накладные расходы, но исключает время еды.

При дедлайне текущий незаконченный цикл не считается выполненным. Инварианты: уникальный владелец, обе вилки у едящего, отсутствие одновременно едящих соседей, максимум floor(N/2) едящих, отсутствие удерживаемых ресурсов после завершения. Счётчик нарушений выводится всегда. API-ошибки считаются отдельно.

## Матрица экспериментов

Стандартный (seed=1000, C=100) и индивидуальный (seed=3619, C=235) наборы; N=5/7, THINK=2/15 мс, asymmetric/ordered, по три повтора; EAT=2±1 мс, JITTER=1. Первый график — пропускная способность по N, второй — ожидание двух вилок по N. Лог отключён. Утилита проверяет backend, полноту, хеши и отсутствие нарушений перед анализом.

Фиксированный генератор каждого участника обеспечивает повторяемость модельных задержек при той же последовательности попыток, но не расписание потоков. Тайм-ауты могут менять число обращений к генератору. Времена зависят от ОС, таймеров и нагрузки; методика и ограничения подробно разобраны в отчёте.

## Проверка на macOS или Linux

Этот режим не заменяет Windows-испытания:

```sh
cmake -S . -B build -DLAB_PORTABLE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
python3 scripts/experiments.py --exe build/philosophers --out data/portable-new --backend-required portable-validation
python3 scripts/analyze.py --data data/portable-new
```

Для графиков и DOCX: `python -m pip install -r scripts/requirements.txt`. Поставленные данные `data/portable` сохранены как фактическая серия этой подготовки. Их нельзя переименовывать в Windows-результаты.

## Структура

```text
src/        основная программа и системные обёртки
LAB4NEW/    исходное решение Visual Studio
scripts/    измерения, таблицы, графики, генерация отчёта, Windows-сценарий
tests/      проверки корректности и аварийного завершения
data/       реальные результаты, журнал, протокол проверок
diagrams/   Graphviz, Mermaid, PNG, SVG
report/     DOCX, Markdown, проектирование, подсказки для защиты
.github/    Windows CI
```

Схема перестраивается Graphviz: `dot -Tpng -Gdpi=160 diagrams/architecture.dot -o diagrams/architecture.png`. В отчёт включены полные текстовые исходники схем и графиков. Файлы SVG редактируемы, исходными считаются DOT/Mermaid и Python с JSON.

## Git

Локальная история создана в реальной последовательности разработки, без подделки контрольных версий. Требование регулярной отправки в удалённый репозиторий пока не выполнено. Для публикации после создания пустого индивидуального репозитория:

```sh
git remote add origin URL_ВАШЕГО_РЕПОЗИТОРИЯ
git push -u origin main
```

Предоставьте преподавателю доступ. Не применяйте force push и не переписывайте опубликованную историю. При защите полезно показать `git log --oneline`, `git show HASH` и `git diff HASH1 HASH2`.

Архив `lab04-sync.zip`, если предоставлен рядом с проектом, содержит исходники, отчёт, данные и `lab04-history.bundle` с локальной историей. Восстановить репозиторий из bundle можно командой `git clone lab04-history.bundle lab04-restored`; это сохраняет реальные четыре этапа без создания новой искусственной истории.
