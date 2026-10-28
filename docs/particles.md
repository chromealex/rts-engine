# Particles

Presentation-only particle effects: CPU simulation (`engine/fx/ParticleSystem`), drawn through the
RHI with one dynamic vertex buffer per frame. The simulation never sees them; effects are started from
`SimEvent`s by `modules/rts/fx/RtsFx` (the same read-only event collector as `RtsAudio`).

## Data (per game)

```
games/<id>/fx/fx.json        which effect plays for which event (bindings)
games/<id>/fx/<name>.json    one effect = 1..8 emitters
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
JSON, bindings). MCP: `fx_list`, `fx_get`, `fx_set`. The play-test shows effects too.

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
