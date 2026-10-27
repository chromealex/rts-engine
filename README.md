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

## Звук

`engine/audio` — микшер, банки звуков из данных, процедурный синтез, музыка; вывод через
[miniaudio](third_party/miniaudio) (CoreAudio, AAudio/OpenSL ES, Web Audio, WASAPI, ALSA/PulseAudio).
`modules/rts/audio` превращает события симуляции в звуки; в симуляцию ничего не пишет.
Нет устройства — игра идёт молча. Нет `audio.json` — игра просто без звука.

Звук игры — файл `audio.json` рядом с `game.json` (другое имя: `"audio"` в game.json):

```jsonc
{
  "volume": { "sfx": 0.9, "music": 0.6, "voice": 1, "ui": 0.7, "ambient": 0.7 },
  "sounds": {
    "sword": { "files": ["sfx/sword1.ogg", "sfx/sword2.ogg"], "pitch": [0.9, 1.1], "maxInstances": 3, "cooldown": 0.06 },
    "alarm": { "file": "voice/under_attack.ogg", "bus": "voice", "priority": 10 },
    "blip":  { "synth": { "wave": "square", "freq": [880, 440], "duration": 0.08 }, "bus": "ui" }
  },
  "music":     { "menu": "music/menu.ogg", "match": { "files": ["music/a.ogg", "music/b.ogg"], "shuffle": true } },
  "events":    { "died": "death", "underAttack": "alarm", "notEnough.gold": "need_gold" },
  "weapons":   { "bow": { "attack": "bow_release", "hit": "arrow_hit" } },
  "abilities": { "heal": "heal_cast" },
  "types":     { "footman": { "select": "footman_what", "move": "footman_yes", "died": "death_human" } }
}
```

| Что | Смысл |
|---|---|
| форматы | WAV, OGG Vorbis, MP3, FLAC; `"synth"` — звук из параметров (`sine/square/saw/triangle/noise`, свип частоты, огибающая, вибрато, фильтр, слои) |
| шины | `sfx` и `ambient` — позиционные (затухание и панорама от камеры), `voice`, `ui`, `music` — нет |
| `maxInstances`, `cooldown`, `priority` | прореживание в больших боях: 40 лучников не дают 40 одинаковых выстрелов |
| события (cues) | `attack hit miss died spawned trainStarted ready buildPlaced buildDone gathered captured lost playerDefeated victory defeat notEnough notEnoughSupply noRoom researched levelUp ability readyToPlace sold regionClaimed wave lowPower upgraded message underAttack baseUnderAttack select move attackOrder buildOrder click error` |
| поиск звука | тип → оружие/способность по id → `events` |
| музыка | плейлисты `menu`, `match`, `battle` (включается при сильном бое у камеры), `victory`, `defeat`; кроссфейд |

Звуки боя слышны только там, где нет тумана войны; «нас атакуют» — с паузой, и только если камера
смотрит в другое место. Громкость (общая и музыка) — в меню паузы, хранится в `userDataDir/audio.cfg`.
На телефоне при сворачивании звук ставится на паузу.

Отладка без звуковой карты: `RTS_AUDIO=null` (настоящий аудиопоток без железа), `RTS_AUDIO=off`,
`RTS_AUDIO_CAPTURE=/tmp/mix.wav` — записать всё, что прозвучало, в WAV.

## Проверено / не проверено

- ✅ Linux: клиент, редактор, тесты; Windows .exe (кросс-сборка MinGW, запуск под Wine).
- ✅ macOS: сборка, тесты, запуск игры на Metal; rtsbuild → .app и проект iOS (+ сборка для симулятора).
- ⏳ Android: проект Gradle генерируется, APK не собирался (нет SDK в среде проверки).
- ⏳ Web: не собирался (нет emsdk в среде проверки).
- ⏳ Windows на реальном железе и iOS на устройстве — не запускались.

## Дальше

Модели glTF и анимация, звук, сеть (lockstep), способности и баффы из данных, flow fields и
RVO для больших армий, улучшения ИИ, скриптинг на C# (AOT для iOS).
