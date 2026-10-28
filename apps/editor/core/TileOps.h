// Tile maps in the editor core (shared by the Tiles window and MCP agents): tilesets of the open
// game, the map's "tiles" layer, painting with autotiling. Painting a terrain also bakes its
// gameplay properties (cell kind, cliff level, resource field) into the map's ordinary layers, so
// the saved map is valid for the simulation even without the tileset.
#pragma once
#include <string>
#include <vector>
#include "Workspace.h"
#include "modules/rts/RtsTiles.h"

namespace rts::editor {

void registerTileOps(Workspace& ws);

std::vector<std::string> listTilesets(const Workspace& ws);          // project-relative paths
const game::Tileset* tileset(const Workspace& ws, const std::string& path, std::string* err = nullptr); // cached by mtime
// Tileset of the open map's tiles layer (nullptr: the map has none or it fails to load).
const game::Tileset* mapTileset(const Workspace& ws, std::string* err = nullptr);
// Creates the tiles layer: every cell `fill`, or (fromTerrain) the first terrain matching the cell kind/level.
void initTiles(Workspace& ws, const std::string& path, const std::string& fill, bool fromTerrain);
// Brush stroke step (no undo snapshot: call map.beginEdit() once per stroke). fixedTile >= 0 places
// that atlas tile without autotiling; -1 = automatic.
void paintTiles(Workspace& ws, int cx, int cy, int radius, int terrain, int fixedTile = -1);
void paintTilesRect(Workspace& ws, int x0, int y0, int x1, int y1, int terrain, int fixedTile = -1);

} // namespace rts::editor
