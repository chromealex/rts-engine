# Lua in the game UI

UI documents (`ui/*.rml`) can run Lua 5.4 through RmlUi's Lua plugin. Game UI logic — screens,
menus, shop rules, rewards — lives in the game folder next to the documents; no engine code.

## Where code goes

| What | Where |
|---|---|
| Code inside a document | `<script>...</script>` in `<head>`, or `<script src="shop.lua"/>` |
| Event handlers | `onclick="Buy(document, 1)"`, `onchange`, `onkeydown`, ... (any RmlUi event as `on<event>`) |
| Modules | `ui/<name>.lua`, loaded with `require('<name>')` (`require('menu.tabs')` → `ui/menu/tabs.lua`), once per name |
| Start-up script | `ui/main.lua`: runs once after the engine documents are loaded |

All documents share one Lua state: globals and modules are visible from every document.
Inside an event handler `document`, `element` and `event` are set.

## API

- `rmlui.*`: RmlUi's Lua API — `rmlui.contexts["main"]:LoadDocument("engine/ui/shop.rml"):Show()`,
  `document:GetElementById("x").inner_rml = "..."`, `element:SetClass("active", true)`, `element.style.color`,
  data models (`context:OpenDataModel(name, table)`) — see the RmlUi Lua manual.
  Document paths: `engine/ui/<file>` picks the game's `ui/<file>` first.
- `game.*` (client):
  - `game.tr(key [, {name = value}])` → localized text; `game.language()`, `game.setLanguage(tag)`;
  - `game.name()`, `game.maps()` → `{title, ...}`;
  - `game.start(i [, {difficulty = 0..2, spectate = bool}])` (1-based map index);
  - `game.screen()` → `menu | lobby | playing | paused | over`; `game.pause()`, `game.resume()`, `game.menu()`;
  - `game.howto([open])` toggles "How to play";
  - `game.touch()`, `game.platform()`;
  - `game.log(...)` (also `print`).

## Sandbox

Game content is untrusted code (games are downloaded data), so the state only has: base (without
`dofile` / `loadfile`), `string` (without `dump`), `table`, `math`, `utf8`, `coroutine`, and `os.time /
clock / date / difftime`. No `io`, `package`, `debug`, `os.execute` and friends. Every chunk is loaded in
text mode: precompiled bytecode is refused (it can't be verified and can crash the VM).
Tests: `ui_lua_sandbox` in `tests/tests.cpp`.

## Engine side

- Lua 5.4.7: `third_party/lua` (target `rts_lua`, guarded so the engine-wide scripting module can own it).
- RmlUi Lua plugin: `third_party/rmlui/Source/Lua` (`rts_rmlui_lua`), text-only chunk loading patched in.
- `engine/ui/UiLua.h`: `luaSandbox`, `luaRunFile`, `luaRunString`; `UiLayer::attachLua(L)` after `init()`,
  before loading documents; the caller closes the state after `UiLayer::shutdown()`.
- `apps/client/ClientScript.cpp`: creates the state (to be replaced by the engine's shared state),
  the `game` table, runs `ui/main.lua`.

Next: screens and navigation helpers, a persistent player profile, store / ads calls, match start
parameters and match results for meta menus.
