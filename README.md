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

### Ресурс-паки (.rpak) и размер сборки

Контент игры (папка игры + `engine/assets`) попадает в сборку одним файлом `content/content.rpak`:
каждый файл сжат zstd отдельно, одинаковые файлы хранятся один раз, уже сжатые форматы (png, ogg, mp3, …)
кладутся как есть. Движок монтирует `content.rpak` автоматически; файлы из пака важнее файлов в папке,
так что без пака (при разработке) всё работает по-старому. `platform::mountPack("patch.rpak")` подключает
пак поверх (патч, DLC). Каждая сборка печатает отчёт о размере и пишет его в `dist/<платформа>/size-report.txt`.

```bash
rtsbuild ... --no-pack          # класть папки как есть (отладка)
rtsbuild ... --pack-level 9     # уровень zstd (по умолчанию 19 для Release, 3 для Debug)

assetpipe pack -o content.rpak games/skirmish engine/assets=engine   # собрать пак вручную
assetpipe list content.rpak       # файлы, размеры, сжатие
assetpipe verify content.rpak     # распаковать всё и проверить хеши
assetpipe extract content.rpak out/
assetpipe diff old.rpak new.rpak  # что изменилось между версиями (для патчей)
```

Что не нужно в сборке, перечисляется в `.rpakignore` в папке игры (маски `*`, `?`; `папка/` — только каталоги).
Скрытые файлы (`.git`, `.DS_Store`), реплеи `*.rpl` и вложенные `*.rpak` пропускаются всегда. Сборка пака
воспроизводима: одинаковый контент даёт байт-в-байт одинаковый файл; «хеш контента» пака идентифицирует набор данных.

### Импорт чужих архивов (распаковщики)

Форматы, которых движок не знает (zip, tar, dmg, архивы других игр), подключаются плагинами-распаковщиками
без пересборки движка: папка с `unpacker.json` и программой на любом языке (протокол —
[`tools/unpackers/PROTOCOL.md`](tools/unpackers/PROTOCOL.md)). Распаковщики работают только в инструментах сборки:
чужой архив превращается в файлы внутри `.rpak`, игра по-прежнему читает только `.rpak`.

Из коробки: zip (и `.oramap`), tar, dmg, архивы Warcraft I/II (`.WAR/.SUD/.INS`), Westwood `.PAK` (Dune II, Lands of Lore,
Kyrandia), Z от Bitmap Brothers (`.PAC`, `.RSC`).

```bash
assetpipe formats                                   # какие форматы известны
assetpipe probe ~/Downloads/Warcraft2/war2/DATA/*   # что это за файлы
assetpipe list MAINDAT.WAR                          # что внутри
assetpipe import -o war2.rpak MAINDAT.WAR=imported/war2 SFXDAT.SUD=imported/war2/sfx
assetpipe unpackers new tools/unpackers myformat    # заготовка нового распаковщика
assetpipe unpackers check                           # проверить все плагины
```

У всех команд есть `--json` — так их вызывает MCP-сервер. В `game.json` можно перечислить `"imports"`, и `rtsbuild`
сам распакует их в пак при сборке (`"optional": true` — если файла нет на этой машине, сборка идёт без него).
Пак с импортированным сторонним контентом помечается флагом «external»: такие сборки только для личного
использования, их нельзя коммитить и распространять.

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

## Проверено / не проверено

- ✅ Linux: клиент, редактор, тесты; Windows .exe (кросс-сборка MinGW, запуск под Wine).
- ✅ macOS: сборка, тесты, запуск игры на Metal; rtsbuild → .app и проект iOS (+ сборка для симулятора).
- ⏳ Android: проект Gradle генерируется, APK не собирался (нет SDK в среде проверки).
- ⏳ Web: не собирался (нет emsdk в среде проверки).
- ⏳ Windows на реальном железе и iOS на устройстве — не запускались.

## Дальше

Модели glTF и анимация, звук, сеть (lockstep), способности и баффы из данных, flow fields и
RVO для больших армий, улучшения ИИ, скриптинг на C# (AOT для iOS).
