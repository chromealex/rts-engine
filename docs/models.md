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
build/linux-release/rts_modelview golem.glb@Walk --baked                        # skin from the baked clip texture
```

The gallery (`--game`) cycles every type through idle → move → attack (+ hit) → death → respawn using
the game's own view code, so view blocks, overrides and procedural animation look exactly as in a
match; effect anchors are drawn as yellow markers, view events (clip events, launches, deaths) as
pink rings and listed in the corner, together with the first type's portrait. Keys: Space next clip,
Q quality, V variant, T team colour, P pause; drag/wheel for the camera. `--types a,b` limits the
gallery, `--start T --fixed-dt 0.016` make screenshots reproducible.

In the editor, **Окна → Models** shows the selected type rendered by the view (portrait), its
clips, materials, variants and nodes (the names to use in `procedural`, `attach`, `fx`, `muzzle`)
and the warnings of its view block. Model files of the open game are watched: saving a `.glb` /
`.gltf` / `.obj` / `.fbx` reloads the views. Agents get the same data through the MCP operation
`model_info` (`{"type": "footman"}`).

## Formats

| Format | What is supported |
|---|---|
| glTF 2.0 `.glb`, `.gltf` (+ `.bin`, images, data URIs) | node hierarchy, meshes (triangles / strips / fans, indexed or not), all accessor types incl. normalized and sparse (`KHR_mesh_quantization`), skins (any joint count: meshes are split into palettes of 80 joints), TRS animations with step / linear / cubic-spline keys, metallic-roughness materials with base colour / metallic-roughness / normal / occlusion / emissive textures, alpha modes, double sided, `KHR_texture_transform`, `KHR_materials_unlit`, `KHR_materials_emissive_strength`, `KHR_materials_pbrSpecularGlossiness` (converted), `KHR_materials_variants` |
| Wavefront `.obj` + `.mtl` | positions, UVs, normals, polygons, negative indices, vertex colours, groups/objects as nodes; `Kd`, `d`/`Tr`, `Ns`, `Ke`, `Pr`/`Pm`, `map_Kd`, `map_Bump`/`norm`, `map_Ke` |
| Autodesk FBX `.fbx` (binary and ASCII, FBX 6.x / 7.x, via [ufbx](https://github.com/ufbx/ufbx)) | converted on load to Y up, right-handed, metres (unit scale applied to geometry and translations, node scales stay clean; pivots and geometric transforms baked); node hierarchy, polygon meshes (triangulated), normals (generated when missing), UV0, vertex colours, tangents, one primitive per material, skins (cluster bind matrices, 4 strongest influences per vertex, unweighted vertices stay rigid with the mesh node, split into palettes of 80 joints), every animation stack becomes a clip (baked to linear T/R/S keys at 30 fps, constant channels dropped); materials from Lambert / Phong / PBR shaders: base colour × factor, roughness / metalness factors, normal map, emission, opacity (< 1 → `Blend`), double sided, unlit; textures from the file name relative to the `.fbx` (falling back to the bare file name next to it) or embedded in the file |
| built-in shapes | units `worker soldier ranged vehicle tank flyer mushroom generic`; structures `hall house barracks workshop factory tower mine tree rock mushroomhouse structure` |

Not supported (warning, skipped): `TEXCOORD_1` / extra FBX UV and colour sets, more than 4 joint
influences, cameras, lights, FBX blend shapes, vertex caches and non-transform property animation,
roughness / metalness textures of FBX materials. glTF morph targets are supported (below). Draco / meshopt geometry and KTX2 / WebP textures are an error when the file requires them
(re-export without compression). Textures: PNG, JPEG, TGA, KTX2 (below).

**KTX2 textures.** Any texture path (OBJ `map_*`, material overrides `texture` / `teamMask` /
`normalMap` / `emissiveMap`, glTF images with a plain `source`) may point at a `.ktx2` file; it is
detected by its identifier, not by extension. Payloads: ETC2 RGB8 / RGBA8, EAC R11, BC1, BC3, BC4,
BC7, ASTC 4x4 (UNORM or sRGB) or plain RGBA8, with no supercompression or Zstandard. BasisLZ/ETC1S
and UASTC need a transcoder and are rejected (as is glTF `KHR_texture_basisu`, which requires
them); 3D textures are rejected, arrays and cube maps keep the first layer/face.

Producing them with KTX-Software (`ktx create`, 4.3+): ASTC is encoded directly, e.g.
`ktx create --format ASTC_4x4_SRGB_BLOCK --generate-mipmap --zstd 18 albedo.png albedo.ktx2`;
plain RGBA8 works the same way with `--format R8G8B8A8_SRGB`. KTX-Software does not encode
ETC2/BC itself: compress with an ETC2/BC7 encoder (etcpak, Compressonator, NVIDIA Texture Tools)
straight to `.ktx2` (with Zstandard if the tool offers it; otherwise re-wrap, see `ktx create --help`
for raw block input in your KTX-Software version). Prefer
`--zstd` (supercompression scheme 2): it shrinks block data well and decodes fast. Never use
`--encode basis-lz` / `uastc`. Use `*_SRGB` formats for colour, `*_UNORM` for normal/data maps.
The parser (`engine/render/TextureCodec.h`) keeps the raw blocks of every mip in
`Image::compressed` for direct GPU upload; until the RHI uploads them, level 0 is also decoded on
the CPU into `Image::rgba`, so KTX2 textures already work everywhere, only uncompressed in VRAM.
ASTC has no CPU decoder yet: ASTC files are skipped with a warning until the GPU path lands.

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

### Skinning modes

`"skinning": "auto" | "baked" | "palette"`. **Baked** (the default whenever possible): every clip
of the model is sampled once at load (30 fps, row 0 = bind pose) into a float texture of joint
matrices; the GPU skins from it, so identical units are drawn **instanced** (32 per draw call) and
cost no CPU per frame — 300 animated golems take 61 draw calls instead of 1801. Cross-fades and
blend spaces still work (two samples mixed in the shader). **Palette**: joint matrices computed on
the CPU per object per frame — needed for per-object bone edits; `auto` switches to it when the type
uses masked overlay clips, procedural modifiers on nodes, additive clips or foot IK. In baked mode
overlay clips (`hit`, `cast`, layered attacks) play on the base layer.

### Animation LOD

Objects off screen keep their clip clocks but skip the pose; beyond 40 / 70 units from the camera
the pose is updated every 0.1 / 0.2 s (palette mode and sockets). `"lod"` swaps models by distance:

```json
"lod": [ { "model": "models/knight_lod1.glb", "distance": 25 }, { "model": "models/knight_lod2.glb", "distance": 50 } ]
```

LOD models share the base model's clips and nodes **by name** (same skeleton, fewer triangles /
materials); they get no sockets, effects or IK.


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
"layer": "base"|"overlay"|"hit"|0..2, "mask": "<node>", "events", "blend" }`. A `mask` puts the clip on an overlay layer
limited to that node's subtree, so a unit can attack while its legs keep walking. Without an
`"animations"` block clips are matched by name (`idle`, `walk`/`run`, `attack`/`shoot`,
`death`/`die`, `gather`/`work`, `hit`, `spawn`, `cast`, …). Missing clips fall back (carry → move →
idle; no idle = bind pose); a misspelt clip is reported with the list of clips the file has.

**Blend space** (`move` / `carry`): `"blend": [{"clip": "Walk", "speed": 1.4}, {"clip": "Run", "speed":
3.2}]` mixes the two clips around the current speed, phase-synchronized (feet stay in step), and
scales the playback rate to the real speed.

**Clip events**: `"events": {"0.35": "hit", "0.0": "step", "0.5": "step"}` — clip time → name. They
are reported in `RtsView::events()` every time the clip passes that time (loops included).

**Additive clips** on top of any action (breathing, wounded limp, recoil):
`"additive": [{"clip": "Breathe", "when": "idle", "weight": 1}]` — adds `clip(t) - clip(0)`; the
weight fades with the state.

**Foot IK**: `"footIK": [{"hip": "thigh.L", "knee": "shin.L", "foot": "foot.L"}, ...]` — two-bone legs
put their feet on the terrain under them when standing (ramps, cliffs, slopes).

**Morph targets** (glTF: blend shapes for faces, squash, damage, wobble): up to 8 per primitive,
deformed on the GPU from a float texture; weights come from the node / mesh defaults and from
`weights` animation channels, so clips drive them like bones.

## Sockets, muzzles, effects and events

```json
"attach": [
  { "node": "hand.R", "model": "models/sword.glb", "offset": [0, -0.1, 0], "rotate": [-90, 0, 0], "scale": 1 },
  { "node": "spine", "model": "models/sack.glb", "carry": "gold" },
  { "node": "spine", "model": "models/banner.glb", "when": "moving" }
],
"muzzle": { "node": "Barrel", "offset": [0, 0, 0.7] },
"fx": [ { "effect": "smoke", "node": "Chimney", "offset": [0, 0.3, 0], "hpBelow": 0.5 },
        { "effect": "exhaust", "node": "Hull", "when": "moving", "intensity": 0.6 } ]
```

- **attach** draws another model on a node of this one (`rotate` in degrees, `when` = states,
  `carry` = resource id or `"any"`: shown while carrying it). Materials named `*team*` take the
  team colour.
- **muzzle**: projectiles of this type start at that point and converge onto their simulated path;
  a `launch` view event is reported there.
- **fx**: effect anchors listed in `RtsView::effects()` every frame while active (`when`, `hpBelow`;
  the intensity grows as health drops). A particle system spawns `effect` at `pos`.
- **View events** (`RtsView::events()`, this frame): clip events, `launch`, `spawn`, `death`, each
  with entity, type, owner and world position — the hook for sounds and particles.

## Stages, construction and dissolve

```json
"stages": [
  { "build": 0, "model": "models/scaffold.glb" },
  { "build": 0.6 },
  { "hp": 0.5, "variant": "burnt", "fx": [ { "effect": "fire", "offset": [0, 1.5, 0] } ] }
],
"construction": "stages",
"dissolveSpawn": true, "dissolveDeath": true
```

- **stages** replace the model (or only its material `variant`) by construction progress (`build`:
  the stage with the highest threshold ≤ progress) or by damage (`hp`: the lowest threshold ≥ health).
  A stage may bring its own `fx`. A stage with another model keeps only whole-object procedural
  modifiers.
- **construction**: `grow` (default: the model rises from the ground), `dissolve` (materializes
  with the progress, glowing edge) or `stages` (default when stages have `build`).
- **dissolve**: `dissolveSpawn` burns new units in, `dissolveDeath` burns corpses away (instead of
  fading); `"dissolve": true` sets both.

Selected and hovered objects get an outline (`RtsView::setSelectionOutline`). The client shows the
building to place as a translucent model (`RtsView::drawGhost`), and `game::Portrait`
(`modules/rts/view/RtsPortrait.h`) renders any type into a texture for HUD portraits and buttons.

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
| `look` | head turns towards the target | `node`, `limit` (70°), `turnSpeed` |
| `align` | tilt onto the terrain slope (vehicles, buildings) | `node` (optional), `amp` (0..1), `turnSpeed` |
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

`games/showcase` is "Iron and Gold" with imported views:
- footman: skinned golem in palette mode — blend space Walk/Run, clip events, sword on the right
  arm, additive breathing, lean, shake on hit, dissolve on death;
- archer: the same golem baked (instanced), a far LOD, muzzle on the hand, dissolve on spawn;
- peasant: built-in shape carrying a gold sack or logs on its back;
- catapult: tank — variant, team mask, turret aim, barrel recoil, wheels, terrain alignment, muzzle,
  exhaust effect while moving;
- gryphon: a morph-target slime (wobble clip on weights);
- farm: windmill (.gltf + .bin + .png, alpha-masked sails spinning) built through a scaffold stage,
  burnt variant and smoke below half health;
- tower: OBJ crystal (translucent, team tint, KTX2 ETC2 texture) that dissolves in while built.

The models are generated by `tools/models/gen_sample_models.py` (CC0).

## Engine API (for engine work)

- `render::loadModel(path, reader, &warnings)` → `Model` (CPU), `uploadModel(dev, model)` (morph
  texture, KTX2 blocks kept compressed when `Device::supports` the format).
- `render::Animator` (clip layers, cross-fades, masks, blend spaces, additive slots, `baseSample()`),
  `bindPose / sampleClip / sampleClipAdditive / worldMatrices / solveTwoBoneIK`,
  `bakeAnimations(dev, model)` + `bakedRow`, `applyProcedural(mods, inputs, runtime, pose, objectXf)`.
- `Renderer3D::drawModel(ModelDraw)` — instance transform, pose or baked sample, morph weights,
  material overrides, tint, team colour, hit flash, dissolve, outline, variant; frustum culling,
  state sorting with instanced batches (32 per draw), blended back-to-front, shadows with skinning,
  morphs and alpha test. Shaders: `engine/render/ModelShaders.h` (GLSL + MSL permutations:
  instanced, palette / baked skinning, morph, normal map, alpha mask, outline, quality tier).
- RHI additions: `drawIndexed(..., instanceCount)`, `PixelFormat::RGBA32F` (data textures read in
  vertex shaders), block-compressed formats (`ETC2_*`, `BC1/3/7`, `ASTC_4x4`) with `supports()`,
  texture mipmaps.
- CMake `RTS_FBX=OFF` drops the FBX importer (ufbx, ~0.5 MB of code) from size-sensitive builds.
