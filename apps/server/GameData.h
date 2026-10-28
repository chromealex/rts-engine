// Game data for network play outside the client: loading a match's rules and map exactly like the
// game does (2D tile maps get their tileset applied), and the server's match verdict (re-simulating
// the confirmed input). Used by rts_server, rts_netbot and the network tests.
#pragma once
#include <map>
#include <string>
#include "engine/data/Json.h"
#include "engine/net/server/Server.h"
#include "engine/sim/Simulation.h"

namespace rts::netgame {

struct MatchData {
    Json manifest, rules, map;
    std::string module = "rts";
};

// gameDir: folder with game.json; mapFile relative to it. Throws std::runtime_error with a readable message.
MatchData loadMatchData(const std::string& gameDir, const std::string& mapFile);

// A Simulation for the module named in game.json ("rts"), or nullptr if this build lacks it.
std::unique_ptr<Simulation> makeSimulation(const std::string& module, const SimConfig& cfg);

// The arbiter rts_server installs: gameDirs maps game.json "id" to its folder.
net::Arbiter makeArbiter(std::map<std::string, std::string> gameDirs);

} // namespace rts::netgame
