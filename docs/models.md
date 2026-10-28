# Models, materials and animation

Every object type is drawn from the `"view"` block of its type in `rules.json`. A view is either a
**built-in shape** (procedural, always available) or one or more **model files**. Both go through the
same pipeline: PBR materials, team colours, skeletal clips and procedural animation.

```json
"footman": {
  "view": {
    "model": "models/knight.glb",          // or ["models/tree1.glb", "models/tree2.glb"]: one picked per object
    "height": 1.1,                          // fit: scale so the model is this tall (world units = map cells)
    "animations": { "move": { "clip": "Walk", "speedRef": 2.6 }, "attack": "Slash", "die": "Death" },
    "team": ["Cloth", "Banner"],
    "procedural": [ { "type": "lean" } ]
  }
}
```

A model that fails to load (missing file, unsupported feature) falls back to the shape with a warning
`type 'footman': models/knight.glb: file not found (using the built-in shape)`. Warnings go to stderr,
to the editor console and to `RtsView::warnings()`. Nothing in a view block can stop a game from
starting.

Check a model or a whole game with the viewer:

```sh
build/linux-release/rts_modelview games/mygame/models/knight.glb@Walk          # one model, one clip
build/linux-release/rts_modelview a.glb b.gltf c.obj --quality low              # side by side, low tier
build/linux-release/rts_modelview --game games/showcase                         # every type, as the game draws it
```

The gallery (`--game`) cycles every type through idle → move → attack (+ hit) → death → respawn using
the game's own view code, so view blocks, overrides and procedural animation look exactly as in a
match. Keys: Space next clip, Q quality, V variant, T team colour, P pause; drag/wheel for the camera.

## Formats

| Format | What is supported |
|---|---|
| glTF 2.0 `.glb`, `.gltf` (+ `.bin`, images, data URIs) | node hierarchy, meshes (triangles / strips / fans, indexed or not), all accessor types incl. normalized and sparse (`KHR_mesh_quantization`), skins (any joint count: meshes are split into palettes of 80 joints), TRS animations with step / linear / cubic-spline keys, metallic-roughness materials with base colour / metallic-roughness / normal / occlusion / emissive textures, alpha modes, double sided, `KHR_texture_transform`, `KHR_materials_unlit`, `KHR_materials_emissive_strength`, `KHR_materials_pbrSpecularGlossiness` (converted), `KHR_materials_variants` |
| Wavefront `.obj` + `.mtl` | positions, UVs, normals, polygons, negative indices, vertex colours, groups/objects as nodes; `Kd`, `d`/`Tr`, `Ns`, `Ke`, `Pr`/`Pm`, `map_Kd`, `map_Bump`/`norm`, `map_Ke` |
| Autodesk FBX `.fbx` (binary and ASCII, FBX 6.x / 7.x, via [ufbx](https://github.com/ufbx/ufbx)) | converted on load to Y up, right-handed, metres (unit scale applied to geometry and translations, node scales stay clean; pivots and geometric transforms baked); node hierarchy, polygon meshes (triangulated), normals (generated when missing), UV0, vertex colours, tangents, one primitive per material, skins (cluster bind matrices, 4 strongest influences per vertex, unweighted vertices stay rigid with the mesh node, split into palettes of 80 joints), every animation stack becomes a clip (baked to linear T/R/S keys at 30 fps, constant channels dropped); materials from Lambert / Phong / PBR shaders: base colour × factor, roughness / metalness factors, normal map, emission, opacity (< 1 → `Blend`), double sided, unlit; textures from the file name relative to the `.fbx` (falling back to the bare file name next to it) or embedded in the file |
| built-in shapes | units `worker soldier ranged vehicle tank flyer mushroom generic`; structures `hall house barracks workshop factory tower mine tree rock mushroomhouse structure` |

Not supported (warning, skipped): morph targets / blend shapes, `TEXCOORD_1` / extra FBX UV and colour sets,
more than 4 joint influences, cameras, lights, FBX vertex caches, FBX property (non-transform)
animation, roughness / metalness textures of FBX materials. Draco / meshopt geometry and KTX2 / WebP textures are an error when the file requires them
(re-export without compression). Textures: PNG, JPEG, TGA.

Conventions: Y up, glTF assets face +Z (`"forward"` changes it), one world unit = one map cell. Model
files are read through the content system (loose files, `.rpak` packs, Android assets, web preload),
paths are relative to the game folder; glTF buffers and images are relative to the model file.

## Placement

| key | default | meaning |
|---|---|---|
| `shape` | per kind | built-in shape (used when there is no `model` or it fails) |
| `model` | — | model path or array of paths (variety: the object id picks one) |
| `scale` | 1 | final multiplier |
| `height` | — | scale the model so its bind-pose height is this many units |
| `fit` | true | structures without `height`: scale the model to its footprint (keeps proportions) |
| `center` | true | put the bounds centre at the object position and the lowest point on the ground |
| `forward` | `+z` (models), `+x` (shapes) | the model's forward axis: `+x -x +z -z` |
| `yaw` | 0 | extra rotation, degrees |
| `offset` | [0,0,0] | after fitting |
| `color` | [1,1,1] | tint (like player colours: ~sRGB) |
| `flyHeight` | 2.4 | air units |
| `turnSpeed` | 10 (units), 0 (structures) | facing smoothing, rad/s; 0 = instant |
| `corpse` | 2.5 (units), 3 (structures) | seconds a dead object stays (death clip, fall, sink, fade) |

## Materials

Imported materials keep their PBR values. Overrides per material name, `"*"` for all:

```json
"materials": {
  "*":     { "roughness": 0.7 },
  "Cloth": { "team": "mask", "teamMask": "models/knight_team.png" },
  "Glass": { "alpha": "blend", "color": [0.6, 0.8, 1, 0.5], "emissive": [0.2, 0.4, 1], "emissiveStrength": 2 },
  "Leaves":{ "alpha": "mask", "cutoff": 0.4, "doubleSided": true, "castShadow": true }
}
```

| key | meaning |
|---|---|
| `color` | base colour factor `[r,g,b(,a)]` (linear) |
| `metallic`, `roughness` | factors (multiplied with the metallic-roughness texture) |
| `emissive`, `emissiveStrength` | glow (linear colour × strength) |
| `alpha` | `opaque`, `mask` (alpha test, `cutoff`), `blend` (sorted, no shadow) |
| `doubleSided`, `unlit`, `castShadow`, `receiveShadow`, `vertexColors` | switches |
| `team` | where the player colour goes: `tint` (whole material × team colour), `mask` (red channel of `teamMask`), `alpha` (base texture alpha 0 = team colour, Warcraft III style), `vertex` (vertex colour alpha 0 = team colour), `none` |
| `teamStrength` | 0..1 |
| `teamMask`, `texture`, `normalMap`, `emissiveMap` | replace / add textures (paths relative to the game) |
| `uvScale` | `[u, v]` tiling |

`"team"` on the view (not on a material) picks the tinted materials: `"all"`, `"none"` or a list of
names. Without it, materials whose name contains `team` are tinted (`TeamCloth`, `Banner_Team`), and
the built-in shapes tint their team parts.

`"variant": "winter"` selects a `KHR_materials_variants` variant of the file.

Quality tiers (`MaterialQuality`, chosen per GPU like shadows, `RTS_MATERIALS=low|medium|high`):
**low** — Blinn-Phong, material factors and base colour texture only (weak Android GPUs);
**medium** — GGX, metallic-roughness, normal and emissive maps; **high** — plus occlusion maps.

## Skeletal clips

States of an object map to clips:

| state | when | default loop |
|---|---|---|
| `idle` | nothing else | yes |
| `move` / `carry` | moving / moving with cargo | yes |
| `attack` | on every attack event (base layer when standing, or an overlay layer) | no |
| `gather`, `build` | gathering / building or repairing, standing | yes |
| `work` | structure producing | yes |
| `construct` | structure under construction | yes |
| `spawn` | trained / spawned | no |
| `hit` | damaged (overlay layer 2) | no |
| `cast` | ability used (overlay layer 1) | no |
| `die` | death (the corpse plays it, then sinks/fades) | no |

```json
"animations": {
  "idle": "Idle",
  "move": { "clip": "Run", "speedRef": 3.0 },             // playback speed follows the real speed
  "attack": { "clip": ["Attack1", "Attack2"], "mask": "Spine" },   // random pick; overlay on the upper body
  "die": "Death",
  "hit": { "clip": "Flinch", "layer": "hit", "speed": 1.5 }
}
```

Clip spec: a name, an array of names (random) or `{ "clip", "speed", "speedRef", "loop", "fade",
"layer": "base"|"overlay"|"hit"|0..2, "mask": "<node>" }`. A `mask` puts the clip on an overlay layer
limited to that node's subtree, so a unit can attack while its legs keep walking. Without an
`"animations"` block clips are matched by name (`idle`, `walk`/`run`, `attack`/`shoot`,
`death`/`die`, `gather`/`work`, `hit`, `spawn`, `cast`, …). Missing clips fall back (carry → move →
idle; no idle = bind pose); a misspelt clip is reported with the list of clips the file has.

## Procedural animation

`"procedural"` is a list of modifiers applied after the clips. Each targets a `node` of the model
(by name) or, without `node`, the whole object. They work on rigid models, skinned models and shapes.

| preset | does | main keys |
|---|---|---|
| `bob` | hop while moving (by distance) | `amp`, `freq` (steps per unit) |
| `hover` | float up and down | `amp`, `freq` |
| `sway` | rock around an axis | `amp` (deg), `freq`, `axis` |
| `spin` | continuous rotation (rotors, windmills) | `spin` (deg/s), `axis` |
| `wheel` | roll with travelled distance | `radius`, `axis` |
| `pulse` | breathe in scale | `amp`, `freq` |
| `recoil` | kick back on attack | `amp`, `axis`, `decay` |
| `lunge` | jab forward on attack | `amp`, `decay` |
| `shake` | jitter when hit | `amp`, `freq`, `decay` |
| `flap` | wings | `amp` (deg), `freq` |
| `pop` | grow in on spawn | `amp`, `decay` |
| `squash` | squash on attack | `amp` |
| `lean` | bank into turns | `amp`, `limit` (deg) |
| `aim` | turn towards the combat target (turrets) | `node`, `limit` (deg), `turnSpeed` (deg/s) |
| `aimPitch` | raise towards the target (barrels) | `node`, `limit`, `turnSpeed` |
| `fall` | tip over on death | `amp` (deg), `duration` |
| `sink` | sink into the ground on death | `amp`, `duration` |
| `custom` | build your own | everything below |

All keys: `node`, `channel` (`translate` / `rotate` / `scale`), `axis`, `amp` (units; degrees for
rotations), `wave` (`sine abs saw triangle square const`), `freq`, `by` (`time` / `distance`),
`phase`, `randomPhase` (default true: identical units move out of sync), `spin`, `on` (`attack hit
spawn death`: amplitude follows an envelope with `attack` rise and `decay`), `when` (state or list:
`idle moving attacking gathering building carrying constructing producing dead air`, weight fades
with `fade`), `limit`, `turnSpeed`, `scaleBySpeed` + `speedRef`.

Defaults: without a `"procedural"` key, units get `bob` (if no move clip), `hover` (air), `lunge` (no
attack clip), `pop` (no spawn clip), `fall` + `sink` (no death clip); structures `sink`. An explicit
list replaces the defaults (`"proceduralDefaults": true` keeps them too; `[]` turns everything off).

## Example

`games/showcase` is "Iron and Gold" with imported views: a skinned golem (clips, normal map, emissive
eyes, team cloth), a tank (variants, team mask, turret aim, barrel recoil, wheels), a windmill
(.gltf + .bin + .png, alpha-masked sails spinning) and an OBJ crystal tower (translucent, team tint).
The models are generated by `tools/models/gen_sample_models.py` (CC0).

## Engine API (for engine work)

- `render::loadModel(path, reader, &warnings)` → `Model` (CPU), `uploadModel(dev, model)`.
- `render::Animator` (clip layers, cross-fades, masks), `bindPose / sampleClip / worldMatrices`,
  `applyProcedural(mods, inputs, runtime, pose, objectXf)`.
- `Renderer3D::drawModel(ModelDraw)` — instance transform, pose, material overrides, tint, team
  colour, hit flash, variant; frustum culling, opaque state sorting, blended back-to-front, shadows
  with skinning and alpha test. Shaders: `engine/render/ModelShaders.h` (GLSL + MSL permutations).
