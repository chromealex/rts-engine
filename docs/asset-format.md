# Asset formats (RTSEngine)

Source assets are kept in open, standard formats that any tool or agent can edit:

| Kind | Formats |
|---|---|
| Images | PNG |
| Sound | WAV, OGG, MP3, FLAC |
| Models | glTF |
| Data | JSON |

On top of these, the engine defines two small JSON descriptors:

- `*.sprite.json` — 2D sprites and animations;
- `*.tileset.json` — tile-based terrain.

Everything a game ships goes into `content.rpak` (see `engine/pack`). Converting assets into
platform-specific formats at build time — textures to KTX2 with ASTC/ETC2/BC compression, audio
to Ogg/Opus — is planned in `rtsbuild`. Only the sources are kept in the repository.

Paths inside descriptors are relative to the descriptor file.

## `*.sprite.json` — `rts.sprite/1`

```json
{
  "format": "rts.sprite/1",
  "image": "units.png",
  "teamMask": "units.mask.png",
  "pixelsPerCell": 16,
  "frames": [ {"x": 0, "y": 0, "w": 16, "h": 16, "ax": 8, "ay": 8}, ... ],
  "sprites": {
    "trike": {
      "directions": [
        {"frames": [5]}, {"frames": [6]}, {"frames": [7]}, {"frames": [8]}, {"frames": [9]},
        {"frames": [8], "flipX": true}, {"frames": [7], "flipX": true}, {"frames": [6], "flipX": true}
      ],
      "fps": 0, "loop": true,
      "parts": [ {"sprite": "tank_turret", "rotate": "aim"} ]
    }
  }
}
```

| key | meaning |
|---|---|
| `image` | Sheet with all frames, RGBA PNG. |
| `teamMask` | Optional single-channel PNG the same size as the image. 255 marks pixels drawn in the player's colour. In those pixels the image stores luminance, so `final = mix(rgb, rgb.r * teamColor, mask)`. |
| `pixelsPerCell` | How many image pixels make up one map cell at scale 1. |
| `frames` | Rectangles in the sheet. `ax`/`ay` is the anchor: the frame pixel placed at the object's position. |
| `sprites` | Named sprites. A type points at one with `view.sprite` set to `"gfx/units.sprite.json#trike"`. |
| `directions` | One entry per facing, clockwise, starting from north. Each entry has the frames of its animation and an optional `flipX` for mirrored facings. A single entry means the sprite has no facing. |
| `fps` | Animation speed. 0 means the frames are chosen by state (facing) and not played over time. |
| `loop` | Whether the animation repeats. |
| `parts` | Further sprites drawn on top. `rotate` is `body` (the unit's facing) or `aim` (the weapon's target), for turrets. |

## `*.tileset.json` — `rts.tileset/1`

```json
{
  "format": "rts.tileset/1",
  "image": "terrain.png", "tileSize": [16, 16], "columns": 32, "count": 389,
  "groups": {"icon9": [127, 128, ...]},
  "autotile": {
    "mode": "nesw16",
    "kinds": {"s": {"tiles": [...16]}, "r": {"tiles": [...16]}, "d": {...}, "m": {...}, "spice": {...}, "thickSpice": {...}},
    "base":  {"r": "s", "d": "s", "m": "r", "spice": "s", "thickSpice": "spice"}
  }
}
```

**`nesw16` auto-tiling.** Each kind lists 16 tiles. A cell takes `tiles[mask]`, where `mask` is
built from the neighbours of the same kind: N=1, E=2, S=4, W=8. `base` names the kind drawn under
a cell's edges. For example, rock edges blend into sand.

**Using a tileset on a map.** A map refers to the tileset and gives the kind of every cell. This
part is presentation only: the simulation still uses `terrain.rows`.

```json
"look": {"tileset": "gfx/terrain.tileset.json", "kinds": ["ssrrrm...", "..."]}
```

## Status

| Feature | Status |
|---|---|
| Writing both descriptors | The importers (`tools/importers`) write them. |
| Drawing sprites, team masks and `look` tilesets | Renderer work (models agent). Until then, `view.shape` is used. |
| Turning PNG into KTX2 and WAV into Ogg in `rtsbuild` | Planned. |
