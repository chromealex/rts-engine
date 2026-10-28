// Campaigns (agent-mech): a game folder may have campaign.json, a chain of missions:
//   {"start": "m1", "missions": [{"id": "m1", "name": "...", "map": "maps/m1.json", "briefing": "text or lang key",
//     "next": "m2", "carry": {"resources": ["gold"], "tags": ["hero"], "types": ["knight"], "max": 12}}, ...]}
// Missions end through the trigger action endMission (result + next) or a normal victory / defeat. Between missions
// the surviving units and resources listed in "carry" move into the next map (players[0] = the campaign player).
#pragma once
#include <string>
#include "../../engine/data/Json.h"
#include "../../engine/sim/Simulation.h"

namespace rts::game {

// Units and resources of `player` that the spec keeps: {"resources": {id: amount}, "units": [{"type", "hp", "rank", "level"}]}.
Json collectCarry(Simulation& sim, const Json& carrySpec, uint8_t player);
// Puts a carry into a mission map before setup: places the units at players[player-1]'s start.
void applyCarry(Json& map, const Json& carry, uint8_t player);
// After a mission: the next mission id (endMission "next", else the mission's "next") and whether it was won.
struct MissionOutcome { bool finished = false, won = false; std::string next; };
MissionOutcome missionOutcome(Simulation& sim, const Json& mission, uint8_t player);
// After setup: adds the carried resources and restores hp / ranks of the carried units (placed as plain map entities).
void restoreCarry(Simulation& sim, const Json& carry, uint8_t player);

} // namespace rts::game
