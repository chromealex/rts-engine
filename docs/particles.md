# Particles

Presentation-only particle effects: CPU simulation (`engine/fx/ParticleSystem`), drawn through the
RHI with one dynamic vertex buffer per frame. The simulation never sees them; effects are started from
`SimEvent`s by `modules/rts/fx/RtsFx` (the same read-only event collector as `RtsAudio`).

## Data (per game)

```
games/<id>/fx/fx.json        which effect plays for which event (bindings)
games/<id>/fx/<name>.json    one effect = 1..16 emitters
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
    --game games/<id> [--name fire] [--prefab Explosion] [--scale 1] [--ignore-root-rotation] [--json] [--dry-run]
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

