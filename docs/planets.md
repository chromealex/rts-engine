# Planets, orbits and terrain generation — design and contract

Written by MAIN as stage 0 for the agents `agent-planet-sim`, `agent-planet-render`, `agent-terrain-gen`.
Everything here is additive: a map without `world` / `generate` loads, plays and looks exactly as before
(checksums of every game in `games/` must not change).

## 1. Bodies

A map either describes today's flat battlefield (implicitly one body of kind `plane`), or has a `world` block:

```jsonc
"world": {
  "bodies": [
    {"id": "sun",   "kind": "star",   "radius": 40, "visual": {"color": [255, 220, 160]}},
    {"id": "terra", "kind": "planet", "radius": 64, "faceSize": 48, "seaLevel": 0,
     "orbit": {"around": "sun", "radius": 900, "period": 1200, "phase": 0.1},   // period in seconds, phase in turns
     "orbitAltitude": 12,                                                       // shell for move.layer "orbit"
     "terrain": {"faces": [ {"rows": [...], "levels": [...]}, ... 6 faces ], "corners": [ ... ] | "cornersFile": "maps/x.terra.r16"},
     "generate": { ... see §4 ... }},
    {"id": "luna",  "kind": "moon",   "radius": 20, "faceSize": 16, "orbit": {"around": "terra", "radius": 160, "period": 300}}
  ],
  "start": "terra"            // body the camera starts on
}
```

- `kind`: `plane` (today's map; only as body 0 and only implicitly for now), `planet` / `moon` (cube-sphere surface),
  `star` (no surface; light source, visual, may be impassable/damaging later).
- Orbits are circular and deterministic: angle(t) = phase + t / period (turns), position via `fxCosTurns`/`fxSinTurns`
  in the parent's orbital plane (XZ plane of the parent). A body without `orbit` is fixed at the world origin
  (or at `"pos": [x, y, z]`).
- All gameplay quantities are Fixed; sizes in world units (the flat-map cell is 1 unit).

## 2. Cube-sphere surface

Six faces, each an N x N cell grid (`faceSize` = N). Cell id = `face * N * N + y * N + x`.

Face order and basis (cubemap convention). A face point is `n + u * s' + v * t'` with s, t in [-1, 1]:

| face | n | u | v |
|---|---|---|---|
| 0 | +X | (0, 0, -1) | (0, -1, 0) |
| 1 | -X | (0, 0, +1) | (0, -1, 0) |
| 2 | +Y | (+1, 0, 0) | (0, 0, +1) |
| 3 | -Y | (+1, 0, 0) | (0, 0, -1) |
| 4 | +Z | (+1, 0, 0) | (0, -1, 0) |
| 5 | -Z | (-1, 0, 0) | (0, -1, 0) |

- Cell coordinate a in [0, N] maps to s = 2a/N - 1, then the **tangent warp** s' = tan(s / 8 turn)
  (`fxTanTurns(s / 8)`) for near-uniform cells. Direction = normalize(face point); surface point =
  direction * (radius + height).
- Heights: `corners` per face, `(N+1)*(N+1)` int16 in 1/16 units (same unit as flat-map `terrain.corners`), faces in
  order 0..5, row-major. Edge/corner vertices shared between faces are stored on every face and must be equal;
  loaders average duplicates defensively; tools must write them equal.
- Per-face gameplay layers use the flat-map characters (`rows`, `levels`, fields, ...) per face.
- Neighbours across a face edge follow from the basis above (helper functions live in the sim; the view and
  the generator use the same code, `modules/rts/RtsSphere.h`, owned by agent-planet-sim — until it lands,
  agents implement against this table).
- Map objects/players on a planet: `"at": {"body": "terra", "face": 2, "x": 10.5, "y": 7}` (cell coordinates).

## 3. Simulation model (agent-planet-sim)

- Positions are **body-local** Vec3 (origin = body centre); an entity's body is a component (absent = body 0).
  World position = body centre(t) + local position. Units on different bodies only interact through the orbit layer.
- A `Surface` abstraction (plane = today's code path, bit-identical; cube-sphere = new) owns: cell lookup from a
  position, cell centre, neighbour iteration (8-connected, across face seams), surface height/normal ("up"),
  distance between two positions, moving a position toward another along the surface, projection onto the
  surface. Pathfinding (A*, HPA*), clearance, vision/LOS, fog, spatial hash, combat ranges, building footprints
  and AI go through it.
- `move.layer: "orbit"`: units on the shell radius + orbitAltitude, free great-circle movement, can see/attack the
  surface below within range; interplanetary transfer between bodies' orbit shells is a timed transit computed
  from the bodies' positions at departure.
- New maths is Fixed only: `engine/core/FixedMath.h` (sin/cos/tan/atan2/asin/acos in turns, normalize, cross).

## 4. Terrain generation (agent-terrain-gen)

```jsonc
"generate": {"preset": "continents", "seed": "match" | 12345, "players": 4, "symmetry": "auto", "params": { ... }}
```

- On a flat map: top-level `"generate"`; on a planet: `body.generate`.
- Deterministic integer/Fixed code only (noise, erosion, biomes, water, resources, start positions) — it runs at
  match start in lockstep (`"seed": "match"` takes the match seed), in headless, and in the editor/MCP (bake to a
  static map). Same seed + params + engine version = bit-identical map on every platform.
- Fair starts: symmetric layouts on flat maps; on planets maximally separated start sites, flattened base areas,
  equalised resources and path distances, validated with the pathfinder.
- Presets per genre: continents, islands/archipelago, highlands/canyons, Dungeon Keeper caves (dig layer), lanes
  (MW2), classic Warcraft/Red Alert land maps, TA metal maps, planets (rocky, ocean, moon).

## 5. Rendering (agent-planet-render)

Cube-sphere terrain with per-face quadtree/CDLOD chunks (reuse the flat terrain's materials, quality tiers and
water), orbit camera (surface ↔ planet ↔ system zoom), picking on spheres, units oriented to the local up,
fog of war and ground overlays on spheres, atmosphere/ocean with quality tiers, stars and orbit lines, a sphere
minimap. The flat path (`RtsTerrainRender`, `RtsTileView`) must not change.

Implementation (all presentation, float; nothing here touches the simulation):

| file | what |
|---|---|
| `modules/rts/view/RtsPlanet.{h,cpp}` | `PlanetWorld` / `PlanetBody` from the map `world` block (`loadPlanetWorld`: the one place to switch to sim bodies), circular orbits `center(body, seconds)`, `PlanetSurface` (corner directions from `RtsSphere` converted to float once per body, duplicates averaged, heights, normals, `heightAt` on the same triangles as the mesh, `raycast`) |
| `RtsPlanetRender.{h,cpp}`, `RtsPlanetShaders.h` | `PlanetRenderer`: per face a quadtree (leaf 16 x 16 cells, one quad per cell; level k = 2^k cells per quad), CDLOD selection (`lodRange` 48, doubling per level), geomorph to the coarser grid in the vertex shader, skirts on every chunk edge (LOD cracks and face seams), lazy chunk builds (max 24 per frame, the parent stands in), culling per chunk: bounding sphere vs frustum + horizon (angular cap beyond the body's limb). Chunk vertices are chunk-relative (float precision on big bodies). Ocean = the same chunks projected onto the sea sphere (alpha blended, only chunks with corners near sea level). Sky: fullscreen starfield + star glow; stars = emissive spheres; far bodies (< 3 px) = plain spheres; atmosphere shell (analytic rim scattering, additive, Medium+ material tier); orbit lines at planet / system zoom; aerial haze and a night side ambient |
| `RtsPlanetCamera.h` | `PlanetCamera`: focus direction on a body + parallel-transported forward vector (no poles, no seams), pitch, distance. Zoom regimes blend the target from the surface point to the body centre (`planetBlend`) to the system centre (`systemBlend`); the frame turns into the orbital plane at system zoom. `focus(body)` = smooth transition. `PlanetViewFrame`: the renderer draws in a "view world" (focus point at the origin, local up = +Y), so `RTSCamera`, shadows, the light map and overlays work unchanged near the focus |
| `RtsPlanetView.cpp` | `RtsView` on planet maps: `setWorld` / `updateWorld` (editor: only chunks over changed corners rebuild), `drawPlanets` (objects placed in their body's tangent frame: local up = normal of the sphere, flat-map style east (+a) / south (+b) axes so flat facings keep working; selection rings, air shadows, move markers; health bar anchors hidden behind the body), `pickPlanet`, fog of war per cell (vision atlas 3 x 2 faces of N x N), `flatToBody` / `bodyToFlat` |
| `apps/client/ClientPlanet.cpp`, `MinimapPlanet.cpp` | orbit camera input (wheel / pinch zoom, MMB / one-finger drag rotates over the surface, Q / E or two-finger twist turn, arrows / WASD / screen edges move, PageUp / PageDown pitch, Tab next body, Space back to base); globe minimap around the focus (fog, units, view box; click = look there), system panel at system zoom (click = focus a body) |
| `apps/editor/core/PlanetOps.*` | sculpt / paint / material brushes on bodies (3D angular brush across face seams; duplicated seam corners written equal); MCP: `terrain_sculpt` / `terrain_paint` / `terrain_material` with `body` + `face` + `x`, `y` (cells on that face) or `dir` [x, y, z]; `planet_info` |

Until the simulation has bodies (agent-planet-sim §3), the flat sim map is laid onto face 2 of the start body
(centred, scaled to fit: `RtsView::flatToBody`); objects with `SceneObject::body >= 0` are taken as body-local.
When the sim lands: fill `SceneObject::body` (and body-local `pos` / `facing` / `target`) in `sceneFromSim`, give
per-body vision to `PlanetRenderer::setVision`, and replace `loadPlanetWorld` / `PlanetWorld::center` by the sim's
bodies and orbit positions (both are single functions).

Quality: Low material tier = vertex colours by cell kind, no atmosphere shell; Medium+ = triplanar splat of the
visual `terrain.layers` by cell kind + slope cliff texture (no normal maps on spheres yet) + atmosphere. Shadows only
at surface zoom (the shadow map fit follows the RTS camera). Particles (`ClientFx`) are skipped on planet maps for
now (flat coordinates).

Test knobs: `RTS_PLANET_CAM="distance[,pitch degrees[,body id]]"` overrides the start view, `RTS_TERRAIN_STATS=1`
prints `[planet] chunks / drawn / culled / tris`, `RTS_MINIMAP_DUMP=file.ppm` writes the planet minimap (the HUD is
not in screenshots).

Measured (llvmpipe software GL, 1280 x 760, faceSize 128 planet with radius 81 + a 32 moon + a 64 planet + star):
surface view 63 chunks / 40k triangles, planet view 55 chunks / 35k triangles, system view 18 chunks / 11.5k
triangles; ~55 ms per frame at surface / planet zoom vs ~30 ms for the flat skirmish map on the same renderer
(CPU culling + selection < 0.1 ms; the cost is fill rate of the software rasteriser).
