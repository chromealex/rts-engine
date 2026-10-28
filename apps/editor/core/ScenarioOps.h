// scenario_run: headless scripted check of a mechanic (see ScenarioOps.cpp).
#pragma once
#include "Workspace.h"
#include "SimTools.h"

namespace rts::editor {
void registerScenarioOps(Workspace& ws);
// Event log entry / trigger log shared with match_run: {"t", "event", "player", "a", "b", "value", "x", "z"}.
Json eventJson(const SimRefs& refs, const SimEvent& ev);
}
