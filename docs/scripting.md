# Scripting (Lua)

Mechanics that the data blocks of `rules.json` can't express are written as **Lua 5.4 scripts** in
the game folder. Scripts are content: they are read when a match starts, packed into
`content.rpak` like any other file, edited in the editor with hot reload, and the engine never has
to be rebuilt.

There are two kinds of scripts:

| | simulation scripts | view scripts |
|---|---|---|
| listed in | rules / map `"scripts": ["scripts/x.lua"]` | rules / map `"viewScripts": ["scripts/view/x.lua"]` |
| runs | inside the deterministic tick, on every machine of a match, in replays | on the local machine only, every frame |
| numbers | integers and `fx` (fixed point); **no floats** | normal Lua numbers |
| may | read and change the game (`game.*` actions) | read the game, show notices / effects / sounds |

A game without `"scripts"` runs only the engine's standard library (below): no "scripts" system, no budget, identical checksums.

```json
// rules.json
{
  "scripts": ["scripts/rules.lua", "scripts/abilities.lua"],
  "scriptBudget": 1000000,            // optional: VM instructions per tick for all simulation scripts
  "types": {
    "archer": {"abilities": [{"id": "mend", "target": "none", "cooldown": 20, "autocast": true,
                "effects": [{"type": "script", "fn": "chain_heal", "jumps": 3, "amount": 60}]}]}
  }
}
```

Sample: `templates/scripted` (Iron and Gold + a war chest, veterans, a scripted chain heal and a
custom defeat condition).

## A first script

```lua
-- scripts/rules.lua
local util = require("lib.util")            -- scripts/lib/util.lua (only files of the game)

function on_start()                          -- global on_<event> functions are handlers
  state.kills = {}                           -- persistent data lives in `state`
  game.message(0, "Hold the bridge!")        -- 0 = every player
end

-- every 30 seconds the player with the most units gets 50 gold
every(30, function(tick)
  local best, most = nil, -1
  for _, p in ipairs(game.players()) do
    local n = game.count{owner = p, kind = "unit"}
    if game.player_alive(p) and n > most then best, most = p, n end
  end
  if best then game.give(best, "gold", 50) end
end)

on("unit_died", function(e)
  state.kills[e.owner] = (state.kills[e.owner] or 0) + 1
  if state.kills[e.owner] >= 50 then game.defeat(e.owner) end
end)
```

## Determinism rules (simulation scripts)

Lockstep multiplayer and replays re-run the same scripts on every machine and must get bit-identical
results on Windows, macOS, iOS, Android and the web. The engine enforces most of it; the rest is a
small contract.

### Numbers: integers and `fx`

Lua 5.4 has integers and floats. Floats can differ between platforms, so a simulation script never
sees one:

1. **Float literals are rejected when a file loads** — `x * 1.5`, `1e3`, `.5`, `0x1p4` give
   `scripts/x.lua:12: float literal 1.5 - use fx("1.5") or an integer`.
2. **Float arithmetic raises an error** — `/` and `^` always produce floats in Lua, so `7 / 2`,
   `n / 2`, `2 ^ 3` fail with *"float arithmetic is not allowed in simulation scripts (use fx()
   numbers, or // for integer division)"*. Use `7 // 2` (integer division, = 3) or `fx(7) / 2`
   (= 3.5). The guard sits inside the VM (`third_party/lua/rts_luaconf.h`), so it also catches
   floats made from strings (`"1.5" + 1`).
3. **Every API function that takes a number accepts only integers or `fx`**; a float gives
   *"got a float - use fx() or integers"*.

`fx` is the engine's fixed-point number (Q47.16, `engine/core/Fixed.h`), the same type all
gameplay math uses:

```lua
local a = fx(3)              -- 3
local b = fx(3, 4)           -- 0.75  (3 / 4)
local c = fx("1.25")         -- 1.25  (decimal string, parsed with integers only)
local d = a * b + 2          -- fx mixes with integers: + - * / // % unary -
if d >= 4 then ... end       -- < <= > >= work between fx and integers
print(d, tostring(c))        -- "4.25"  (formatted without floats)
fx.sqrt(d) fx.abs(x) fx.min(a, b) fx.max(a, b) fx.lerp(a, b, t)
fx.floor(d) fx.ceil(d) fx.round(d) fx.int(d)   -- -> integers (also d:floor() ...)
fx.is(v)  math.type(v) == "fx"
```

**Pitfall:** `fx(1) == 1` is `false` — Lua only calls `__eq` when both values are tables or
userdata. Compare `fx` with `fx` (`v == fx(1)`) or use `<`, `<=`, `>=`.

Values from the game are `fx` when they can be fractional (positions, hp, resources, time) and
integers when they can't (ticks, counts, players, levels, handles). `math` keeps `abs`, `min`, `max`,
`floor`, `ceil`, `sqrt` (→ fx), `tointeger`, `type`, `maxinteger`, `mininteger` and
`math.random` (simulation RNG: `math.random(n)`, `math.random(a, b)` integers, `math.random()` an
fx in [0, 1)). `tonumber("12")` works; `tonumber("1.5")` is an error (use `fx("1.5")`).

### State: `state`, `game.data(h)`, not upvalues

Everything a script remembers between ticks must be in the global table **`state`**, in an
entity's own table **`game.data(h)`**, or in trigger variables (`game.set_var`). The engine writes
them into the simulation canonically (sorted keys) **when the world is observed** — before a
checksum, a rollback snapshot or a save — not after every tick: they are hashed into the checksum
(desync detection), copied by rollback snapshots, saved, and read back into the VM when the
netcode rewinds or a save is loaded. A tick that only changes Lua tables costs nothing until then.

- **`game.data(h)`** returns the entity's own table (created on first use, gone when the entity
  dies, kept by `transform`). Only tables handed out since the last write are serialized again, and
  only changed ones are re-hashed, so per-unit records belong here, not in one big `state` table:
  ```lua
  local d = game.data(h)            -- {} the first time
  d.morale = (d.morale or 10) - 1
  for _, h in ipairs(game.units{has = "morale"}) do ... end   -- entities whose table has the key
  game.has_data(h, "morale")  game.clear_data(h)
  ```
  View scripts get a read-only copy of the stored table.
- Allowed in `state` and `game.data`: integers, `fx`, strings, booleans and nested tables whose keys
  are integers or strings. A value that can't be stored (function, float, table key, cycle) is left
  out of the saved state and logged; keep such values out of persistent tables.
- **Globals are read-only after loading**: assigning a new global in a handler is an error, reading
  an undefined one too (typos show up immediately). Define functions and constants at the top level.
- **Don't change file-level `local` variables from handlers** (`local count = 0` ... `count = count + 1`
  in a handler): such state isn't part of the snapshot, so a rollback can't restore it. Use `state`.
  Constants and helper functions as locals are fine.
- Don't keep references to `state` sub-tables or `game.data` tables in locals across ticks (a rollback
  replaces them); call `game.data(h)` again.

### Order and randomness

- `pairs()` iterates in **sorted key order** (booleans, integers, then strings), never in hash order,
  so loops over tables are deterministic. `sorted_pairs` is the same function. `next` is not
  available (iteration order would leak). Keys of other types can't be iterated with `pairs`.
- `game.units{...}` returns handles sorted by entity index.
- Randomness only from `math.random` / `game.random` (the simulation RNG).
- `tostring` never shows addresses (`"table"`, `"function"`), `string.format("%p")` is refused.
- String hashing and `table.sort` pivots use constant seeds (identical on all machines).
- Not available: `os`, `io`, `debug`, `package`, `load`, `loadfile`, `dofile`, `collectgarbage`,
  `string.dump`; `setmetatable` refuses `__gc` and `__mode` (they depend on garbage collection).

### Budget and errors

- All simulation scripts together get `scriptBudget` VM instructions per tick (default 1,000,000).
  An endless loop is stopped with *"instruction budget exceeded"* — even inside `pcall` — and the
  game goes on; handlers that didn't get to run in that tick are skipped once.
- Any error is logged with `file:line` (headless output, editor **Console** and **Скрипты** windows,
  MCP `play_status.scriptErrors`, `script_check`) and **disables that handler** for the rest of the
  match (deterministically: the flag is simulation state). Functions called from data
  (`"fn"` of effects and trigger hooks) are disabled the same way.
- Loading errors (syntax, float literals, a missing file) skip that file; the match still starts.

## Events

Handlers are registered at the top level of a file (while it loads):

| registration | called | argument |
|---|---|---|
| `on("start", fn)` / `function on_start()` | once, first tick | — |
| `on("tick", fn)` / `on_tick` | every tick | tick |
| `every(seconds, fn)` | every N seconds (seconds: integer or fx) | tick |
| `on("unit_died", fn)` / `on_unit_died` | a unit or structure died | `e.unit` (dead handle), `e.owner`, `e.unit_type`, `e.x`, `e.z`, `e.killer` (last damage source, may be dead too), `e.killer_owner` (when the killer lives) |
| `on("spawned" \| "built" \| "trained" \| "upgraded", fn)` | created / construction done / trained / upgraded | `e.unit`, `e.owner`, `e.unit_type` (`trained`: `e.producer`) |
| `on("researched", fn)` | a tech finished | `e.player`, `e.tech`, `e.producer` |
| `on("captured", fn)` | changed owner | `e.unit`, `e.owner`, `e.old_owner`, `e.by` |
| `on("player_defeated", fn)` | a player is out | `e.player`, `e.surrendered` |
| `on("ability", fn)` | a unit used an ability | `e.caster`, `e.player`, `e.ability`, `e.index`, `e.target`, `e.x`, `e.z` |
| `on("damage", fn)` | damage dealt | `e.unit`, `e.source`, `e.player` (source owner), `e.amount` (fx) |
| `on("level_up", fn)` | a hero levelled | `e.unit`, `e.player`, `e.level` |
| `on_area(name, "enter" \| "leave", fn)` | a unit entered / left a map area (map `"areas"`, checked every 5 ticks) | `e.unit`, `e.owner`, `e.area`, `e.x`, `e.z` |
| `game.after(seconds, "fn_name", arg)` | once, later (a timer kept in the simulation state) | `arg` (storable like `state`) |

Every event table also has `e.event`, `e.tick`, `e.player`. Scripts run in the **scripts** phase at
the end of the tick (after triggers) and see every event of that tick.

## The `game` table

Entities are integer **handles**. A handle of a dead or removed unit is harmless: queries return
`nil` / `false`, actions do nothing.

Queries (simulation and view scripts):

| function | result |
|---|---|
| `game.tick()`, `game.time()`, `game.tick_rate()` | tick, seconds (fx), ticks per second |
| `game.players()` | player slots `{1, 2, ...}` |
| `game.player(p)` | `{slot, team, alive, name, faction, supply, supply_cap}` |
| `game.player_alive(p)`, `game.team(p)`, `game.hostile(a, b)` | |
| `game.resource(p, "gold")`, `game.resources(p)` | fx / `{gold = fx, ...}` |
| `game.units(filter)`, `game.count(filter)`, `game.nearest(x, z, filter)` | handles sorted by index / count / nearest handle |
| `game.unit(h)` | `{id, type, name, owner, x, y, z, hp, max_hp, energy, structure, under_construction, level, garrison, tags, statuses}` |
| `game.alive(h)`, `game.owner(h)`, `game.type_of(h)`, `game.pos(h)` → x, z, `game.hp(h)` → hp, max | |
| `game.stat(h, "damage")`, `game.has_status(h, "slow")` | effective stat (techs, buffs, statuses) |
| `game.type_info("footman")`, `game.types()` | `{id, name, structure, max_hp, speed, armor, cost, tags}` |
| `game.map_size()` → w, h, `game.passable(x, z [, "ground"\|"air"\|"naval"])`, `game.level(x, z)`, `game.height(x, z)`, `game.area(name)` | map |
| `game.var(name)` | trigger variable (fx) |
| `game.data(h)`, `game.has_data(h, key)` | the entity's own persistent table (view: read-only copy) / does it have the key |
| `game.orders(h)`, `game.current_order(h)` | the order queue / its first order: `{order, x, z, target, param, state}` (`order`: `move attack_move attack patrol gather return build repair enter cast dig claim_cell need haul board unload follow rearm trade pick_item merge`) |
| `game.target(h)`, `game.idle(h)` | weapon target (else the current order's target) / no orders, not moving, not fighting, nothing in production |
| `game.facing(h)` → dx, dz, `game.velocity(h)` → vx, vz, `game.path(h)`, `game.energy(h)` → energy, max | `path`: remaining waypoints `{{x, z}, ...}` ending at the goal |
| `game.log(...)`, `print(...)` | script log |

Filter: `{owner = 2, type = "footman", tag = "mechanical", kind = "unit" | "structure", area = "gate", x = 10, z = 12, radius = 6, has = "morale", sort = "id" | "dist", limit = 5}` (all optional; `has`: the entity's `game.data` table has that key; `sort = "dist"`: nearest to x, z first, ties by handle).
Radius queries use a bucket grid of the living units, built on the first radius query and kept while
the script only reads (any action, or C++ running in between, drops it): many `game.units{x, z, radius}`
calls in one handler cost one pass over the units plus the buckets they touch. Results are exactly
those of a full scan (same order).

Actions (simulation scripts only):

| function | |
|---|---|
| `game.give(p, res, amount)`, `game.take(p, res, amount)` → taken | resources |
| `game.spawn(type, owner, x, z)` → handle or nil | owner 0 = neutral, 255 = hostile creeps |
| `game.kill(h [, silent])`, `game.damage(h, amount [, source [, damageType]])`, `game.heal(h, amount)` | |
| `game.set_owner(h, p)` | ownership change: per-type counts move, production / orders / target are cleared, `captured` fires |
| `game.set_pos(h, x, z [, exact])` (alias `game.teleport`) | mobile units only: the nearest passable cell (unless `exact` or flying), path dropped; false when it can't be placed |
| `game.set_facing(h, dx, dz)`, `game.impulse(h, dx, dz)`, `game.knockback(h, x, z, distance)` | turn; push by a vector / away from a point (negative: towards), stopped by terrain in 8 steps |
| `game.set_hp(h, hp)`, `game.set_energy(h, e)` | clamped to [0, max]; hp 0 = a normal death |
| `game.set_stat_base(h, stat, value \| nil)` | replaces the type's base value of a stat (`maxHp speed armor sight range ...`) for this unit; techs, levels, buffs apply on top; `maxHp` keeps the hp fraction; nil restores |
| `game.transform(h, type)` | the unit becomes another type in place (same handle, `game.data`, hp fraction); unit ↔ structure is refused |
| `game.add_status(h, "status" [, seconds [, caster]])`, `game.buff(h, stat, add, mul [, seconds])` | statuses of rules `"statuses"`; stat modifiers |
| `game.order(units, "move"\|"attack"\|"patrol"\|"stop"\|"hold", x, z \| target)` | through the normal command path |
| `game.command(p, {type = "train", units = {h}, param = "footman", x =, z =, target =, queue =})` | any player command, validated like network input (see below) |
| `game.cast(h, ability, target \| x, z)`, `game.player_ability(p, id, x, z [, free])` | abilities |
| `game.set_var(name, v)`, `game.add_var(name, v)` | trigger variables, shared with rules triggers |
| `game.message(p, text [, x, z])` | a notice (p = 0: everyone) |
| `game.victory(p)`, `game.defeat(p)`, `game.draw()` | end conditions |
| `game.clear_data(h)` | forget the entity's `game.data` table |
| `game.research(p, tech)`, `game.enable_type(p, type, on)`, `game.add_score(p, n)`, `game.enable_trigger(id, on)` | |
| `game.random(n)`, `game.random(a, b)` | simulation RNG |

`game.command` types (all player commands of the engine): `move attack stop hold patrol gather build
train cancel rally smart send research place sell deploy ability reinforce claim auto_build upgrade
surrender player_ability dig paint_room pick_up drop slap possess direct build_road connect_road transit
stance retreat load unload guard follow formation build_line merge split trade trade_route tribute
diplomacy buy_item use_item drop_item pick_item learn bounty rotate lock end_turn`. `param` is a number
or a name: a type (`train build upgrade build_line`), a tech (`research`), a player ability, a stance
(`aggressive defensive hold passive`) or a formation (`line box wedge loose none`). Packed params can
be given by field instead: rect commands (`dig paint_room build_road`) `w, h, extra`; `build_line`
`end_x, end_z` (cells); `trade` `resource, sell`; `tribute` `resource, to, amount`; `diplomacy`
`other, ally`; `bounty` `resource, reward`.

Not available yet: fog reveal from scripts (use a `reveal` effect in an ability).

## Data hooks: scripts inside rules

```json
{"type": "script", "fn": "chain_heal", "jumps": 3, "amount": 60}
```

- **Ability effect** (any effect list: abilities, projectiles, areas, statuses...): `fn(ctx)` with
  `ctx = {caster, owner, target, x, z, scale, source_type, targets, args}` — `targets` are the
  handles the effect's filter selected (`radius`, `affects`...), `args` is the effect's JSON. It runs
  in the scripts phase of the same tick.
- **Trigger action** `{"type": "script", "fn": "f", ...}`: `fn(ctx)`, `ctx = {player, unit, x, z,
  tick, trigger, args}`.
- **Trigger condition** `{"type": "script", "fn": "f", ...}`: `fn(ctx)` returns true / false.

Functions named in data that the scripts don't define are reported when the match loads.

## Script commands (player input with data)

A game defines its own player commands in rules; each becomes a button (with hotkey) on the command
card of the client and of the editor play-test, can be sent from view scripts and through MCP
(`play_command` with `command: "script"`), and arrives in the simulation as one generic command
(`kCmdScript`) that the engine validates before the game's handler sees it.

```json
"commands": {
  "rally_cry": { "label": "Rally cry", "hotkey": "Y", "tip": "+25% damage for 10 s",
                 "targets": ["footman", "archer", "tag:infantry"], "cost": { "gold": 25 }, "cooldown": 30 },
  "muster":    { "label": "Muster", "hotkey": "U", "targets": ["footman"], "cooldown": 5,
                 "args": [{ "name": "where", "type": "point" },
                          { "name": "pace", "type": "enum", "values": ["walk", "run"], "default": "run" },
                          { "name": "count", "type": "int", "min": 1, "max": 20, "default": 5 }] }
}
```

```lua
function on_command(player, id, args)      -- or per command: "fn": "global_function_name"
  if id == "muster" then game.order(args.units, "move", args.where.x, args.where.z) end
  -- return false: refused (cost and cooldown are not taken)
end
```

| key | |
|---|---|
| `label`, `icon`, `hotkey` (one letter / digit), `tip` | the button; script hotkeys win over the built-in ones (A, S, H, P) |
| `targets` | unit types / `"tag:x"` / `"any"`: shown when the selection has such an own unit, and only those units reach the handler (`args.units`, sorted by handle); without `targets` the command needs no selection |
| `args` | array `[{name, type, ...}]` (or an object `{name: {type, ...}}`): `int` (`min`, `max`, `default`), `enum` (`values`, `default`), `bool`, `point` (`{x, z}` in the handler), `unit` (a living unit; `own: true` = of the sender), `entity` |
| `cost`, `cooldown` | resources taken and cooldown (seconds, per player) started when the handler accepts; while unaffordable / cooling down the command is dropped (the card shows the remaining seconds) |
| `fn` | handler function (default `on_command`) |

The first `point` / `unit` / `entity` argument is what a click picks: the button enters a targeting
mode and the next left click fills it in; the other arguments take their defaults (view scripts and
MCP pass them by name). Wire format: `param` = the command's index in id order, `args` = one integer
per value (points: x and z as raw fx), so replays and network play carry them. Invalid input (unknown
index, wrong count, out of range, dead or foreign handles, a point off the map) is dropped silently.
Each player's handlers have their own instruction budget per tick (rules `"commandBudget"`, default
200000): a player who sends endless loops only burns their own commands. `game.commands()` lists the
definitions, `game.command_cooldown(p, id)` gives the remaining seconds; view scripts send with
`view.send("muster", {where = {x = 10, z = 12}, pace = "walk", units = {h1, h2}})`.

## View scripts

```lua
-- scripts/view/hud.lua  (rules: "viewScripts": ["scripts/view/hud.lua"])
view.on("unit_died", function(e)
  if e.owner == view.player() then view.notice("We lost a " .. e.unit_type) end
end)
view.on_frame(function(dt) end)            -- dt in seconds (float)
```

View scripts are presentation: floats are fine, `game.*` queries work, actions don't exist. Their
outputs are requests the host shows: `view.notice(text)`, `view.effect(name, x, z)` (particle
effect of `fx/`), `view.sound(name, x, z)`, and script commands as the local player
(`view.send(id, args)`, above). They never change the simulation directly, so they don't need the
determinism rules.

## Engine standard library (rule-level mechanics in Lua)

Part of the engine's own mechanics is written in Lua: `modules/rts/lua/*.lua`, compiled into the
engine (`embed.cmake` turns them into a header, so every platform loads them without file access)
and run by every game, before the game's own scripts, in the same deterministic VM. The C++ core
keeps everything hot (ECS, commands, movement, pathfinding, avoidance, targeting and weapons,
projectiles, vision, hashing, data parsing) and offers aggregate queries and primitives in the
low-level `rts` table (`modules/rts/RtsScriptStd.cpp`); Lua decides the rules.

| module | replaces (C++) | called |
|---|---|---|
| `victory.lua` (`std.victory`) | `RtsModule::sysVictory` | "victory" system, every 10 ticks (`std.period.victory`) |
| `waves.lua` (`std.waves`) | `RtsModule::sysWaves` (+ wave objective scan, kept as `rts.wave_objective`) | "waves" system, only in games with waves |
| `triggers.lua` (`std.triggers`) | `RtsModule::sysTriggers`, `triggerConditions`, `runTriggerActions` (parsing stays in `RtsTriggers.cpp`) | "triggers" system, only in games with triggers |
| `commands.lua` (`std.auto_build`, `std.sell`) | the `kCmdAutoBuild` and `kCmdSell` branches of `RtsModule::applyCommand` | from `applyCommand` |

Every module is **bit-identical** to the C++ it replaced: same iteration order (ECS order comes from
the C++ helpers), the same `Fixed` operations (`fx` metamethods call the engine's `Fixed`), the same
RNG calls. `tests/checksums/check.sh` compares the checksums of every sample game and of the fixtures
in `tests/checksums/games` (seeds 1-3) with `baseline.txt`, recorded from the C++ implementation;
`tests/checksums/perf.py` compares two builds (wall time, or exact instruction counts with `--ir`).

- Modules run without the instruction budget (engine code) and keep their state in the C++
  `GameState` (hashed and rolled back as before), so a game with no scripts of its own has
  unchanged checksums.
- A module schedules itself: `std.period.<name> = N` (every N ticks; 0 = never in this game).
- **Override**: a game replaces a module with `scripts/std/<name>.lua` listed in its rules
  `"scripts"` (same contract: define `std.<name>`). Its changes are then the game's own rules.
- Kept in C++ after measuring: the per-player economy pass (upkeep, capacity, income) — it runs over
  all 256 player slots every tick, and the Lua version cost +50% instructions on `ironfront`.


## Editor, hot reload, MCP

- Window **Скрипты** lists the files and the script log of the running play-test; script errors
  also appear in **Console**. When a `.lua` file under `scripts/` changes on disk, its syntax is
  checked and a running play-test restarts with the new scripts (the VM is built from the sources
  when the match starts).
- MCP: `script_api` (this reference as JSON), `script_list`, `script_read`,
  `script_write {path, text, register}` (compiled first — a syntax error or a float literal is
  returned and nothing is saved; `register: true` adds it to rules `"scripts"`), `script_check
  {seconds, seed}` (compiles everything and runs a headless bot match, returns errors with
  file:line). `game_file_write` checks `.lua` files the same way.
- Headless: `rts_headless --project templates/scripted --seed 3` prints the script log (errors also with
  `--quiet`).

## Engine side

`modules/rts/RtsScript.h` (design notes), `RtsScript.cpp` (VM, sandbox, fx, serialization),
`RtsScriptEngine.cpp` (handlers, dispatch, `state`), `RtsScriptApi.cpp` (`game` table and
`scriptApiReference()`), `modules/rts/view/RtsViewScript.*` (view VM). Loaders call
`game::embedScripts(rules, map, reader)` next to `applyTilesToMap`: script sources travel inside the
rules JSON, so the simulation needs no file access. Lua 5.4.7 is vendored in `third_party/lua`
(compiled as C, without the os / io / package / debug libraries; local changes in `README.rts.md`).
