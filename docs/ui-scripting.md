# Game screens and Lua in the UI

A game can have its own screens — main menu, level select, shop, profile, settings, rewards, popups —
written as RmlUi documents (HTML/CSS) with Lua logic. Everything lives in the game folder; no engine code.
Agents build them through MCP (`ui_*` tools, below).

## Files

| File | What |
|---|---|
| `ui/screens/<name>.rml` | one screen; links `../theme.rcss`, `../screens.rcss`, `../skin.rcss`; content inside `<div class="root" data-model="meta">` |
| `ui/screens.json` | `{"start": "home", "afterMatch": "result", "screens": {"dialog": {"popup": true}}}` — `start` replaces the engine main menu, `afterMatch` opens after every match with `params.result` |
| `ui/profile.json` | default player profile (any JSON: currencies, level, unlocks); the player's copy is saved per game |
| `ui/shop.json` | `items` (bought with profile currencies), `products` (real money, ids from game.json `store.products`), `rewarded` (watch an ad, `cooldown` s) — each `{id, name, description, icon, price, give, once}` |
| `ui/main.lua` | optional: runs once at start; `require('x')` loads `ui/x.lua` |
| `lang/<language>.json` | texts: `@key` as a whole text node / attribute in documents, `game.tr('key', {n = 1})` in Lua |

Engine side: `engine/assets/ui/lib/meta.lua` (ui / profile / shop, runs before `ui/main.lua`),
`engine/assets/ui/screens.rcss` (styles), `engine/ui/templates/*.rml` (MCP templates).

## In documents

- Lua: `<script>...</script>`, `<script src="x.lua"/>`, `onclick="ui.open('shop')"` (any `on<event>`; `element`,
  `event`, `document` are set). All documents share one Lua state.
- Data model `meta` (`{{...}}`, `data-for`, `data-if`, `data-class-*`, `data-attr-*`):
  - `profile` — `{{profile.gold}}`, `{{profile.stats.wins}}`;
  - `params` — what `ui.open(name, params)` passed; `screen` — current screen;
  - `shop` — rows of `shop.list()`: `id, kind (item|product|rewarded), name, description, icon, priceText, owned, affordable, ready`;
  - `game` — `{name, maps, language, touch}`; `safe` / `safeStyle` (put `data-attr-style="safeStyle"` on `.screen`: notch, home indicator, ad banner);
  - any key set with `ui.set(key, value)` (`state` is a free table);
  - events: `data-event-click="call('shop.buy', it.id)"` (any global function by dotted name), `open('shop')`, `back()`.
- Static text: `@meta.play` (engine keys `meta.*`, `currency.*`, `price.*`, `shop.*`; the game adds its own).
  Screens reload in the new language when it changes.
- CSS (`screens.rcss`): `.screen` > `.screen-header` (`.screen-title`, `button.back`) + `.screen-body` + `.screen-footer`;
  `.currencies .currency`, `.tabs`, `.cards .card` (`.card-title .card-text .card-icon`), `.list .list-row(.locked)`,
  `.stats .stat` (`.stat-value .stat-label`), `.progress-bar > .progress-fill`, `.badge`, `.avatar`,
  `button.primary / .secondary / .big`, `.popup-bg > .popup.panel`, `.row .grow .center .muted .spacer`.
  Skins (`ui/skin.rcss`) restyle them. RmlUi has flexbox and `@media (orientation: portrait)`; no box-shadow, filters
  or `linear-gradient` (use `decorator: vertical-gradient(#a #b)`).

## Lua API

- `ui` — `open(name, params)`, `back()`, `replace`, `reset`, `home()`, `popup(name, params)`, `closePopup(name?)`,
  `current()`, `root()`, `params(name?)`, `document(name?)`, `isOpen(name)`, `set(key, value)`, `get`, `refresh`,
  `screen(name, {open = fn(params, doc), close = fn(doc), back = fn() -> handled})`, `start(name)`.
  Esc / Android back calls `ui.back()`.
- `profile` — `get(path, default)`, `set(path, v)`, `add(path, n)`, `can(costs)`, `spend(costs) -> ok, missing`,
  `give(gifts)` (numbers add, other values are set), `merge(t)`, `reset()`, `save()`, `data`. Paths are dotted
  (`"stats.wins"`); every change is saved and refreshes `{{profile...}}`.
- `shop` — `list(kind?)`, `buy(id) -> ok, reason` (currencies), `buyReal(productId, fn(ok, state))` (store; grants once
  per purchase token, then finishes it; late purchases arrive by themselves), `watch(id, fn(ok))` (rewarded ad,
  cooldown), `restore(fn)`, `cooldown(id)`.
- `game` — `tr(key, args)`, `hasText(key)`, `language()`, `languages()`, `setLanguage(tag)`, `name()`, `maps()`,
  `start(i, {difficulty, spectate})`, `difficulty(n?)`, `screen()`, `pause()`, `resume()`, `menu()`, `online()`,
  `howto(open?)`, `engineMenu(on?)`, `notice(text)`, `lastResult()`, `safeArea()`, `touch()`, `platform()`,
  `preview()`, `time()`, `json('ui/x.json')`, `decode(text)`, `encode(v)`, `log(...)` (= `print`),
  `on(event, fn)`, `off(event, fn)`.
- Events: `screen` (menu | lobby | playing | paused | over), `open` / `close` `{screen}`,
  `matchEnd {won, seconds, map, mapTitle, difficulty, spectator, online}`, `language`, `purchase`, `profile`,
  `shop {id, ok, reason}`, `update (dt)`, `back`, `resize`.
- `storage` — `load(name)`, `save(name, table)`, `erase(name)`: JSON files per game in the user folder.
- `store` — `available()`, `provider()`, `products(fn(list, err))`, `buy(id, fn(p))`, `finish(token)`, `restore(fn)`;
  `ads` — `available()`, `ready(fmt)`, `preload(fmt)`, `show('rewarded' | 'interstitial', fn({shown, rewarded, error}), placement)`,
  `banner(show, top)`, `enabled(on?)` (engine/services, docs/plugins.md).
- `rmlui` — RmlUi's own Lua API (`document:GetElementById(id).inner_rml = ...`, `element:SetClass(...)`, ...).

## Sandbox

Game content is untrusted code (games are downloaded data): base (without `dofile` / `loadfile`), `string`
(without `dump`), `table`, `math`, `utf8`, `coroutine`, `os.time / clock / date / difftime`. No `io`, `package`,
`debug`, `os.execute` and friends. Every chunk is loaded as text: precompiled bytecode is refused.
`game.json` reads only `ui/*.json`; `storage` names are `[A-Za-z0-9_-]`. Tests: `ui_lua_sandbox`, `ui_lua_meta_and_templates`.

## MCP tools

| Tool | What |
|---|---|
| `ui_api` | this reference as JSON (+ `full: true` for this file) |
| `ui_screen_list` | screens, `screens.json`, which data files exist, templates |
| `ui_screen_create` | `{name, template, title?, start?, popup?, overwrite?}` — blank, home, levels, shop, profile, settings, result, dialog; writes `screens.json` / `profile.json` / `shop.json` when missing |
| `ui_screen_delete` | removes a screen and its config entries |
| `ui_screens_config` | `{start, afterMatch, popups}` |
| `ui_validate` | Lua syntax (files, `<script>`, `on*` attributes), screen links, missing texts per language, JSON |
| `ui_preview` | `{screen?, params?, profile?, device?, landscape?, width?, height?, lang?, lua?}` → PNG from the real client (safe area applied, cut-out drawn) + Lua / RmlUi log; the player's save is never touched |

Edit files with `game_file_read` / `game_file_write`, images with `asset_import`. Typical loop:
`ui_api` → `ui_screen_create` → `game_file_write ui/screens/<name>.rml` → `ui_validate` → `ui_preview` → repeat.

Client flags behind `ui_preview`: `RTSGame --ui-screen <name> [--ui-params json] [--ui-profile json] [--ui-lua code]`
with `RTS_UI_SAVE_DIR` (scratch saves) and `RTS_UI_SAFE_AREA=l,t,r,b`.

## Engine side

- Lua 5.4.7: `third_party/lua` (target `rts_lua`, guarded so the engine-wide scripting module can own it).
- RmlUi Lua plugin: `third_party/rmlui/Source/Lua` (`rts_rmlui_lua`), text-only chunk loading patched in.
- `engine/ui/UiLua.h`: `luaSandbox`, `luaRunFile`, `luaRunString`, `luaPushJson`, `luaToJson`;
  `UiLayer::attachLua(L)` after `init()`, before loading documents; the caller closes the state after `shutdown()`.
- `apps/client/ClientScript.cpp`: the state (to be replaced by the engine's shared one), `game` / `storage` /
  `store` / `ads`, events, then `meta.lua`, `ui/main.lua`, `ui/screens.json` start.
- `apps/editor/core/UiOps.cpp`: the MCP tools.
