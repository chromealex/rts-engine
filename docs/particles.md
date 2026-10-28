# Particles

Presentation-only particle effects: CPU simulation (`engine/fx/ParticleSystem`), drawn through the
RHI with one dynamic vertex buffer per frame. The simulation never sees them; effects are started from
`SimEvent`s by `modules/rts/fx/RtsFx` (the same read-only event collector as `RtsAudio`).

## Data (per game)

```
<project>/fx/fx.json        which effect plays for which event (bindings)
<project>/fx/<name>.json    one effect = 1..16 emitters
```

- Effect format: comment at the top of `engine/fx/FxEffect.h` (unknown fields are errors).
- Bindings format and lookup order: comment at the top of `modules/rts/fx/RtsFx.h`.
- Cues: `attack hit miss died spawned ready buildPlaced buildDone gathered captured levelUp ability
  upgraded sold regionClaimed wave trail damaged`; `died.structure|air|unit` style keys pick by category.
- Textures: `builtin:soft|spark|ring|smoke` need no files. Anything else is an image path in the game
  folder ("sprites/smoke.png"), loaded by `render::TextureCache::resolve` — the same function as sprites
  and tiles, so the atlas packer covers particles too. Particles from one texture (or one atlas page)
  share a draw call; flipbook frames use the sprite's uv rect.

Editor: window **Эффекты** (list, parameters, live preview with a quality switch and a fill-rate meter,
JSON, bindings). MCP: see below. The play-test shows effects too.

Optional modules (all off by default; an effect that doesn't use them renders exactly as before):
`blend: premultiplied`, `align: upright`, `aspect`, `offset`, timed `bursts` with cycles, `startColor`
(constant / two colours / gradient), `space: local`, `maxParticles`, `velocity`, `speedLimit`,
`colorBySpeed` / `sizeBySpeed`, `noise`, flipbook `cycles` / `startFrame`, shapes `hemisphere`, `cone`,
`box`, `line` with `arc` / `thickness` / `radial`, `hidden`, ground `collision`, `subEmitters` (birth /
death / collision play other effects, nesting stops at depth 3), ribbon `trail` and particle `light`
(the apps add them to `Renderer3D::lights` via `collectLights`). Exact keys: top of `engine/fx/FxEffect.h`.
Trails start at `minQuality: medium` (medium draws every other ribbon point), particle lights are capped
at 32 (8 on low) and go through the light map like any other light.

## The rest of Unity's ParticleSystem

Every module of Unity's ParticleSystem now has an engine key (exact list: second block at the top of
`engine/fx/FxEffect.h`), so imported effects are no longer flattened:

- **Curves everywhere**: any range (`life`, `speed`, `size`, `rate`, `spin`, `drag`, `speedLimit`, noise
  strength, velocity / force axes, ...) also takes `{ "curve": [[t, v], ...], "curveMin": [...] }` (or the
  shorthand `[[t, v], ...]`) — Unity's MinMaxCurve modes 1:1. Start values and rates follow the emitter time,
  over-lifetime values the particle age; random-between-two-curves keeps one random per particle.
- **Main / emission**: `prewarm`, random `delay`, burst `chance` and endless `cycles: 0`, 3D size
  (`sizeY`/`sizeZ`), 3D rotation, `gravityScale`, start colour gradient over time or between two gradients,
  `ringBuffer`.
- **Shape**: `donut`, cone `length` (volume), box `emitFrom` shell / edge, `mesh` (built-in primitives or
  model files: vertex / edge / triangle), `shapeRotation`, arc modes (loop, ping-pong, burst spread, step),
  `randomDirection`, `sphericalDirection`, `randomPosition`, `alignToDirection`.
- **Over lifetime / by speed**: `velocityOverLife` (linear, orbital, offset, radial, speed modifier),
  `force` (random per frame), speed limit per axis with `limitDampen`, drag by size / speed,
  `inheritVelocity` (initial / current), `lifeBySpeed`, `colorMin` / `sizeOverLifeMin` (two gradients /
  curves), separate size axes, `spin` curves, `spin3D`, `spinBySpeed`.
- **External forces**: `externalForces` + effect-level `forceFields` (sphere / cylinder: direction, gravity
  with focus, vortex with attraction, drag) and `ParticleSystem::setWind`.
- **Noise**: octaves, per-axis strength, damping, remap curve, rotation and size amounts.
- **Flipbook**: single / random `row`, `frameOverTime`, `framesBySpeed`, `flip`, `sprites` (one sprite per
  frame; sprites of one atlas page still share a draw call).
- **Collision / triggers / sub-emitters**: up to 6 `planes` + ground, particle `radius`, `maxKillSpeed`;
  `trigger` volumes (inside / outside / enter / exit: kill or fire `"on": "trigger"` sub-emitters);
  sub-emitters `trigger` / `manual` (`ParticleSystem::emitManual`), `inherit` color / size / rotation /
  lifetime / duration, up to 8.
- **Trails**: `mode: "ribbon"` (particles of an emitter connected, `ribbons`), `lifeColor`, `ratio`,
  `lifeFraction`, `minDistance`, `textureMode` stretch / tile / distribute, `dieWithParticles: false`
  (trails fade out after their particle).
- **Lights**: radius / intensity curves over the lifetime.
- **Renderer**: `align` world / local, `pivot`, `cameraStretch`, `sort` (distance, oldest / youngest in
  front, none), `sortOrder`, `minScreenSize` / `maxScreenSize`, **mesh particles** (`meshes`: up to 4,
  `builtin:cube|sphere|capsule|cylinder|plane|quad` or FBX / OBJ / glTF through the model loader,
  `fx::meshFromModelFile` in `engine/fx/FxModelMesh.h`; lit by the sun on the CPU), blends `multiply`,
  `subtract`, `opaque` (`gfx::BlendMode::Multiply` / `Subtract`).

Not supported on purpose (they need what OpenGL ES 3.0 weak GPUs can't afford or what the engine doesn't
have): soft particles and distortion (depth / grab textures), custom vertex streams and custom data (only
feed custom shaders), custom simulation space (effects have no transform hierarchy), VFX Graph.

Cost control for the new modules, per tier: noise octaves 1 / 2 / 4 (low / medium / high); mesh particle
vertices per frame 4 000 / 16 000 / 64 000 (the rest are skipped); trails and ribbons start at `medium` by
default (medium draws every other point); prewarm runs in 1/8 s steps on low (1/15 s otherwise); particle
lights 8 on low; alpha sorting (also `sort: "distance"`) is off on low. Mesh particles are expanded on the
CPU like quads (no instancing), one draw per texture and blend.

## Weak GPUs (OpenGL ES 3.0 is the floor)

| Tier (`FxQuality`) | budget | spawn | max on-screen size | alpha sort | default for |
|---|---|---|---|---|---|
| off | 0 | — | — | — | — |
| low | 600 | 0.4x | 12% of screen height | no | Mali-4xx/T/G31-52, Adreno 3xx-51x, PowerVR SGX/GE/GX6, software GL |
| medium | 2500 | 0.7x | 25% | yes | other phones/tablets |
| high | 12000 | 1x | 45% | yes | desktop |

`RTS_PARTICLES=off|low|medium|high` overrides the GPU guess; the pause menu switches it in game.
What the renderer avoids on purpose: instancing, compute, transform feedback, depth reads (soft
particles), `discard`, per-pixel fog, highp in the fragment shader. One-shot effects that start
off-screen or beyond the tier's distance are not spawned. Emitters with `"minQuality": "medium"` drop
out on weak devices — use it for sparks, shockwaves and other secondary layers.

Fill rate is the real limit on old Mali/Adreno: keep an effect under ~1–1.5x screen fill (the editor
shows it) by shrinking `size`/`sizeOverLife` or the burst before anything else.

## MCP (agents)

| Op | What |
|---|---|
| `fx_list` | effects with emitters, lifetime, textures, load errors, where each is bound; binding warnings |
| `fx_get` / `fx_set` | read / write `fx/<name>.json` (`fx` = bindings) as `json` or `text`; validated by the engine loader — errors name the key (`emitters[1].trail.time: must be in 0.01..10`) and block saving, missing textures / sub-effects are warnings |
| `fx_delete` | refuses while bound unless `unbind: true` |
| `fx_templates` | recipes on built-in textures: explosion, fire, smoke, muzzle_flash, magic_aura, heal, frost, lightning, blood, dust_trail, rain, snow, portal, level_up; `color` tint, `scale` |
| `fx_import_unity` | `{source: .prefab / folder / .unitypackage, name, prefab, scale}`: runs the importer below, returns its report + the engine's check of every written file |
| `fx_bind` | `{effect, cue, type?, weapon?, ability?, category?, height, scale, teamColor, below}` → `fx/fx.json` (`effect: null` = none, `remove: true` = back to the fallback) |
| `fx_preview` | GUI: plays an effect at `x, z` in the Scene view (Game view during a play-test) and moves the camera there; `advance` + `freeze` hold a moment for `view_screenshot`, `moving` circles it (trails), `pitch`, `quality`, `loop`, `stop` |

Core ops live in `apps/editor/core/FxOps.cpp` (also work in `rtsmcp` without an editor), `fx_preview` in
`apps/editor/EditorFxOps.cpp`. Every write notes `Change::Fx`: the editor reloads effects and textures.

## Unity import

```
python3 tools/importers/unity_vfx/import.py --source <file.prefab | folder | file.unitypackage> \
    --game <project> [--name fire] [--prefab Explosion] [--scale 1] [--ignore-root-rotation] [--json] [--dry-run]
```

Standard library only (TGA is decoded in Python; PSD / JPG / TIF / EXR need ImageMagick or ffmpeg, else a
warning and `builtin:soft`). The prefab must be text-serialized (Unity's default "Force Text"). The folder
is indexed by `.meta` GUIDs together with its enclosing `Assets/` folder, so materials and textures may live
elsewhere in the project. Output: `fx/<name>.json` (every ParticleSystem of the hierarchy is a layer =
emitter, positioned by its transform), `fx/<name>_<child>.json` for systems used as sub-emitters,
`fx/textures/*.png` (main textures, exported sprites), `models/<file>` (FBX / OBJ / glTF of mesh shapes and
mesh particles, copied as they are), and a report (`--json`) of what was mapped exactly (`systems[].modules`),
approximately (`approximate`) or not at all (`dropped`).

Space: Unity is Y-up left-handed; engine world = Unity (x, y, −z), and emitter directions are written in
the effect frame (play direction +Y; for it, effect (x, y, z) = world (z, y, x)). Positions and directions
(`direction`, `offset`, local `velocityOverLife` / `force` / `speedLimitAxes`, `orbitalOffset`, collision
planes, trigger volumes, force fields) go through that frame; angular velocities (`orbitalX/Y/Z`, force-field
`rotation`) are axial vectors and change sign under the mirror; mesh particle orientation (`rotation3D`,
`spin3D`, mesh `rotation` / `spin`) is engine-world Euler, Unity (x, y, z) → (−x, −y, z); billboards keep
the mirrored `rotation` / `spin` (−z). Model files (FBX / OBJ / glTF, not `builtin:*`) are mirrored in X by Unity's
import, which with the z mirror is a half turn about the model's Y: mesh particles get R·Ry(180) =
Ry(y + 180)·Rx(−x)·Rz(−z) (rotation / spin about x and z change sign), mesh shapes `shapeRotation`·Ry(180).
Engine arcs sweep from +X towards +Z around +Y, Unity's from +X towards +Y around +Z: through the mirror the
engine starts at Unity's arc end and sweeps back (same sector; loop / ping-pong run the other way). Unity stores rotations, spins and orbital speeds in radians: they are
written in degrees. The prefab root's rotation is kept (`--ignore-root-rotation` for prefabs instantiated
with identity). `--scale` converts Unity metres to game units (sizes, speeds, radii, forces, noise, lights).

Values: a Unity MinMaxCurve maps exactly onto an engine range — constant → number, two constants →
`[min, max]`, curve → `{ "curve" }`, two curves → `{ "curve", "curveMin" }` (Hermite and weighted tangents
sampled into ≤ 16 linear keys, < 1 % of the range for ordinary curves; step keys stay sharp). Start values
and rates follow the emitter time, over-lifetime values the particle age, as in Unity. Gradients merge colour
and alpha keys (fixed mode as steps). Everything is clamped to the engine's ranges (a `limits` note when hit).

| Unity module | Engine keys | Still approximate / dropped |
|---|---|---|
| main | `flipRotation`, `duration`, `loop`, `prewarm`, `delay` (range), `life` / `speed` (−500..500) / `size` / `rotation` (curves over the duration), `sizeY` / `sizeZ` (3D size), `rotation3D`, `startColor` (colour, two colours, `{gradient, overTime}`, `{gradient, gradientMin}`, random colour), `gravity` + `gravityScale` (modifier curve), `maxParticles`, `ringBuffer`, `space` | custom simulation space dropped (no runtime transform hierarchy) |
| emission | `rate` / `perMeter` (curves), `burst` / `bursts` (time, count range, cycles — 0 = forever —, interval, `chance`) | |
| shape | sphere / hemisphere (+shell), cone (base / shell / volume: `length`, `emitFrom`), circle → `disc` / edge circle → `ring` (outward), `donut` (`donutRadius`), box / rectangle (`emitFrom` shell / edge), edge (`line`), `mesh` (built-in primitives `builtin:cube…`, FBX / OBJ / glTF copied to `models/`, `emitFrom` vertex / edge / triangle, scale in `box`), sprite → `box` (rect / pixels per unit, z 0); `shapeRotation` where the shape itself must turn (box, line, disc / ring, donut, hemisphere, mesh, and spheres / cones with an arc < 360; a full-arc cone keeps its rotation in `direction`); `randomDirection`, `sphericalDirection`, `randomPosition`, `alignToDirection`, `arc` + `arcMode` / `arcSpeed` / `arcSpread`, `thickness` | radius mode loop / ping-pong; sprite outline → its rect; a mesh renderer's own transform; skinned meshes in bind pose |
| velocityOverLifetime | `velocity` (constant, local) or `velocityOverLife` (x / y / z curves, `space`, `orbitalX/Y/Z`, `orbitalOffset` = system position + offset, `radial`, `speedModifier`) | random values of several Unity axes mixed by a rotated system share one random number |
| forceOverLifetime, limitVelocity, inheritVelocity, lifetimeByEmitterSpeed, externalForces | `force` (x / y / z, `space`, `randomize`), `speedLimit` (curve) / `speedLimitAxes`, `limitDampen`, `drag` (curve), `dragBySize` / `dragBySpeed`, `inheritVelocity` (`initial` / `current`, amount), `lifeBySpeed`, `externalForces` + effect `forceFields` (ParticleSystemForceField: sphere / cylinder, ranges, length, direction, gravity + focus, rotation + attraction, drag) | per-axis limits of a non-axis-aligned system; force-field curves → average, hemisphere / box fields → sphere, vector fields dropped |
| colour / size / rotation over lifetime and by speed | `color` + `colorMin`, `sizeOverLife` + `sizeOverLifeMin` / `Y` / `Z`, `spin` (curve) + `spin3D`, `colorBySpeed`, `sizeBySpeed`, `spinBySpeed` | separate size-by-speed / rotation-by-speed axes |
| noise | `noise` (strength curve, `axes`, frequency, scroll, octaves, `damping`, `position` / `rotation` / `size`, `remap`) | separate-axis strengths share the strongest curve's shape; scroll / amount curves → average |
| textureSheetAnimation | `frames`, `row` (index / random), `fps` / `frameOverTime` / `framesBySpeed`, `cycles`, `startFrame`, `flip`, `sprites` (each Sprite exported to `fx/textures/`) | row by mesh index |
| renderer + material | `align` (camera / velocity / ground / upright / world / local), `stretch`, `aspect`, `cameraStretch`, `meshes` (≤ 4), `pivot`, `sort`, `sortOrder` (sorting order or −fudge), `minScreenSize` / `maxScreenSize`, `hidden`; `texture`, tint folded into `color`, `blend` additive / alpha / premultiplied / multiply / subtract / opaque (cutout meshes → opaque) | cutout billboards → alpha; custom shader code / Shader Graph beyond blend + texture + colour; soft particles, distortion (need depth / grab textures — avoided on OpenGL ES 3.0); custom vertex streams and customData (only feed custom shaders) |
| collision, triggers | `collision` (`planes` from plane transforms, world → the terrain, `bounce` / `dampen` / `lifeLoss`, kill speeds, `radius`), `trigger` (sphere colliders of the prefab; box / capsule → bounding sphere; inside / outside / enter / exit, `radius`) | colliders outside the prefab; curves → average; world collision with other geometry |
| subEmitters | `subEmitters` (birth / death / collision / trigger / manual, `chance`, `inherit`, ≤ 8) → effects of their own | |
| trails, lights | `trail` (particle / `ribbon` + `ribbons`, `lifeFraction` + `time`, `ratio`, width / colour over trail, `lifeColor`, `minDistance`, `textureMode`, `dieWithParticles`, `sizeAffectsLifetime`, material slot 2), `light` (template from the prefab or any asset by GUID, `radiusOverLife` / `intensityOverLife`) | |
| nested prefabs, variants | the source prefab's systems are imported in place (modifications, removed components, `m_TransformParent`; ≤ 4 levels) | sources missing from the folder |

Not convertible, explained in the report: **VFX Graph** (`.vfx`, `VisualEffect` components — GPU node
graphs, not ParticleSystem modules), scripts (`MonoBehaviour`: bind effects to events with `fx_bind`
instead), audio, animators, scene meshes / colliders not used by a particle module. Test material:
`tests/data/unity_vfx` (synthetic, hand-written: `Assets/FX` = the classic modules, `Assets/Modules` = every
other module, nested prefabs, variants, force fields, meshes, sprites); tests:
`tools/importers/unity_vfx/test_import.py` (CTest `unity_vfx_importer`) and `fx_unity_importer_output_is_valid`
in `rts_tests`.
