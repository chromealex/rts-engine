# Grid fields

Per-cell layers over the map that the simulation updates with deterministic C++ kernels: fire, fuel, water,
lava, poison gas, creep, influence, noise, heat, pollution... A field is data (rules `"fields"`), its
initial values live in the map, Lua reads and writes it through the `field` table, the view draws it as an
overlay. Gap `G5` of `docs/lua-coverage.md`.

Code: `modules/rts/RtsGridFieldData.*` (format), `RtsGridFields.*` (kernels, effects, hooks, hash, saves),
`RtsGridFieldsApi.cpp` (Lua), `modules/rts/view/RtsFieldView.*` (overlay, GLSL + MSL),
`apps/editor/core/FieldOps.*` + `apps/editor/windows/FieldsWindow.cpp` (editor, MCP). Samples:
`templates/wildfire` (forest fire + creep, data only), `templates/lua_lab` (`scripts/fire.lua`).

## Data

```jsonc
// rules.json
"fields": {
  "fuel": {"type": "int", "min": 0, "max": 100, "view": {"show": false}},
  "fire": {
    "type": "int",                 // "int" (integers) or "fixed" (fx, |v| < 32768, 1/65536 steps)
    "resolution": 1,               // map cells per field cell: 1, 2, 4, 8 (coarse fields are cheaper)
    "default": 0, "min": 0, "max": 100,
    "perPlayer": false,            // true: one layer per player slot (influence, creep)
    "kernels": [
      {"type": "spread", "period": 10, "above": 1, "set": 100, "chance": 0.1, "fuelChance": 0.0175,
       "fuel": {"field": "fuel", "min": 1}},
      {"type": "consume", "period": 10, "fuel": "fuel", "amount": 1, "above": 1, "out": 0}
    ],
    "effects": [
      {"type": "damage", "above": 1, "perSecond": 18, "damageType": "fire", "structures": false},
      {"type": "speed", "above": 1, "mul": 0.7},
      {"type": "blockVision", "above": 80, "height": 2}
    ],
    "view": {"preset": "fire", "particles": {"effect": "fire_smoke", "max": 24}}
  }
},
"types": {
  "hive": {"field": {"creep": {"emit": 60, "radius": 5, "mode": "max", "period": 20}}}   // emitter
}
```

All values (`default`, `min`, `max`, `above`, `set`, `amount`, ...) are in the field's units (integers for
`int` fields, decimals for `fixed`). Shares (`rate`, `percent`, `chance`) are 0..1.

### Kernels

Each kernel runs every `period` ticks (`phase` shifts it), in the listed order, after the emitters.

| kernel | keys | does |
|---|---|---|
| `decay` | `amount`, `percent` | moves every cell toward `default` by `amount` and / or `percent` of the distance |
| `diffuse` | `rate`, `neighbours` 4 / 8 | cell += rate * (average of the neighbours - cell); the map edge reflects (no flux out) |
| `spread` | `above`, `below` (default = above), `set`, `chance`, `fuelChance`, `fuel` {`field`, `min`, `materials` [terrainMaterial layer indices], `terrain` "map chars", `tags` [type tags]}, `neighbours` | cells >= above set neighbours that are < below and have fuel (any listed condition) to max(value, set). `chance` 0 = deterministic threshold spread; else the probability per neighbour and run is chance + fuelChance * fuel value, drawn from `sim.rng()` |
| `flow` | `rate`, `unitsPerHeight`, `minDepth`, `neighbours` | the value is a depth over the terrain (cliff levels or heightfield); each cell sends rate * min(depth, steepest drop / 2) to its lower neighbours in proportion to the drop. Exactly conserving |
| `consume` | `fuel` (field id), `amount`, `above`, `out` | cells >= above eat `amount` of the fuel field per run; when it is used up the cell becomes `out` (fire burns out, the ash never burns again) |

Sources: type emitters (`emit` with `mode` add / max / set over a disc of `radius`; per-player fields write
the owner's layer; `whileBuilding`), the effect `{"type": "field", "field": "fire", "add" | "set" | "max": n,
"radius": r}` in any effect list (abilities, player abilities, weapon `onHit`, areas, statuses), Lua, the
map's initial values.

### Effects (per cell, every `period` ticks, default 10)

| effect | keys | |
|---|---|---|
| `damage` | `perSecond`, `damageType`, `scaled` (x value / above) | units / structures standing in cells >= above |
| `speed` | `mul` | movement speed multiplier (stacks) |
| `blockMove` | | cells >= above are impassable (MapGrid `blocked`: pathing, flow fields, placement) |
| `blockVision` | `height` | a line-of-sight blocker of that height (switches `fogOfWar.lineOfSight` on) |
| `status` | `status`, `seconds` (default: two periods, refreshed while inside) | rules `statuses` |
| `yield` | `resource`, `perSecond`, `scaled` | per-player fields: per owned cell to that player; plain fields: per unit standing there to its owner |
| `reveal` | `all` | fog of war: per-player fields reveal to the layer's owner, plain fields to everyone |

Filters: `tags`, `notTags`, `units`, `structures`; `who` for per-player fields: `any` (highest layer),
`enemies` (highest layer of a player hostile to the unit), `own`.

### Maps

```jsonc
"gridFields": {
  "fuel": {"values": "0*130 18 18 0*25 ..."},          // RLE "v*n v ..." over row-major field cells (layer 0)
  "creep": {"values": {"1": "...", "2": "..."}},       // per-player layers
  "fire": {"fill": [{"circle": [22.5, 10.5, 1.2], "value": 100}, {"rect": [0, 0, 4, 4], "value": 50, "mode": "max"}],
           "kernels": [...]}                            // any definition key overrides the rules field
}
```

A map may add fields of its own (Lua only: effects and emitters of the rules refer to rules fields). The map
key `"fields"` stays the resource-field array; an object there is accepted as an alias of `"gridFields"`.

## Lua

```lua
field.get("fire", x, z [, player])           -- value at a world position (int or fx); field.get_cell(id, cx, cz)
field.set(id, x, z, v [, player]) / field.add(...) / field.set_cell / field.add_cell   -- simulation scripts
field.fill_rect(id, x0, z0, x1, z1, v [, "set" | "add" | "max" [, player]])   -> cells changed
field.fill_circle(id, x, z, r, v [, mode [, player]])
field.sum(id, x0, z0, x1, z1) / field.max(...) -> v, x, z / field.count(id, x0, z0, x1, z1, above)
field.find(id, {above =, below =, x =, z =, radius =, limit =}) / field.find(id, function(v, x, z) ... end, opts)
field.cell(id, x, z) -> cx, cz / field.center(id, cx, cz) -> x, z / field.info(id) / field.ids()
on_field_threshold("fire", 50, function(e) ... end [, "rise" | "fall"])   -- e: field, x, z, cx, cz, value, player
```

Bulk calls return integers / fx only. `find` returns at most `limit` cells (64 by default, 4096 at most) and
examines at most 65536 cells. Threshold hooks see crossings made by kernels, emitters, effects and scripts;
they run in the scripts phase of the same tick (crossings caused by script writes: the next tick); at most
4096 per tick are kept. View scripts get the read-only part of the table.

## Determinism, hash, saves, rollback

Integer math only; kernels visit 16x16 chunks in index order and cells row-major, read the previous values
and write a scratch buffer (double buffering); random spread draws `sim.rng()` in that order. The state is the
`GridFieldStore` world resource (only in games with fields: other games keep their checksums): values, the
cells whose movement block is applied, pending threshold crossings. `hashState` adds per-chunk hashes that
are recomputed only for chunks written since the last checksum. Saves append the field block after the
script state (RLE); rollback snapshots copy the resource with the world.

## Cost

Every chunk keeps a count of non-default cells; kernels only visit live chunks (diffuse / spread / flow also
their neighbours), so an idle field costs nothing. Measured (tests `grid_fields_perf_256`, Release, this
machine's cloud vCPU): see the commit / report numbers; a fully live 256x256 field with an 8-neighbour diffuse
+ decay every 5 ticks is well under 1 ms per tick on average.

## View

`"view"` of a field: `show`, `preset` (`fire`, `lava`, `water`, `gas`, `creep`, `heat`), `colors` [[value,
"#rrggbb", alpha], ...] (up to 4 stops), `range` [lo, hi], `threshold`, `blend` `alpha` / `add`, `animate`
`none` / `fire` / `water` / `pulse` / `drift`, `speed`, `height` (above the ground), `cells` (hard cell edges),
`fog` `explored` (default) / `visible` / `none`, `particles` {`effect` (fx/<name>.json), `above`, `max`,
`every` seconds}. Per-player fields are tinted with the player colours. Weak GPUs (material quality Low or
shadows off; `RTS_FIELDS=low`): half-resolution texture, no animation, no particles. Shaders: GLSL + MSL in
`RtsFieldView.cpp` (the Metal path is not compiled on Linux CI).

## Editor and MCP

Window **Поля**: pick a field, mode (set / add / max / erase), value, radius, player; paint in the Scene view
(Shift erases). MCP: `field_list`, `field_get` (rect of values or an ASCII preview), `field_paint` (x, z,
radius or rect), `field_set_rules` (rules.json or the map's overrides; validated before saving).
