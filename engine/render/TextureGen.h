// Procedural textures for materials made on request (editor Materials window, MCP texture_generate):
// tileable patterns rendered on the CPU into RGBA8, deterministic for the same spec and seed.
//
//   {"pattern": "brick", "size": 256, "colors": [[0.55,0.25,0.2], [0.8,0.78,0.72]], "scale": 8, "seed": 3}
//
// Patterns: solid, noise, stripes, checker, brick, tiles, panels, metal, wood, camo, gradient, dots,
// hex, scales. "output": "color" (default), "normal" (normal map from the pattern's relief) or
// "mask" (white where the pattern's second colour is: a team mask). Colours are sRGB 0..1.
#pragma once
#include <string>
#include <vector>
#include "Model.h"
#include "engine/data/Json.h"

namespace rts::render {

// Fills `out` (width, height, rgba). Throws std::runtime_error with a readable message on a bad spec
// (unknown pattern, size out of 8..2048).
void generateTexture(const Json& spec, Image& out);
std::vector<std::string> texturePatterns();
// One-line description per pattern and the keys it reads (MCP schema / editor tooltips).
std::string texturePatternHelp();

} // namespace rts::render
