# Game data format (RTSEngine)

A game is a folder `games/<id>/`:

```
game.json        manifest: name, id, version, rules file, maps list, orientation, icon, input
rules.json       resources, types (units/structures/objects), techs, factions, game rules, triggers
maps/*.json      terrain, players, objects, waves, triggers, areas, victory overrides
audio.json       sounds, music and which sound plays on which game event (optional: no file = silent)
models/ ui/ ...  assets (glTF models, textures, UI, sounds)
```

JSON allows `//` comments. **Numbers are plain decimals** (`1.5`, never `1e3`): the simulation is
fixed-point and deterministic. Seconds are game seconds, distances are map cells. Every block below
is optional — the blocks present on a type decide what it can do.

## game.json

```json
{ "name": "Iron and Gold", "id": "com.studio.ironandgold", "version": "0.1.0", "build": 1,
  "module": "rts", "rules": "rules.json",
  "maps": [{ "file": "maps/valley.json", "title": "River Valley (1v1)" }],
  "orientation": "landscape", "icon": "icon.png" }
```
`"audio"`: another name for the sound file (default `audio.json`).

Optional `"camera"` block (2D games, see "2D games" below):
`{"projection": "ortho", "pitch": 90, "yaw": 0, "fov": 43, "zoom": 20, "zoomMin": 8, "zoomMax": 40, "pixelsPerCell": 32, "rotate": false}`.

## rules.json — top level

| key | meaning |
|---|---|
| `supplyCap` | max supply/population (default 200) |
| `maxQueue` | production queue length (5) |
| `costModel` | `"upfront"` (pay when ordering) or `"progressive"` (Total Annihilation: pay while building; stalls when short) |
| `sellRefund` | fraction refunded when selling a structure (0.5) |
| `buildRadius` | >0: structures only within N cells of own structures with `extendsBase` |
| `levelHeight` | visual height of one cliff level |
| `waveClock` | seconds; structures with `waveSpawn` release their units together on this clock (lane battlers) |
| `autoBuild` | `{"pool": [types], "cost": {res: n}, "growth": 0.5}` — a BUILD button: the sim picks a type from the pool and a spot next to the base automatically; cost grows by `growth` per structure already auto-built |
| `victory` | `"noStructures"` (default), `"noUnits"`, `"noEntities"`, `"keyUnits"` (lose when all `key` objects die), or an object: `{"mode": "tickets", "tickets": 500, "drain": 1}`, `{"mode": "survive", "seconds": 600}`, `{"mode": "score", "seconds": 240, "keyLoses": true}`, `{"mode": "territory", "share": 0.9}` (a team owning that share of all claimable cells wins; a player with no territory is out) |
| `heights` | `{"blockVision": true, "missUphill": 0.3, "rangePerLevel": 1, "damagePerLevel": 0.1}` — cliff levels: lower ground can't see up, shots uphill can miss, higher ground adds range and damage |
| `regions` | `{"claimCost": {"food": 100}, "claimCostGrowth": 0.5, "buildOnlyOwned": true, "productionPerRegion": 0.1}` — territory (Northgard; with flags: Z) |
| `upkeep` | `[{"supply": 50, "gatherMult": 0.7}, {"supply": 80, "gatherMult": 0.4}]` — gathering yields less with a big army (Warcraft III) |
| `armorTypes`, `damageTable` | `"damageTable": {"pierce": {"heavy": 0.5, "light": 1.5}}` multipliers by damage type → armor type |
| `resources` | see below |
| `types` | see below |
| `techs` | see below |
| `factions` | see below |
| `triggers` | see "Triggers" (also allowed in maps) |
| `diggable`, `rooms`, `territory`, `hand`, `possession` | see "Dungeon management and territory" |
| `playerAbilities` | see "Player abilities" |
| `storage` | `"player"` (default: one stock per player) or `"buildings"`: goods live in building inventories, see "Settlers economy" |
| `roadNetwork` | `{"cost": {"stone": 1}, "speed": 1.5, "required": true}` — player-built road cells (map char `+`), faster movement, deliveries only between buildings on one network |
| `movement` | `{"avoidance": "rvo", "neighbors": 8, "timeHorizon": 1.5}` — local avoidance between moving units: `rvo` (default, ORCA velocity obstacles in fixed point, idle allies step aside) or `simple` (separation only); `neighbors` = agents considered per unit (quality/cost knob), `timeHorizon` in seconds. See "Movement and bodies" |
| `lanes` | `{"left": 4, "right": 2, "spacing": 0.35}` — Mushroom Wars 2 lanes between garrison buildings (replaces map `roads` for sends) |

## resources

```json
"resources": {
  "gold":   {"name": "Золото", "start": 500},
  "ore":    {"name": "Руда", "field": true, "regrow": 0.5, "cellMax": 100},              // Red Alert / Dune: harvested from map fields
  "metal":  {"kind": "flow", "start": 500, "storage": 1000},                               // Total Annihilation: income/expense per second, capped by storage
  "power":  {"kind": "capacity", "deficitProduction": 0.5, "deficitDisables": ["defense"]},// Red Alert power: provided vs consumed
  "spice":  {"capped": true, "storage": 1000},                                             // Dune silos: stock limited by storage
  "coins":  {"global": true, "start": 100},                                                // with "storage": "buildings": not a good, stays player-wide
  "ore2":   {"field": true, "convertsTo": {"credits": 4}}                                  // delivered ore is credited as 4 credits each
}
```
Kinds: `stock` (default: gathered/earned, spent), `flow` (per-second income from `income`, spent while
building in progressive mode; at 0 with negative income production slows by `deficitProduction`),
`capacity` (sum of `provides` minus `consumes`; a deficit slows production and disables types with
any tag in `deficitDisables`).

## Movement and bodies

`"move": {"speed": 2.8, "radius": 0.4, "mass": 0.64, "layer": "ground"}`

- `radius` is the real body size (cells): units keep `r1 + r2` apart, weapon and ability ranges are measured
  edge to edge (`range` + both radii, structures by their footprint), and paths keep clearance from walls and
  buildings for bodies larger than half a cell (tight gaps cost more; nothing becomes unreachable).
- `mass` (default `4 * radius²`) decides who yields when bodies overlap. Units that are fighting, holding
  position or working (gathering, building, digging) are anchored (x8) and are barely pushed; moving units (x2)
  push idle ones (x1) aside. A 0.03-cell dead zone and a per-tick push cap remove jitter.
- With `movement.avoidance: "rvo"` moving units steer around each other with reciprocal velocity obstacles
  (nearest `neighbors`, deterministic order). Idle allies in the way are asked to yield: they step to a free side
  cell and walk back once the way is clear, so a unit can pass a gap of standing units; units blocked by anchored
  ones re-path around them.
- Melee units (range < 2) attacking a target pick a free slot on a ring around it (slots by angle, claimed
  per target, ties by entity index) and path around allies already fighting instead of pushing into the front
  rank; ranged units stop and shoot. A group given an attack-move spreads into an arc whose flanks run ahead.

## types — blocks

```json
"footman": {
  "name": "Пехотинец", "tags": ["infantry", "bio"],
  "health": {"max": 420, "armor": 2, "armorType": "heavy", "regen": 0},
  "move": {"speed": 2.8, "radius": 0.4, "layer": "ground"},          // layer: ground | air | naval | sand (sand: can't enter rock/ramps)
  "sight": 8,
  "weapons": [{"id": "sword", "range": 1.0, "minRange": 0, "damage": 12, "cooldown": 1.2,
               "targets": ["ground"], "projectileSpeed": 0, "splash": 0, "damageType": "normal"}],
  "cost": {"gold": 135, "time": 18, "supply": 1},
  "requires": ["barracks", "tech_id"],
  "xp": 20, "bounty": {"gold": 5}, "score": 1
}
```

| block | meaning |
|---|---|
| `structure` | `{"footprint": [w, h], "on": "geyser", "takesOverNode": true, "terrain": "rock"\|"creep"\|"any", "extendsBase": true}` — `on`: must be built exactly on an object of that type (refinery on a geyser, TA extractor on a metal spot); `takesOverNode` inherits its resource |
| `invulnerable` | can't be damaged (trees, mines, decorations) |
| `energy` | `{"max": 200, "regen": 0.5, "start": 50}` — mana for abilities |
| `abilities` | list, see "Abilities" |
| `hero` | `{"levels": [200, 500, 900], "perLevel": [{"stat": "maxHp", "add": 50}, {"stat": "damage", "add": 0.1}]}` — gains xp from kills nearby; unique per player |
| `unique` | at most one per player |
| `squad` | `{"members": 4, "reinforceCost": {"manpower": 25}, "reinforceTime": 4}` — Company of Heroes squads: hp pool split over members, damage scales with members alive |
| `supply` | supply provided (farms, houses, overlords) |
| `income` | `{"metal": 2, "energy": -5}` per second while complete (and not disabled) |
| `storage` | `{"metal": 500}` raises caps |
| `provides` / `consumes` | capacity resources: `{"power": 100}` |
| `produces` | types it trains; **structures listed here are built Command & Conquer style** (sidebar: produce, then place with a Place command) |
| `builds` | structures a worker can construct |
| `researches` | tech ids |
| `upgrades` | structure types it can turn into in place (levels L1 → L2) |
| `buildMode` | `walk` (default: builder builds; several builders add up), `warp` (Protoss: starts it and leaves), `morph` (Zerg: the builder becomes the structure) |
| `buildPower` | builder speed multiplier |
| `gather` | `{"gold": {"carry": 10, "time": 2}}` — what it gathers (nodes or fields); flow resources (TA reclaim) are credited directly |
| `resourceNode` | `{"resource": "gold", "amount": 12000, "ownerOnly": false}` — `amount` < 0 = infinite (farms); `ownerOnly`: only the owner can gather (refinery, farm) |
| `dropoff` | resources accepted for return trips: `["gold", "wood"]` |
| `charges` | `{"max": 3, "interval": 11}` — larva: parallel production, one charge per item |
| `creep` | radius of creep it spreads (structures with `"terrain": "creep"` need it) |
| `fieldSpawner` | `{"resource": "ore", "rate": 10, "radius": 3}` — grows a resource field around it (ore mine, spice bloom) |
| `deploysTo` | unit → structure (MCV → Construction Yard) |
| `wreck` | `{"metal": 100}` — leaves a type `"wreck"` (define it with a `resourceNode`) to reclaim |
| `sellable` | default true for structures |
| `garrison` | `{"max": 20, "unit": "mushroomling", "interval": 1.0, "capturable": true, "defense": 1.0, "decay": 1.0}` — Mushroom Wars: grows units inside, sends them along roads |
| `entersGarrison` | units that merge into/assault garrisons on arrival |
| `capture` | `{"time": 10, "radius": 3, "victoryPoint": false, "region": false}` — control point: captured by standing next to it; `victoryPoint` drains enemy tickets; `region` flips its region and the capturable structures in it (Z flags) |
| `neutral` | `{"leash": 10, "wander": false, "wanderRadius": 20}` — creeps (owner -1) return home beyond the leash; wanderers roam (Dune sandworm) |
| `waveSpawn` | `{"unit": "trooper", "count": 2}` — releases units on the `waveClock`; they attack-move to the enemy base |
| `worker` | AI/UI hint (default: has gather or builds) |
| `key` | losing all key objects loses the game in `keyUnits`/`score` modes (commander, HQ) |
| `countsForVictory` | override |
| `score` | points to the killer's team (score victory) |
| `view` | presentation only (shape, colour, model; see README "Models & materials") |
| `sprite` | presentation only: draw the type from a sprite sheet (2D games, see below) |

Stats usable in modifiers: `maxHp`, `armor`, `damage` (multiplier), `range` (added), `speed`, `sight`,
`gatherRate` (multiplier; also dig/claim/work speed, alias `workRate`), `attackSpeed` (multiplier),
`productionSpeed` (multiplier on training/research/autoTrain speed of a producer).

More type blocks (dungeon management and territory, see the section below):

| block | meaning |
|---|---|
| `digs` | `{"rate": 2}` — worker digs cells its owner marked (hardness per second); idle diggers find jobs themselves |
| `claimer` | `{"time": 0.5}` — worker walks to unowned cells next to own territory and claims them (seconds per cell, x2 for enemy cells) |
| `claim` | `{"radius": 1.5, "rate": 2}` — the unit converts cells around it (cells per second): only cells touching own territory, never while hostile units stand on them (territory war front line) |
| `claimedByTerritory` | `true` — a neutral structure (portal) joins the player owning territory next to it |
| `capturable` | `true` — with `territory`: the structure flips to whoever owns the ground under its centre |
| `portal` | `{"interval": 20, "pool": ["goblin"]}` — recruits one attracted creature per interval (pool empty: every type with `attractedBy`), up to the supply capacity |
| `attractedBy` | `{"rooms": {"lair": 1, "library": 9}, "resources": {"gold": 500}, "weight": 1}` — when a portal may recruit this type (room cells owned, stock), chosen by weight |
| `needs` | creature needs and morale, see below |
| `autoTrain` | `{"unit": "soldier", "interval": 4}` — structure continuously produces units while the population cap allows (pays the unit's cost if any); they attack-move to the nearest front (closest non-own cell touching own territory, else the nearest enemy, or the rally point) and keep pushing it when idle |

### Abilities

```json
"abilities": [
  {"id": "storm", "name": "Буря", "target": "point", "range": 9, "cooldown": 12, "energy": 75, "level": 1, "autocast": true,
   "effects": [{"type": "damage", "amount": 80, "radius": 2, "damageType": "magic", "affects": "enemies"}]},
  {"id": "heal", "target": "unit", "effects": [{"type": "heal", "amount": 60}]},
  {"id": "roar", "target": "none", "effects": [{"type": "buff", "stat": "damage", "mul": 1.25, "duration": 15, "radius": 6, "affects": "allies"}]},
  {"id": "stun", "target": "unit", "effects": [{"type": "stun", "duration": 2}]},
  {"id": "wolves", "target": "none", "effects": [{"type": "spawn", "unit": "wolf", "count": 2, "duration": 60}]},
  {"id": "blink", "target": "point", "range": 8, "effects": [{"type": "blink"}]},
  {"id": "devotion", "aura": true, "radius": 7, "effects": [{"type": "buff", "stat": "armor", "add": 2, "affects": "allies"}]}
]
```
`target`: none | self | unit | point. `affects`: enemies | allies | all. Autocast abilities are also
what the AI uses.

## techs

```json
"techs": {
  "iron_swords": {"name": "Железные мечи", "cost": {"gold": 100, "time": 30}, "requires": ["blacksmith"],
                  "effects": [{"tags": ["infantry"], "stat": "damage", "mul": 1.15}, {"type": "archer", "stat": "range", "add": 1}]}
}
```
An effect without `tags`/`type` applies to every type. Repeatable upgrades: `"levels": 5, "costGrowth": 0.5` — the
tech can be researched 5 times, each level re-applies its effects (multipliers compound) and costs
`cost * (1 + costGrowth * levelsDone)`; `researched` counts levels. Requirements can be types or techs (ages: a tech
required by the next tier's types).

## factions

```json
"factions": {
  "terran": {"name": "Терраны", "start": [{"type": "command_center", "offset": [0, 0]}, {"type": "scv", "offset": [3, 0], "count": 4}]}
}
```
Map players choose a faction and a `start` point; start objects spawn there.

## Maps

```json
{
  "name": "River Valley", "size": [64, 48],
  "players": [{"name": "Blue", "team": 1, "faction": "terran", "start": [8, 24], "resources": {"gold": 800}},
              {"name": "Zombies", "team": 9, "waves": true}],
  "terrain": {
    "rows":   ["....~~~~....", "..."],      // one char per cell: . ground ~ water # blocked , no-build = shallow / ramp R rock
    "levels": ["000011110000", "..."],      // cliff level per cell 0..9; units change level only over / ramps
    "heights": [0, 0, 16, ...]              // optional fine visual height per cell (1/16 units)
  },
  "fields": [{"resource": "ore", "rows": ["0000999000", "..."]}],   // density digits 1..9 of cellMax (or {"center": [x,z], "radius": 3})
  "regions": {"rows": ["AAAABBBB", "..."], "names": {"A": "Долина", "B": "Холмы"}},
  "entities": [{"type": "townhall", "owner": 1, "pos": [9, 24]}, {"type": "goldmine", "owner": 0, "pos": [6, 16], "amount": 8000},
               {"type": "ogre", "owner": -1, "pos": [32, 10]}],     // owner: 0 neutral, 1..N players, -1 hostile creeps
  "roads": [["base_a", "village_1"]],                                 // by entity id (Mushroom Wars)
  "roadsOnly": false,
  "areas": {"relic": [26, 4, 4, 4]},                                  // named rectangles [x, z, w, h] for triggers
  "waves": [{"time": 60, "player": 2, "at": [60, 24], "spread": 3, "spawn": [{"type": "zombie", "count": 10}],
             "every": 45, "growth": 1.3, "repeats": 10}],
  "victory": {"mode": "survive", "seconds": 900},                     // overrides rules.victory
  "triggers": []
}
```

## audio.json

Presentation only: nothing here reaches the simulation. A missing file or audio device just means
no sound. Malformed structure is an error (`audio: sound 'x': unknown bus 'drums'`); a missing or
undecodable sound file, an unknown key, type, weapon or sound name is a warning and only that sound
stays silent.

```json
{
  "volume": {"master": 1, "sfx": 0.9, "ui": 0.7, "voice": 1, "music": 0.6, "ambient": 0.7},
  "sounds": {
    "swing": {"files": ["sfx/swing1.ogg", "sfx/swing2.ogg"], "pitch": [0.9, 1.1], "maxInstances": 4, "cooldown": 0.05},
    "alarm": {"file": "voice/under_attack.ogg", "bus": "voice", "priority": 10},
    "blip":  {"synth": {"wave": "square", "freq": [880, 440], "duration": 0.08}, "bus": "ui"}
  },
  "music": {
    "menu":  "music/menu.ogg",
    "match": {"files": ["music/a.ogg", "music/b.ogg"], "shuffle": true, "loop": true, "volume": 0.8},
    "battle": ["music/fight.ogg"]
  },
  "events":    {"died": "death", "underAttack": "alarm", "notEnough": "need_more", "notEnough.gold": "need_gold"},
  "weapons":   {"sword": {"attack": "swing", "hit": "clang"}, "bow": {"attack": "bow", "hit": "arrow_hit"}},
  "abilities": {"storm": "thunder"},
  "types":     {"footman": {"select": "footman_what", "move": "footman_yes", "died": "death_human",
                            "weapons": {"sword": {"hit": "clang_heavy"}}, "abilities": {"roar": "roar"}}}
}
```

`volume`: mix level per bus, 0..4 (default 1); the player's own volume settings multiply on top.

### sounds

| key | meaning |
|---|---|
| `file` / `files` / `synth` | exactly one. Files: WAV, OGG Vorbis, MP3, FLAC, paths relative to the game folder; `files` = variations, a random one per play, never the same twice in a row |
| `bus` | `sfx` (default), `ambient`, `voice`, `ui`, `music` |
| `spatial` | attenuated and panned around the camera when the event has a position; default true for `sfx` / `ambient`, false otherwise (announcements are heard anywhere) |
| `volume` | 0..4 (1) |
| `pitch` | number or `[min, max]` random per play (1) |
| `maxInstances` | at most this many at once; a new play stops the oldest (4; 0 = no limit) |
| `cooldown` | seconds between two starts (0) — 40 archers firing together make a few sounds, not 40 |
| `priority` | -100..100 (0): when all voices are busy, the lowest priority (oldest first) is stopped; a new sound below everything playing is dropped |
| `loop` | loops until stopped (false) |

`synth` — a sound made from parameters instead of a file (prototypes, playables); an object or an
array of layers that are mixed:

| key | meaning |
|---|---|
| `wave` | `sine` (default), `square`, `saw`, `triangle`, `noise` |
| `freq` | Hz, or `[start, end]` for an exponential sweep (440); for `noise` it is the grain rate |
| `duration` | seconds, 0.001..10 (0.2) |
| `attack`, `release` | seconds of fade-in / fade-out (0.005, min(0.1, duration)) |
| `volume` | 0..4 (1) |
| `duty` | square pulse width 0.02..0.98 (0.5) |
| `vibrato` | `[rate Hz, depth]`, depth as a fraction of the frequency |
| `lowpass` | cutoff Hz (off) |
| `crush` | keep only N bits, 1..15 (off) |
| `delay` | seconds before this layer starts (0) |

### music

A playlist: a file name, an array of files, or `{"files": [...], "shuffle": false, "loop": true,
"volume": 1}`. Changes crossfade. Playlists the client plays by name: `menu` (main menu), `match`,
`battle` (replaces `match` while heavy fighting happens near the camera, back after ~15 s of calm),
`victory`, `defeat`. All optional.

### events, types, weapons, abilities

Which sound plays on which game event ("cue"). Lookup for an object: its entry in `types`, then its
weapon / ability by id in `weapons` / `abilities`, then `events`.

| cue | when | heard by |
|---|---|---|
| `attack` | a weapon fires (`weapons.<id>.attack` first) | everyone who sees the spot |
| `hit` | damage lands (`weapons.<id>.hit` of the shooter's type first) | everyone who sees the spot |
| `miss` | a shot misses (uphill) | everyone who sees the spot |
| `died` | an object dies (`types.<id>.died`) | everyone who sees the spot |
| `spawned` | an object appears | everyone who sees the spot |
| `ability` | an ability is cast (`abilities.<id>` first) | everyone who sees the spot |
| `captured` | the player captures a building / point; for others: someone captured something in view | owner |
| `lost` | the player's building / point was captured | previous owner |
| `trainStarted`, `ready` | production started / a unit is ready | owner |
| `buildPlaced`, `buildDone` | construction placed / finished | owner |
| `readyToPlace`, `sold`, `upgraded`, `researched`, `levelUp`, `regionClaimed`, `lowPower`, `gathered` | as named | owner |
| `notEnough`, `notEnough.<resource>` | can't afford (the resource-specific one first) | owner |
| `notEnoughSupply`, `noRoom` | supply cap / no place for an automatic build (fallbacks: `notEnough`, `error`) | owner |
| `underAttack`, `baseUnderAttack` | an enemy hits the player's unit / structure where the camera isn't looking; at most every 12 s | owner |
| `select` | the player selects an own unit (`types.<id>.select`) | player |
| `move`, `attackOrder`, `buildOrder` | the player gives an order (fallback: `move`) | player |
| `victory`, `defeat`, `playerDefeated` | match end / another player is out | player |
| `wave` | a scripted wave starts | everyone |
| `message` | a trigger message | its addressees (everyone for a message to all) |
| `click`, `error` | interface | player |

"Everyone who sees the spot" respects fog of war; a spectator hears all world sounds and no
player-only cues.

## Dungeon management and territory

```json
"diggable": {"D": {"hardness": 3}, "G": {"hardness": 6, "yields": {"gold": 250}}, "J": {"hardness": 8, "yields": {"gold": 40}, "infinite": true}},
"territory": {"claimBy": "both", "startFromRegions": false, "startRadius": 3, "buildOnlyOwned": true},
"rooms": {
  "lair":     {"name": "Логово", "cost": {"gold": 10}, "supply": 1},
  "hatchery": {"cost": {"gold": 12}, "income": {"food": 0.05}},
  "treasury": {"cost": {"gold": 15}, "storage": {"gold": 500}},
  "library":  {"cost": {"gold": 20}, "income": {"research": 0.02}, "work": {"yields": {"research": 1}}},
  "training": {"cost": {"gold": 20}, "work": {"xp": 3, "cost": {"gold": 0.5}}}
},
"hand": {"max": 8, "ownTerritoryOnly": true, "slapDamage": 5, "slapDuration": 10, "slapSpeed": 1.5, "slapWork": 1.5},
"possession": {"cost": {"mana": 100}, "drain": {"mana": 2}}
```

- **Diggable terrain**: map characters listed in `diggable` (one character, not `. ~ # , = / R` or a digit) are
  solid cells. Players mark cells with the dig command; idle `digs` workers dig marked cells next to ground they
  can reach; the cell becomes ground (paths update) and `yields` is credited (`infinite`: the seam stays).
- **Territory** (per-cell owner layer, `MapGrid::territory`; `GameState::territoryCells[slot]` / `territoryTotal`
  for the HUD share): starts from map `"territory": {"rows": ["..11..", ...]}` (digits = player slots), from owned
  regions (`startFromRegions`) and `startRadius` cells around starting structures and start points. Claimed by
  `claimer` workers and/or units with `claim` (`claimBy`: `workers`, `units`, `both`). `buildOnlyOwned`: every
  footprint cell must be own territory. Losing a cell removes a room painted on it.
- **Rooms** are painted on own territory (paint command with a rect); `cost` is per cell. Effects scale with the
  player's total cells of the type: `supply` (creature capacity), `income`, `storage`, `provides` per cell; `work`
  is earned per creature working inside (`yields` per second, `xp` per second for types with `hero` levels, `cost`
  per second). Painting over another room replaces it; erasing refunds `sellRefund`.
- **Creature needs** (`needs` on a type):
  ```json
  "needs": {"hunger": {"interval": 60, "room": "hatchery", "time": 4, "cost": {"food": 1}},
            "sleep": {"interval": 90, "room": "lair", "time": 8},
            "pay": {"interval": 120, "amount": {"gold": 50}},
            "works": ["library", "training"], "workTime": 10,
            "happiness": {"start": 1, "unmetPerSecond": 0.01, "recovery": 0.005, "slap": 0.1, "unpaid": 0.3, "leaveBelow": 0.2, "leaveAfter": 30}}
  ```
  A need becomes active after `interval`; the idle creature walks to the nearest own room of that type and stays
  `time` seconds (paying `cost`; sleeping heals). Payday deducts `amount`; unpaid wages, slaps and unmet needs
  lower happiness; below `leaveBelow` for `leaveAfter` seconds the creature walks out through an own portal.
  Idle creatures work in `works` rooms (the AI may take them from work to fight).
- **Hand**: pick up own units (`max` in the hand, off the map: no orders, no combat), drop them on a visible own
  territory cell; slap: `slapDamage` (never kills), speed/work buff for `slapDuration`, happiness down.
- **Possession**: one unit per player is steered directly (direction vector per tick, fire button); the AI
  ignores it; `cost` once, `drain` per second (released when unaffordable).

## Settlers economy (Knights and Merchants)

```json
"storage": "buildings",
"roadNetwork": {"cost": {"stone": 1}, "speed": 1.5},
"types": {
  "storehouse": {"structure": {"footprint": [3, 3]}, "storehouse": {"capacity": 2000}},
  "woodcutter": {"structure": {"footprint": [2, 2]}, "recipe": {"out": {"wood": 1}, "time": 10, "from": "wood", "radius": 8}, "cost": {"planks": 3, "stone": 2}},
  "sawmill":    {"structure": {"footprint": [2, 2]}, "recipe": {"in": {"wood": 1}, "out": {"planks": 2}, "time": 8, "buffer": 2}},
  "farm":       {"structure": {"footprint": [3, 3]}, "recipe": {"out": {"grain": 1}, "time": 12}},
  "serf":       {"move": {"speed": 3}, "carrier": {"capacity": 1}},
  "soldier":    {"trainFrom": "recruit", "cost": {"sword": 1, "armor": 1, "time": 6},
                 "needs": {"hunger": {"interval": 120, "at": "inn", "time": 3, "cost": {"food": 1}, "starve": 1}}}
}
```

- With `"storage": "buildings"` every `stock` resource that is not `global` is a **good**: it lives in building
  inventories. The player's stock of a good is the sum of its **storehouses** (`storehouse.capacity` = total
  goods); paying takes goods out of storehouses, income and bounties are put into them. Starting goods (resource
  `start`, map player `resources`) go into the first storehouses; map entities can carry `"inventory": {"wood": 5}`.
- **Recipes**: a workplace consumes `in` from its own inventory and produces `out` into it every `time` seconds
  (`productionSpeed` stat and power apply); it keeps `buffer` cycles of inputs and stops when `buffer` cycles of
  output are waiting. Raw producers have no `in`; `from` + `radius` deplete the nearest resource node or field
  cell of that resource (wood from trees, stone from a quarry, fish from water fields).
- **Carriers** (`carrier.capacity` per trip) haul on request, deterministically (every 0.5 s, buildings in
  entity order): construction sites and workplace inputs first (source: workplace output, else nearest
  storehouse), then workplace outputs to the nearest storehouse with room. With `roadNetwork.required` the
  source and destination must touch the same road network.
- **Construction**: goods in a structure's `cost` are not paid at once; carriers deliver them to the site and
  building progress can't exceed the delivered share (builders still build). Other resources are paid up front.
- **Roads**: map char `+`, the road command (rect), or the connect command (the sim paves the shortest path from
  a structure to the road network of an own storehouse). Movement on road cells is `speed` times faster.
- **Recruits**: `trainFrom` consumes an idle own unit of that type within 15 cells of the producer.
- **Food**: needs with `"at": "<structure type>"` are satisfied at the nearest own structure of that type (cost
  taken from the stock); `starve` hp per second is lost while the need is active.

## Lanes (Mushroom Wars 2)

`"lanes": {"left": 4, "right": 2, "spacing": 0.35}`: for every pair of garrison buildings the sim finds an A* path
around obstacles and other buildings, smooths it into a fixed-point Catmull-Rom spline (about two samples per
cell) and adds `left` + `right` parallel lanes `spacing` cells apart (pulled back towards the main lane where they
would hit an obstacle). Sent units are spread over the lanes round-robin and follow their lane exactly. Lanes are
rebuilt when garrison buildings or obstacles change (`GameState::lanes`, `lanesDirty`, event `kEvLanesChanged`).

## Player abilities

```json
"playerAbilities": [
  {"id": "nuke", "name": "Ядерный удар", "cost": {"money": 500}, "cooldown": 60, "target": "point",
   "effects": [{"type": "damage", "amount": 200, "radius": 3}]},
  {"id": "drone", "cost": {}, "cooldown": 5, "charges": {"max": 2, "rearmCost": {"money": 100}, "rearmTime": 20},
   "effects": [{"type": "damage", "amount": 60, "radius": 1.5}, {"type": "stun", "duration": 2, "radius": 1.5}]}
]
```
Not tied to a unit. Effects are the ability effects (`damage`, `heal`, `stun`, `buff`, `spawn`; not `blink`).
Charges rearm one at a time after `rearmTime`, paying `rearmCost`. The AI fires them at enemy concentrations.

## Commands (for tools and network code)

Besides the unit commands (`RtsModule.h`): `kCmdSurrender` (the issuing player is defeated, event value 1),
`kCmdDig` / `kCmdPaintRoom` (point = first cell, `param = rectParam(w, h, extra)`: dig `extra` 0 mark / 1 unmark;
room `extra` = room index or -1 erase), `kCmdPickUp` (units), `kCmdDrop` (point; units or `param` = count),
`kCmdSlap` (units), `kCmdPossess` (units[0]; empty = release), `kCmdDirect` (point = direction, `param` bit 0 = fire,
optional target), `kCmdPlayerAbility` (`param` = index, point), `kCmdBuildRoad` (rect, `extra` 1 = remove),
`kCmdConnectRoad` (units = own structures).

## Triggers

Event → conditions → actions, evaluated inside the deterministic simulation.

```json
{"id": "relic", "once": true, "enabled": true, "forEachPlayer": false,
 "event": {"type": "enterArea", "area": "relic", "player": 1, "unit": "hero"},
 "conditions": [{"type": "count", "player": "triggering", "kind": "structure", ">=": 3}],
 "actions": [{"type": "message", "text": "Реликвия найдена!"}, {"type": "give", "resource": "gold", "amount": 500}]}
```

Events: `start`, `time` (`seconds`), `every` (`seconds`), `enterArea` / `leaveArea` (`area`, unit filter),
`unitDied`, `built`, `trained` (unit filter), `researched` (`tech`), `captured`, `wave`, `regionClaimed`,
`playerDefeated`, `ability`, `levelUp`, `resource` (`resource`, comparison: fires when it becomes true),
`variable` (`name`, comparison). Event `player`: a slot number, `"any"` (default), `"creeps"`.

Unit filter (events, conditions, actions): `"unit": "type"`, `"tags": [...]`, `"kind": "structure"|"unit"`, `"area": "name"` or `[x,z,w,h]`.

Comparisons: one of `">=", ">", "<=", "<", "==", "!="` with a number.

Conditions: `count` (objects matching the filter), `resource`, `variable`, `researched`, `random`
(`chance`), `time` (seconds), `alive`, `regions` (owned count). `"not": true` negates.

Actions: `spawn` (`unit`, `count`, `at` [x,z] / `area`, `attackMove`), `give` / `take` (`resource`,
`amount`), `setOwner`, `kill`, `remove`, `damage`, `heal` (`amount`), `buff` (`stat`, `add`/`mul`,
`duration`), `message` (`text`), `setVariable` / `addVariable` (`name`, `value`), `research` (`tech`),
`enableType` / `disableType` (`unit`), `victory` / `defeat`, `enableTrigger` / `disableTrigger`
(`trigger`), `addScore` (`amount`), `setRegionOwner` (`region` name), `attackMove` / `move` (units
matching the filter to `at` / `area`), `setTickets` (`amount`).

Action `player`: `"triggering"` (default: the player of the event), `"all"`, `"enemies"`, `"allies"`,
`"creeps"`, `"neutral"`, or a slot number. `"target": "triggeringUnit"` acts on the event's unit.

## Recipes (mechanics of known games)

- **Warcraft III**: workers gather gold from mines and wood from trees (`resourceNode` + `gather`), `supply` from farms, `upkeep`, heroes with `hero` + `abilities`, creeps with owner -1 and `neutral.leash`, `bounty`.
- **StarCraft**: three factions with `buildMode` walk/warp/morph, larva `charges`, `creep`, refinery `structure.on` + `takesOverNode`, cliff `levels` + ramps + `heights.missUphill`.
- **Red Alert / C&C**: ore `fields` + harvesters + refinery `dropoff`, `fieldSpawner` mines, `power` capacity with `deficitDisables`, construction yard `produces` structures (sidebar), `deploysTo` MCV, `sellable`, `buildRadius`.
- **Dune II**: spice `fields`, `capped` spice with silos `storage`, `terrain: "rock"` for structures, sandworm `neutral.wander` + `move.layer: "sand"`.
- **Total Annihilation**: `flow` metal/energy with `income`, `costModel: "progressive"`, commander `key` + `victory: keyUnits`, extractors `structure.on: metal_spot`, `wreck` reclaim.
- **Age of Empires II**: four `stock` resources, farms with infinite `ownerOnly` nodes, ages as techs required by types, `techs` with tag effects.
- **Company of Heroes**: `squad`, `capture` points with `victoryPoint`, `victory: tickets`, `income` from captured points via triggers or `income` on point types.
- **They Are Billions**: map `waves` from a `waves: true` player, `victory: survive`, `buildRadius`.
- **Northgard**: `regions` with `claimCost` and `buildOnlyOwned`.
- **Z**: regions with `capture.region` flags, `productionPerRegion`, factories in sectors with `capturable`.
- **Mushroom Wars**: `garrison` structures, `roads` + `roadsOnly`, `entersGarrison` units, Send command.
- **Lane battlers**: `autoBuild`, `waveClock` + `waveSpawn`, `upgrades`, `victory: score`.
- **Dungeon Keeper**: `diggable` rock and gold veins, imps with `digs` + `claimer`, `territory.buildOnlyOwned`, painted `rooms` (lair `supply`, hatchery food `income`, library `work`), neutral `portal` with `claimedByTerritory`, creatures with `attractedBy` + `needs`, `hand`, `possession`, `victory: keyUnits` (dungeon heart).
- **Knights and Merchants / Settlers**: `"storage": "buildings"`, storehouses, `recipe` chains (woodcutter → sawmill, farm → mill → bakery), `carrier` serfs, `roadNetwork`, materials delivered to sites, `trainFrom` recruits, inn food `needs` with `starve`.
- **Mushroom Wars 2**: `garrison` villages + `lanes` instead of hand-made `roads`.
- **Territory war (State.io)**: `territory` with `claimBy: units` + `startFromRegions`, soldiers with `claim`, `capturable` towns, barracks with `autoTrain`, `playerAbilities`, repeatable `levels` techs, `victory: territory`.

## Sample games (data only, `games/<id>/`; maps from `games/_tools/mapgen.py`)

| id | like | recipe |
|---|---|---|
| `frostcrown` | Warcraft III | gold mines + pines, burrows (`supply`), `upkeep` tiers, altar hero with `hero` levels, aura and `whirl`, creep camps (`neutral.leash`, `bounty`), siege demolisher |
| `starfall` | StarCraft | factions guild (`buildMode: warp`) / swarm (`morph`, larva `charges`, `creep`, `terrain: creep`) / legion (walk), geysers + `structure.on`, plateaus with `levels` + ramps, `heights.missUphill` |
| `ironfront` | Red Alert | `mcv` `deploysTo` yard, sidebar `produces` structures, ore `fields` + `fieldSpawner` mines, ore `convertsTo` credits, `power` capacity + `deficitDisables`, `buildRadius`, trigger: free harvester per refinery |
| `ambersands` | Dune II | structures `terrain: rock`, spice blooms on sand, capped credits + silo `storage`, wind traps, sandworm `neutral.wander` + `move.layer: sand` |
| `metaltide` | Total Annihilation | `flow` metal/energy, `costModel: progressive`, commander `key` + `victory: keyUnits`, extractors on metal spots, rock/wreck reclaim |
| `fourages` | Age of Empires II | four stocks, camps as dropoffs, farms (infinite `ownerOnly` nodes), ages as techs, blacksmith tech effects, rams vs buildings |
| `holdtheline` | Company of Heroes | `squad` infantry with reinforcement, `victoryPoint` capture points + `victory: tickets`, fuel/munition points with `income` |
| `lastcolony` | They Are Billions | `victory: survive`, map `waves` from a `waves` player with growth, `buildRadius`, towers and walls |
| `clanlands` | Northgard | 3x3 map `regions`, `regions.claimCost` + growth, `buildOnlyOwned` |
| `flagsectors` | Z | sector `regions`, flags with `capture.region`, `capturable` factories, `productionPerRegion` |
| `laneclash` | lane battler | `autoBuild` pool, `waveClock` + `waveSpawn`, `upgrades`, `victory: score` with `keyLoses` |
| `underlord` | Dungeon Keeper | `diggable` rock/gold/gem, imps with `digs` + `claimer`, `rooms`, neutral `portal` + `claimedByTerritory`, creatures with `attractedBy`, `needs` and hero levels from training, `hand`, `possession`, leveled tech paid in research |
| `borderrush` | State.io (portrait) | `territory` with `claimBy: units` + `startFromRegions`, soldiers with `claim`, `autoTrain` capital and towns, `capturable` towns, `playerAbilities` (air strike, drones with charges), leveled techs incl. `productionSpeed`, `victory: territory` |
| `hearthhaul` | Knights and Merchants | `storage: buildings`, storehouse, recipe chains (woodcutter → sawmill, farm → mill → bakery, armoury), serf `carrier`s, `roadNetwork`, delivered construction materials, `trainFrom: recruit`, inn `needs` with `starve` |
| `sporelanes` | Mushroom Wars 2 | `garrison` hives/huts/towers, `lanes` instead of `roads` |

## 2D games

Still the 3D engine, seen through an orthographic camera; objects can be sprites and the ground a
tile map. Two flavours, both pure data (samples: `games/tiles2d`, `games/km2d`; their placeholder
art is generated by `tools/art2d/make_samples.py`):

- **Tile 2D** (Red Alert, Dune II, Warcraft II): `camera` straight down (`pitch: 90`), a map
  `tiles` layer drawn from a tileset, sprite units and buildings (`"mode": "ground"`).
- **3D terrain + sprites** (Knights and Merchants): the normal terrain (levels, ramps, heights),
  `camera` at a fixed angle (`pitch: 50`), billboard sprites standing on the ground.

### game.json `camera`

| key | meaning |
|---|---|
| `projection` | `"ortho"` or `"perspective"` (default) |
| `pitch`, `yaw`, `fov` | degrees; pitch 90 = straight down, yaw 0 = north up |
| `zoom`, `zoomMin`, `zoomMax` | start/limits: visible map cells vertically (ortho) or camera distance (perspective); without `zoom` the game frames the map |
| `pixelsPerCell` | ortho: start zoom so that one world unit is that many screen pixels (pixel-perfect sprites drawn with the same `pixelsPerCell`) |
| `rotate` | allow camera rotation (default: false for ortho) |

The editor uses the projection and angle for its Scene and Game views.

### type `sprite`

```json
"tank": { ...,
  "sprite": {"sheet": "sprites/tank.png", "frame": [32, 32], "facings": 8, "mirror": false,
             "mode": "billboard", "pixelsPerCell": 32, "anchor": [0.5, 1.0], "offset": [0, 0],
             "team": "magenta", "filter": "nearest", "depthBias": 1.0, "lift": 0,
             "shadow": {"size": [0.8, 0.45], "alpha": 0.35}, "corpseTime": 4,
             "actions": {"idle": [0, 1], "move": [1, 4, 8], "attack": {"start": 5, "count": 2, "fps": 6},
                         "work": [5, 2, 4], "build": {"start": 1, "count": 4, "loop": false}, "die": [7, 3, 6]}}}
```

| key | meaning |
|---|---|
| `sheet` | image (PNG/JPEG/TGA), required. **Rows = facings** (row 0 faces east/+X, then clockwise on screen: SE, S, SW, W, ...), **columns = animation frames** |
| `frame` | frame size in pixels (default: whole sheet width, square) |
| `facings` | 1, 4, 8, 16, 32... rows; `mirror: true` stores only north → east → south (`facings/2 + 1` rows) and flips them for the west side |
| `actions` | column ranges `[start, count, fps]` or `{"start", "count", "fps", "loop"}`. `idle`, `move`, `attack`, `work` (gathering/building workers), `build` (structure under construction; non-looping = frame follows the progress), `die` (played where the object disappeared, then the last frame stays `corpseTime` s). Missing actions fall back to `idle` (`work` → `attack` → `idle`) |
| `mode` | `billboard` (faces the camera; default), `upright` (vertical quad), `ground` (lies on the terrain; top-down games) |
| `size` / `pixelsPerCell` / `scale` | world size: `[w, h]` cells, or frame pixels / `pixelsPerCell`; default width = footprint width (structures) or 1 |
| `anchor` | pivot in the frame, 0..1 from the top-left (default `[0.5, 1]`, ground mode `[0.5, 0.5]`) |
| `offset` | `[right, front]` cells from the object centre; default for standing structures: the front edge of the footprint, so a 3/4-view drawing stands on its footprint |
| `team` | `"magenta"` (default): pixels in shades of magenta (r = b, g = 0) become the player's colour times their brightness; `"none"` |
| `shadow` | soft blob under the sprite (`true`, `false` or `{size, alpha}`; default on for standing units) |
| `depthBias`, `lift` | pull towards the camera so hills don't cut the sprite; air units float `lift` cells (default 1.2) above their shadow |

Sprites are batched (one draw call per sheet), alpha-tested, depth-tested against the 3D scene and
painter-sorted; textures are read with `platform::readContentFile`. Types without `sprite` keep
their 3D `view`, so a map can mix both.

### Tilesets (`tilesets/<name>.json`)

```json
{"name": "Desert", "atlas": "tilesets/desert.png", "tileSize": [32, 32], "columns": 8, "filter": "nearest",
 "terrains": [
   {"id": "sand",  "char": "s", "cell": ".", "color": [0.84, 0.71, 0.47], "tiles": [0, 1, {"tile": 2, "weight": 0.6}]},
   {"id": "spice", "char": "o", "cell": ".", "field": {"resource": "crystal", "density": 9},
    "under": "sand", "depleted": "sand", "tiles": [4, 5], "autotile": {"type": "corner16", "first": 104}},
   {"id": "rock",  "char": "r", "cell": "R", "level": 0, "under": "sand", "tiles": [6, 7], "autotile": {"type": "blob47", "first": 8}},
   {"id": "cliff", "char": "c", "cell": "#", "under": "sand", "autotile": {"type": "mask16", "first": 120}},
   {"id": "water", "char": "w", "cell": "~", "under": "sand", "autotile": {"type": "blob47", "tiles": [56, 57, "... 47 indices"], "with": ["deep"]}}]}
```

Atlas tile `i` is at column `i % columns`, row `i / columns`. Per terrain:

| key | meaning |
|---|---|
| `char` | character of the terrain in the map's `tiles.rows` |
| `cell`, `level`, `field` | **gameplay**: cell kind (`. ~ # , = / R`, as in `terrain.rows`), cliff level 0..9, resource field `{resource, density 1..9}` |
| `tiles` | base variants (weights optional, default 1; picked by a deterministic per-cell hash) — also used for the interior of autotiled terrain |
| `autotile` | edges/corners chosen from the 8 neighbours. `mask16`: 4 edges, index N=1 E=2 S=4 W=8. `corner16`: 4 corners (a corner counts when its 3 cells are the same terrain), index NE=1 SE=2 SW=4 NW=8. `blob47`: 8 neighbours, a corner only counts with both its edges; the 47 shapes in ascending order of the mask N=1 NE=2 E=4 SE=8 S=16 SW=32 W=64 NW=128 (index 0 = isolated, 46 = full). `first` (consecutive tiles) or `tiles` (explicit list); `with`: other terrains that count as the same. Out-of-map neighbours count as the same |
| `under` | terrain whose base is drawn below the transparent edge pieces |
| `depleted` | view only: shown once the cell's resource field is used up |
| `color` | palette colour (editor) |

### Map `tiles`

```json
"tiles": {"tileset": "tilesets/desert.json", "rows": ["sssrrr", "..."], "fixed": [[x, y, tile]], "seed": 0}
```

When a map with `tiles` is loaded (game, headless, editor play), the tile terrains' gameplay is
written into `terrain.rows`, `terrain.levels` and a `fields` entry (`"fromTiles": true`) before the
simulation starts — deterministic and idempotent; the simulation itself knows nothing about tiles.
The editor also bakes it while painting, so saved maps carry valid terrain anyway. `fixed` places
exact atlas tiles (autotile off). Visuals: autotile per cell, at most two quads per cell (`under` +
the tile), one static mesh.

Editor: window **Тайлы** (palette, brush, autotile on/off, atlas picker; paints in the Scene view).
Agents: `tileset_list`, `tiles_init {tileset, fill | fromTerrain}`, `tile_paint {terrain, x, y, radius | rect, tile?}`,
`tile_info {x, y}`, `tiles_remove`.
