// Strategic layer (agent-mech stage 5, TB): a turn-based campaign map of provinces and armies that launches RTS
// battles on the same game data and takes their result back (Total War, Company of Heroes 3, Empire at War).
//
// Deterministic like the simulation (Fixed math, seeded RNG, fixed iteration order); the whole state is JSON
// (toJson / fromJson) so it can be saved, shown through MCP and hashed. Battles are either auto-resolved
// (Lanchester square law on hp x dps) or simulated: a battle map is built from the two armies and played by bots
// on RtsModule; the survivors return to the map.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "engine/core/Fixed.h"
#include "engine/data/Json.h"

namespace rts::strategic {

struct Stack { std::string type; int count = 0; };

struct Province {
    std::string id, name;
    std::vector<int> adj;              // neighbour indices (symmetric)
    int owner = 0;                     // player slot, 0 neutral
    std::vector<std::pair<int, Fixed>> income;   // resource index -> per turn
    Json terrain;                      // optional battle terrain rows (map "terrain" block)
    Fixed x{}, y{};                    // position on the strategic map (presentation, AI distances)
};

struct Army {
    int id = 0, owner = 0, at = 0;     // province index
    int movesLeft = 0;
    std::vector<Stack> units;
    int total() const { int n = 0; for (auto& s : units) n += s.count; return n; }
};

struct BattleReport {
    int turn = 0, province = 0, attacker = 0, defender = 0, winner = 0;
    bool simulated = false;
    std::vector<Stack> attackerBefore, defenderBefore, attackerAfter, defenderAfter;
};

class StrategicGame {
public:
    // def: the campaign map (see docs/game-format.md "Strategic map"); rules: the RTS rules.json used for battles.
    static StrategicGame create(const Json& def, const Json& rules, uint64_t seed = 1);   // throws std::runtime_error
    static StrategicGame fromJson(const Json& state, const Json& rules);                  // a saved toJson()
    Json toJson() const;
    uint64_t checksum() const;

    // orders (false + err when not allowed)
    bool move(int player, int army, int to, std::string* err = nullptr);
    bool recruit(int player, int province, const std::string& type, int count, std::string* err = nullptr);
    // ends `player`'s turn (must be the current one); the next player collects income
    bool endTurn(int player, std::string* err = nullptr);
    void botTurn(int player);   // a simple strategic AI plays `player`'s turn (orders, then ends it)

    int findProvince(const std::string& id) const;
    int current() const { return current_; }
    int turn() const { return turn_; }
    int winner() const { return winner_; }
    const std::vector<Province>& provinces() const { return provinces_; }
    const std::vector<Army>& armies() const { return armies_; }
    const std::vector<BattleReport>& battles() const { return battles_; }
    Fixed treasury(int player, int res) const;

    // battles: "auto" (default) or "simulate"; simulate runs RtsModule with bots for at most `battleSeconds`
    std::string battleMode = "auto";
    int battleSeconds = 240;
    int battleW = 48, battleH = 32;
    // builds the RTS map JSON of a battle (attacker = player 1 on the left, defender = player 2 on the right)
    Json battleMap(const Army& attacker, const Army& defender, int province) const;

private:
    void fight(int attackerArmy, int defenderArmy);
    void autoResolve(Army& a, Army& d, BattleReport& r);
    void simulate(Army& a, Army& d, BattleReport& r);
    Fixed strength(const Army& a) const;
    int armyIndexAt(int province, int ownerNot) const;   // a hostile army there, -1 none
    void checkWinner();
    int typeIndex(const std::string& id) const;

    Json rules_;
    std::vector<std::string> resources_;
    std::vector<std::string> playerNames_;
    std::vector<Province> provinces_;
    std::vector<Army> armies_;
    std::vector<std::vector<Fixed>> treasury_;          // [player][resource]
    std::vector<std::pair<std::string, std::vector<std::pair<int, Fixed>>>> recruitCost_;   // type -> cost
    std::vector<BattleReport> battles_;
    int movesPerTurn_ = 1, current_ = 1, turn_ = 1, nextArmy_ = 1, winner_ = 0;
    uint64_t seed_ = 1;
};

} // namespace rts::strategic
