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
`fx/textures/*.png`, and a report (`--json`) of what was mapped exactly (`systems[].modules`),
approximately (`approximate`) or not at all (`dropped`).

Space: Unity is Y-up left-handed; engine world = Unity (x, y, −z), and emitter directions are written in
the effect frame (play direction +Y). The prefab root's rotation is kept (`--ignore-root-rotation` for
prefabs instantiated with identity). `--scale` converts Unity metres to game units (sizes, speeds, radii,
gravity, noise, lights).

| Unity module | Engine key | Notes |
|---|---|---|
| main: duration, looping, startDelay | `duration`, `loop`, `delay` | random delay → its middle; prewarm dropped |
| startLifetime / startSpeed / startSize (3D) / startRotation | `life`, `speed`, `size` (+`aspect`), `rotation` | curves over the system duration → their range; negative speed reverses the direction |
| startColor (colour / two colours / gradient / random) | `startColor` | gradient over duration → random from the gradient |
| gravityModifier, forceOverLifetime | `gravity` | force added to gravity (local forces rotated once) |
| simulationSpace, maxParticles, simulationSpeed, scalingMode | `space`, `maxParticles`, folded into times/speeds/sizes | custom space → world |
| emission: rateOverTime / rateOverDistance / bursts | `rate`, `perMeter`, `burst` / `bursts` (time, count, cycles, interval) | curves → average; probability → fewer particles |
| shape: sphere, hemisphere (+shell) | `sphere` / `hemisphere`, `radial: 1`, `thickness`, `arc` | |
| shape: cone (base / shell / volume), circle, edge circle, donut | `cone` (`spread` = angle at the rim), `radial: 1` for circles | cone volume → base, donut → its circle |
| shape: box (+shell/edge), rectangle, edge | `box`, `line` | mesh / sprite shapes → sphere of the shape radius |
| velocityOverLifetime / limitVelocity (+drag) | `velocity`, `speedLimit`, `drag` | orbital / radial dropped; dampen → hard limit |
| colorOverLifetime, sizeOverLifetime, rotationOverLifetime | `color` (colour + alpha keys, fixed mode as steps), `sizeOverLife`, `spin` | Hermite curves sampled and simplified |
| colorBySpeed, sizeBySpeed | `colorBySpeed`, `sizeBySpeed` | |
| noise | `noise` (strength, frequency, scroll) | octaves → one, rotation/size amount dropped |
| textureSheetAnimation | `frames`, `fps` / `cycles`, `startFrame` | single row → whole sheet, sprite mode dropped |
| trails (particle mode) | `trail` (time = ratio × lifetime, width / colour over trail, material slot 2) | ribbon mode dropped |
| lights | `light` (Light template colour / range / intensity, ratio, max) | |
| collision (planes / world) | `collision` (ground + plane height, bounce, dampen, lifetime loss, min kill speed) | one horizontal plane |
| subEmitters birth / death / collision | `subEmitters` → effects of their own | trigger / manual dropped; inherit colour only |
| renderer: billboard / stretched / horizontal / vertical / mesh / none | `align` camera / velocity (`stretch` = velocity scale, `aspect` = length scale) / ground / upright, `hidden` | mesh particles → billboards |
| material: _MainTex / _BaseMap, _TintColor (×2) / _Color / _BaseColor | `texture`, tint folded into `color` | |
| material blend: _SrcBlend/_DstBlend, URP _Surface/_Blend, Standard _Mode, `.shader` Blend, Shader Graph | `blend` additive / alpha / premultiplied | multiply / subtractive → nearest; custom shader code, soft particles, distortion dropped |

Not convertible, explained in the report: **VFX Graph** (`.vfx`, `VisualEffect` components — GPU node
graphs, not ParticleSystem modules), scripts (`MonoBehaviour`: bind effects to events with `fx_bind`
instead), audio, animators, meshes, nested prefab instances (import them separately). Test material:
`tests/data/unity_vfx` (synthetic, hand-written); tests: `tools/importers/unity_vfx/test_import.py`
(CTest `unity_vfx_importer`) and `fx_unity_importer_output_is_valid` in `rts_tests`.

