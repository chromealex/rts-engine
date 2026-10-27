# RTSEngine

Движок для любых RTS на C++20: от Mushroom Wars до Warcraft, Red Alert и Total Annihilation.
Игра — это **данные** (`game.json`, `rules.json`, карты), а не код: один и тот же клиент, редактор и
сборщик работают с любой игрой.

Платформы: macOS и iOS (Metal), Android и HTML5 (OpenGL ES 3 / WebGL2), Windows (OpenGL 3.3),
Linux (для разработки и CI).

## Состав

```
engine/
  core/      Fixed (Q47.16), Vec3, Random (PCG32), Hash, ECS
  data/      JSON без float (числа сразу в fixed-point)
  sim/       детерминированная симуляция, команды, реплеи
  gfx/       RHI + бэкенды Metal и GL (ES3/WebGL2/GL3.3), рендер в текстуру
  render/    3D-рендер: свет, тени (shadow map + PCF), туман, тонмаппинг; Dear ImGui через RHI
  platform/  окно/ввод/цикл: macos, ios, android, web, windows, linux
  assets/    общие ресурсы движка (шрифт Noto Sans с кириллицей, OFL)
modules/rts/ универсальный RTS-слой: типы из данных, приказы, A*, бой, экономика, стройка,
             производство, гарнизоны (Mushroom Wars), дороги, туман войны, ИИ
             view/ — отрисовка любых типов по блоку "view" в rules.json
apps/
  client/    игра (меню, матч с ботами, HUD, мини-карта, мышь и тач)
  editor/    редактор карт (рельеф, объекты, дороги, игроки, проверка, тест с ботами, меню «Сборка»)
  headless/  матчи ботов и проверка реплеев без графики
tools/rtsbuild/  сборщик: exe / app / проект Xcode / проект Gradle / web
games/
  skirmish/  «Iron and Gold» — пример в духе Warcraft/Red Alert
  mushrooms/ «Mushroom Clash» — пример в духе Mushroom Wars
platform/    шаблоны упаковки (Info.plist, Gradle-проект, HTML-оболочка)
```

## Быстрый старт (macOS)

```bash
cmake -S . -B build/mac -DCMAKE_BUILD_TYPE=Release      # нужны Xcode и CMake
cmake --build build/mac -j 8
open build/mac/RTSEditor.app                              # редактор (проект games/skirmish)
open build/mac/RTSGame.app                                # игра
./build/mac/rts_tests
```

Другую игру в редакторе: `RTSEditor.app/Contents/MacOS/RTSEditor --project games/mushrooms`.
Чтобы игра запускалась с другим набором данных, задайте `-DRTS_CONTENT_DIR=.../games/mushrooms`.

## Сборка под платформы — rtsbuild

```bash
./build/mac/rtsbuild --project games/skirmish --target macos     # dist/macos/<Name>.app (подписан ad-hoc)
./build/mac/rtsbuild --project games/skirmish --target ios       # dist/ios/xcode/RTSEngine.xcodeproj + сборка для симулятора
./build/mac/rtsbuild --project games/skirmish --target android   # dist/android/<Name> (Gradle) + APK, если есть SDK/NDK и gradle
./build/mac/rtsbuild --project games/skirmish --target web       # dist/web (нужен emsdk)
./build/mac/rtsbuild --project games/skirmish --target windows   # .exe через mingw-w64 (brew install mingw-w64) или исходники + build.bat
./build/mac/rtsbuild --project games/skirmish --target all
```

То же самое доступно в редакторе: меню **Сборка**. Сгенерированные проекты iOS и Android самодостаточны
(в них скопированы исходники движка и контент), их можно отдавать дальше. Для iOS на устройство:
откройте проект в Xcode и выберите команду в Signing (или `--team <TeamID>`, или `"appleTeamId"` в game.json).

Идентификатор, название, версия, ориентация и иконка (`icon.png`, 1024×1024; если нет — генерируется) — в `game.json`.

## Как описывается игра

Тип объекта — набор необязательных блоков; что задано, то объект и умеет:

| Блок | Смысл |
|---|---|
| `health` | здоровье, броня, тип брони, регенерация |
| `move` | скорость, слой (`ground`/`air`/`naval`), радиус |
| `structure` | здание с размером `footprint` на сетке |
| `weapons` | дальность, урон, тип урона, перезарядка, снаряд, сплэш, по каким слоям бьёт |
| `cost` | ресурсы, время, лимит |
| `produces` / `builds` | кого обучает / что строит |
| `gather` / `resourceNode` / `dropoff` | добыча ресурсов (Warcraft/StarCraft) |
| `income` / `storage` + ресурс `"kind": "flow"` | поточная экономика (Total Annihilation) |
| `supply` | даёт лимит |
| `requires` | дерево технологий |
| `garrison` + `entersGarrison` | здания-гарнизоны, отправка процента войск, захват (Mushroom Wars) |
| `view` | внешний вид: `shape`, `scale`, `color` |

Глобально: `resources`, `damageTable` (тип урона × тип брони), `supplyCap`, `victory`.
Карта: размер, рельеф (`rows`: `.` земля, `~` вода, `#` скалы, `,` без стройки, `=` брод; `heights`),
игроки и команды, стартовые ресурсы, объекты, дороги, `roadsOnly`.

## Детерминизм

Симуляция побитово одинакова на всех платформах: только fixed-point, свой RNG, команды в
каноническом порядке, контрольные суммы в реплеях. Проверено: Linux x86-64 (GCC, Clang, MinGW),
Linux ARM64 (GCC), macOS ARM64 (AppleClang) — одинаковые контрольные суммы матчей.

## Графика и мобильные

Один проход forward-рендера: солнце (Ламберт + блик), карта теней с подгонкой под видимую часть
карты и привязкой к текселям, аппаратный PCF (1/4/9 выборок), полусферный эмбиент, туман по
дальности, ACES. Качество теней выбирается по GPU (для старых Mali/Adreno/PowerVR — низкое) и
меняется в меню паузы.

## Сетевая игра

`engine/net` — мультиплеер для любой игры на движке, без кода игры. По сети идут только команды:
симуляция детерминирована, поэтому у всех одинаковое состояние.

- **Транспорт:** UDP со своим слоем надёжности (iOS, Android, ПК) и WebSocket (HTML5; на нативе —
  запасной вариант, если UDP закрыт). Адрес: `host[:27015]`, `udp://…`, `ws://host:27016`.
- **Lockstep с откатами.** Сервер подтверждает ввод каждого тика: когда пришли команды всех игроков
  или вышел дедлайн (`lagToleranceMs`); опоздавшие команды переносятся на следующий тик — один
  лагающий игрок не останавливает остальных. Клиент не ждёт подтверждения: играет вперёд со своими
  командами, а при расхождении восстанавливает снимок и пересчитывает (снимок ~10 мкс).
  `rollbackWindow: 0` — классический lockstep для слабых устройств.
- **Сервер `rts_server`** не нужен контент игры: комнаты (слоты, боты, зрители, готовность, проверка
  версии данных), автоматический матчмейкинг по очередям из `server.json` (рейтинг Elo с расширением
  окна, общая карта, баланс команд, добор ботами), сверка контрольных сумм (рассинхрон), реплеи
  (`rts_headless --replay` их проигрывает), переподключение, зрители с задержкой.
- **В игре:** `NetClient` (`engine/net/NetClient.h`) — лобби и матч. Игра строит `Simulation` по
  `match()` и вызывает `attach(sim)`, затем `advance(dt)` вместо `sim.advance(dt)`; интерфейс
  по-прежнему отдаёт команды через `sim.submit()`.

```bash
./build/mac/rts_server --config apps/server/server.json            # UDP 27015, WebSocket 27016
./build/mac/rts_netbot --server 127.0.0.1 --project games/mushrooms --queue 1v1   # бот в очередь
./build/mac/rts_netbot --server ws://127.0.0.1:27016 --project games/mushrooms --queue 1v1
./build/mac/rts_net_tests                                          # сетевые тесты
docker build -f apps/server/Dockerfile -t rts-server .             # сервер в контейнере
```

Для `wss://` (страница игры по HTTPS) поставьте перед сервером TLS-прокси (nginx, Caddy).

## Проверено / не проверено

- ✅ Linux: клиент, редактор, тесты; Windows .exe (кросс-сборка MinGW, запуск под Wine).
- ✅ macOS: сборка, тесты, запуск игры на Metal; rtsbuild → .app и проект iOS (+ сборка для симулятора).
- ⏳ Android: проект Gradle генерируется, APK не собирался (нет SDK в среде проверки).
- ⏳ Web: не собирался (нет emsdk в среде проверки).
- ⏳ Windows на реальном железе и iOS на устройстве — не запускались.

## Дальше

Модели glTF и анимация, звук, сеть (lockstep), способности и баффы из данных, flow fields и
RVO для больших армий, улучшения ИИ, скриптинг на C# (AOT для iOS).
