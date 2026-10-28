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

With several models one is picked per object. An entry can be an object: `"file"` plus any view
keys that differ for that model (they replace the type's keys), e.g. a glTF tank and a voxel tank
with other node names in one type:

```json
"model": [ "models/tank.glb",
           { "file": "models/voxel_tank.vox", "variant": "", "materials": {},
             "procedural": [ { "type": "aim", "node": "Turret" } ] } ]
```

A model that fails to load (missing file, unsupported feature) falls back to the shape with a warning
`type 'footman': models/knight.glb: file not found (using the built-in shape)`. Warnings go to stderr,
to the editor console and to `RtsView::warnings()`. Nothing in a view block can stop a game from
starting.

Check a model or a whole game with the viewer:

```sh
build/linux-release/rts_modelview templates/mygame/models/knight.glb@Walk          # one model, one clip
build/linux-release/rts_modelview a.glb b.gltf c.obj --quality low              # side by side, low tier
build/linux-release/rts_modelview --game templates/showcase                         # every type, as the game draws it
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
and the warnings of its view block. Model files of the open game are watched: saving any model file
reloads the views (converted formats are re-converted first, see below). Agents get the same data through the MCP operation
`model_info` (`{"type": "footman"}`).

## Formats

| Format | What is supported |
|---|---|
| glTF 2.0 `.glb`, `.gltf` (+ `.bin`, images, data URIs) | node hierarchy, meshes (triangles / strips / fans, indexed or not), all accessor types incl. normalized and sparse (`KHR_mesh_quantization`), skins (any joint count: meshes are split into palettes of 80 joints), TRS animations with step / linear / cubic-spline keys, metallic-roughness materials with base colour / metallic-roughness / normal / occlusion / emissive textures, alpha modes, double sided, `KHR_texture_transform`, `KHR_materials_unlit`, `KHR_materials_emissive_strength`, `KHR_materials_pbrSpecularGlossiness` (converted), `KHR_materials_variants` |
| Wavefront `.obj` + `.mtl` | positions, UVs, normals, polygons, negative indices, vertex colours, groups/objects as nodes; `Kd`, `d`/`Tr`, `Ns`, `Ke`, `Pr`/`Pm`, `map_Kd`, `map_Bump`/`norm`, `map_Ke` |
| Autodesk FBX `.fbx` (binary and ASCII, FBX 6.x / 7.x, via [ufbx](https://github.com/ufbx/ufbx)) | converted on load to Y up, right-handed, metres (unit scale applied to geometry and translations, node scales stay clean; pivots and geometric transforms baked); node hierarchy, polygon meshes (triangulated), normals (generated when missing), UV0, vertex colours, tangents, one primitive per material, skins (cluster bind matrices, 4 strongest influences per vertex, unweighted vertices stay rigid with the mesh node, split into palettes of 80 joints), every animation stack becomes a clip (baked to linear T/R/S keys at 30 fps, constant channels dropped); materials from Lambert / Phong / PBR shaders: base colour × factor, roughness / metalness factors, normal map, emission, opacity (< 1 → `Blend`), double sided, unlit; textures from the file name relative to the `.fbx` (falling back to the bare file name next to it) or embedded in the file |
| Collada `.dae` (1.4 / 1.5, UTF-8 / UTF-16) | `<unit>` and `Y_UP` / `Z_UP` / `X_UP` applied through a `root` node; node hierarchy from `<matrix>` / translate / rotate / scale / lookat stacks, `instance_geometry` / `instance_controller` / `instance_node`; `triangles`, `polylist`, `polygons`, `trifans`, `tristrips` with positions, normals (smoothed when missing), first UV set, vertex colours; profile_COMMON phong / blinn / lambert / constant (diffuse colour or texture via sampler → surface → image, emission, transparency, shininess → roughness, double sided, bump → normal map) bound per instance; skins (bind-shape matrix, joints by sid / id / name under `<skeleton>`, 4 strongest weights, palettes of 80 joints); animations (nested, matrix and single-value targets like `rotZ.ANGLE`, LINEAR / STEP / BEZIER) split by `library_animation_clips` (else one clip `Default`); skeleton-only files with clips (0 A.D.) load with a warning. Not supported (warning): morph controllers, material / visibility animation, `<skew>`, extra UV sets, lines, splines, embedded images, HERMITE (linear) |
| 3D Studio `.3ds` | meshes with UVs and the local matrix, materials (diffuse, shininess, transparency, texture map; 8.3 names matched case-insensitively), smoothing groups split normals, keyframer hierarchy and tracks → node tree + clip `Take 001`; Z up converted. Ignored: bump / specular / opacity maps, objects parented to cameras or lights keep no animation |
| Stanford `.ply` (ASCII, binary LE / BE) | any property types, positions, normals (smoothed when missing), UVs (`s/t`, `u/v`, `texture_u/v`, per-corner `texcoord`), vertex colours, polygon lists, `comment TextureFile`; Y up as Stanford / MeshLab write it (`comment up_axis Z` converts). Point clouds are an error |
| `.stl` (ASCII and binary) | several solids, facet normals (recomputed when zero), VisCAM / Materialise colours; Z up converted |
| MagicaVoxel `.vox` | models, palette (default when absent), scene graph (transform / group / shape nodes with names, translations, rotations; shared meshes; hidden nodes and layers skipped), greedy-meshed visible faces with flat normals, palette colours as vertex colours, `MATL` emission and metal; Z up converted, MagicaVoxel's front (−Y) faces +Z; **team colour: the last 8 palette slots (colour indices 248–255, the bottom row of MagicaVoxel's palette)** — their brightness shades the team colour; several frames → clip `Idle` (10 fps). Glass / transparency ignored |
| Tiberian Sun / Red Alert 2 `.vxl` + `.hva` | from the user's own copy. Every section is a node named like it (`Body`, `Turret`, `Barrel`: aim / recoil presets work), greedy-meshed; palette from the file, else `<name>.pal`, `unittem.pal` / `unit.pal` next to it or one folder up (grey ramp + warning otherwise); remap range (16–31) = team colour shaded by the ramp; HVA frame 0 is the bind pose, more frames → clip `Idle` (15 fps); a `barrel` section follows the `turret`; Z up / facing +X converted. Normal tables ignored (flat normals) |
| Warcraft III `.mdx` (binary) / `.mdl` (text), versions 800 and Reforged 900–1100, + `.blp` textures (BLP1 JPEG / paletted, BLP2 paletted / DXT1/3/5 / BGRA) | from the user's own copy or community models (never committed). Z up / facing +X converted to Y up / facing +Z, 128 units = 1 cell. Every object (bone, helper, attachment, emitter, ...) is a node under `root` (attachments like `Hand Right Ref`, `Overhead Ref` work as sockets); pivot-based transforms become TRS nodes (bind translation = pivot − parent pivot, inverse bind = T(−pivot)); classic vertex groups (bones averaged, ≤ 4) or Reforged `SKIN` weights; every sequence is a clip named like it (`Stand - 2`, `Attack Slam`), keys in [start, end] plus loop-seam boundary keys, Hermite / Bezier → cubic spline, global sequences tiled per clip. Materials: team-colour layer (replaceable id 1) under the texture → `team: alpha` (BaseAlpha), only team colour → tint, team glow → tinted glow sprite; filter Transparent → alpha test 0.75, Blend / Additive / Modulate → blend (approximated); unshaded, two-sided; HD: diffuse / normal / ORM (+ team mask from ORM alpha) / emissive. Textures are looked up relative to the model, by file name next to it, then relative to its parent folders (WC3 archive paths like `Units\Human\Footman\Footman.blp` work if the folder layout is kept), then as converted `.png` / `.tga` / `.dds`. Not rendered: particles, ribbons, lights; geoset alpha animation (geosets hidden in `Stand` are dropped), billboards, texture animation. Test unit: `tools/models/gen_mdx_sample.py` |
| Total Annihilation `.3do` (+ `.gaf` textures) | from the user's own copy. Every piece is a node with its exact name (`base`, `turret`, `sleeve`, `barrel`, `flare`: aim / recoil / spin presets move them), offsets from 16.16 fixed point, (x, y, −z) as Spring reads it (faces +Z); polygons fan-triangulated, flat shaded (selection primitive, lines and duplicate faces skipped). Colours from the TA palette: `<model>.pal`, `palette.pal` or `../palettes/PALETTE.PAL` (built-in approximation + warning otherwise). Textures from GAF (`<name>.gaf`, or a converted `.png` / `.tga` / `.jpg`, in the model folder, `textures/`, `../textures/`); missing ones keep the palette colour (one warning). Team colour: GAF entries with ≥ 10 frames (one per player) → mask of the pixels that differ between frames 0 and 1; converted textures listed in `teamtex.txt` use their magenta pixels. No clips (TA animates pieces from scripts — use `procedural`) |
| C&C Generals / Zero Hour `.w3d` (+ `.dds` / `.tga`) | from the user's own copy. Hierarchy pivots → nodes (names kept: `Turret`, `Barrel`, `FireFX01`), HLOD with the highest-detail LOD (or the older HMODEL), meshes (first material pass: textures, vertex colours, alpha test / blend, unlit), skins (one or two bone influences, palettes of 80 joints), animations (uncompressed, time-coded and adaptive-delta channels) named after the part behind the dot (`ABTank_SKN.ABTank_ATKA` → `ABTank_ATKA`); Z up / +X → Y up / +Z. **Separate files:** a hierarchy the model names but doesn't contain is read from `<HierarchyName>.w3d` next to it (with its animations); more animations are listed in `<model>.anims.txt` next to the model (one `.w3d` per line, `#` comments). Textures: the name, then `.dds` / `.tga`, in the model folder, `../Textures/`, `../textures/`, `textures/`, `Art/Textures/`. Team colour: meshes named `HOUSECOLOR…` → tint, textures `ZHCA…` → alpha (BaseAlpha), `ZHCD…` → tint (approximation, warning). Not supported: visibility / morph channels, Euler channels, passes after the first, emitters, pre-3.0 meshes |
| built-in shapes | units `worker soldier ranged vehicle tank flyer mushroom generic`; structures `hall house barracks workshop factory tower mine tree rock mushroomhouse structure` |

Not supported (warning, skipped): `TEXCOORD_1` / extra FBX UV and colour sets, more than 4 joint
influences, cameras, lights, FBX blend shapes, vertex caches and non-transform property animation,
roughness / metalness textures of FBX materials. glTF morph targets are supported (below). Draco / meshopt geometry and KTX2 / WebP textures are an error when the file requires them
(re-export without compression). Textures: PNG, JPEG, TGA, BMP, KTX2 (below), BLP and DDS (classic formats, detected by content).

**DDS textures** (W3D and any texture path; detected by content): DXT1–5, ATI1/BC4, ATI2/BC5
(normal maps: blue rebuilt), DX10 headers, uncompressed bit-mask formats (A8R8G8B8, R5G6B5, L8, …);
BC1 / BC3 / BC7 keep all mips compressed for the GPU, like KTX2. Signed and volume DDS are rejected.

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

### Converted formats (`modelconv`)

Everything else that [assimp](https://github.com/assimp/assimp) imports — Blender `.blend` (up to
2.7x), LightWave `.lwo` / `.lws` / `.lxo`, DirectX `.x`, MilkShape `.ms3d`, Quake `.md2` / `.md3` /
`.md5mesh`, Valve `.smd`, Blitz3D `.b3d`, AC3D `.ac`, 3ds Max `.ase`, `.3mf`, `.off`, `.cob`, `.iqm`,
`.irr`, `.ogex`, `.x3d`, `.pmx`, `.vrm` and more (`modelconv --formats` lists them) — is converted on
the host into a glTF binary **sidecar** next to the source:

```
<project>/models/ship.lwo       <- referenced by rules.json as "models/ship.lwo"
<project>/models/ship.lwo.glb   <- what the game loads
```

`render::loadModel` reads `<path>.glb` for any path whose format the runtime doesn't read, so
`rules.json` keeps the original name and the source stays the file you edit. Who converts:

- **the editor**: when a type uses such a model, and every time the file is saved (Models window,
  the same watcher as hot reload); the console shows `сконвертирована модель …` or the error;
- **rtsbuild**: every build converts the game's convertible models that have no sidecar yet, ships
  only the `.glb` and leaves the sources out of the pack (`модель models/ship.lwo: converted to …`);
- **by hand / agents**: `modelconv models/ship.lwo` (one file; `--json` for scripts and MCP),
  `modelconv --project path/to/mygame` (every model of a game, skips up-to-date sidecars; `--force`).

The converter triangulates, generates missing normals, keeps 4 bone weights per vertex, names
unnamed clips (`Default`, `Clip1`, …), and rewrites texture paths from the authoring machine
(`C:\art\ship.png`) to the file next to the model; textures are not embedded, they ship as they are
(PNG, JPEG, TGA, BMP, KTX2, DDS). Meshes whose bones are missing from the hierarchy are exported
unskinned (warning). Files assimp can't read are reported (`modelconv: models/old.blend: …`), never
a crash: on Linux and macOS each conversion runs in a child process.

`modelconv` needs assimp (BSD-3): configure the tools with `-DRTS_ASSIMP=ON` — an installed assimp
(`find_package`) is used, otherwise v5.4.3 is downloaded from GitHub once (`RTS_FETCH_ASSIMP`).
Without it (the default) the tools still build: sidecars you already have are used and packed, models
without one are reported by rtsbuild. The game itself never links assimp.

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

### Material library (`materials.json`)

Materials shared by many types live in `materials.json` next to `rules.json`; a view refers to them
by name wherever a material override goes, and can give a node its own material:

```json
// materials.json
{ "materials": {
    "steel_plates": { "color": [1, 1, 1], "metallic": 0.35, "roughness": 0.5, "uvScale": [2, 2],
                      "texture": "textures/steel_plates.png", "normalMap": "textures/steel_plates_normal.png" },
    "rusty_plates": { "base": "steel_plates", "color": [0.8, 0.5, 0.35], "roughness": 0.85 } } }

// rules.json, a type's "view"
"materials": { "Hull": "rusty_plates", "*": { "use": "steel_plates", "roughness": 0.3 } },
"assign":    { "Turret": "steel_plates", "Wheel*": { "color": [0.1, 0.1, 0.1], "roughness": 0.9 } }
```

- A library entry has the keys of a material override (table above) plus `base` (inherit another
  entry; chains up to 8) and `description`.
- In `"materials"` a value is an override object, a library name, or `{"use": "name", ...}` (the
  library material with some keys changed). The keys change the model's material; textures the
  model has stay unless the reference sets them; `"replace": true` starts from a blank material.
- `"assign"` maps node names or globs (case-insensitive `*`, `?`) to a material: the meshes of the
  node **and everything under it** get a fresh material (`"Turret"` covers the barrel on it). Later
  entries win. The fresh material multiplies the model's vertex colours (voxel colours show through
  a texture); `"vertexColors": false` ignores them.
- Normal maps work on any model: primitives without tangents get them when the model is uploaded;
  voxel meshes get box-projected UVs (16 voxels per texture repeat).

**Procedural textures.** `texture_generate` (MCP and the Materials window) writes tileable PNGs
into the game folder: `solid`, `noise`, `stripes`, `checker`, `brick`, `tiles`, `panels` (riveted
plates), `metal` (brushed), `wood`, `camo`, `gradient`, `dots`, `hex`, `scales`, each with colours,
`scale`, `width`, `variation`, `seed`; `"output": "normal"` makes the matching normal map, `"mask"`
a team mask (white where the second colour is). Same spec, same bytes.

**Editor and agents.** **Окна → Материалы** lists the library, previews a material on a sphere or
a box (live, with its textures), edits it, generates textures and applies a material to a type by
material name or node. Agents use the same operations over MCP:

| operation | what |
|---|---|
| `material_list {type?}` | the library, and per type: overrides, assignments, the model's material and node names |
| `material_get {name}` | one entry and its `base` chain flattened |
| `material_set {name, material, merge?}` | create / change a library entry (checked: unknown keys, ranges, missing base) |
| `material_delete {name, force?}` | refuses while types or other entries use it |
| `material_apply {type, material, target? \| node?, remove?}` | write `"materials"` / `"assign"` of the type's view |
| `texture_generate {pattern, ...}`, `texture_patterns` | procedural textures |
| `material_preview {material, shape?}` | (editor) show it in the Materials window, then `view_screenshot` |

`templates/showcase` has a library material (`steel_plates`, made with these operations) on the
catapult turret.

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

`templates/showcase` is "Iron and Gold" with imported views:
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
