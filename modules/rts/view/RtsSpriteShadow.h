// Shadows and lighting of 2D sprites (Rts2DView), pure part: no GPU, linked into rts_tests.
//
// Data (docs/game-format.md, "type sprite" / "sprites"):
//   rules.json / game.json "sprites": {"shadow": "cast" | "blob" | "none", "lit": bool}
//       game-wide defaults (game.json wins over rules.json); default "blob", unlit (the old look).
//   type "sprite": {"shadow": "cast" | "blob" | "none" | true | false | {"mode", "size", "alpha"},
//                   "castHeight": h, "castDepth": d, "shadowOffset": [right, front], "lit": bool}
//       the object form without "mode" is a blob (old files); size/alpha always describe the blob
//       that stands in for a cast shadow on low tiers.
//
// Tiers (ShadowQuality): Off = blobs only; Low = buildings cast, units keep blobs; Medium/High =
// every "cast" sprite casts (units keep a faint contact blob under the feet).
//
// Blobs and ground-mode sprites are projected decals: cut from the ground surface triangles (the
// exact triangles of the terrain mesh / tile map under them), never flat quads hovering over slopes.
#pragma once
#include <cstdint>
#include <vector>
#include "../../../engine/data/Json.h"
#include "../../../engine/render/Math3D.h"
#include "../../../engine/render/Renderer3D.h"
#include "../RtsMap.h"

namespace rts::game {

enum class SpriteShadowMode : uint8_t { None, Blob, Cast };

struct SpriteDefaults {
    SpriteShadowMode shadow = SpriteShadowMode::Blob;
    bool lit = false;
};
// rules.json "sprites" and game.json "sprites" (game.json keys win). Unknown values keep the default.
SpriteDefaults parseSpriteDefaults(const Json& rulesSprites, const Json& gameSprites);
bool parseShadowMode(const Json& j, SpriteShadowMode& out);   // "cast"/"blob"/"none"/true/false

struct SpriteShadowSpec {
    SpriteShadowMode mode = SpriteShadowMode::None;
    float blobW = 0, blobH = 0, blobAlpha = 0.35f;   // world size of the blob (ellipse)
    float castHeight = 0;                            // world height of the caster quad
    float offsetRight = 0, offsetFront = 0;          // added to the sprite's own offset (caster foot)
    float castDepth = 0;                             // > 0: second caster this far behind (a box: buildings)
    bool lit = false;
};
// `width`/`height`: drawn sprite size; footprint in cells (structures); `ground`: "mode": "ground".
SpriteShadowSpec parseSpriteShadow(const Json& sprite, const SpriteDefaults& d, bool structure, bool ground,
                                   float width, float height, int footW, int footH);

struct ShadowDraw { bool cast = false; float blobK = 0; };   // blobK: blob alpha multiplier (0 = none)
ShadowDraw shadowDrawFor(SpriteShadowMode mode, render::ShadowQuality q, bool structure);

// ---- projector geometry
struct GroundSurface {
    const MapGrid* grid = nullptr;
    float levelHeight = 1.5f;
    bool tiles = false;          // 2D tile map: flat cells (RtsTileView), else the terrain mesh
};
// Ground triangles (world, 3 points each) of the cells overlapping [minX, maxX] x [minZ, maxZ].
void surfaceTriangles(const GroundSurface& s, float minX, float minZ, float maxX, float maxZ, std::vector<render::float3>& out);
// Clips a triangle to the rectangle origin + a*aU + b*aV (0 <= a <= lenU, 0 <= b <= lenV) in XZ
// (aU, aV horizontal unit vectors). Returns the polygon size (0, or 3..7).
int clipToRect(const render::float3 (&tri)[3], render::float3 origin, render::float3 aU, float lenU, render::float3 aV, float lenV,
               render::float3 (&out)[8]);

// Caster quad facing: the camera's ground axis (`gRight`, `gFront` towards the camera) turned
// towards the sun when the sun shines almost along the sprite plane (the shadow would be a sliver).
render::float3 casterRight(render::float3 gRight, render::float3 gFront, render::float3 sunDir);

} // namespace rts::game
