// agent-mech stage 5, TB: turn-based mode on the same simulation (Heroes of Might and Magic / XCOM battles,
// Advance Wars). Players act one after another; during a turn only the active player's units move (up to their
// move budget = type speed x turns.moveSeconds) and shoot (turns.attacks per weapon); the others stand still and
// don't return fire. kCmdEndTurn (or turns.timeLimit) passes the turn to the next living player.
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

uint8_t RtsModule::turnPlayer(Simulation& sim) const { return sim.world().resource<GameState>()->mech.turnPlayer; }

namespace {
uint8_t nextAlive(Simulation& sim, uint8_t after) {
    const auto& ps = sim.players();
    if (ps.empty()) return 0;
    // players in slot order after `after`, wrapping around
    std::vector<uint8_t> slots;
    for (auto& p : ps) if (p.alive) slots.push_back(p.slot);
    if (slots.empty()) return 0;
    std::sort(slots.begin(), slots.end());
    for (uint8_t s : slots) if (s > after) return s;
    return slots.front();
}
} // namespace

void RtsModule::mechTickTurns(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MechState& M = gs.mech;
    const MechRules& R = rules_.mech;
    auto startTurn = [&](uint8_t p) {
        M.turnPlayer = p;
        ++M.turnIndex;
        M.turnStart = sim.tick();
        sim.emit({kEvTurn, p, kNullEntity, kNullEntity, int64_t(M.turnIndex), {}});
    };
    if (M.turnPlayer == 0) startTurn(nextAlive(sim, 0));
    else if (auto* pi = sim.player(M.turnPlayer); !pi || !pi->alive) startTurn(nextAlive(sim, M.turnPlayer));
    else if (R.turnTimeLimit.raw > 0 && sim.tick() >= M.turnStart + uint32_t((R.turnTimeLimit * int64_t(sim.tickRate())).floorInt()))
        startTurn(nextAlive(sim, M.turnPlayer));
    std::vector<Entity> add, stop;
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& tr) {
        if (u.hp.raw <= 0) return;
        const UnitType& t = rules_.types[u.type];
        if (t.structure && t.weapons.empty()) return;
        auto* tb = w.tryGet<TurnBudget>(e);
        if (!tb) { add.push_back(e); return; }
        if (o.player != M.turnPlayer) { tb->lastPos = tr.pos; return; }
        if (tb->turn != M.turnIndex) {   // a new turn of this unit's owner: full budget
            tb->turn = M.turnIndex;
            tb->move = t.speed * R.turnMoveSeconds;
            tb->shots = R.turnAttacks;
            tb->lastPos = tr.pos;
            return;
        }
        tb->move -= dist2D(tr.pos, tb->lastPos);
        tb->lastPos = tr.pos;
        if (tb->move.raw <= 0) { tb->move = Fixed{}; if (auto* m = w.tryGet<Mobile>(e); m && m->moving) stop.push_back(e); }
    });
    for (Entity e : add) {
        TurnBudget tb;
        tb.lastPos = w.get<Transform>(e).pos;
        w.add<TurnBudget>(e, tb);
    }
    for (Entity e : stop) stopMoving(sim, e);
}

bool RtsModule::mechTurnCommand(Simulation& sim, const Command& cmd) {
    MechState& M = state(sim).mech;
    if (cmd.type == kCmdEndTurn) {
        if (cmd.player == M.turnPlayer) {
            M.turnPlayer = nextAlive(sim, M.turnPlayer);
            ++M.turnIndex;
            M.turnStart = sim.tick();
            sim.emit({kEvTurn, M.turnPlayer, kNullEntity, kNullEntity, int64_t(M.turnIndex), {}});
        }
        return true;
    }
    if (cmd.type == kCmdSurrender || cmd.player == 0) return false;
    return cmd.player != M.turnPlayer;   // not your turn: ignored
}

bool RtsModule::mechTurnFire(Simulation& sim, Entity e) {
    World& w = sim.world();
    if (w.get<Owner>(e).player != state(sim).mech.turnPlayer) return false;
    auto* tb = w.tryGet<TurnBudget>(e);
    if (!tb || tb->turn != state(sim).mech.turnIndex || tb->shots <= 0) return false;
    --tb->shots;
    return true;
}

void RtsModule::mechTurnStat(Simulation& sim, Entity e, Stat s, Fixed& mul) const {
    if (s != Stat::Speed) return;
    World& w = sim.world();
    const MechState& M = w.resource<GameState>()->mech;
    if (w.get<Owner>(e).player != M.turnPlayer) { mul = Fixed{}; return; }
    const auto* tb = w.tryGet<TurnBudget>(e);
    if (tb && tb->turn == M.turnIndex && tb->move.raw <= 0) mul = Fixed{};
}

} // namespace rts::game
