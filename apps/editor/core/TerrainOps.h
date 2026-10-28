// Terrain sculpting and materials in the editor core (shared by the Terrain window and MCP agents):
// heightfield corners (raise / lower / smooth / flatten / noise / set), the optional per-cell material
// layer of the terrain renderer (map key terrainMaterial), and 16-bit PNG heightmap import / export.
// The first sculpt on a map without corners creates them from the surface the player sees now
// (cliff levels, ramps, fine heights), so sculpting never jumps. Corners are presentation unless the
// rules say terrain.model "heightfield" (docs/terrain.md); these ops never touch other gameplay layers.
#pragma once
#include <string>
#include <vector>
#include "Workspace.h"

namespace rts::editor {

void registerTerrainOps(Workspace& ws);

enum class Sculpt { Raise, Lower, Smooth, Flatten, Noise, Set };
bool parseSculpt(const std::string& s, Sculpt& out);
const char* sculptName(Sculpt s);

// Creates corners from the current visual surface when the map has none. levelHeight: rules.levelHeight.
void ensureCorners(MapDoc& doc, float levelHeight);
// One brush dab (no undo snapshot: call doc.beginEdit() once per stroke). x, z: centre in world units
// (cells), radius in cells, strength in world units (raise/lower/noise amplitude, set = target height,
// smooth/flatten 0..1 blend). Smooth falloff towards the rim. Returns the touched corner rect.
struct CornerRect { int x0 = 0, y0 = 0, x1 = -1, y1 = -1; };
CornerRect sculpt(MapDoc& doc, float levelHeight, Sculpt op, float x, float z, float radius, float strength, uint32_t seed = 1);
// Material layer per cell (0..3 = terrain.layers index, 255 = by cell kind); stored as the map key
// "terrainMaterial": rows of digits ('.' = by kind).
std::vector<uint8_t> materialLayer(const MapData& d);
void paintMaterial(MapDoc& doc, int cx, int cy, int radius, int layer);
// 16-bit grayscale PNG <-> corners. scale: world units of the full 0..65535 range; offset: world
// height of value 0. Import resamples to (w+1)x(h+1).
bool exportHeightmap(const MapData& d, const std::string& file, float& scale, float& offset, std::string* err);
bool importHeightmap(MapDoc& doc, const std::string& file, float scale, float offset, std::string* err);

} // namespace rts::editor
