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
- 2D tile maps (`RtsTileView`) are drawn separately and must not change: check `games/tiles2d` and `games/km2d`.

## Rules for the simulation (`model: "heightfield"`)
- Fixed-point only: `MapGrid::surfaceHeight(Fixed x, Fixed z)` (bilinear on corners), slope per cell, passability by
  `maxSlope` per move layer, uphill speed, line of sight / range by height, unit and projectile Y from the surface.
- Cliff `levels` are derived from heights for the existing bonuses (`heights.blockVision` etc.).
- Corners are hashed in `hashState` only when the model is `heightfield` (old checksums unchanged).
- Large maps: hierarchical pathfinding so A* stays bounded on 1024x1024.

## Branches
| Branch | Scope |
|---|---|
| `agent-terrain` | terrain rendering (chunks, LOD, splat materials, quality tiers, water), visual `terrain` block, editor sculpt/paint ops + window + MCP |
| `agent-terrain-fx` | territory colours and glowing borders, diggable rock blocks and gold veins, lanes (MW2 splines) and K&M roads |
| `agent-terrain-sim` | `terrain.model`, heightfield in the simulation, `cornersFile`, hierarchical pathfinding, sample heightfield game map |
