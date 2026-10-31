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

## Vision (agent-vision) — how it is computed

`modules/rts/RtsVision.cpp`, system `vision`, every 5 ticks. Cells lit last pass fall back to explored (only those in
`visibleCells`), then every unit lights the cells it sees for its owner and the allies sharing vision (`see()`).

- **Definition** = the brute-force reference (`visionFlatRef`, `visionSphereRef`): per unit, every cell of the sight disk;
  cliff levels: hidden when the target level is above the eye level (air units / no `blockVision`: everything); line of
  sight (heightfield + `blockVision`, or `fogOfWar.lineOfSight`): integer ray march over `visionBase` (ground, 1/64 units)
  and `visionOcc` (ground + `vision.blocks` structures / units, smoke `blocksVision` areas, undug rock `rockHeight`).
- **Fast path** (default) produces exactly the same `vision`, `visibleCells` (same order) and `visionBase/visionOcc`:
  per unit cached cell lists keyed by (cell, sight, type) on flat maps / (exact position, sight, type) on planets, replayed
  while no 16 x 16 tile they read changed; `visionBase/visionOcc` updated incrementally from changed corners / levels /
  dig cells / blockers (diffed against the last pass) instead of rebuilt over the map; line-of-sight rays from per-radius
  offset tables (no divisions). Caches are derived and live in the module (`VisionCache`), tied to the `GameState` by
  `visionToken` (not saved / hashed): a snapshot restore, a loaded save or another simulation rebuilds them from scratch.
- **Verification:** `RTS_VISION=ref` runs the reference, `RTS_VISION=check` runs both every pass and aborts on any
  difference; `RtsModule::setVisionReference(true)` for tests (`vision_fast_equals_reference_flat` / `_planet`: random maps,
  moving / parked units, blockers, smoke, digging, sculpting, rollback, save loading). `rts_headless --profile` prints
  per-system time.
- Measured (Release, arm64 VM): Big Planet 0.72 → 0.064 ms/tick (80% → 27% of the tick); faceSize-256 planet 0.42 → 0.043;
  1024² heightfield, 500 moving units 0.47 → 0.12; metaltide 0.015 → 0.004. Checksums of every template unchanged.

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

## Detail surface (agent-terrain, sub-cell terrain)
The gameplay grid stays one height per cell corner (or one level per cell); what is *drawn* can carry much more
detail. The detail surface is a visual heightmap with `k` samples per cell, decoupled from the simulation.

**Data**
| Where | What |
|---|---|
| visual `terrain.style` (rules.json / game.json) | `"classic"` (default: cell-aligned plateaus, vertical walls - existing games unchanged, pixel-identical) or `"smooth"` (synthesised sub-cell detail) |
| visual `terrain.detailLook` | `{scale 1..8 (default 4), bevel 0.35 (cliff half-width, cells), wobble 0.3 (outline warp, cells), roughness 0.04 (fine noise, world units), blendSharpness 0.5, macro 0.25, lodRange 24, rockColor [r,g,b]}` |
| map `terrain.detail` | `{"scale": k, "file": "maps/<name>.detail.r16", "splat": "maps/<name>.detail.splat"}`: int16 LE, absolute height in 1/16 units, `(w*k+1) x (h*k+1)` row-major (sample `(i, j)` at world `(i/k, j/k)`); optional splat = RGBA8 weights of layers 0..3 per sample. Takes precedence over synthesis. Written by the editor (`terrain_detail`, sculpt, import) |

**Synthesis** (`modules/rts/view/RtsTerrainDetail.*`, deterministic, presentation only):
- levels maps: the classic per-cell surface looked up through a smooth domain warp (organic outlines), blurred with
  three box passes (≈ Gaussian: bevelled cliffs, rounded plateau corners, ramps blended into both levels), then fine
  noise scaled per cell kind (rock rough, sand / water calm);
- heightfield maps: Catmull-Rom bicubic through the corners (passes through every corner, so the sim surface is kept
  at the corners) + fine noise;
- edits rebuild only the samples around changed chunks (`synthMargin`).

**Rendering** (`RtsTerrainDetailMesh.cpp`, shader variant `TERRAIN_DETAIL`):
- per 32x32-cell chunk, CDLOD levels with steps of 2^L samples, built lazily when first drawn and LRU-evicted over
  `detailBudgetMB` (96); geomorph to the next level in the vertex shader; skirts + walls to the apron;
- per-pixel normals from a detail normal texture (rg = n.xz, b = cavity, a = macro variation) - on every tier, so the
  Low tier (vertex colours) gets small forms and lighting too; automatic rock on steep slopes (`rockColor`, or the
  cliff texture triplanar on High);
- splat weights at detail resolution (painted, or derived from cell kinds with warped soft borders); height-based
  blending between layers, macro variation and a second rotated sample on High against visible tiling;
- texture sides capped at 4096 (WebGL2 / GLES3 safe); quality: Low tier caps k at 2, samples capped at 4097² desktop
  / 2049² mobile (`detailScaleFor`);
- `RtsView::heightAt` samples the detail surface (same triangles as the mesh), so units, overlays and fog follow it;
  water bodies sit at the detail shore and extend one cell under the bank (organic lake outlines).

**Consistency with the simulation**: heightfield maps - the editor keeps corners = detail at the corners (sculpt and
import), so the sim surface is the bilinear of the drawn one at cell scale. Levels maps - the sim is unchanged;
`map_validate` warns where the visual surface on walkable cells differs from the simulated ground by more than 0.5
(units would float or sink), with the count and the first cell.

**Editor / MCP** (`apps/editor/core/TerrainDetailOps.cpp`, window **Рельеф** → "Детально"): `terrain_detail
{action: info|create|remove, scale}`, `terrain_sculpt {op: raise|lower|smooth|flatten|noise|set|erode|stamp, x, y |
points (world units, fractions ok), radius (world units), strength, hardness, detail, stamp: crater|mesa|dunes|rocks|
<png>, angle}`, `terrain_paint_material {layer, x, y | points, radius, strength, hardness}`, `heightmap_export /
heightmap_import {detail: true, detailScale}` (16-bit PNG at `(w*k+1) x (h*k+1)`), `terrain_heights` reads the drawn
surface.

**Performance** (VM, Mesa llvmpipe software GL, 1280x800, heightfield test maps from `mapgen.py --big`, k=4,
gameplay camera): 256² - 135k triangles, 31 ms/frame (classic 8k / 19 ms), build 83 ms; 1024² - 135k triangles,
32 ms (classic 9k / 17 ms), build 1.0 s (surface 0.3 s + 4096² normal/splat textures 0.7 s), 4 MB of LOD meshes in
use; Low tier (k=2) 1024²: 35k triangles, 24 ms, build 0.29 s. Whole 1024² map in view: 896 chunks, 57k triangles.
Texture memory k=4 on 1024²: 2 x 4096² RGBA8 (+mips) ≈ 170 MB on desktop; mobile caps the samples at 2049² (≈ 43 MB).

Apron: outside the map the detail normal / splat textures would repeat their edge texels in streaks; the
`TERRAIN_DETAIL` shader fades to the geometric normal and to a high-mip (blurred) edge material within 2 cells
of the border, and the border walls keep their geometric normals.

### Planets (agent-terrain)
The same idea per cube-sphere body: a detail grid of `M = N*k` samples per face side, `(M+1)^2` per face, indexed
like the corners (`face`-major, row-major; sample `(i, j)` at face coordinates `(i/k, j/k)` cells).

**Data** (world body `terrain`, visual only - the simulation never reads them):
| Key | What |
|---|---|
| `style` | `"smooth"` / `"classic"`; default: the game's visual `terrain.style` (templates/planets: `"smooth"`) |
| `detailLook` | `{scale (samples per cell, default the game's detailLook.scale = 4), roughness (fine noise, world units; default the game's roughness x 1.5), craters 0..1 (default 0.6 on moons, 0 otherwise)}`. The planet generator (`GenPlanet.cpp`) writes hints: moons / barren `{craters 0.7, roughness 0.08}`, ocean worlds `{roughness 0.04}` |
| `detail` | `{"scale": k, "file": "maps/<map>.<body>.detail.r16"}`: explicit heights, int16 LE in **1/256** units (`kPlanetDetailUnit`; 1/16 steps show as faceted normals on fine relief), 6 x (N*k+1)^2. Seam duplicates must be equal (the loader averages them). The editor writes it; drawn whenever present unless the body says `style: "classic"` |

**Seams**: seam samples are the corner duplicates of a `CubeSphere(M)` (`cornerDuplicates`): one canonical
direction (bit-identical on every face), one averaged height, one normal. Synthesis is a function of the direction
except the Catmull-Rom basis (rows continue across the seam through the coarse surface), and the seam average removes
that. Normals / cavity of edge samples read their neighbours across the seam (no one-sided bias, no seam lines).

**Synthesis** (`modules/rts/view/RtsPlanetDetail.cpp`, `PlanetSurface::initDetail`): Catmull-Rom through the corners
+ 3D value noise on the direction (2 octaves, scaled per cell kind like the flat roughness) + on moons a crater field
(3D cell lattice, 2 octaves: 3-cell spacing with 0.5-1.4-cell craters and 1.1-cell spacing with small ones; bowl +
raised rim). Explicit detail at another k is resampled bilinearly. `heightAt`, `normalAt`, `raycast` follow the grid
(exactly the drawn triangles), so units, rings, picking and effects stand on it; `coarseHeightAt` keeps the corner
surface. Corner edits resynthesise only the samples around the changed corners (+2 corners across seams), flag
painting only around the changed cells.

**Rendering** (`RtsPlanetRender`): the per-face CDLOD quadtree runs in samples (leaf = 16 x 16 samples), LOD range
`lodRange * 2^level * 1.5/k`, geomorph onto the coarser sample grid, skirts on every chunk edge (depth in cells) -
no cracks between LODs or across seams. Shader variant `PLANET_DETAIL` (GLSL + MSL): vertex attribute 6 = texel
centre in a per-body atlas (tex3, RGBA8, faces in 3 x 2 blocks of (M+1)^2 texels: rgb body-local normal, a cavity)
-> per-pixel normal on every tier, cavity darkening, rock colour on steep slopes when no cliff texture is drawn
(Low tier); colours / splat per sample = bilinear of the corner colours. k=0 bodies keep the exact old path
(pixel-identical). Budget: <= 2.4M samples per body on desktop, 0.6M on touch devices, atlas side <= 4096 / 2048,
Low tier caps k at 2.

**Editor / MCP** (`apps/editor/core/PlanetDetailOps.cpp`): `terrain_detail {body, action: info|create|remove,
scale}`; `terrain_sculpt {body, face + x, y | dir | points, op raise|lower|smooth|flatten|noise|set, radius (cells,
sub-cell ok), strength, hardness, detail}` - detail defaults to true when the body has a grid; 3D dabs across seams
(duplicates written equal), the body corners follow the detail at the corner samples; corner sculpts carry the
detail along (bilinear delta). Window **Рельеф** -> "Детально" works on planets. `MapDoc` loads / saves
`maps/<map>.<body>.detail.r16` (live documents pass it to the view inline as `detail.blob`).

**Performance** (VM llvmpipe, 1280x800, templates/planets Big Planet: N=128, k=4 = 1.58M samples): synthesis +
normals + atlas 107 ms at load, atlas 1539x1026 RGBA8 (6 MB), CPU grid ~50 MB; surface zoom 153 chunks / 98k
triangles, 46 ms/frame (classic look: 26 chunks / 17k, 21 ms - but classic planets have no material textures; Low
tier k=2: 82 chunks / 52k, 33 ms); planet zoom 68 chunks / 44k triangles, 27-50 ms (classic 32 / 20k, 19 ms).
