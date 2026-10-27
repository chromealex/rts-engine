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

## Проверено / не проверено

- ✅ Linux: клиент, редактор, тесты; Windows .exe (кросс-сборка MinGW, запуск под Wine).
- ✅ macOS: сборка, тесты, запуск игры на Metal; rtsbuild → .app и проект iOS (+ сборка для симулятора).
- ✅ Web: собирается (Emscripten 3.1.50), матч запускается в headless Chromium (WebGL2/SwiftShader).
- ⏳ Android: проект Gradle генерируется; APK и запуск в эмуляторе — в CI (job `android`), локально не собирался.
- ⏳ Windows на реальном железе и iOS на устройстве — не запускались (в CI: MSVC-сборка и сборка iOS для симулятора).

## Платформы и CI

`.github/workflows/ci.yml` на каждый push в `master`/`agent-*` и на pull request:

| Job | Что делает |
|---|---|
| `linux` | gcc и clang (x64) + gcc (ARM64), предупреждения = ошибки, тесты, матчи ботов, скриншоты под Xvfb |
| `windows-msvc` | Visual Studio 2022, тесты, матчи, `rtsbuild --target windows` |
| `windows-mingw` | кросс-сборка MinGW-w64, тесты и матчи под Wine |
| `macos` | universal-сборка Xcode, тесты, матчи, `rtsbuild` → .app и проект iOS со сборкой для симулятора |
| `android` | `.so` через NDK, APK через `rtsbuild`, в эмуляторе: запуск → касание → фон → возврат → «назад» → повторный запуск, проверка logcat на падения |
| `web` | Emscripten, запуск матча в headless Chromium, проверка консоли и кадра |
| `determinism` | сравнивает контрольные суммы матчей (обе игры × 3 сида) со всех платформ — должны совпасть побитово |

Скриншоты, APK, web-сборка и .app сохраняются как артефакты прогона.

Локально:

```bash
# Windows .exe с Linux/macOS
cmake -S . -B build/mingw -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=platform/cmake/mingw-w64.cmake
# матчи ботов на всех играх → файл контрольных сумм (сравнить между машинами через diff)
.github/scripts/headless-checksums.sh build/linux-release/rts_headless sums.txt
# смоук-тест web-сборки
node platform/web/smoke.mjs build/web
```

Параметры командной строки в web-сборке задаются через URL: `index.html?map=0&spectate` = `--map 0 --spectate`.
Режим скриншота (`RTS_SCREENSHOT`, `RTS_EXIT_AFTER_FRAMES`) шагает ровно по 1/60 с на кадр, поэтому
кадр N одинаков на любой машине.

## Дальше

Модели glTF и анимация, звук, сеть (lockstep), способности и баффы из данных, flow fields и
RVO для больших армий, улучшения ИИ, скриптинг на C# (AOT для iOS).
