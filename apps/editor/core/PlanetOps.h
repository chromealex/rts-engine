// Planet editing in the editor core (editor GUI and MCP agents), docs/planets.md: sculpt corner heights and
// paint cells / materials of a cube-sphere body in the map's `world` block. Brushes are 3D: a dab covers
// every corner (cell) within an angular radius of a surface direction, so strokes cross face seams;
// duplicated seam corners are written equal (docs/planets.md §2).
// The existing ops terrain_sculpt / terrain_paint / terrain_material take an optional `body` (id) with
// `face` + `x`, `y` (cell coordinates on that face) or `dir` [x, y, z]; without `body` they are unchanged.
// planet_info lists the bodies.
#pragma once
#include <string>
#include <vector>
#include "TerrainOps.h"
#include "engine/render/Math3D.h"

namespace rts::editor {

void registerPlanetOps(Workspace& ws);

// A surface body of the open planet map (MapData::bodies, by id or decimal index); null when missing.
DocBody* planetBody(MapDoc& doc, const std::string& id, std::string* err = nullptr);
// Unit direction of a face point (face 0..5, a / b in cell units 0..N); float version of RtsSphere.
render::float3 planetDir(int face, float a, float b, int faceSize);
// One brush dab on a body (no undo snapshot; call doc.beginEdit() once per stroke). radius in cells,
// strength as terrain_sculpt. Returns the number of corners changed.
int planetSculpt(MapDoc& doc, const std::string& body, Sculpt op, render::float3 dir, float radius, float strength, uint32_t seed = 1);
// Cells within the radius get map character `cell` or material layer (0..3, -1 = by kind: rows faces[f].material). Returns the number of cells changed.
int planetPaint(MapDoc& doc, const std::string& body, render::float3 dir, float radius, char cell);
int planetMaterial(MapDoc& doc, const std::string& body, render::float3 dir, float radius, int layer);

// Detail grids (PlanetDetailOps.cpp, docs/terrain.md "Detail surface"): DocBody::detail, k samples per cell.
// Creates the grid from the surface the view draws (same synthesis and look keys); false when it exists.
bool ensurePlanetDetail(Workspace& ws, DocBody& b, int k);
// One dab on the detail grid (creates it at k=4, capped by the sample budget): Raise / Lower / Smooth /
// Flatten / Noise / Set; radius in cells (sub-cell allowed), hardness 0..1. The corners follow the detail.
// Returns the samples changed, -1 on error (err set).
int planetDetailSculpt(Workspace& ws, const std::string& body, Sculpt op, render::float3 dir, float radius, float strength, float hardness,
                       uint32_t seed, std::string* err);
// Corner sculpt on a body with a detail grid: the detail takes the corner change (bilinear per cell).
void planetDetailFollowCorners(DocBody& b, const std::vector<int16_t>& oldCorners);
// terrain_detail {body, action info / create / remove, scale}
Json planetDetailOp(Workspace& ws, const Json& p);

} // namespace rts::editor
