# Terrain — design and contract

One data model, three ways a game can use it. Old games keep their look and their checksums.

## Data

| Where | What | Who reads it |
|---|---|---|
| map `terrain.rows` / `levels` / `fields` / regions / dig / territory | **gameplay layers** (cells, cliff levels, resource fields, regions, diggable rock, territory) | simulation (`MapGrid`), view, editor |
| map `terrain.heights` | per-cell fine height, 1/16 units — **visual only** | view, editor |
| map `terrain.corners` | heightfield: `(w+1)*(h+1)` corner heights, 1/16 units, row-major; corner `(x,y)` = top-left corner of cell `(x,y)`. Large maps may use `terrain.cornersFile` (raw int16 LE, `maps/<name>.r16`) | `MapGrid::corner`, `MapDoc::corners`; view always; simulation only with `model: "heightfield"` |
| rules `terrain.model` | `"flat"` / `"levels"` (default, today's behaviour) / `"heightfield"` | simulation (agent-terrain-sim) |
| rules/game `terrain` visual keys (materials per cell kind, splat, water, territory/dig/lane looks) | presentation | view (agent-terrain, agent-terrain-fx) |

Without a `terrain` block and without `corners` everything behaves and looks exactly as before.

## Rules for the view
- The view reads the sim layers and never writes them. `heights` and (outside `model: "heightfield"`) `corners` are presentation.
- Surface height in the view: `corners` when present (bilinear), otherwise today's `heights` + `level * levelHeight` + ramps.
  `RtsView::heightAt` stays the single function every visual uses to put things on the ground.
- Geometry is static per chunk (32x32 cells); what changes in a match (fields, creep, territory, dig, fog) is sampled from
  small per-cell textures, so the mesh is not rebuilt during play. Editor edits rebuild only dirty chunks.
- Maps up to 1024x1024 (and larger): chunk culling + CDLOD-style LOD with geomorphing on large maps.
- Quality tiers for weak Android (vertex colours / splat / splat + normals + triplanar), shaders in GLSL and MSL with
  `RTS_*_SHADOW` and `RTS_*_LIGHTS`. Textures via `TextureCache::resolve(path)`; tiling splat textures are kept out of atlases.
- 2D tile maps (`RtsTileView`) are drawn separately and must not change: check `templates/tiles2d` and `templates/km2d`.

## Rules for the simulation (`model: "heightfield"`) — implemented (agent-terrain-sim)
- Data keys: rules `terrain.{model, maxSlope (number or {ground, sand}), buildSlope, uphillSpeed, downhillSpeed,
  eyeHeight, targetHeight}`, type `move.maxSlope`; full description in docs/game-format.md "Heightfield terrain".
- `MapGrid::surfaceHeight(x, z)` (bilinear, fixed), `cellHeight`, `gradient`, `slope[]` (1/256 units per cell),
  `hf` flag; `enableHeightfield()` derives slopes and cliff `level` = floor(h / levelHeight). Passability by slope
  per move layer / slope class (`UnitType::pathLayer` = layer | class << 4); `canStep` ignores cliffs on heightfields.
- Uphill/downhill speed factor, mobile unit `pos.y` = surface, projectile Y from the surface, LOS vision,
  `RtsModule::heightAdvantage` for range/damage/missUphill.
- Corners and the model are hashed only when the heightfield is active (old checksums unchanged).
- `terrain.cornersFile`: raw int16 LE; `applyCornersFile(map, reader)` (called by `applyTilesToMap`, so every
  loader gets it) attaches the bytes as in-memory `terrain.cornersBlob`; `MapDoc` loads/saves it (file above
  257x257 corners). Size cap `kMaxMapSize` = 2048.
- Large maps: HPA* (`RtsHpa.cpp`) on maps >= 256x256, built at setup, clusters invalidated by `MapGrid::block` /
  `cellChanged`; clearance updated locally on block; fog reset, soft obstacles and spatial hash reset are O(used).
- The view should keep reading `corners` for its own surface; on heightfield games mobile units now carry a real
  `pos.y` (the view may keep using `heightAt`, which matches the sim surface when it is bilinear on corners).

## Branches
| Branch | Scope |
|---|---|
| `agent-terrain` | terrain rendering (chunks, LOD, splat materials, quality tiers, water), visual `terrain` block, editor sculpt/paint ops + window + MCP |
| `agent-terrain-fx` | territory colours and glowing borders, diggable rock blocks and gold veins, lanes (MW2 splines) and K&M roads |
| `agent-terrain-sim` | `terrain.model`, heightfield in the simulation, `cornersFile`, hierarchical pathfinding, sample heightfield game map |

## View implementation (agent-terrain)
- `modules/rts/view/RtsTerrainSurface.h`: the surface math (levels mode as before; heightfield = two triangles per
  cell, diagonal (0,0)-(1,1)). `RtsView::heightAt` and the editor's sculpt ops use it; the mesh is built from it.
- `RtsTerrainRender.*`: 32x32 chunks with a per-chunk input hash (only dirty chunks rebuild), frustum culling, CDLOD
  (5 levels, geomorph + skirts) on heightfield maps above 256 cells, per-cell textures for fields / creep / fog of war,
  water sheet or water bodies. `RtsGroundFx` is invalidated whenever terrain geometry changes.
- Materials, tiers, editor ops and MCP tools: `docs/game-format.md` § Terrain materials.
- Measured (VM, Mesa llvmpipe, 1280x800, whole 1024² map in view): LOD off 1.19M triangles / 374 ms,
  LOD on 41k / 112 ms; gameplay zoom 32k triangles / ~20 ms on 512² and 1024² alike; full build 1024² ~100 ms.
