// Post-processing operations of the editor core (MCP agents and the Post effects window): the
// "postfx" block of game.json and of the open map. Every change answers with a frame-cost estimate
// per device class and warnings for heavy effects. Format: engine/render/PostFxData.h, docs/postfx.md.
#pragma once
#include "Workspace.h"

namespace rts::editor {

void registerPostFxOps(Workspace& ws);

// The block being edited: "game" = game.json, "map" = the open map.
Json postFxBlock(Workspace& ws, const std::string& scope);
// Writes the block back (game: saves game.json; map: one undo step). An empty block is removed.
void setPostFxBlock(Workspace& ws, const std::string& scope, const Json& block);
// Neutral LUT strip (size x size squares of size x size), RGBA8 top row first.
std::vector<uint8_t> neutralLut(int size);
// Minimal PNG encoder (stored deflate blocks): RGBA8, top row first.
std::string encodePngRgba(int w, int h, const std::vector<uint8_t>& rgba);

} // namespace rts::editor
