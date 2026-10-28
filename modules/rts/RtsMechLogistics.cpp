// agent-mech stage 4, L: logistics (Mindustry, Factorio). Conveyor pieces carry one packet per cell in their
// direction; producers put packets on adjacent belts, crafters turn belt inputs into outputs, structures with
// "beltInput" take packets into the owner's stock. Belts are ordinary 1x1 structures (built, attacked, sold);
// their direction comes from build_line, the map entity "dir" or kCmdRotate.
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

namespace {
const int kBdx[5] = {0, 1, 0, -1, 0};
const int kBdy[5] = {0, 0, 1, 0, -1};
uint8_t dirFromName(const std::string& s) {
    if (s == "e" || s == "east" || s == ">") return 1;
    if (s == "s" || s == "south" || s == "v") return 2;
    if (s == "w" || s == "west" || s == "<") return 3;
    if (s == "n" || s == "north" || s == "^") return 4;
    return 0;
}
} // namespace

void RtsModule::mechLogisticsSetup(Simulation& sim, const Json& map) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const size_t n = size_t(gs.grid.w) * size_t(gs.grid.h);
    gs.mech.beltRes.assign(n, -1);
    gs.mech.beltAmt.assign(n, Fixed{});
    gs.mech.beltProg.assign(n, Fixed{});
    // map entities may give a conveyor its direction: {"type": "belt", "pos": [x, y], "dir": "e"}
    for (auto& ej : map["entities"].items()) {
        if (!ej.has("dir")) continue;
        const uint8_t d = dirFromName(ej.getStr("dir"));
        if (!d) throw std::runtime_error("map entity dir: expected e, s, w or n, got '" + ej.getStr("dir") + "'");
        const int x = int(ej["pos"][size_t(0)].fx().floorInt()), y = int(ej["pos"][size_t(1)].fx().floorInt());
        if (!gs.grid.inside(x, y)) continue;
        bool found = false;
        auto& plan = gs.mech.beltPlan;
        for (auto& p : plan) if (p.first == gs.grid.index(x, y)) { p.second = d; found = true; }
        if (!found) plan.push_back({gs.grid.index(x, y), d});
    }
    (void)w;
}

bool RtsModule::applyMechLogisticsCommand(Simulation& sim, const Command& cmd) {
    World& w = sim.world();
    if (cmd.type == kCmdLock) {   // doors with a lock (Dungeon Keeper, Stronghold): a locked gate is a wall for everyone
        MapGrid& g = state(sim).grid;
        for (Entity e : cmd.units) {
            if (!w.alive(e) || w.get<Owner>(e).player != cmd.player || !rules_.types[w.get<Unit>(e).type].mech.gate) continue;
            auto* bl = w.tryGet<BridgeLaid>(e);
            auto* f = w.tryGet<Footprint>(e);
            if (!bl || !f) continue;   // still being built
            const bool lock = cmd.param == 1 || (cmd.param == 0 && bl->laid);
            if (lock == !bl->laid) continue;
            g.block(f->x, f->y, f->w, f->h, lock ? +1 : -1);
            for (int y = f->y; y < f->y + f->h; ++y) for (int x = f->x; x < f->x + f->w; ++x) if (g.inside(x, y)) g.cellChanged(x, y);
            bl->laid = !lock;
        }
        return true;
    }
    if (cmd.type != kCmdRotate) return false;
    for (Entity e : cmd.units) {
        if (!w.alive(e) || !w.has<Belt>(e) || w.get<Owner>(e).player != cmd.player) continue;
        Belt& b = w.get<Belt>(e);
        b.dir = (cmd.param >= 1 && cmd.param <= 4) ? uint8_t(cmd.param) : uint8_t(b.dir % 4 + 1);
    }
    return true;
}

void RtsModule::mechTickLogistics(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    MechState& M = gs.mech;
    const Fixed dt = sim.dt();
    const size_t n = size_t(g.w) * size_t(g.h);
    if (M.beltRes.size() != n) return;
    // belts and receivers on the grid this tick
    std::vector<uint8_t> dirAt(n, 0);
    std::vector<Fixed> speedAt;
    std::vector<int32_t> recvAt(n, -1);              // index into `recv`
    struct Recv { Entity e; uint8_t owner; const MechType* mt; };
    std::vector<Recv> recv;
    std::vector<Entity> addBelt, addCrafter;
    speedAt.assign(n, Fixed{});
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& tr) {
        if (u.hp.raw <= 0 || (u.flags & kUnitUnderConstruction)) return;
        const UnitType& t = rules_.types[u.type];
        const MechType& mt = t.mech;
        if (mt.beltSpeed.raw > 0) {
            auto* b = w.tryGet<Belt>(e);
            if (!b) { addBelt.push_back(e); return; }
            int x, y; g.cellXY(tr.pos, x, y);
            if (!g.inside(x, y)) return;
            dirAt[size_t(g.index(x, y))] = b->dir;
            speedAt[size_t(g.index(x, y))] = mt.beltSpeed;
            return;
        }
        if (!mt.beltAccepts && mt.outRes < 0 && mt.craftIn.empty()) return;
        if ((mt.outRes >= 0 || !mt.craftIn.empty()) && !w.has<Crafter>(e)) addCrafter.push_back(e);
        if (auto* fp = w.tryGet<Footprint>(e))
            for (int y = fp->y; y < fp->y + fp->h; ++y)
                for (int x = fp->x; x < fp->x + fp->w; ++x)
                    if (g.inside(x, y)) recvAt[size_t(g.index(x, y))] = int32_t(recv.size());
        recv.push_back({e, o.player, &mt});
    });
    for (Entity e : addBelt) {
        int x, y; g.cellXY(w.get<Transform>(e).pos, x, y);
        uint8_t d = 1;
        if (g.inside(x, y)) for (auto& p : M.beltPlan) if (p.first == g.index(x, y)) d = p.second;
        w.add<Belt>(e, Belt{d});
    }
    for (Entity e : addCrafter) {
        const MechType& mt = rules_.types[w.get<Unit>(e).type].mech;
        Crafter c;
        c.in.assign(rules_.resources.size(), Fixed{});
        c.out.assign(rules_.resources.size(), Fixed{});
        (void)mt;
        w.add<Crafter>(e, c);
    }
    // packets: advance, then hand over in cell order (a packet moves at most one cell per tick)
    for (size_t c = 0; c < n; ++c) {
        if (M.beltRes[c] < 0) continue;
        if (!dirAt[c]) { M.beltRes[c] = -1; M.beltAmt[c] = Fixed{}; M.beltProg[c] = Fixed{}; continue; }   // belt gone: packet lost
        M.beltProg[c] = fxMin(kOne, M.beltProg[c] + speedAt[c] * dt);
    }
    std::vector<uint8_t> moved(n, 0);
    for (size_t c = 0; c < n; ++c) {
        if (M.beltRes[c] < 0 || moved[c] || M.beltProg[c] < kOne) continue;
        const int x = int(c % size_t(g.w)) + kBdx[dirAt[c]], y = int(c / size_t(g.w)) + kBdy[dirAt[c]];
        if (!g.inside(x, y)) continue;
        const size_t nc = size_t(g.index(x, y));
        const int16_t res = M.beltRes[c];
        if (dirAt[nc]) {
            if (M.beltRes[nc] >= 0) continue;   // next piece busy
            M.beltRes[nc] = res; M.beltAmt[nc] = M.beltAmt[c]; M.beltProg[nc] = Fixed{}; moved[nc] = 1;
        } else if (recvAt[nc] >= 0) {
            const Recv& r = recv[size_t(recvAt[nc])];
            if (!(r.mt->beltAccepts & (uint64_t(1) << res))) continue;
            bool taken = false;
            if (auto* cr = w.tryGet<Crafter>(r.e); cr && !r.mt->craftIn.empty()) {
                for (auto& a : r.mt->craftIn)
                    if (a.res == uint16_t(res) && cr->in[size_t(res)] < a.value * 2) { cr->in[size_t(res)] += M.beltAmt[c]; taken = true; }
                if (!taken && r.mt->beltAccepts == 0) continue;
            }
            if (!taken) {
                bool forStock = true;
                for (auto& a : r.mt->craftIn) if (a.res == uint16_t(res)) forStock = false;   // a full crafter waits
                if (!forStock) continue;
                gs.players[r.owner].stock[size_t(res)] += M.beltAmt[c];
                sim.emit({kEvDelivered, r.owner, kNullEntity, r.e, int64_t(res), w.get<Transform>(r.e).pos});
            }
        } else {
            continue;   // end of the line: the packet waits
        }
        M.beltRes[c] = -1; M.beltAmt[c] = Fixed{}; M.beltProg[c] = Fixed{};
    }
    // producers and crafters
    for (const Recv& r : recv) {
        auto* cr = w.tryGet<Crafter>(r.e);
        if (!cr) continue;
        const MechType& mt = *r.mt;
        if (mt.outRes >= 0) {
            cr->outTimer += dt;
            if (cr->outTimer >= mt.outInterval && cr->out[size_t(mt.outRes)] < mt.outAmount * 4) {
                cr->outTimer = Fixed{};
                cr->out[size_t(mt.outRes)] += mt.outAmount;
            }
        }
        if (!mt.craftIn.empty()) {
            bool have = true;
            for (auto& a : mt.craftIn) if (cr->in[a.res] < a.value) have = false;
            bool room = true;
            for (auto& a : mt.craftOut) if (cr->out[a.res] >= a.value * 4) room = false;
            if (have && room) {
                cr->timer += dt * mechIncomeMult(sim, r.e);
                if (cr->timer >= mt.craftTime) {
                    cr->timer = Fixed{};
                    for (auto& a : mt.craftIn) cr->in[a.res] -= a.value;
                    for (auto& a : mt.craftOut) cr->out[a.res] += a.value;
                }
            }
        }
        // put one packet on a free adjacent belt that does not point back into this structure
        auto* fp = w.tryGet<Footprint>(r.e);
        if (!fp) continue;
        int res = -1;
        for (size_t k = 0; k < cr->out.size(); ++k) if (cr->out[k].raw > 0) { res = int(k); break; }
        if (res < 0) continue;
        Fixed amount = mt.outRes == res ? mt.outAmount : Fixed{};
        for (auto& a : mt.craftOut) if (a.res == uint16_t(res)) amount = a.value;
        amount = fxMin(amount.raw > 0 ? amount : cr->out[size_t(res)], cr->out[size_t(res)]);
        // ring around the footprint: top row, right column, bottom row, left column (fixed order)
        std::vector<std::pair<int, int>> ring;
        for (int x = fp->x; x < fp->x + fp->w; ++x) ring.push_back({x, fp->y - 1});
        for (int y = fp->y; y < fp->y + fp->h; ++y) ring.push_back({fp->x + fp->w, y});
        for (int x = fp->x + fp->w - 1; x >= fp->x; --x) ring.push_back({x, fp->y + fp->h});
        for (int y = fp->y + fp->h - 1; y >= fp->y; --y) ring.push_back({fp->x - 1, y});
        for (auto [x, y] : ring) {
            if (!g.inside(x, y)) continue;
            const size_t c = size_t(g.index(x, y));
            if (!dirAt[c] || M.beltRes[c] >= 0) continue;
            const int tx = x + kBdx[dirAt[c]], ty = y + kBdy[dirAt[c]];
            if (tx >= fp->x && tx < fp->x + fp->w && ty >= fp->y && ty < fp->y + fp->h) continue;   // points back in
            M.beltRes[c] = int16_t(res); M.beltAmt[c] = amount; M.beltProg[c] = Fixed{};
            cr->out[size_t(res)] -= amount;
            break;
        }
    }
}

void RtsModule::hashLogistics(Simulation& sim, Hasher& h) {
    const MechState& M = state(sim).mech;
    h.u32(0x10C1);
    for (size_t c = 0; c < M.beltRes.size(); ++c)
        if (M.beltRes[c] >= 0) { h.u32(uint32_t(c)); h.i32(M.beltRes[c]); h.fx(M.beltAmt[c]); h.fx(M.beltProg[c]); }
}

} // namespace rts::game
