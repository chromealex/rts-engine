// Agent ops for overlays (rules.json / game.json "overlays", see modules/rts/view/RtsOverlayData.h):
// overlay_templates, overlay_list, overlay_get, overlay_set, overlay_delete. Every change is validated
// against the open game's rules, saved, and reloads the editor views (live in the play-test).
// overlay_preview (show overlays in the Scene / Game views regardless of their conditions) needs the
// editor window: the Overlays window registers the real implementation.
#pragma once
#include "Workspace.h"

namespace rts::editor {
void registerOverlayOps(Workspace& ws);
}
