// Branding operations of the editor core (MCP agents and the Branding window): game icons
// (game.json "icons") and the splash / loading screen ("splash"). Formats: tools/branding/Icons.h,
// engine/splash/SplashData.h, docs/game-format.md "Icons and splash".
//   branding_get    both blocks + what they resolve to + warnings
//   branding_set    replace / merge / remove the blocks (validated before saving)
//   icons_preview   PNG contact sheet of every generated icon size per platform (image content)
//   splash_preview  PNG strip of splash frames at a few times (image content)
#pragma once
#include "Workspace.h"

namespace rts::editor {

void registerBrandingOps(Workspace& ws);
std::string base64Encode(const std::string& bytes);

} // namespace rts::editor
