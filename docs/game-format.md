# Game data format (RTSEngine)

A game is a folder `games/<id>/`:

```
game.json        manifest: name, id, version, rules file, maps list, orientation, icon, input
rules.json       resources, types (units/structures/objects), techs, factions, game rules, triggers
maps/*.json      terrain, players, objects, waves, triggers, areas, victory overrides
audio.json       sounds, music and which sound plays on which game event (optional: no file = silent)
models/ ui/ ...  assets (glTF models, textures, UI, sounds); 2D images may be packed into atlases (see Texture atlases)
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
| `fogOfWar` | `{"mode": "full"}` — fog of war: `full` (default: unexplored black, explored dim), `explored` (the whole map starts explored, enemies only in sight), `none` (no fog, everyone sees everything); `shareAllies` (true). A map may override `mode`. The other keys are visual, see "Fog of war look" |
| `heights` | `{"blockVision": true, "missUphill": 0.3, "rangePerLevel": 1, "damagePerLevel": 0.1}` — cliff levels: lower ground can't see up, shots uphill can miss, higher ground adds range and damage. With `terrain.model: "heightfield"` the advantage is the height difference / `levelHeight` (fractional), `missUphill` scales with it (full chance from one level below) and vision follows real line of sight |
| `terrain` | `{"model": "heightfield", "maxSlope": {"ground": 0.8, "sand": 0.6}, "buildSlope": 0.35, "uphillSpeed": 0.5, "downhillSpeed": 0.2, "eyeHeight": 1, "targetHeight": 0.5}` — terrain model, see "Heightfield terrain". `model`: `"levels"` (default: cliff levels + ramps), `"flat"` (levels ignored), `"heightfield"`. Other keys of this block are visual (view) and ignored by the simulation |
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

`"move": {"speed": 2.8, "radius": 0.4, "mass": 0.64, "layer": "ground", "maxSlope": 1.6}`

- `maxSlope` (heightfield terrain only): the steepest cell this type can enter (rise per cell); default
  `terrain.maxSlope` of its layer. At most 15 different values per game.

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
| `view` | presentation only: shape or model, materials, clips, procedural animation (docs/models.md) |
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
`target`: none | self | unit | point (and structure | group | player, see "Abilities and effects").
`affects`: enemies | allies | all. Autocast abilities are also what the AI uses. Delivery (projectiles,
areas, chains, channels), statuses, costs, charges, cast times and level tables: "Abilities and effects".

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
    "heights": [0, 0, 16, ...],             // optional fine visual height per cell (1/16 units)
    "corners": [0, 0, 16, ...]              // optional heightfield: (w+1)*(h+1) corner heights, 1/16 units, row-major
    // or, for big maps: "cornersFile": "maps/<name>.r16"   raw int16 little-endian, (w+1)*(h+1) values
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
  "spatial": {"minDistance": 10, "maxDistance": 55, "airAbsorption": 1500, "speedOfSound": 150,
              "reverb": {"size": 0.6, "damping": 0.45, "wet": 0.4, "near": 0.04, "far": 0.65}},
  "groups": {"melee": {"max": 6}, "ui": {"max": 3, "bus": "ui", "steal": "oldest"}},
  "sounds": {
    "swing": {"files": ["sfx/swing1.ogg", "sfx/swing2.ogg"], "pitch": [0.9, 1.1], "maxInstances": 4, "cooldown": 0.05, "group": "melee"},
    "alarm": {"file": "voice/under_attack.ogg", "bus": "voice", "priority": 10},
    "blip":  {"synth": {"wave": "square", "freq": [880, 440], "duration": 0.08}, "bus": "ui"},
    "boom":  {"file": "sfx/boom.ogg", "minDistance": 14, "maxDistance": 80, "far": {"sound": "boom_far", "from": 0.25}},
    "boom_far": {"file": "sfx/boom_far.ogg", "maxDistance": 120, "air": 0.5}
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

### spatial — 3D sound and distance

Positional sounds are placed in 3D around the listener (the RTS client puts the ear on the view
ray above the camera's focus; units on cliffs and flyers are higher). Distances are world units
(map cells). Zooming out raises the ear, so the battle sounds farther away; the audible area
grows with the square root of the zoom.

| key | meaning |
|---|---|
| `model` | distance curve: `inverse` (default, natural), `linear`, `exponential` |
| `minDistance`, `maxDistance` | full volume up to min, silent at max (12, 60); the last 20 % of the range always fades out |
| `rolloff` | steepness of the curve (1) |
| `airAbsorption` | Hz of the low-pass at max distance: far sounds lose their highs (2500; `false` = off) |
| `reverb` | `false`, or `{"size": 0.5, "damping": 0.5, "wet": 0.35, "near": 0.05, "far": 0.6}` — `near` / `far` = reverb send at min / max distance: far sounds get roomy and lose their attack |
| `speedOfSound` | world units per second; distant sounds arrive late (0 = off; capped at 3 s) |
| `rear` | 0..1: sources behind the listener get duller and a bit quieter (0) |

The distance keys (`model`, `minDistance`, `maxDistance`, `rolloff`) are defaults for every sound;
a sound can override them and add `air` / `reverb` multipliers (below).

### sounds

| key | meaning |
|---|---|
| `file` / `files` / `synth` | exactly one. Files: WAV, OGG Vorbis, MP3, FLAC, paths relative to the game folder; `files` = variations, a random one per play, never the same twice in a row |
| `bus` | `sfx` (default), `ambient`, `voice`, `ui`, `music` |
| `spatial` | attenuated and panned around the camera when the event has a position; default true for `sfx` / `ambient`, false otherwise (announcements are heard anywhere) |
| `volume` | 0..4 (1) |
| `pitch` | number or `[min, max]` random per play (1) |
| `maxInstances` | at most this many of this sound at once (4; 0 = no limit); `steal` decides who makes room |
| `steal` | for `maxInstances`: `farthest` (default), `oldest`, `quietest`, `none` — see "groups" |
| `group` | a group name or an array of up to 4: limits shared with other sounds (see "groups") |
| `cooldown` | seconds between two starts (0) — 40 archers firing together make a few sounds, not 40 |
| `priority` | -100..100 (0): when all voices are busy, the lowest priority (oldest first) is stopped; a new sound below everything playing is dropped |
| `loop` | loops until stopped (false) |
| `model`, `minDistance`, `maxDistance`, `rolloff` | override the `spatial` defaults: small sounds (footsteps, swords) fade early, explosions carry far |
| `air` | multiplier of the air absorption, 0..4 (1; 0 = keeps its highs at any distance) |
| `reverb` | multiplier of the distance reverb send, 0..4 (1; 0 = always dry) |
| `duckMusic` | 0..1: music is lowered by this fraction while the sound plays (0.4 on the `voice` bus, else 0) |
| `sync` | `now` (default), `beat`, `bar`: start on the music's grid (stingers) |
| `compress`, `quality` | build-time codec for this sound's files (see "Compression at build time") |
| `far` | a distant variant: `"boom_far"` or `{"sound": "boom_far", "from": 0.3}` — both play together and crossfade from `from` (fraction of this sound's distance range) to max distance; the far sound keeps its own distances, so it can carry farther (a low rumble instead of a crack) |

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

### groups

Limits shared by several sounds, so a big battle stays readable: six swords hitting at once
sound like a fight, sixty sound like noise, and they hide the sounds that matter.

```json
"groups": {
  "melee":      {"max": 6},
  "missiles":   {"max": 6, "cooldown": 0.02},
  "explosions": {"max": 4},
  "deaths":     {"max": 4, "steal": "quietest"},
  "ambient":    {"max": 5, "bus": "ambient"}
}
```

| key | meaning |
|---|---|
| `max` | sounds of the group at once (0 = no limit, only `cooldown`) |
| `cooldown` | seconds between two starts of any sound of the group (0) |
| `steal` | who makes room when the group is full (below; default `farthest`) |
| `bus` | every sound on that bus belongs to the group: a limit per bus |

Sounds join with `"group": "melee"` (or `["melee", "loud"]`: every group must have room).
Variations (`files`) are one sound; a sound with a `far` variant counts once, and its far half
stops with it.

| `steal` | when full |
|---|---|
| `farthest` | the sound farthest from the listener stops; a new sound farther than all playing ones is not started — fighting off-screen never cuts off the fight on screen. Non-positional sounds: the oldest |
| `oldest` | the sound started first stops |
| `quietest` | the sound that is quietest now stops (content loudness measured at load, volume, bus, distance); a new sound quieter than all of them is not started |
| `none` | nothing stops: the new sound is not started |

A playing sound with a higher `priority` than the new one is never stopped for it; when the
whole group is more important, the new sound is not started. Stopping fades in a few ms (no click).

### music

Music streams from the files (OGG Vorbis recommended); changes crossfade. Playlists the client
plays by name, all optional: `menu`, `match`, `battle` (replaces `match` while heavy fighting
happens near the camera, back after ~15 s of calm), `victory`, `defeat`. A playlist named
`match.<map>` (map file name without extension, e.g. `match.valley`) wins over `match` on that
map; the same for `battle`, `victory`, `defeat`.

```json
"music": {
  "menu":  "music/menu.ogg",
  "match": {"files": ["music/a.ogg", {"file": "music/b.ogg", "loopStart": 12.0}], "shuffle": true,
            "bpm": 96, "beatsPerBar": 4, "transition": "bar"},
  "match.valley": {"layers": [{"file": "music/valley_base.ogg"},
                              {"file": "music/valley_drums.ogg", "intensity": 0.3},
                              {"file": "music/valley_brass.ogg", "intensity": 0.7}],
                   "loopStart": 4.0, "loopEnd": 68.0, "bpm": 112}
}
```

A playlist is a file name, an array of entries, or an object:

| key | meaning |
|---|---|
| `files` | entries played in order: a file name or `{"file", "loopStart", "loopEnd"}` (seconds). With `loopStart` the track plays its intro once and then loops from `loopStart` to `loopEnd` (default: the end) seamlessly until the music changes |
| `layers` | adaptive music instead of `files`: synchronized stems of equal length, looping together (`loopStart` / `loopEnd` on the playlist). A layer with `"intensity": x` fades in as the music intensity passes x; in a match the intensity follows the fighting near the camera, so a layered `match` builds up instead of switching to `battle` |
| `shuffle`, `loop` | shuffle the entries (no immediate repeats) / start over after the last one (false, true) |
| `volume` | 0..4 (1) |
| `bpm`, `beatsPerBar`, `offset` | beat grid (`offset`: seconds before the first beat); used by `transition` and by synced stingers |
| `transition` | switching away from this playlist waits for the next `beat` or `bar` (default `bar` when `bpm` is set, else `now`) |

Stingers are sounds on the `music` bus: `"sync": "beat"` or `"bar"` starts them on the playing
music's grid. Any sound can duck the music while it plays: `"duckMusic": 0.5` lowers it by half
(default 0.4 for the `voice` bus, so announcements are always heard; 0 otherwise).

### Compression at build time

`rtsbuild` re-encodes the referenced sound and music files for each target before packing; the
project folder is never changed. Profiles: `desktop` (macOS, Windows, Linux), `mobile` (iOS,
Android), `web`. `--audio mobile` forces a profile, `--audio off` ships the files as they are;
`rts_audiopack --project <game> --profile mobile` shows what a profile would do.

```json
"build": {
  "desktop": {"sounds": "keep", "music": "vorbis", "musicQuality": 0.6},
  "mobile":  {"sounds": "adpcm", "music": "vorbis", "musicQuality": 0.3, "maxRate": 32000},
  "web":     {"musicQuality": 0.25}
}
```

| key | meaning |
|---|---|
| `sounds`, `music` | codec: `keep` (file as is), `pcm` (16-bit WAV), `adpcm` (IMA ADPCM WAV: 4:1, nearly free to decode — short effects), `vorbis` (OGG: music, long sounds) |
| `soundQuality`, `musicQuality` | Vorbis quality -0.1..1 (0.5 / 0.4; mobile 0.4 / 0.3, web 0.4 / 0.25) |
| `maxRate` | resample anything above this rate (0 = never; mobile and web 32000) |
| `monoSounds` | positional sounds become mono: the mixer pans them anyway (true) |
| `longSound` | seconds: longer sounds (ambient loops, stingers) use the music codec (8) |
| `reencodeLossy` | also re-encode files that are already OGG/MP3 (false: lossy to lossy only loses quality) |

Defaults without a `build` section: `desktop` keeps everything; `mobile` and `web` use ADPCM for
sounds and Vorbis for music at 32 kHz. Sounds on the `music` bus count as music. A sound or a
playlist can override with `"compress"` and `"quality"`. Re-encoded files get the extension of
their codec and audio.json in the build points at them; a result that isn't smaller keeps the
original. Loop points stay exact (Vorbis is sample-accurate; ADPCM pads a loop with its own start).

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

## Terrain overlays (visual `terrain` block)

Presentation only (`modules/rts/view/RtsGroundFx.*`): the view reads the territory, dig, room and road layers and
the lanes and never writes them. All keys are optional; put them under `terrain` in rules.json or game.json (game.json
wins per block; sim keys like `model` live in the same block). Each block may be `false` to switch the overlay off.
Colours are `[r, g, b]` (0..1) or `"#rrggbb"`. `texture` paths go through `TextureCache::resolve` (atlases work);
the texture is mapped in world space every `textureScale` units and mixed by `textureStrength` (0..1).

```json
"terrain": {
  "territory": {"fill": 0.28, "border": 0.26, "glow": 0.85, "pulse": true, "colors": [[0.2,0.45,0.95], "#e83a33"],
                "rooms": {"lair": [0.62,0.32,0.22]}, "roomFill": 0.5, "texture": "terrain/floor.png", "textureScale": 2},
  "dig":       {"height": 1.15, "mark": [1,0.8,0.22], "progress": true,
                "kinds": {"D": {"rock": [0.4,0.35,0.3]}, "G": {"style": "vein", "vein": "#f9c238"}, "J": {"style": "gem", "vein": [0.35,0.85,0.95], "glow": 0.55}}},
  "lanes":     {"color": [0.74,0.63,0.44], "edge": [0.5,0.4,0.27], "alpha": 0.75, "width": 0, "sideLanes": false, "all": false},
  "roads":     {"color": [0.72,0.63,0.48], "edge": [0.4,0.33,0.24], "width": 0.7, "alpha": 0.96, "cobbles": true, "texture": ""}
}
```

| Block | What is drawn | Keys |
|---|---|---|
| `territory` | owner colour on owned cells, a glowing (pulsing on high quality) border along edges between owners, painted rooms as a floor tint | `fill` alpha, `border` width (cells), `glow` emissive, `pulse`, `colors` per player slot from 1 (default: player colours), `rooms` tint per room id, `roomFill` alpha, `texture*` |
| `dig` | diggable cells as raised rock blocks with walls (the terrain under them stays flat and shows when dug); `style` per diggable character: `plain`, `vein` (gold-tinted top, nuggets, streaks on walls), `gem` (glowing crystals). Default style: no `yields` = plain, `yields` = vein, `infinite` = gem. The local player's dig marks (framed highlight) and digging progress (cracks + bar) | `height` 0.2..1.25, `mark` colour, `progress`, `kinds.<char>`: `rock`, `vein`, `style`, `glow` |
| `lanes` | lanes (`lanes` rules) as one soft path ribbon per pair of neighbouring buildings (a pair is skipped when a third building lies between them), map `roads` between buildings as ribbons | `color`, `edge`, `alpha`, `width` (0: covers every lane of the pair), `sideLanes` thin tracks per lane, `all` draw every pair |
| `roads` | road cells (map `+`, `roadNetwork` building) as road surfaces with edges, joins, crossings, rounded corners and dead ends; cobbles on high quality | `color`, `edge`, `width` 0.2..1, `alpha`, `cobbles`, `texture*` |

What the viewer sees follows fog of war: territory, rooms, rock and roads are updated only where the cell is visible
(remembered elsewhere). Weak GPUs (`MaterialQuality::Low`; testing: `RTS_GROUNDFX=low`) get no border pulse, no
vein/gem geometry (a tint instead), no rock bevels, no road edges, cobbles or lane tracks. 2D tile maps are not
overlaid.

## Player abilities

```json
"playerAbilities": [
  {"id": "nuke", "name": "Ядерный удар", "cost": {"money": 500}, "cooldown": 60, "target": "point",
   "effects": [{"type": "damage", "amount": 200, "radius": 3}]},
  {"id": "drone", "cost": {}, "cooldown": 5, "charges": {"max": 2, "rearmCost": {"money": 100}, "rearmTime": 20},
   "effects": [{"type": "damage", "amount": 60, "radius": 1.5}, {"type": "stun", "duration": 2, "radius": 1.5}]}
]
```
Not tied to a unit. Effects are the ability effects (any effect except `blink`; see "Abilities and effects").
Charges rearm one at a time after `rearmTime`, paying `rearmCost`. The AI fires them at enemy concentrations.

## Abilities and effects

Everything a skill does is a list of **effects** (a *payload*). Effects can carry their own payload
(`"effects": [...]`): a projectile delivers it on impact, an area applies it when it triggers, a chain
applies it on every jump, a status ticks it, a weapon applies it on hit. Skills are compositions of
these primitives; nothing is coded per skill. All keys below are optional; a game that uses none of
them runs exactly as before (bit-identical replays).

### Abilities (unit) and player abilities

```json
{"id": "barrage", "name": "Ракетный залп", "target": "point", "range": 10, "cooldown": 8, "energy": 40,
 "cost": {"gold": 25}, "initialCooldown": 3, "castTime": 0.5, "backswing": 0.3, "charges": 2,
 "requires": ["rocketry"], "level": 1, "autocast": true, "smart": "densestEnemy",
 "delivery": {"type": "projectile", "speed": 10, "arc": true, "arcHeight": 3, "scatter": 1.2},
 "effects": [{"type": "damage", "amount": [20, 30], "radius": 1.5, "falloff": 0.5}],
 "levels": [{"cooldown": 7, "effects": [{"amount": [30, 40]}]}, {"cooldown": 6, "effects": [{"amount": [40, 55]}]}]}
```

| key | meaning |
|---|---|
| `target` | `none`, `self`, `unit`, `point`, `structure` (a structure, e.g. a garrison), `group` (units sent together, on a lane), `player` |
| `cost` | resources (besides `energy`); player abilities use `cost` only |
| `cooldown`, `initialCooldown` | seconds; the `cooldownRate` stat speeds cooldowns up |
| `charges` | unit abilities: N charges, one regenerates per `cooldown` (player abilities: `{"max", "rearmCost", "rearmTime"}`) |
| `castTime`, `backswing` | windup before the effect (event `kEvCastStart`; stun / silence / a new order cancel it) and the time the caster stays busy after; player abilities: `castTime` = activation delay |
| `requires` | types / techs that must exist (abilities granted by a tech tree) |
| `level` | hero level needed; `levels`: a table for levels 2, 3, ... — each entry overrides keys of the ability (effects merge by index). Unit abilities use the hero level, player abilities the loadout level |
| `smart` | target rule for autocast / auto skills / the AI: `auto`, `lowestHpAlly`, `densestEnemy`, `strongestEnemyStructure`, `weakestOwnStructure`, `ownStructureUnderAttack`, `largestEnemyGroup`, `self`, `enemyPlayer`, `debuffedOwn` |
| `delivery` | how the effects get there: `instant` (default), `projectile`, `area` (delayed / persistent), `chain`, `channel`, `line`, `cone` (see below) |
| `levels`, `aura`, `radius` | as before (`aura`: passive buff around the owner) |

Player abilities also take `passive: true` + `status` (always-on player status, e.g. income
bonuses), `autoEvery` (seconds: cast by itself on its smart target — map / timer skills), and
maps give each player a **loadout**: `"players": [{"loadout": ["freeze", {"id": "nuke", "level": 3}]}]`
(without a loadout every player ability is available; `rules.loadoutSlots` caps the count).
Triggers can cast them: `{"type": "castAbility", "ability": "storm", "at": [20, 8], "free": true}`.

### Delivery

| `delivery.type` | keys | |
|---|---|---|
| `projectile` | `speed`, `homing`, `arc` + `arcHeight` (ballistic), `collide` (hits the first enemy on its way), `scatter` | the payload applies at impact (the hit unit is the target); reuses the `Projectile` entity (`effList` / `effIndex` tell the view which ability) |
| `area` | `delay`, `lifetime`, `interval`, `trigger` (`once`, `periodic`, `contact` + `maxTriggers`), `motion` (`static`, `toward`, `lane`, `wander`, `follow`) + `speed`, `sight` (reveal), `teleportTo` [dx, dz] (portal), `deathSpawn` (soul trap) | an `Area` entity (Transform + Area) — artillery markers, ground fields, mines, moving hazards, portals |
| `chain` | `jumps`, `jumpRadius`, `jumpFalloff` | jumps to the nearest unhit target that passes the filter; amounts scale by `jumpFalloff` per jump |
| `channel` | `duration`, `interval` | repeats while the caster stays still; stun / silence / moving cancel it |
| `line`, `cone` | `length`, `width` / `angle` | shapes the effects that have no shape of their own |

The same keys work inside effects of type `projectile`, `area` and `chain`, which is how skills nest
(a projectile that leaves a burning area, an area that drops projectiles every second, ...).

### Effects

Common keys: `amount` (number or `[min, max]` rolled with the sim RNG), `percent`, `radius`, `duration`,
`chance`, `damageType`, `falloff` (splash: damage at the edge = 1 - falloff), and the **target filter**:

| filter key | meaning |
|---|---|
| `affects` | `enemies` (default for harmful), `allies` (default for helpful), `own`, `self`, `all`, `any` (+ neutral) |
| `shape` | `target`, `circle` (default with a `radius`), `line`, `cone`, `ring` (+ `ring` width), `map`, `group` |
| `layers` | `["ground", "air", "naval"]` |
| `kind` | `unit`, `structure`, `any`; `garrisons: true` (only garrison structures); `inTransit: true` (only units walking to a structure) |
| `tags`, `ofType`, `level` | unit tags, one type, structure level range `[min, max]` (types have `"level": n`) |
| `sort`, `maxTargets`, `ownFirst` | `nearest`, `farthest`, `lowestHp`, `highestHp`, `levelLow`, `levelHigh`, `garrisonLow`, `garrisonHigh`, `random`; take the first N |
| `area` | only inside a named map area |

| `type` | keys | what it does |
|---|---|---|
| `damage`, `heal`, `stun`, `buff`, `spawn`, `blink` | as before; `spawn` also `limit` (alive per caster), `owner` (`caster`, `hostile`, `neutral`) | |
| `status` | `status` (id or inline object), `duration` | applies a status (below) |
| `dispel` | `dispel` (status tags, empty = all), `from` (`enemies` = cleanse, `allies` = purge, `any`) | removes statuses (and reverts temporary captures / transforms) |
| `knockback` | `amount` (negative = pull), `fromCaster` | pushes units away from the point (lane walkers are thrown back on their lane) |
| `teleport` | `mode` (`targets` / `caster`) | moves units to the point (lane walkers continue from the nearest lane point) |
| `resource` | `resource`, `amount` (+ give / − take), `percent` (of the stock), `transfer` | gain, drain, steal (players chosen by the filter / the target's owner) |
| `energyBurn` | `amount`, `percent` (damage per energy burned) | |
| `reveal` | `radius`, `duration` | vision for the caster's side |
| `kill` | `percent` | kills (a share of) the targets |
| `area`, `projectile`, `chain`, `cast` | delivery keys + `effects` | nest payloads (`cast` just applies its effects, e.g. with a `chance`) |
| `garrisonKill`, `garrisonPanic` | `amount`, `percent` | units in the garrison die / flee |
| `garrisonAdd` | `amount`, `percent`, `ofLost` | reinforcements, resurrection (a share of the units lost) |
| `garrisonConvert` | `percent` | defectors walk to the caster's nearest structure |
| `capture` | `duration`, `keepChance` | takes the structure; with a duration it reverts unless recaptured |
| `swap` | | swaps owners and garrisons with the caster's nearest structure (or the nearest enemy one when cast on an own structure) |
| `transform` | `to`, `duration`, `keepChance` | becomes another type for a while (cast tower) |
| `level` | `delta` (+ up / − down the `upgrades` chain), `duration` | |
| `equalize` | | averages the garrisons of the target owner's structures |
| `link`, `redirect` | `duration` | linked structures share units; production goes to the caster's nearest other structure |
| `sendBack` | `mode` (`home`, `reverse`, `throwBack` + `amount` cells) | groups in transit go back / are thrown back |
| `clone` | `percent`, `fake` | copies of units in transit (illusions die on any hit and do nothing on arrival) |
| `formation` | | a group closes rows onto its main lane |
| `convert` | `percent` | units change sides (hypnosis, betrayal) |
| `playerStatus` | `status`, `duration` | a status on players (silence, freeze regen, wind, alerts) |
| `equalizeResource` | `resource` | averages a resource between the chosen players |
| `sacrifice` | `percent`, `gain` | kills own units / garrison for resources per unit |
| `stealSkill` | `duration`, `transfer` | takes a random skill the enemy has and you don't |
| `cooldown` | `amount` | seconds added to (or taken from) the players' skill cooldowns |

### Statuses

```json
"statuses": {
  "poison": {"name": "Яд", "tags": ["debuff", "magic"], "tick": {"damage": 4, "interval": 1}, "stacking": "stack", "maxStacks": 5},
  "frost": {"tags": ["debuff", "magic"], "mods": [{"stat": "speed", "mul": 0.6}, {"stat": "attackSpeed", "mul": 0.8}]},
  "aegis": {"tags": ["buff"], "shield": 120, "view": {"color": [0.4, 0.7, 1]}}
}
```

| key | meaning |
|---|---|
| `tags` | status tags for `dispel` (`buff`, `debuff`, `magic`, any word) |
| `flags` | `stun`, `silence`, `disarm`, `root`, `invulnerable`, `invisible`, `lock` (no sending / production), `blind`, `hidden` (garrison counts hidden), `flying` (off the lane, straight to the goal), `fake`, `ignoreDefense`, `spreads` (sent units carry it into the structure they enter — plague), `freezeRegen`, `alert`, `revealed` |
| `mods` | stat modifiers per stack (`stat`, `add`, `mul`) — on players they apply to all their objects (wind, global bonuses) |
| `tick` | `damage` / `heal` per stack, `interval`, `effects` (a payload around the holder: kill aura, burning) |
| `shield` | absorbs damage (or assaulting units on a garrison); the status ends when it breaks |
| `stacking`, `maxStacks` | `refresh` (default), `stack`, `independent` |
| `onExpire`, `onCaptured`, `onHit` | payloads: when it runs out (two-stage skills), when the structure is captured (revenge), on every weapon hit of the holder (enchanted weapons) |
| `incomeMult` | player statuses: `{"mana": 2}` multiplies income + regen |
| `garrisonPerSecond` | structure: units per second (negative: plague) |

Types can carry permanent statuses (`"statuses": ["thorns"]`). Weapons take `"onHit": {"chance": 0.3,
"effects": [...]}` (or a plain effect list) and `"chargeTime"` (charging shot: must stay on one target).
New stats: `energyRegen`, `defense` (garrison defense multiplier), `garrisonCap`, `cooldownRate`,
`upgradeCost`, `lifesteal`, `damageTaken`, `scale` (view). Resources take `"regen"` (per second for every
player: mana, morale).

For the view: `Statuses` (per object: `list` of `StatusInst` with `def` = index into `Rules::statuses`,
`stacks`, `until`, `shield`), player statuses in `PlayerEconomy::statuses`, `Area` entities, ability
`Projectile`s (`effList`), `GroupTag` (group id of units sent together). Events: `kEvStatusStart` /
`kEvStatusEnd`, `kEvAreaStart` / `kEvAreaTrigger` / `kEvAreaEnd`, `kEvProjectileLaunch`, `kEvImpact`,
`kEvChainJump`, `kEvCastStart`, `kEvChannelStart` / `kEvChannelEnd`, `kEvAlert`, `kEvGarrisonChanged`.

The AI casts every skill through the same smart targets (heals on the most wounded ally, area damage on the
densest enemy group, structure skills on the strongest enemy / the own structure under attack, group
skills on the largest group in transit, dispels on debuffed own objects), keeps 20% of a capped resource in
reserve, and leaves `autoEvery` skills to fire by themselves.

### Recipes

| skill | data |
|---|---|
| Rocket barrage | `"delivery": {"type": "projectile", "arc": true, "scatter": 1.5}` + `damage` with `radius` + `falloff`; `charges: 3` |
| Frost arrow | weapon `"onHit": {"effects": [{"type": "status", "status": "frost", "duration": 3}]}` (frost: `mods` speed × 0.6) |
| Heal over time | `{"type": "status", "status": {"tick": {"heal": 10, "interval": 1}, "tags": ["buff"]}, "duration": 5, "affects": "allies"}` |
| Bloodlust | `{"type": "status", "status": {"tags": ["buff", "magic"], "mods": [{"stat": "attackSpeed", "mul": 1.4}, {"stat": "speed", "mul": 1.25}, {"stat": "scale", "mul": 1.2}]}, "duration": 20, "affects": "allies"}` |
| Chain lightning | `"delivery": {"type": "chain", "jumps": 4, "jumpRadius": 5, "jumpFalloff": 0.75}` + `damage` |
| Shield | `status` with `"shield": 150` on an ally |
| Knockback stomp | `"target": "none"`, `[{"type": "knockback", "amount": 3, "radius": 3}, {"type": "stun", "duration": 1, "radius": 3}]` |
| Poison | status with `tick.damage`, `"stacking": "stack"` — from an ability or a weapon `onHit` |
| Artillery strike | `"delivery": {"type": "area", "delay": 2.5, "radius": 3}` (marker shows during the delay) |
| Summon wolves | `{"type": "spawn", "unit": "wolf", "count": 2, "duration": 60, "limit": 4}` |

#### Bunker Wars skills as primitives

Garrison structures, groups and lanes come from `garrison`, `kCmdSend` and `lanes`. `structure` / `group` /
`player` are player-ability targets.

| skill | data |
|---|---|
| Hypnosis | group: `convert` `percent` 0.5 (`shape: group`) |
| Defector | structure: `garrisonConvert` `percent` 0.3 |
| Plague | structure: status `{"flags": ["spreads"], "garrisonPerSecond": -1}` |
| Freeze | group: status `stun` (`shape: group`) |
| FreezeWave | group: `area` `motion: lane`, `trigger: periodic`, effects: status `stun` in a radius |
| Shield | own structure: status `shield` N (absorbs N assaulting units) |
| Invulnerability | own structure: status `invulnerable` |
| Teleport | group: `teleport` (`shape: group`) to the clicked point |
| ShortWay | point: `area` `trigger: contact`, `teleportTo` [dx, dz], `affects: own`, `lifetime` |
| Barrier | point: `spawn` a static unit type with `blocksLanes` and hp (a non-structure, so lanes don't route around it) |
| Mines | point: `area` `trigger: contact`, `maxTriggers` N, effects `kill` / `damage` |
| Tornado / SandStorm | `area` `motion: lane` (along a group's lane) or `wander` / `toward`, `trigger: periodic`, effects `kill` in a radius |
| BlizzardRain | `area` `trigger: periodic`, effects: `projectile` with `scatter` and `damage` |
| ChainLightning | `chain` with `kind: structure` + `garrisonKill` (over structures) or `damage` (over units) |
| SkeletonArmy | point: `area` with `deathSpawn` (units dying inside rise for the caster) |
| Rage | group: status `mods` damage / speed / `scale` up |
| Reverse / GoHome | group: `sendBack` `mode: reverse` / `home` |
| ThrowBack | group: `sendBack` `mode: throwBack`, `amount` cells |
| Panic | structure: `garrisonPanic` `percent` |
| Sabotage | structure: status `lock` + `disarm` (no sending, no production, towers silent) |
| Swap | structure: `swap` |
| Possession | structure: `capture` with `duration` (reverts unless recaptured) |
| CastTower / CastBuilding | structure: `transform` `to` + `duration`, `keepChance` |
| Downgrade / Upgrade | structure: `level` `delta` −1 / +1, `duration` |
| IncreaseProduction | own structure: status `mods` `productionSpeed` × 2 (and/or `garrisonCap`) |
| RedirectProduction | own structure: `redirect` `duration` |
| Landing | structure: `garrisonAdd` `amount` |
| Resurrection | own structure: `garrisonAdd` `percent` + `ofLost` |
| Spy | point: `reveal`; FogOfWar: own structures status `hidden` / `invisible` |
| Silence | player: `playerStatus` with `silence` |
| StealSkill | player: `stealSkill` `duration` |
| BreakAllMagic | none: `dispel` `shape: map`, `affects: any` (statuses on objects and players) |
| MoraleThief | player: `resource` `morale` `amount` −N, `transfer: true` |
| Sacrifice | own structure: `sacrifice` `percent`, `gain` |
| CloseRows | own group: `formation` |
| FlyingWave / Landing by air | own group: status `flying` |
| Worm | `spawn` with `owner: hostile` of a creature whose weapon `onHit` does `garrisonKill` |
| KillAura | own group: status with `tick.effects` `damage`/`kill` in a radius |
| BFG9000 | point: `projectile` (`collide`) delivering `garrisonKill` / `kill` in a large `radius` with `falloff` |
| Bomb | structure: `garrisonKill` `percent` |
| Average units | own structure: `equalize`; average morale: `equalizeResource` |
| Link | own structure: `link` `duration` |
| FastConvert | own structures: status `mods` `upgradeCost` × 0.5 |
| Defense aura | `area` `motion: follow` around a structure, `trigger: periodic`, status `mods` `defense` |
| IgnoreDefense / Suppress | group status `ignoreDefense`; structure status `mods` `defense` × 0.5 |
| Disable / blind towers | structure status `disarm` / `blind` |
| Two-stage slow | status slow with `onExpire`: a weaker slow status |
| Lift and drop | status `stun` (+ `scale` for the view) with `onExpire`: `damage` / `kill` |
| Illusion / fake units | group: `clone` `fake: true`; Clone: `clone` |
| Betrayal | group: `convert` (they keep walking into their former side) |
| Wind | player (all): `playerStatus` with `mods` `speed` |
| Alerts | self: `playerStatus` with `alert` (event `kEvAlert` on enemy sends) |
| Freeze regen / cooldown multiplier | `playerStatus` with `freezeRegen` / `mods` `cooldownRate` |
| Enhance weapon / charging shot | tower status `mods` damage / attackSpeed / range, or `onHit`; weapon `chargeTime` |
| Random N structures | `shape: map`, `kind: structure`, `sort: random`, `maxTargets: N` |
| Auto / map skills | `autoEvery`, or a trigger action `castAbility` |
| Passive skills | `passive: true` + `status` (in the loadout), or `requires` a tech |

Still needs code: **flying towers moving between points** (garrisons are fixed structures; a mobile
garrison would need garrisons on units).

## Commands (for tools and network code)

Besides the unit commands (`RtsModule.h`): `kCmdSurrender` (the issuing player is defeated, event value 1),
`kCmdDig` / `kCmdPaintRoom` (point = first cell, `param = rectParam(w, h, extra)`: dig `extra` 0 mark / 1 unmark;
room `extra` = room index or -1 erase), `kCmdPickUp` (units), `kCmdDrop` (point; units or `param` = count),
`kCmdSlap` (units), `kCmdPossess` (units[0]; empty = release), `kCmdDirect` (point = direction, `param` bit 0 = fire,
optional target), `kCmdPlayerAbility` (`param` = index, point), `kCmdBuildRoad` (rect, `extra` 1 = remove),
`kCmdConnectRoad` (units = own structures). `kCmdPlayerAbility` with a `structure` / `group` / `player`
ability takes the clicked object in `target` (or the nearest one to `point`).

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

## Texture atlases (game.json `atlas`, optional)

When a game is packaged (`rtsbuild`, `assetpipe pack --atlas <profile>`), small 2D images are
packed into atlas pages: fewer texture switches/draw calls, trimmed transparent borders, identical
images stored once, and pages compressed by the pack's zstd. Game code keeps using the original
paths (`units/orc/grunt/walk_0.png`); the lookup is transparent at runtime. Loose-folder builds
(`--no-pack`, editor) use the images as they are. The whole block is optional:

```json
"atlas": {
  "enable": true,                                   // false (or rtsbuild --no-atlas): ship images as is
  "maxPage": { "web": 2048, "android": 2048, "default": 4096 },  // or a single number
  "pageSize": "mul4",          // page sides: multiples of 4 (default) or "pow2"
  "padding": 2,                // empty texels between images
  "extrude": 1,                // edge texels repeated around each image (no filtering seams)
  "trim": true,                // cut fully transparent borders (offsets are kept)
  "maxTextureSize": 0,         // larger images stay standalone; 0 = maxPage / 4
  "mipLevels": 2,              // groups with "mips": padding/alignment safe down to 2^levels
  "pageFormat": "rtex",        // "rtex" (raw RGBA, zstd in the pack, fastest load) or "png"
  "groups": { "units": { "rotate": false, "mips": true }, "tiles": { "trim": false } },
  "files": {                   // glob -> settings; `*` also matches '/'; later rules win
    "ui/splash/*": { "noAtlas": true },
    "units/orcs/*": { "group": "orcs" },
    "textures/water.png": { "wrap": "repeat" },
    "units/*/shadow.png": { "kind": "mask" }
  }
}
```

Profiles are rtsbuild targets (`macos`, `ios`, `android`, `web`, `windows`, `linux`); built-in
page limits: 2048 for `web`/`android`, 4096 otherwise. Targets with equal settings share one pack.

**What goes together.** An image's group key is *usage group* + *kind* (+ opaque/alpha when the
group splits by alpha, + mips). Images only share a page with the same key:

| usage group | default folders (first path component, `engine/` prefix ignored) | defaults |
|---|---|---|
| `ui` | `ui/`, `icons/`, `portraits/`, `cursors/`, `hud/` | trim |
| `units` | `sprites/`, `units/`, `buildings/`, `structures/`, `characters/` | trim |
| `tiles` | `tiles/`, `tilesets/`, `terrain/` | no trim (sheet grids stay intact), opaque and alpha tiles on separate pages |
| `effects` | `effects/`, `fx/`, `vfx/`, `particles/` | trim |
| any name | set with `"group"` (or `"atlasGroup"`) in `files` | as `units` |

Other folders (`models/`, `textures/`, ...) are not atlased unless a `files` rule names a group:
3D meshes use UVs outside 0..1 and wrap. *Kind* is `color`, `normal` (`*_n`, `*_nrm`, `*_normal`,
`normals/`) or `mask` (`*_mask`) — normal maps and masks are never mixed with colour and never
trimmed. Never atlased: `noAtlas`, `"wrap": "repeat"` (also `*_repeat`, `*_tiling`, `*_wrap`),
images above `maxTextureSize` (after trimming), and an image that would be alone in its group
(one page = no saved texture switch, only padding). Images referenced as a frame-grid sprite
sheet anywhere in the game's JSON (`"sheet": "sprites/tank.png"`) are never trimmed or rotated, so
frame uvs map linearly onto the atlas region. Inside a group, images of one folder (one unit's
frames, one faction) stay on the same page whenever they fit; pages are shrunk to the smallest size
that holds them. Per-file keys: `group`/`atlasGroup`, `noAtlas`, `wrap`, `kind`, `rotate` (allow a
90° turn, off by default so facings never rotate), `trim`, `mips`, `splitAlpha`, `padding`,
`extrude`; the same keys (except `group`) work per group in `groups`.

**In the pack.** Atlased images are removed; `_atlas/` is reserved and holds the pages
(`_atlas/<group>-<n>.rtex`) and `_atlas/atlas.json`:

```json
{ "version": 1,
  "pages": [ { "file": "_atlas/units-0.rtex", "group": "units", "w": 1016, "h": 864, "alpha": true, "mips": false } ],
  "textures": { "units/orc/grunt/walk_0.png": [0, 3, 3, 30, 38, 33, 29, 96, 96, 0] } }
```

Texture entry: `[page, x, y, w, h, offsetX, offsetY, sourceW, sourceH, rotated]` — integer pixels.
`(x, y, w, h)` is the trimmed content in the page (when `rotated` = 1 it occupies `h x w` turned 90°
clockwise); `(offsetX, offsetY)` is its position in the original `sourceW x sourceH` image, so
pivots given in original-image pixels stay valid. `.rtex`: 24-byte header (`RTEX`, version,
width, height, flags: 1 alpha, 2 mips, 4 left-delta filtered rows) + RGBA8 rows top to bottom,
straight alpha. Output is byte-identical for the same input.

**Runtime API** (`engine/atlas/TextureLibrary.h`, link `rts_atlas`):

```cpp
atlas::TextureLibrary lib(dev, [](const std::string& p, std::string& o) { return platform::readContentFile(p, o); });
const atlas::TextureRef* t = lib.get("units/orc/grunt/walk_0.png");   // nullptr if missing
// quad: t->w x t->h source pixels at (t->offsetX, t->offsetY) from the original image's top-left
// (pivot in original pixels: pos - pivot + offset); UVs t->uv = TL, TR, BR, BL (rotation applied);
// t->texture equal for images on one page -> batch by it; t->opaque -> draw without blending.
```

`atlas::mapUV(*t, px, py, u, v)` maps a point in original-image pixels to the texture (trim and
rotation included) — e.g. frame corners of a sheet. For a renderer that already maps frames via a
rect (`u0 + (u1 - u0) * lu`), the rect of an atlased sheet is `t->uv[0..1]` .. `t->uv[4..5]`.
The 2D sprite/tile renderer's `render::TextureCache::resolve()` already understands atlases:
an atlased image resolves to the page texture + its rect (sprite sheets are never trimmed, so
frame uvs stay exact); a trimmed or rotated image is rebuilt as a standalone texture.
Plain (non-atlased) images come back the same way (own texture, full UVs, no trim). CPU pixels of
an original image: `lib.loadImage(path, img)` or `atlas::ImageSource` (the UI uses it, so RmlUi
`<img src>` and decorators keep working with atlased files).

**Build report.** `rtsbuild` prints atlas statistics and writes `size-report.json` next to
`size-report.txt` (`<out>/<target>/`): `pack` {rawBytes, packBytes, entries, duplicates, top[]} and
`atlas` {enabled, profile, maxPage, candidates, atlased, duplicates, pages, occupancyPercent,
sourceBytes, atlasBytes, storedBytes, bytesSaved, gpuBytesBefore, gpuBytesAfter,
textureBindsBefore, textureBindsAfter, groups[] {group, textures, unique, pages,
occupancyPercent}, excluded[] {path, reason}}. Preview: `assetpipe atlas --profile web -o <dir>
games/<id> engine/assets=engine` writes the pages as PNG plus the manifest and the report.

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

Map size: 4..2048 cells per side. The editor saves `corners` inline up to 256x256 cells and as
`cornersFile` (`maps/<map name>.r16`, next to the map) above that; loaders (game, headless, editor, MCP) read
the file through the content reader, so it also works from packs.

## Terrain materials (visual `terrain` block)

Presentation only (`modules/rts/view/RtsTerrainRender.*`). Optional keys in the same `terrain` block as the overlays
(rules.json or game.json; game.json wins per key). **Without them every game looks exactly as before** (vertex
colours per cell kind, plateaus with sharp cliff walls, ramps, soft colour transitions, apron, one water sheet).

```json
"terrain": {
  "layers": [                                   // up to 4 tiling splat layers (world-space, mipmapped, wrap)
    {"id": "snow",  "texture": "textures/terrain/snow_repeat.png", "normal": "textures/terrain/snow_n_repeat.png", "scale": 6, "tint": [1, 1, 1]},
    {"id": "earth", "texture": "textures/terrain/earth_repeat.png", "scale": 4}
  ],
  "cells": {"grass": {"layer": "snow", "color": [0.9, 0.93, 0.97]},  // layer id, or {layer, color (vertex colour override)}
            "sand": "earth", "ramp": "earth", "rock": "earth", "blocked": "earth", "lakebed": "earth", "shallows": "earth", "dug": "earth"},
  "cliff": {"texture": "textures/terrain/rock_repeat.png", "normal": "textures/terrain/rock_n_repeat.png",
            "scale": 3, "tint": [1, 1, 1], "slope": [0.3, 0.55]},     // blended in where 1 - normal.y is in that range
  "splatStrength": 1,                           // 0..1: mix of the textures over the vertex colours
  "water": {"mode": "auto", "color": [0.22, 0.45, 0.62, 0.78], "offset": -0.12},
  "lod": "auto", "lodRange": 128                // heightfield maps: CDLOD on above 256 cells per side ("on"/"off")
}
```

| key | meaning |
|---|---|
| `layers` | splat layers (≤ 4): `texture` albedo (sRGB), optional `normal` (tangent-space, +Y up), `scale` world units per repeat, `tint`. Loaded with `TextureCache::resolve(path, …, tiling)`: **never taken from an atlas page** (name them `*_repeat.png` so the packer skips them too) |
| `cells` | cell kind → layer id or index; kinds: `grass` (.), `sand` (, no-build), `ramp` (/), `rock` (R), `blocked` (#), `lakebed` (~), `shallows` (=), `dug` (diggable rock floor). `color` overrides the vertex colour of the kind on every tier |
| `cliff` | texture on cliff walls and steep slopes (Medium: one projection, High: triplanar + normal map) |
| `water.mode` | `sheet` = one plane under the whole map (classic, default for cliff-level maps), `bodies` = each connected water area gets its own surface just below its lowest shore (lakes on plateaus, valleys of heightfield maps; default for heightfield maps), `auto` |
| `lod`, `lodRange` | big heightfield maps: chunks drawn with 2^k cell steps (k ≤ 4) by camera distance, geomorphing in the vertex shader, skirts against cracks |

**Map key `terrainMaterial`** (optional, visual): rows of `0`..`3` (layer index) per cell, other characters = by cell
kind. Painted with the editor's **Рельеф** window or `terrain_material`.

**Quality tiers** (pause menu *Ground textures*, `Renderer3D::materialQuality()`, capped by the device default;
`RTS_TERRAIN=low|medium|high` overrides for testing): Low = vertex colours (weak Mali / Adreno / PowerVR, also every
game without `layers`), Medium = splat textures + cliff texture, High = + normal maps and triplanar cliffs. Shaders:
`modules/rts/view/RtsTerrainShaders.h` (GLSL + MSL, sun + soft shadows + point lights + clouds as the lit shader).

**Performance**: the terrain is cut into 32x32-cell chunks, frustum-culled; the editor rebuilds only chunks whose cells
changed; during a match nothing is rebuilt: resource fields, creep and (heightfield maps) fog of war are read by the
shader from small per-cell textures. `RTS_TERRAIN_STATS=1` prints chunks / triangles / frame time. Sample:
`games/frostcrown` (generated CC0 textures, `tools/terrain/make_textures.py`).

**Editor / MCP** (heightfield maps, `apps/editor/core/TerrainOps.cpp`, window **Рельеф**):
`terrain_sculpt {op: raise|lower|smooth|flatten|noise|set, x, y | points, radius, strength, seed}` (the first sculpt
creates corners from the visible surface: cliff levels, ramps, fine heights), `terrain_heights {points | grid}`,
`terrain_material {layer, x, y, radius}`, `heightmap_export {file, scale?}` / `heightmap_import {file, scale, offset}`
(16-bit grayscale PNG, `(w+1) x (h+1)` pixels, height = offset + value / 65535 * scale). Corners are visual unless
rules `terrain.model` is `heightfield` (see below).

## Heightfield terrain

`rules.terrain.model: "heightfield"` + a map with `terrain.corners` / `cornersFile` (without corners the map
plays like `"levels"`). Corner `(x, y)` is the top-left corner of cell `(x, y)`; heights in 1/16 world units.
Everything is integer / fixed point and deterministic.

- **Surface**: ground height at any point is bilinear between the 4 corners of its cell; ground units (and the
  `y` of every mobile unit) stand on it; projectiles start at the shooter's ground + 0.6 and aim at the
  target's ground + 0.4.
- **Slopes**: per cell steepness = max(gradient length, steepest cell edge), in height units per cell.
  A unit can't enter cells steeper than its `move.maxSlope` (or `terrain.maxSlope` of its layer; `sand` for
  `layer: "sand"`); pathfinding, local avoidance and spawning follow the same rule. Cliff/ramp rules
  (`levels`, `/`) are replaced by slopes. Structures need cells no steeper than `terrain.buildSlope`.
- **Speed**: `speed * (1 - uphillSpeed * climb)` going up, `* (1 + downhillSpeed * descent)` going down, where
  climb/descent = height change per cell along the move; clamped to 0.25..2.
- **Vision**: line of sight from `eyeHeight` above the unit's cell to `targetHeight` above each cell in sight
  range; a cell behind a ridge is hidden (`heights.blockVision: false` turns this off, air units see over).
  Targeting only picks enemies in a cell the player sees.
- **Cliff level** of a cell = floor(cell height / `levelHeight`) (0..9), so level-based rules and triggers keep
  working. Range / damage / miss bonuses use the exact height difference (see `heights`).
- **Checksums**: corners and the model are part of the state hash only for heightfield games.
- **Big maps** (256x256 cells and more): long paths use hierarchical pathfinding (16x16 clusters with border
  entrances, refined with A*), built at match start and rebuilt only around structures that block cells.
  `rts_headless --project DIR --bench-paths N` times path queries; `python3 games/_tools/mapgen.py --big 1024
  <scratch>/maps/big.json` writes a 1024x1024 test map (keep it out of `games/`).

MCP: `map_info.heightfield` (`corners`, `min`, `max`, `storage`, `model`), `map_heightfield {"enabled"}` adds a flat
heightfield or removes it, `map_validate` warns when the model is heightfield but the map has no corners and flags
units/structures standing on slopes too steep for them. Sculpting: `terrain_sculpt`, heightmap PNG import/export (see Terrain materials). The view draws the relief from the corners (two triangles per cell, diagonal (0,0)-(1,1)); `RtsView::heightAt` follows the same triangles, so models and overlays sit on the drawn ground.

## Sample games (data only, `games/<id>/`; maps from `games/_tools/mapgen.py`)

| id | like | recipe |
|---|---|---|
| `frostcrown` | Warcraft III | gold mines + pines, burrows (`supply`), `upkeep` tiers, altar hero with `hero` levels, aura and `whirl`, chain lightning (`chain` delivery, `levels`), rejuvenation (HoT status, `charges`, `smart`), bloodlust; hunters with frost-arrow `onHit`, demolisher artillery strike (delayed `area`), creep camps (`neutral.leash`, `bounty`), siege demolisher |
| `starfall` | StarCraft | factions guild (`buildMode: warp`) / swarm (`morph`, larva `charges`, `creep`, `terrain: creep`) / legion (walk), geysers + `structure.on`, plateaus with `levels` + ramps, `heights.missUphill` |
| `ironfront` | Red Alert | `mcv` `deploysTo` yard, sidebar `produces` structures, ore `fields` + `fieldSpawner` mines, ore `convertsTo` credits, `power` capacity + `deficitDisables`, `buildRadius`, trigger: free harvester per refinery |
| `ambersands` | Dune II | structures `terrain: rock`, spice blooms on sand, capped credits + silo `storage`, wind traps, sandworm `neutral.wander` + `move.layer: sand` |
| `metaltide` | Total Annihilation | `flow` metal/energy, `costModel: progressive`, commander `key` + `victory: keyUnits`, extractors on metal spots, rock/wreck reclaim, `terrain.model: heightfield` (ridge that blocks sight, too steep for the commander and hammers - `maxSlope` 0.8 - but climbable by constructors and peewees with `move.maxSlope` 1.6; two gentle saddles), height bonuses via `heights` |
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
| `spellbunkers` | Bunker Wars / Mushroom Wars 2 skills | sporelanes + mana/morale `regen`, named `statuses`, 26 player skills (structure / group / player targets, areas moving along lanes, chains over structures), per-player `loadout` of 16, an `autoEvery` map skill and a `passive` |

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

## Fog of war look

Visual keys of rules.json `"fogOfWar"` (game.json `"fogOfWar"` overrides key by key; editor window
**Туман войны**, or `rules_merge {path: "fogOfWar", value: {...}}` over MCP):

```json
"fogOfWar": {
  "mode": "full",                 // simulation: full | explored | none (see rules table)
  "show": true,                   // false: no fog drawn (enemies outside sight stay hidden)
  "color": [8, 10, 14],           // 0..255 or 0..1
  "unexplored": 0.92, "explored": 0.47,   // opacity over never-seen / seen-before ground
  "style": "smooth",              // smooth | cells | grid (cells + lines) | pattern (dithered edge, classic)
  "softness": 2,                  // blur radius in cells (smooth, pattern)
  "fade": 0.3,                    // seconds for a cell to clear / fog over
  "noise": 0.0,                   // 0..1 cloudy moving edges
  "height": 1.3,                  // sheet height above the ground (covers units and low buildings)
  "grid": {"show": "fog", "color": [255, 255, 255], "alpha": 0.15}   // none | fog | all
}
```

Weak GPUs (shadow quality off/low) draw `smooth` as `cells` without noise. Heightfield maps draw
the fog in the terrain shader (colour, transparency, cells, grid; `pattern` and `noise` are
sheet-only).
