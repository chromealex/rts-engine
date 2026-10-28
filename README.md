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
  render/    3D-рендер: свет, тени (shadow map + PCF), туман, тонмаппинг; Dear ImGui через RHI;
             модели glTF/OBJ, материалы PBR, скелетная и процедурная анимация
  platform/  окно/ввод/цикл: macos, ios, android, web, windows, linux
  assets/    общие ресурсы движка (шрифт Noto Sans с кириллицей, OFL)
modules/rts/ универсальный RTS-слой: типы из данных, приказы, A*, бой, экономика, стройка,
             производство, гарнизоны (Mushroom Wars), дороги, туман войны, ИИ
             view/ — отрисовка любых типов по блоку "view" в rules.json
apps/
  client/    игра (меню, матч с ботами, HUD, мини-карта, мышь и тач)
  editor/    редактор карт (рельеф, объекты, дороги, игроки, проверка, тест с ботами, меню «Сборка»)
  headless/  матчи ботов и проверка реплеев без графики
  modelview/ просмотр моделей, клипов, материалов и всех типов игры
tools/rtsbuild/  сборщик: exe / app / проект Xcode / проект Gradle / web
templates/
  skirmish/  «Iron and Gold» — пример в духе Warcraft/Red Alert
  mushrooms/ «Mushroom Clash» — пример в духе Mushroom Wars
  showcase/  правила Iron and Gold с импортированными моделями (glTF, OBJ, анимации)
platform/    шаблоны упаковки (Info.plist, Gradle-проект, HTML-оболочка)
```

## Быстрый старт (macOS)

```bash
cmake -S . -B build/mac -DCMAKE_BUILD_TYPE=Release      # нужны Xcode и CMake
cmake --build build/mac -j 8
open build/mac/RTSEditor.app                              # редактор (проект templates/skirmish)
open build/mac/RTSGame.app                                # игра
./build/mac/rts_tests
```

Другую игру в редакторе: `RTSEditor.app/Contents/MacOS/RTSEditor --project templates/mushrooms`.
Чтобы игра запускалась с другим набором данных, задайте `-DRTS_CONTENT_DIR=.../templates/mushrooms`.

## Сборка под платформы — rtsbuild

```bash
./build/mac/rtsbuild --project templates/skirmish --target macos     # dist/macos/<Name>.app (подписан ad-hoc)
./build/mac/rtsbuild --project templates/skirmish --target ios       # dist/ios/xcode/RTSEngine.xcodeproj + сборка для симулятора
./build/mac/rtsbuild --project templates/skirmish --target android   # dist/android/<Name> (Gradle) + APK, если есть SDK/NDK и gradle
./build/mac/rtsbuild --project templates/skirmish --target web       # dist/web (нужен emsdk)
./build/mac/rtsbuild --project templates/skirmish --target windows   # .exe через mingw-w64 (brew install mingw-w64) или исходники + build.bat
./build/mac/rtsbuild --project templates/skirmish --target all
```

То же самое доступно в редакторе: меню **Сборка**. Сгенерированные проекты iOS и Android самодостаточны
(в них скопированы исходники движка и контент), их можно отдавать дальше. Для iOS на устройство:
откройте проект в Xcode и выберите команду в Signing (или `--team <TeamID>`, или `"appleTeamId"` в game.json).

Идентификатор, название, версия, ориентация и иконка (`icon.png`, 1024×1024; если нет — генерируется) — в `game.json`.

### Инструменты сборки: проверка и автоустановка

Перед сборкой `rtsbuild` проверяет, что нужно платформе, и ищет это не только в `PATH` (редактор,
запущенный из Finder, видит лишь `/usr/bin:/bin:/usr/sbin:/sbin`): Homebrew (`/opt/homebrew/bin`,
`/usr/local/bin`), `~/.local/bin`, `~/Library/Python/*/bin`, `~/emsdk`, `$EMSDK`, `ANDROID_HOME` /
`ANDROID_SDK_ROOT`, `~/Library/Android/sdk`, `~/Android/Sdk`, `%LOCALAPPDATA%\Android\Sdk`, `JAVA_HOME`,
`/usr/libexec/java_home`, Visual Studio (vswhere) и свою папку `~/.rtsengine/toolchains`. Найденные папки
добавляются в `PATH` дочерних процессов.

| Платформа | Нужно | Ставится автоматически |
|---|---|---|
| web | CMake, Python 3, Emscripten (emsdk), Ninja — по желанию | emsdk (≈ 400 МБ загрузки, ≈ 1.5 ГБ на диске), CMake + Ninja (≈ 50 МБ) |
| android | JDK 17–23, Android SDK (platform 35, build-tools, platform-tools), NDK, CMake из SDK, Gradle | всё: Temurin JDK 17, command line tools + `sdkmanager` (лицензии принимаются), NDK ≈ 700 МБ, Gradle 8.10.2 |
| macos | Xcode Command Line Tools, CMake | CLT — через установщик Apple (`xcode-select --install`), CMake |
| ios | Xcode (App Store), CMake | CMake |
| windows (на Windows) | Visual Studio 2022 / Build Tools (C++), CMake | CMake; VS — подсказка с командой `winget` |
| linux | компилятор C++, CMake | CMake |

```bash
rtsbuild --check-toolchain web            # что есть, чего нет (--json — для редактора и MCP)
rtsbuild --install-toolchain web          # поставить недостающее (или один id: emsdk, cmake, jdk, android-ndk, gradle, ...)
rtsbuild --project ... --target web --install-missing   # поставить и сразу собрать
```

Без `--install-missing` нехватка инструментов останавливает сборку с кодом выхода 3 и строкой
`RTS_MISSING_TOOLCHAIN web: emsdk` (её распознаёт редактор). В редакторе окно **Сборка** показывает для
выбранной платформы, что найдено (пути, версии) и чего не хватает, с кнопкой **Установить автоматически**
и галочкой «Устанавливать недостающее автоматически»; ход установки — в **Console** (текст можно выделять и
копировать, «Открыть папку логов» — `<движок>/logs`). Агенты делают то же через MCP: `toolchain_check`,
`toolchain_install`, `build_start {"installMissing": true}`.

Всё ставится в `~/.rtsengine/toolchains` (Windows: `%USERPROFILE%\.rtsengine\toolchains`; другая папка —
переменная `RTS_TOOLCHAINS_DIR`) без `sudo` и изменений в системе: `emsdk/`, `py/` (venv с cmake и ninja),
`cmake/`, `ninja/`, `jdk-17/`, `android-sdk/`, `gradle-8.10.2/`. Повторный запуск безопасен (готовое
пропускается). Удалить всё установленное — удалить эту папку. Исключения: Command Line Tools ставит
установщик Apple, а если найден Android SDK пользователя, недостающие пакеты `sdkmanager` ставит в него.

### Ресурс-паки (.rpak) и размер сборки

Контент игры (папка игры + `engine/assets`) попадает в сборку одним файлом `content/content.rpak`:
каждый файл сжат zstd отдельно, одинаковые файлы хранятся один раз, уже сжатые форматы (png, ogg, mp3, …)
кладутся как есть. Движок монтирует `content.rpak` автоматически; файлы из пака важнее файлов в папке,
так что без пака (при разработке) всё работает по-старому. `platform::mountPack("patch.rpak")` подключает
пак поверх (патч, DLC). Каждая сборка печатает отчёт о размере и пишет его в `dist/<платформа>/size-report.txt`.

```bash
rtsbuild ... --no-pack          # класть папки как есть (отладка)
rtsbuild ... --pack-level 9     # уровень zstd (по умолчанию 19 для Release, 3 для Debug)

assetpipe pack -o content.rpak templates/skirmish engine/assets=engine   # собрать пак вручную
assetpipe list content.rpak       # файлы, размеры, сжатие
assetpipe verify content.rpak     # распаковать всё и проверить хеши
assetpipe extract content.rpak out/
assetpipe diff old.rpak new.rpak  # что изменилось между версиями (для патчей)
```

Что не нужно в сборке, перечисляется в `.rpakignore` в папке игры (маски `*`, `?`; `папка/` — только каталоги).
Скрытые файлы (`.git`, `.DS_Store`), реплеи `*.rpl` и вложенные `*.rpak` пропускаются всегда. Сборка пака
воспроизводима: одинаковый контент даёт байт-в-байт одинаковый файл; «хеш контента» пака идентифицирует набор данных.

### Разбиение сборки и докачка контента

По желанию сборку можно разбить: в приложение попадает только базовый пак с тем, что нужно для запуска,
остальное — в части (`content.001.rpkc`, …) заданного размера, которые игра докачивает из любого источника.
Всё решает автор игры, в `game.json` (общие настройки и отдельно для каждой платформы) или флагами `rtsbuild`:

```json
"packaging": {
  "split": true,                 // разбивать ли
  "chunkSize": "8MB",            // размер одной части
  "baseSize": "40MB",            // предел того, что едет внутри приложения
  "totalSize": "1GB",            // предел всего контента
  "fillBase": false,             // true: добить базу до baseSize (меньше докачки)
  "base": ["maps/intro.json"],   // что обязательно в базе (кроме стандартного: game.json, rules, lang, ui, атласы, первая карта)
  "order": ["maps/scena00*"],    // порядок докачки
  "sources": ["https://cdn.example.com/mygame/{hash}/", "file:///Volumes/DLC/"],   // зеркала по порядку
  "autoDownload": true,          // докачивать всё в фоне сразу после старта
  "targets": {"web": {"baseSize": "15MB"}, "windows": {"split": false}}
}
```

```bash
rtsbuild --project templates/x --target web --split --chunk-size 4MB --base-size 15MB   # флаги важнее game.json
```

Части кладутся рядом со сборкой (`dist/<платформа>/chunks/`) — их выкладывают по адресу из `sources`
(`{hash}` — хеш контента сборки, разные версии не путаются). В игре: файл из нескачанной части запрашивается
автоматически (на десктопе и мобильных `readContentFile` ждёт его, в вебе — возвращает false), прогресс —
`platform::contentProgress()`, явный запрос — `platform::requestContent(paths)`. Каждая часть проверяется по
SHA-256 (подменённую с зеркала не примет и возьмёт со следующего), хранится в кэше и при следующем запуске не
качается. Протоколы: https/http (libcurl на Linux, WinHTTP, NSURLSession, HttpURLConnection на Android,
fetch в вебе), `file://` и папки, а игра может зарегистрировать свою схему (Steam, Play Asset Delivery, P2P).
`assetpipe pack --chunk-size 8MB ...` делает то же вручную, `assetpipe verify` проверяет и части.

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

### Импорт игр (importers)

Импортёр превращает вашу копию старой игры в папку игры для движка. Сейчас есть Dune II:

```bash
python3 tools/importers/dune2/import.py --source "~/Downloads/Dune II" --out ~/RTSGames/dune2
rtsbuild --project ~/RTSGames/dune2 --target macos
```

Получается 2D-игра в формате движка (`docs/game-format.md`, «2D games»): камера сверху, 66 миссий кампании
с тайловым слоем (`tilesets/dune.json` из ICON.ICN: песок, скалы, дюны, горы, спайс с автотайлингом), спрайты
юнитов из SHP (5 направлений с отражением, башни танков наложены на корпус) и зданий из тайлов ICN (стройка,
руины, анимация), цвет дома — оттенки маджента. Плюс `rules.json` (наш баланс Dune II на блоках движка),
картинки CPS и анимации WSA в PNG, звуки VOC → WAV + `audio.json`, названия на английском и русском.
Рельеф приближённый: оригинальный генератор ландшафта не используется, базы стоят на скалах, спайс из
`Field=`, квоты миссий — триггерами. Пехота, орнитоптер и транспортник пока рисуются 3D-фигурами.
Папка помечена `"external": true` — её нельзя коммитить и распространять.

## Файл проекта .rtsproj

Проект — папка где угодно на диске (`game.json`, `rules.json`, `maps/`, ...). В её корне лежит
файл проекта **`<Название>.rtsproj`** — его открывают двойным кликом, как `.uproject` в Unity/Unreal
или `project.godot`. `game.json` остаётся манифестом игры (его читают клиент, rtsbuild, паки);
`.rtsproj` только помечает папку как проект и хранит сведения редактора:

```json
{"format": 1, "name": "Моя стратегия", "engine": "0.2.0", "manifest": "game.json", "created": "2026-10-28"}
```

- Создаётся вместе с проектом (хаб «Новый проект», MCP `create_project`); у старого проекта без
  него — при первом открытии в редакторе. У шаблонов в `templates/*/` он уже есть (и в новый проект
  не копируется: тот получает свой).
- Везде, где редактор принимает путь проекта, подходит и `.rtsproj`, и папка: `rts_editor "Моя
  стратегия.rtsproj"`, `--project`, «Открыть…» в хабе (показывает `.rtsproj` и папки проектов),
  недавние, MCP `open_game`, `rtsbuild --project x.rtsproj`. В сборку `.rtsproj` не попадает.
- **Двойной клик.** Редактор при запуске сам связывает `.rtsproj` с собой (для пользователя, без прав
  администратора), если связи нет или она ведёт к другому экземпляру редактора; выбранную вами другую
  программу не перебивает. Вручную: «Проект → Сделать редактор программой по умолчанию для .rtsproj»;
  состояние — внизу слева в хабе. Отключить: `RTS_FILE_ASSOC=off`.
  - **macOS:** тип `com.rtsengine.project` объявлен в Info.plist `RTSEditor.app`
    (`platform/macos/EditorInfo.plist.in`, иконки рисует `rts_projicon`); редактор регистрирует бандл
    в LaunchServices (`lsregister -f`) и становится обработчиком по умолчанию. Двойной клик и
    `open -a RTSEditor x.rtsproj` открывают проект в уже запущенном редакторе (при несохранённой
    карте он спросит, сохранить ли её).
  - **Windows:** `HKCU\Software\Classes\.rtsproj` → `RTSEngine.Project` (иконка
    `%USERPROFILE%\.rtsengine\icons\rtsproj.ico`, команда `"<rts_editor.exe>" "%1"`). Если в
    Windows для `.rtsproj` уже выбрана другая программа, хаб подскажет «Открыть с помощью → Всегда».
  - **Linux:** тип `application/x-rtsengine-project` (`~/.local/share/mime/packages/rtsengine-project.xml`),
    `~/.local/share/applications/rtsengine-editor.desktop` (`Exec=<редактор> %f`), иконки в
    `~/.local/share/icons/hicolor`, программа по умолчанию в `~/.config/mimeapps.list`;
    `update-mime-database`, `update-desktop-database`, `xdg-mime` вызываются, если установлены.

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

## Локализация

Игра переводится без изменения `rules.json`: имена в данных — текст на исходном языке игры, переводы
лежат в `<project>/lang/<язык>.json` под ключами `type.footman`, `resource.gold`, `map.valley.title`…
Строки меню и HUD — в `engine/assets/lang/` (сейчас English и Русский), игра может переопределить любую.

```jsonc
// game.json
"sourceLanguage": "en",
"languages": ["en", "ru"]
```

```bash
./build/mac/rts_i18n extract --project templates/skirmish --lang de --write   # заготовка lang/de.json, новые строки помечены // TODO
./build/mac/rts_i18n check   --project templates/skirmish                     # плейсхолдеры, формы мн. числа, непереведённое
```

Язык выбирается так: выбор игрока в меню (запоминается) → язык системы → исходный язык игры.
Для проверки: `--lang ru`, `RTS_LANG=ru`, псевдоязык `--lang qps` (все строки искажены и длиннее —
сразу видно непереведённое и то, что не влезает). Формы множественного числа — по CLDR
(«1 секунду / 2 секунды / 5 секунд»). Подробнее: `engine/i18n/README.md`.

## Интерфейс игры (RmlUi, HTML/CSS)

Меню, HUD, командная карточка, миникарта и пауза клиента — это документы RmlUi
(`.rml` — разметка, `.rcss` — стили), а не код. Движок рисует их через свой RHI
(GL/Metal), шрифты — через stb_truetype (латиница + кириллица).

- Стандартные документы: `engine/assets/ui/` (`menu.rml`, `hud.rml`, `theme.rcss`).
- Игра переопределяет любой файл, положив `ui/<то же имя>` в свою папку: например,
  только `ui/theme.rcss` — свои цвета при стандартной разметке. Шрифт игры — `fonts/ui.ttf`.
- Данные приходят из модели `game` (список полей — в комментарии в `hud.rml`),
  нажатия кнопок превращаются в команды симуляции (`apps/client/ClientUi.cpp`).
- Размеры в `dp` — интерфейс масштабируется под плотность экрана; на сенсорных
  устройствах у корня есть класс `touch` (крупнее кнопки).
- F8 в клиенте — отладчик RmlUi (дерево элементов, стили, журнал).
- Скин игры — `ui/skin.rcss` (подключается после `theme.rcss` во всех документах: цвета, рамки,
  кнопки, раскладка) и шрифт заголовков `fonts/title.ttf` (семейство `title`). У каждого прототипа
  в `templates/` есть свой скин. Портретная раскладка (`@media (orientation: portrait)`) — в теме движка;
  на ПК проверяется флагом `--size 540x960`.
- Тексты в документах пишутся ключами `@menu.play`: перевод берётся из `engine/assets/lang` (и `lang/`
  игры) при загрузке документа, смена языка в меню перезагружает документы.
- Экранная клавиатура (`engine/ui/UiKeyboard.*`): своя, на RmlUi, открывается при фокусе в поле
  ввода на сенсорных устройствах (на ПК — `RTS_OSK=1`). Раскладки (en, ru, символы; Shift, Caps по
  двойному нажатию, смена языка) — в `keyboard.json`, вид — `keyboard.rml` / `keyboard.rcss`; игра
  заменяет любой из файлов в своей `ui/`. Если курсор внизу экрана, клавиатура встаёт сверху.
- Сетевая игра (`lobby.rml`, `apps/client/Lobby.cpp`): адрес сервера и имя, быстрая игра по очередям
  сервера, список комнат (войти / смотреть), своя комната (слоты, боты, готовность, старт, чат).
  Матч ведёт `NetClient`: `attach(sim)`, `advance(dt)` вместо `sim.advance(dt)`, команды — от слота
  `match().localSlot` (0 — зритель); боты, которых сервер поручил этому клиенту, считаются здесь же.

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
- **Выход = сдача.** Ушедший игрок (или не вернувшийся за `reconnectGraceSec`) сдаётся командой в общем
  потоке ввода: исход решает симуляция у всех одинаково, и это видно в реплее. Рейтинг при этом списывается.
- **Вердикт сервера.** Если исход никто не подтвердил (все ушли, результаты расходятся), сервер в фоновом
  потоке прогоняет реплей на своих данных игры (`"games"` в `server.json`) и сам определяет победителя.
- **Переподключение:** после обрыва сети клиент переподключается сам (`NetClient(address, …)` или
  `setReconnect`), симуляция продолжает жить — сервер досылает только ввод после последнего подтверждённого
  тика. Если игра была перезапущена, клиент получает всю историю и пересчитывает матч с начала.
- **В игре:** `NetClient` (`engine/net/NetClient.h`) — лобби и матч. Игра строит `Simulation` по
  `match()` и вызывает `attach(sim)`, затем `advance(dt)` вместо `sim.advance(dt)`; интерфейс
  по-прежнему отдаёт команды через `sim.submit()`.
- **Тексты:** сервер не шлёт готовых фраз — только ключи локализации с аргументами (`NetText`):
  у игроков в одном матче могут быть разные языки. Клиент переводит их через
  `net::translate(loc, event.text)` и `net::displayName(...)` из `engine/net/NetLocalize.h`; строки
  лежат в `engine/assets/lang/*.json` под `net.*`. Названия очередей в `server.json` — тоже ключи (`"@net.queue.1v1"`).

```bash
./build/mac/rts_server --config apps/server/server.json            # UDP 27015, WebSocket 27016
./build/mac/rts_netbot --server 127.0.0.1 --project templates/mushrooms --queue 1v1   # бот в очередь
./build/mac/rts_netbot --server ws://127.0.0.1:27016 --project templates/mushrooms --queue 1v1
./build/mac/rts_net_tests                                          # сетевые тесты
docker build -f apps/server/Dockerfile -t rts-server .             # сервер в контейнере
```

Для `wss://` (страница игры по HTTPS) поставьте перед сервером TLS-прокси (nginx, Caddy).

## Звук

`engine/audio` — микшер, банки звуков из данных, процедурный синтез, музыка; вывод через
[miniaudio](third_party/miniaudio) (CoreAudio, AAudio/OpenSL ES, Web Audio, WASAPI, ALSA/PulseAudio).
`modules/rts/audio` превращает события симуляции в звуки; в симуляцию ничего не пишет.
Нет устройства — игра идёт молча. Нет `audio.json` — игра просто без звука.

Звук игры — файл `audio.json` рядом с `game.json` (другое имя: `"audio"` в game.json). Полное описание
формата — `docs/game-format.md`, раздел audio.json; коротко:

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
| шины | `sfx` и `ambient` — позиционные (3D: затухание, панорама, высота), `voice`, `ui`, `music` — нет |
| расстояние | `spatial`: модель затухания, дальности, «воздух» (вдали пропадают верха), реверберация растёт с расстоянием, скорость звука; у звука — свои дальности и `far` — дальний вариант с кроссфейдом |
| `maxInstances`, `cooldown`, `priority` | прореживание в больших боях: 40 лучников не дают 40 одинаковых выстрелов |
| `groups` | общие лимиты для нескольких звуков (вся рубка, все взрывы, целая шина): `max`, `cooldown`, кого гасить — `farthest` (по умолчанию: дальний бой не обрывает тот, что на экране), `oldest`, `quietest`, `none` |
| события (cues) | `attack hit miss died spawned trainStarted ready buildPlaced buildDone gathered captured lost playerDefeated victory defeat notEnough notEnoughSupply noRoom researched levelUp ability readyToPlace sold regionClaimed wave lowPower upgraded message underAttack baseUnderAttack select move attackOrder buildOrder click error` |
| поиск звука | тип → оружие/способность по id → `events` |
| музыка | плейлисты `menu`, `match`, `battle`, `victory`, `defeat` (и `match.<карта>`); кроссфейд, интро + бесшовные петли, слои по интенсивности боя, переходы по долям/тактам, стингеры, приглушение под голос |
| сжатие при сборке | `rtsbuild` перекодирует звук под платформу: ADPCM для эффектов, Vorbis для музыки, понижение частоты, моно; профили `desktop`/`mobile`/`web` в `audio.json` → `build`; `--audio off` — без перекодирования |

Звуки боя слышны только там, где нет тумана войны; «нас атакуют» — с паузой, и только если камера
смотрит в другое место. Громкость (общая и музыка) — в меню паузы, хранится в `userDataDir/audio.cfg`.
На телефоне при сворачивании звук ставится на паузу.

Отладка без звуковой карты: `RTS_AUDIO=null` (настоящий аудиопоток без железа), `RTS_AUDIO=off`,
`RTS_AUDIO_CAPTURE=/tmp/mix.wav` — записать всё, что прозвучало, в WAV.

## Модели, материалы, анимация

Тип рисуется по блоку `"view"`: встроенная фигура (`"shape"`) или файл модели (`"model"`) —
**glTF 2.0** (`.glb`, `.gltf` + `.bin` + картинки) и **OBJ + MTL**. Всё идёт через один конвейер:

- **Материалы PBR** (metallic-roughness): базовый цвет, normal, metallic-roughness, occlusion,
  emissive; режимы прозрачности opaque / mask / blend, двусторонние, unlit, варианты материалов
  (`KHR_materials_variants`), переопределения в `"materials"`. Цвет игрока: на весь материал,
  по маске-текстуре, по альфе текстуры (как в Warcraft III) или по альфе вершин.
- **Скелетная анимация** на GPU (4 влияния, любое число костей), клипы по состояниям юнита
  (`idle`, `move`, `attack`, `gather`, `build`, `die`, `spawn`, `hit`, `cast`…), плавные переходы,
  слои с маской по костям (атака верхней частью тела на ходу), скорость шага по реальной скорости.
- **Процедурная анимация** на любом узле модели или фигуре: покачивание, вращение (мельницы,
  винты), колёса по пройденному пути, наведение башни на цель, отдача, наклон в повороте,
  падение и погружение при смерти и т.д.; срабатывает по состояниям и событиям боя.
- **Уровни качества** материалов (`low` / `medium` / `high`) выбираются по GPU, как тени;
  `RTS_MATERIALS=low|medium|high` — переопределение для проверки.
- **Толпы**: клипы запекаются в текстуру, одинаковые юниты рисуются инстансингом (300 анимированных
  юнитов — 61 draw call вместо 1801); LOD моделей по дистанции, анимация вне экрана и вдали
  обновляется реже.
- **Сложная анимация**: blend space ходьба/бег, аддитивные клипы (дыхание, отдача), IK ног по
  рельефу, поворот головы к цели, наклон техники по склону, morph targets (blend shapes) на GPU.
- **Сокеты**: оружие в руке, груз на спине (по переносимому ресурсу), точки вылета снарядов,
  точки эффектов (дым, огонь, выхлоп); события клипов и `RtsView::events()/effects()` для звука и
  частиц.
- **Стадии**: строительные леса и стадии постройки, повреждённый вариант по здоровью, появление и
  смерть растворением, обводка выделения, полупрозрачный призрак при постановке здания, портреты.
- **Форматы**: ещё **FBX** (ufbx) и сжатые текстуры **KTX2** (ETC2/BC/ASTC, остаются сжатыми на GPU);
  **Collada**, **3DS**, **PLY**, **STL**; воксели **MagicaVoxel .vox** и **.vxl/.hva** (Tiberian Sun,
  Red Alert 2); модели классических RTS из копии игры пользователя: **Warcraft III .mdx/.mdl** (+ BLP),
  **Total Annihilation .3do**, **C&C Generals .w3d** (+ DDS). Остальное, что читает assimp (.blend,
  .lwo, .x, .ms3d, .md2/.md3, .smd, .b3d…), переводит в glTF утилита **modelconv**: редактор делает
  это сам при открытии и сохранении, сборщик кладёт в пакет только результат.
- **Материалы по запросу**: общая библиотека `materials.json` (наследование через `base`),
  назначение материала узлу модели (`"assign": {"Turret": "steel"}`), процедурные бесшовные
  текстуры с картами нормалей и масками цвета игрока; окно **Материалы** с превью и MCP-операции
  `material_*` / `texture_generate` для агентов.

Сломанная модель не ломает игру: тип рисуется фигурой, причина пишется в консоль редактора.
Просмотр: `rts_modelview модель.glb@Walk` или `rts_modelview --game templates/showcase` (все типы игры
так, как их рисует игра); в редакторе — окно **Models** (клипы, узлы, материалы, портрет,
перезагрузка при сохранении файла модели). Пример — `templates/showcase`, полный формат — `docs/models.md`.

## Освещение

Динамический свет рассчитан на старые Android (Mali-T, Adreno 3xx/4xx): все источники рисуются в
маленькую световую карту над видимой землёй, шейдер берёт свет одной выборкой текстуры, сколько бы
источников ни было. Уровни: `off`, `low` (карта 128², обновление через кадр), `medium` (256²),
`high` (+ 8 источников попиксельно с бликом). Выбирается по GPU, как тени; вручную: `RTS_LIGHTS=…`.
Если во время матча кадры стабильно не успевают, клиент сам понижает свет и тени по ступеням
(сначала самое дорогое и наименее заметное) и после тяжёлого боя возвращает одну ступень; ручной
выбор теней в меню это отключает (`RTS_ADAPTIVE=off`).

- **Свет типов и боя** — `view.light` (костры, кузницы, светящиеся грибы), вспышки выстрелов,
  попаданий, взрывов, свет снарядов (`attackLight`, `projectileLight`, `hitLight`, `deathLight`),
  способностей (`abilityLights`; по умолчанию цвет по действию: урон — оранжевый, лечение — зелёный),
  ореолы `halo` вместо дорогого bloom. Туман войны скрывает вспышки.
- **День и ночь, погода** — блок `lighting` в `game.json` или в карте: `time`, `dayLength`,
  `nightLevel`, `clouds`, `wind`, `storm` (тени облаков и молнии, одинаковые в реплеях), `preset`:
  `desert`, `snow`, `volcanic`, `overcast`, `cave` (подземелье: вечная ночь, свет только от
  источников). Ночью эмиссивные материалы светятся ярче, огни бликуют на воде.
- **Свет на карте** — массив `lights` в карте (фонари, костры, лава).
- **Контактные тени и AO** — мягкие пятна под юнитами и вокруг зданий, на всех уровнях
  (`contactShadow`, `contactShape` у типа).
- **Редактор** — окно «Lighting»: предпросмотр времени суток и грозы, уровень качества света,
  список источников. Для агентов: `light_add`, `light_update`, `light_delete`, `light_list`,
  `lighting_set`, `view_lighting`.

Формат полей — в шапке `modules/rts/view/RtsLights.h`. Тестовые переменные: `RTS_TIME_OF_DAY`,
`RTS_NIGHT`, `RTS_CLOUDS`, `RTS_STORM`.

## Проверка механик через MCP

Агент, который делает игру без кода, проверяет механику точечно, а не только матчем ботов.
- **`scenario_run`** — сценарий на копии открытой карты (ничего не сохраняется): `clear` (пустая карта без
  объектов, волн и стартовых юнитов), `spawn` с именами (`"id": "foot", "count": 3` → `foot#1..3`, `"foot"` — все),
  команды по секундам (те же ключи, что у `play_command`), `bots` для части слотов, `checks` с результатом
  pass/fail (`alive`, `dead`, `count`, `hp`, `resource`, `event`, `trigger`, `variable`, `distance`, `status`,
  `result` + сравнение), `watch` + `every` (состояние объектов во времени), `events` (журнал событий).
- **`play_units` / `play_command`** — команды в живой тест в окне Game (раньше были только внутри редактора, теперь
  видны MCP-клиентам). Ссылки на объекты: id из `play_units`, имена `id` из карты или фильтр
  `{type, owner, tags, kind, area, near + radius}`; `queue: true` — очередь (Shift), `ability` по id.
- **`match_run`** — плюс журнал срабатывания триггеров и `events` (журнал событий по именам).
- **`rules_lint`** — ключи `rules.json` и карты, которые загрузчик не читает (опечатки, блок не на месте), с
  подсказкой («did you mean 'armor'?»). Работает на настоящем загрузчике (`JsonAccessLog` в `engine/data/Json.h`);
  `rules_set`, `rules_merge` и `map_validate` сообщают о таких ключах сразу.
- **`rules_schema`** — `part: "keys"`: все ключи, которые загрузчик спрашивает в каждом блоке (`types.*.weapons[]`),
  собранные по всем играм, и ключи, которых нет в `docs/game-format.md`; `part: "doc"` — сам документ.
- Событие симуляции `kEvTriggerFired` (только для инструментов и представления, не входит в контрольную сумму).
- Ветка `agent-mech-tools`.

## Постэффекты

Полноэкранные эффекты поверх 3D-сцены задаются данными: блок `"postfx"` в `game.json` (карта может
его переопределить). Есть свечение (bloom), цветокоррекция, LUT, виньетка, тилт-шифт, лучи солнца,
FXAA, зерно, аберрация, резкость, пикселизация, постеризация, заливка и кинорамка, плюс готовые
наборы (кино, сочный, нуар, ретро, миниатюра, закат, зима, ночное видение). У каждого эффекта
`minQuality`. Уровень выбирается по GPU: слабые Android получают «низкое», телефоны «среднее», ПК
«высокое»; `RTS_POSTFX` переопределяет уровень. Без эффектов постобработка ничего не стоит. Лёгкие
эффекты собраны в один проход, тяжёлые (bloom, FXAA, лучи) добавляют свои.

Редактор: окно **Постэффекты** (предпросмотр уровней, таблица стоимости кадра по классам устройств).
MCP: `postfx_catalog/get/add/set/remove/preset/enable/estimate/lut_neutral`, `view_postfx`. Ответы
на изменения содержат оценку мс/кадр и предупреждения о тяжёлых эффектах. Подробности — `docs/postfx.md`.

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

Экранная клавиатура: `platform::startTextInput(area)` / `stopTextInput()` открывают и закрывают её на iOS,
Android и в мобильных браузерах (на десктопе — ничего не делают). Текст приходит событиями `Char`
на любом языке, Backspace/Enter — как `KeyDown`/`KeyUp`; `softKeyboardHeight()` и событие `KeyboardChanged`
сообщают, сколько экрана закрыто снизу, `TextInputEnded` — что пользователь сам закрыл клавиатуру.
UI (`UiLayer`) вызывает это сам при фокусе на `<input type="text|password|search|email|number">` и `<textarea>`.

Остальное в `Platform.h` для мобильных и браузера:
- `safeAreaInsets()` — отступы от выреза камеры, скруглённых углов, полоски «домой» и системных панелей
  (iOS, Android, мобильный Safari/Chrome; на десктопе 0, на MacBook с вырезом — в полноэкранном режиме).
- `setFullscreen()` / `isFullscreen()`; встроенные переключатели — F11 (Windows, Linux, web), Alt+Enter (Windows).
  В браузере вместе с полноэкранным режимом фиксируется ориентация (`AppConfig::orientation`).
- Android: кнопка «Назад» приходит игре как `Key::Escape`; полноэкранный режим с прячущимися панелями;
  физическая клавиатура и мышь (Chromebook, DeX) — обычные события клавиш и мыши.
- iPad: клавиатура Magic Keyboard / Bluetooth, мышь и трекпад (правая кнопка, наведение, прокрутка).
- Буфер обмена работает везде; в браузере Ctrl/Cmd+V доходит до игры после того, как браузер отдал текст.
- Web: `userDataDir()` хранится в IndexedDB — настройки и сохранения переживают перезагрузку страницы.

### Нативные плагины: платежи и реклама

Любой SDK платформы подключается плагином на её родном языке — Java (Android), Objective-C (iOS/macOS),
JavaScript (web) или C++ — через один асинхронный мост `engine/platform/Plugins.h`. Игра пользуется
общими API `rts::store` (покупки) и `rts::ads` (реклама) из `engine/services`, а провайдер выбирается
по платформе в `game.json`. Готовые плагины в `plugins/`: **App Store**, **Google Play**, **RuStore**,
**Яндекс Игры** (web: покупки и реклама), **Steam** (покупки через Inventory Service и DLC, Windows/
Linux/macOS), **Yandex Mobile Ads**, **AdMob**, **AppLovin MAX**, **ironSource (Unity LevelPlay)**. В билд попадают только выбранные игрой плагины (`"plugins"` в
game.json или `--plugins`): чужих SDK, зависимостей, записей манифеста и ключей Info.plist в нём нет,
CI это проверяет. Для отладки на десктопе есть заглушки магазина и рекламы (`mock-store`, `mock-ads`;
`RTS_MOCK_STORE`, `RTS_MOCK_ADS`): весь сценарий покупки проходит без телефона, а в релизные сборки
заглушки не компилируются. rtsbuild сам вписывает плагины в Gradle/манифест, Info.plist/Podfile и
HTML; для Google Play и RuStore собирайте отдельно: `--plugins googleplay,admob` и
`--plugins rustore,yandex-ads`. Подробно — `docs/plugins.md`.

### Релизы

`.github/workflows/release.yml`: тег `v1.2.3` (или ручной запуск с выбором папки игры) собирает все
платформы через `rtsbuild` и публикует GitHub Release:

| Платформа | Что выходит | Подпись (секреты репозитория) |
|---|---|---|
| Windows | установщик (Inno Setup, без прав администратора) + portable .zip | `WINDOWS_CERT_PFX_BASE64`, `WINDOWS_CERT_PASSWORD` |
| Android | .apk и .aab для Google Play | `ANDROID_KEYSTORE_BASE64`, `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD` |
| macOS | .app в .zip, с нотаризацией | `MACOS_CERT_P12_BASE64`, `MACOS_CERT_PASSWORD` (Developer ID) + ключ App Store Connect |
| iOS | проект Xcode; с ключом — сборка в TestFlight | `APPLE_TEAM_ID`, `ASC_KEY_ID`, `ASC_ISSUER_ID`, `ASC_KEY_P8_BASE64` |
| Linux | .tar.gz | — |
| Web | .zip; при `DEPLOY_WEB_PAGES=true` (переменная репозитория) — публикация на GitHub Pages | — |

Без секретов релиз всё равно выходит: APK подписан отладочным ключом, .app — ad-hoc, Windows без подписи.
Режим скриншота (`RTS_SCREENSHOT`, `RTS_EXIT_AFTER_FRAMES`) шагает ровно по 1/60 с на кадр, поэтому
кадр N одинаков на любой машине.

## Дальше

Модели glTF и анимация, звук, сеть (lockstep), способности и баффы из данных, flow fields и
RVO для больших армий, улучшения ИИ, скриптинг на C# (AOT для iOS).
