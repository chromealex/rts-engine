# Post effects (`postfx`)

Full-screen effects over the 3D scene: bloom, colour grading, LUTs, vignette, tilt-shift, sun
shafts, FXAA, film grain and a few stylistic ones. They are data (`game.json`, overridable per map),
edited in the editor window **Постэффекты** or over MCP (`postfx_*`). HUD, overlays and the game UI
are drawn after the effects and are never post-processed.

Code: `engine/render/PostFxData.*` (format, catalog, presets, cost model; GPU-free),
`engine/render/PostFx.*` + `PostFxShaders.h` (renderer), `apps/editor/core/PostFxOps.*` (MCP),
`apps/editor/windows/PostFxWindow.cpp` (window). Hook: `Renderer3D::render(pass, &postFx)` +
`Renderer3D::resolvePostFx()`.

## Format

```jsonc
// game.json (the same block in a map overrides it)
"postfx": {
  "enabled": true,                 // false: the whole chain off
  "replace": false,                // map only: ignore the game's effects
  "effects": [
    {"type": "colorGrade", "contrast": 1.1, "saturation": 1.15, "temperature": 0.1},
    {"type": "bloom", "intensity": 0.7, "threshold": 0.75, "minQuality": "medium"},
    {"type": "vignette", "intensity": 0.3},
    {"type": "lut", "texture": "postfx/warm.png"}
  ]
}
```

- One effect per type. A map effect is merged over the game's effect of the same type (only the
  keys it sets), `"enabled": false` on it switches that effect off for the map.
- Fixed order, whatever the list order: fxaa → pixelate → chromaticAberration → sharpen →
  tiltShift → bloom → sunShafts → colorGrade → lut → posterize → vignette → grain → fade → letterbox.
- Every effect: `enabled`, `minQuality` (`low | medium | high`). Unknown keys are reported (MCP: an
  error), values outside the range are clamped.

| type | cost | default minQuality | parameters |
|---|---|---|---|
| `colorGrade` | cheap | low | exposure (stops), contrast, saturation, temperature, tint, lift / gamma / gain `[r,g,b]`, filter `[r,g,b]` |
| `vignette` | cheap | low | intensity, softness, color |
| `grain` | cheap | low | intensity, size, animated |
| `lut` | cheap | low | texture (PNG strip N×N squares: 256×16 or 1024×32, Unreal layout), intensity, flipY (Unity LUTs) |
| `fade` | cheap | low | color, amount |
| `letterbox` | cheap | low | amount, color |
| `pixelate` | cheap | low | size (px) |
| `posterize` | cheap | low | levels |
| `chromaticAberration` | medium | low | intensity |
| `sharpen` | medium | medium | amount |
| `tiltShift` | medium | medium | focus, width, falloff, blur |
| `bloom` | **heavy** | medium | threshold, knee, intensity, radius, tint |
| `fxaa` | **heavy** | medium | subpixel |
| `sunShafts` | **heavy** | high | intensity, threshold, length, color (from the scene's sun; off-screen sun = rays from the edge) |

Presets (`postfx_preset`, window): cinematic, vivid, mobile, noir, retro, miniature, sunset, winter,
nightVision, clean.

## Quality tiers and cost

`PostFxQuality` off / low / medium / high: weak Android GPUs (Mali-4xx/T, Adreno 3xx/4xx/50x,
PowerVR SGX/GE) and software GL get **low**, other phones **medium**, desktops **high**;
`RTS_POSTFX=off|low|medium|high` overrides. When the adaptive governor (`QualityGovernor`) steps
shadows / lights down, post effects go down one tier per step (never below low). Heavy effects run
cheaper plans on lower tiers (`bloomPlan`, `tiltBlurPlan`, `shaftsPlan`: smaller targets, fewer
levels / samples).

- No active effect at the current tier → the scene renders straight to the screen, zero cost.
- Any active effect → scene to an RGBA8 texture + one composite pass. All cheap effects share that
  pass (one shader variant per combination, compiled on first use).
- bloom, fxaa, tiltShift, sunShafts add their own passes (1–9 depending on tier).

The cost model (`postFxEffectWork`, `postFxMs`) counts full-resolution texture reads per pixel and
extra render targets, for three device profiles: weak phone (Mali-T720 / Adreno 306 class, 720p,
~0.8 ms per full-screen read), mid phone (Adreno 610 / Mali-G52 class, 1920×864), desktop
(2560×1440). The renderer uses the same plans, so estimates follow what is drawn. Budget for the
chain: 3 ms on phones, 1.5 ms on desktops.

## Warnings for heavy effects

`postfx_add`, `postfx_set`, `postfx_preset` answer with `heavy: true` and `warnings` when:
- the effect is heavy (ms per frame on a mid phone, at high tier and on a weak phone);
- a heavy effect gets `minQuality: "low"` (forced onto weak phones; % of a 60 fps frame);
- a medium effect is forced onto weak phones and costs ≥ 1 ms there;
- the whole chain goes over budget on a device class (ms and the resulting fps).

The same warnings appear in the editor's activity feed and, in Russian, in the window next to the
effect; the window shows the frame cost table per device class.

## MCP

| tool | what |
|---|---|
| `postfx_catalog` | types, parameters (range, default, help), cost class, ms per device and tier, presets |
| `postfx_get` | game.json and map blocks, resolved chain with cost and warnings |
| `postfx_add` | add / replace an effect (`scope`: game \| map); answer: warnings + cost |
| `postfx_set` | change parameters of an effect (map scope: creates a map override) |
| `postfx_remove` | remove by type or all |
| `postfx_preset` | apply a look (`mode`: replace \| merge) |
| `postfx_enable` | chain on / off without deleting |
| `postfx_estimate` | cost of a hypothetical chain (`effects`) or the current one |
| `postfx_lut_neutral` | write a neutral LUT PNG into the game folder for grading |
| `view_postfx` | editor preview: tier (`quality`), effects in the Scene view on / off |

Typical flow: `postfx_catalog` → `postfx_preset` or `postfx_add` → read `warnings` (tell the user,
raise `minQuality` of heavy effects if phones matter) → `view_postfx {quality: "low"}` +
`view_screenshot` to see what weak phones get.

## GPU notes

- GLES 3.0 floor: RGBA8 render targets, no depth reads, no compute, no MRT. One full-screen
  triangle per pass. Every pass has a depth attachment (Metal pipelines declare Depth32Float).
- Shaders are written once in a GLSL/MSL common subset (`PostFxShaders.h`) and wrapped per
  backend by `buildProgram` (PostFx.cpp): `TEX0..4`, `RETURN`, `TUV` (Metal render targets are
  top-down, `DeviceInfo::renderTargetFlipY`).
- Verified: GL 3.3 core and GLES 3.0 (Mesa llvmpipe), screenshots, all 14 effects together.
  Not verified: MSL compile / Metal run, real Android GPUs, WebGL2.
