// Save states of the RTS module (Simulation::serialize): every component pool in dense order and the dynamic
// part of GameState, field by field (engine/core/Archive.h). Rules, triggers, the planet surface and derived
// pathing caches come from setup on the loading side. Format rule: append fields at the end of a type's list
// and bump kSaveVersion in Simulation.cpp; a component type missing from kPools makes saving fail loudly.
#include "RtsInternal.h"
#include <algorithm>

namespace rts {
inline void io(Archive& a, Transform& x) { a(x.pos, x.prevPos, x.yaw); }
inline void io(Archive& a, Owner& x) { a(x.player); }
}

namespace rts::game {

void io(Archive& a, Unit& x) { a(x.type, x.hp, x.flags, x.lastAttacker, x.facing, x.energy, x.lastHitBy); }
void io(Archive& a, Order& x) { a(x.type, x.target, x.point, x.point2, x.param, x.state, x.timer, x.flow); }
void io(Archive& a, Orders& x) { a(x.queue); }
void io(Archive& a, Mobile& x) { a(x.path, x.pathPos, x.goal, x.moving, x.goalRadius, x.stuck, x.roadPath, x.vel, x.yieldHome, x.yieldUntil, x.yielded, x.flow, x.flowRadius, x.flowStuck); }
void io(Archive& a, Combat& x) { a(x.cooldown, x.target, x.acquireTimer, x.autoTarget, x.chargeTarget, x.chargeStart, x.slotTarget, x.slot, x.slotTick, x.slotPos); }
void io(Archive& a, Cargo& x) { a(x.res, x.amount, x.lastNode, x.lastPos, x.field); }
void io(Archive& a, ProdItem& x) { a(x.item, x.progress, x.paid, x.upgrade); }
void io(Archive& a, Production& x) { a(x.queue, x.rally, x.hasRally, x.rallyTarget, x.ready, x.charges, x.chargeTimer); }
void io(Archive& a, Construction& x) { a(x.progress, x.paid, x.builder, x.selfBuild); }
void io(Archive& a, ResourceStock& x) { a(x.res, x.amount, x.infinite); }
void io(Archive& a, Extractor& x) { a(x.nodeType, x.remaining); }
void io(Archive& a, Footprint& x) { a(x.x, x.y, x.w, x.h); }
void io(Archive& a, Garrison& x) { a(x.count, x.growth, x.damageAcc, x.pendingOut, x.lost, x.pendingGroup, x.pendingTarget); }
void io(Archive& a, Projectile& x) {
    a(x.target, x.source, x.aim, x.speed, x.damage, x.splash, x.damageType, x.owner, x.layers, x.effList, x.effIndex, x.onHit,
      x.onHitChance, x.sourceType, x.start, x.travel, x.total, x.scale);
}
void io(Archive& a, Hero& x) { a(x.level, x.xp); }
void io(Archive& a, Transit& x) { a(x.from, x.to, x.depart, x.arrive, x.fromWorld, x.toWorld, x.arrival); }
void io(Archive& a, Squad& x) { a(x.reinforceTimer, x.reinforcing); }
void io(Archive& a, Buff& x) { a(x.stat, x.add, x.mul, x.until, x.sourceType); }
void io(Archive& a, Buffs& x) { a(x.list, x.stunUntil); }
void io(Archive& a, AbilityState& x) { a(x.readyAt, x.charges, x.chargeAt, x.init, x.busyUntil); }
void io(Archive& a, StatusInst& x) {
    a(x.def, x.owner, x.source, x.sourceType, x.until, x.nextTick, x.stacks, x.shield, x.acc, x.link, x.revertType, x.revertOwner, x.revertsOwner, x.capturedBy);
}
void io(Archive& a, Statuses& x) { a(x.list, x.flags); }
void io(Archive& a, Area& x) {
    a(x.caster, x.sourceType, x.effList, x.effIndex, x.fireAt, x.nextAt, x.endAt, x.radius, x.triggers, x.touched, x.dir, x.dest, x.lane, x.lanePos,
      x.laneReverse, x.follow, x.scale);
}
void io(Archive& a, Channel& x) { a(x.payload, x.target, x.point, x.startPos, x.nextAt, x.endAt, x.ability, x.interval); }
void io(Archive& a, GroupTag& x) { a(x.id, x.from, x.to); }
void io(Archive& a, Capture& x) { a(x.progress, x.team); }
void io(Archive& a, Guard& x) { a(x.home, x.leash, x.returning, x.nextWander); }
void io(Archive& a, Summon& x) { a(x.expire, x.summonerType, x.summoner); }
void io(Archive& a, Needs& x) { a(x.hunger, x.sleep, x.happiness, x.payTimer, x.unhappyTime, x.leaving); }
void io(Archive& a, Possessed& x) { a(x.dir, x.target, x.fire, x.lastTick); }
void io(Archive& a, PortalState& x) { a(x.timer); }
void io(Archive& a, AutoTrainState& x) { a(x.timer); }
void io(Archive& a, ClaimState& x) { a(x.acc); }
void io(Archive& a, Advancer& x) { a(x.unused); }
void io(Archive& a, Inventory& x) { a(x.have, x.incoming, x.reserved); }
void io(Archive& a, Workshop& x) { a(x.progress, x.running); }
void io(Archive& a, Carrier& x) { a(x.from, x.to, x.res, x.amount, x.loaded); }
void io(Archive& a, Lane& x) { a(x.a, x.b, x.offset, x.points); }
void io(Archive& a, LaneWalker& x) { a(x.lane, x.pos, x.reverse); }
// agent-mech
void io(Archive& a, Stance& x) { a(x.mode, x.home, x.homeSet); }
void io(Archive& a, Retreating& x) { a(x.to, x.point); }
void io(Archive& a, Transport& x) { a(x.cargo, x.used, x.unloadAt, x.unloading, x.unloadAtPoint, x.nextUnload); }
void io(Archive& a, Boarded& x) { a(x.carrier, x.fires); }
void io(Archive& a, Veteran& x) { a(x.level, x.xp, x.lastHit); }
void io(Archive& a, RallyPath& x) { a(x.points); }
void io(Archive& a, Shield& x) { a(x.value, x.lastHit); }   // StarCraft shields
void io(Archive& a, Eater& x) { a(x.kills); }               // originals (sandworm)
void io(Archive& a, Weathering& x) { a(x.bare); }                 // fidelity (Dune II foundations)
void io(Archive& a, TrapState& x) { a(x.charges, x.readyAt); }    // fidelity traps
void io(Archive& a, TeleportBack& x) { a(x.home, x.at, x.caster); }   // fidelity group teleports
void io(Archive& a, Formation& x) { a(x.group, x.shape, x.offset); }
void io(Archive& a, BridgeLaid& x) { a(x.laid, x.cells); }
void io(Archive& a, Ammo& x) { a(x.left, x.init); }
void io(Archive& a, Merging& x) { a(x.partner); }
void io(Archive& a, Trader& x) { a(x.home, x.dest, x.loaded); }
void io(Archive& a, ItemBag& x) { a(x.items, x.charges); }
void io(Archive& a, GroundItem& x) { a(x.pos, x.item, x.charges); }
void io(Archive& a, MechState::Objective& x) { a(x.id, x.text, x.state, x.player, x.primary); }
void io(Archive& a, MechState::Dialog& x) { a(x.speaker, x.portrait, x.voice, x.text, x.duration); }
void io(Archive& a, Bounty& x) { a(x.pos, x.target, x.player, x.res, x.reward); }
void io(Archive& a, RoundUnit& x) { a(x.owner, x.type, x.pos); }
void io(Archive& a, MechState& x) {
    a(x.prices, x.groundItems, x.decks, x.allyRequest, x.cyclePhase, x.nextGroup, x.objectives, x.dialogs, x.nextMission, x.missionResult,
      x.cover, x.bounties, x.roundPhase, x.roundIndex, x.phaseEnd, x.roundSnapshot, x.beltRes, x.beltAmt, x.beltProg, x.beltPlan, x.turnPlayer, x.turnIndex, x.turnStart);
}
void io(Archive& a, Suppression& x) { a(x.value); }
void io(Archive& a, Belt& x) { a(x.dir); }
void io(Archive& a, TurnBudget& x) { a(x.move, x.shots, x.lastPos, x.turn); }
void io(Archive& a, Altitude& x) { a(x.target, x.y); }
void io(Archive& a, Crafter& x) { a(x.in, x.out, x.timer, x.outTimer); }
void io(Archive& a, Morale& x) { a(x.value, x.fatigue, x.run, x.lastPos, x.chargeTick, x.lastCharge, x.lastFire, x.state, x.init, x.outOfSupply); }
void io(Archive& a, Turret& x) { a(x.dir, x.init); }
void io(Archive& a, GrowState& x) { a(x.timer); }
// state
void io(Archive& a, TypeMods& x) { a(x.add, x.mul); }
void io(Archive& a, PlayerEconomy& x) {
    a(x.stock, x.cap, x.income, x.capProvided, x.capUsed, x.disabledTags, x.productionMult, x.supplyUsed, x.supplyCap, x.typeCount, x.typeCountAll,
      x.researched, x.researching, x.mods, x.gatherMult, x.regions, x.faction, x.typeDisabled, x.statuses, x.statusFlags);
}
void io(Archive& a, WaveDef& x) { a(x.tick, x.player, x.spawn, x.at, x.spread, x.every, x.growth, x.repeats, x.done, x.route); }
void io(Archive& a, RoomZone& x) { a(x.type, x.owner, x.cells, x.center); }
void io(Archive& a, PlayerAbilityState& x) { a(x.readyAt, x.charges, x.rearm, x.owned, x.level, x.stolenUntil, x.pendingAt, x.pendingPoint, x.pendingTarget, x.nextAuto); }
void io(Archive& a, VictoryRules& x) { a(x.mode, x.tickets, x.ticketDrain, x.surviveSeconds, x.keyLoses, x.share); }
void io(Archive& a, ScriptData& x) { a(x.blob, x.hash); x.serial = 0; }   // scripting: game.data (serial: VM-side only)
void io(Archive& a, ScriptStats::Entry& x) { a(x.stat, x.base); }   // scripting: game.set_stat_base
void io(Archive& a, ScriptStats& x) { a(x.list); }
void io(Archive& a, ScriptStore::Timer& x) { a(x.tick, x.seq, x.fn, x.arg); }   // scripting (RtsScript.h)
void io(Archive& a, ScriptStore& x) { a(x.state, x.started, x.disabled, x.disabledFns, x.inside, x.timers, x.timerSeq, x.cmdReady); }
void io(Archive& a, TriggerState& x) { a(x.enabled, x.fired, x.inside, x.edge, x.vars, x.messages, x.started); }
void io(Archive& a, RoadGraph& x) { a(x.nodes, x.adj); }
void io(Archive& a, SpatialHash& x) { a(x.cell, x.w, x.h, x.buckets, x.used); }

namespace {
// Run-length encoding for big per-cell layers (most cells repeat their neighbour).
template <class T> void ioRle(Archive& a, std::vector<T>& v) {
    if (a.writing()) {
        uint32_t n = uint32_t(v.size());
        io(a, n);
        size_t i = 0;
        while (i < v.size()) {
            size_t j = i + 1;
            while (j < v.size() && v[j] == v[i] && j - i < 0x7FFFFFFF) ++j;
            uint32_t run = uint32_t(j - i);
            T val = v[i];
            io(a, run); io(a, val);
            i = j;
        }
        return;
    }
    uint32_t n = 0;
    io(a, n);
    if (n > 64u * 1024 * 1024) { a.fail(); return; }
    v.assign(n, T{});
    size_t i = 0;
    while (i < n && a.ok()) {
        uint32_t run = 0; T val{};
        io(a, run); io(a, val);
        if (run == 0 || i + run > n) { a.fail(); return; }
        std::fill(v.begin() + ptrdiff_t(i), v.begin() + ptrdiff_t(i + run), val);
        i += run;
    }
}
template <class T> void ioRle(Archive& a, std::vector<std::vector<T>>& v) {
    uint32_t n = a.count(v.size());
    if (!a.writing()) v.assign(n, {});
    for (auto& x : v) ioRle(a, x);
}

void ioGrid(Archive& a, MapGrid& g) {
    a(g.w, g.h, g.hf, g.slopeLimit, g.sandLimit, g.buildLimit);
    ioRle(a, g.flags); ioRle(a, g.level); ioRle(a, g.height); ioRle(a, g.corner); ioRle(a, g.slope);
    ioRle(a, g.blocked); ioRle(a, g.creep); ioRle(a, g.fieldRes); ioRle(a, g.fieldAmt); ioRle(a, g.region);
    ioRle(a, g.dig); ioRle(a, g.digHp); ioRle(a, g.digMark); ioRle(a, g.territory); ioRle(a, g.room);
    ioRle(a, g.foundation);   // fidelity: concrete slabs (Dune II)
}

void ioState(Archive& a, GameState& s) {
    a(s.mech, s.mechActive, s.players);
    ioGrid(a, s.grid);
    a(s.roads, s.hash, s.roadsOnly);
    ioRle(a, s.vision);
    a(s.visibleCells, s.fogMode);
    ioRle(a, s.visionBase); ioRle(a, s.visionOcc);
    a(s.regionCount, s.regionOwner, s.regionAdj, s.regionNames, s.regionCenter, s.victory, s.tickets, s.wavePlayers, s.waves, s.fieldsDirty,
      s.nextWaveTick, s.score, s.autoBuilt, s.winnerTeam, s.trig, s.territoryCells, s.territoryTotal, s.territoryDirty, s.roomZones, s.roomCells,
      s.roomsDirty, s.hand, s.possessed, s.playerAbilities, s.nextGroupId);
    ioRle(a, s.roadComp);
    a(s.roadsDirty, s.lanes, s.lanesDirty, s.laneSignature, s.laneRoundRobin);
}

template <class T> void ioPool(Archive& a, World& w) {
    Pool<T>& p = w.pool<T>();
    uint32_t n = a.count(p.size(), 8);
    if (a.writing()) {
        for (size_t i = 0; i < p.size(); ++i) { Entity e = p.entityAt(i); io(a, e); io(a, p.dataAt(i)); }
        return;
    }
    p.clear();
    for (uint32_t i = 0; i < n && a.ok(); ++i) {
        Entity e; T v{};
        io(a, e); io(a, v);
        p.emplace(e, std::move(v));
    }
}

template <class... T> struct PoolList {
    static constexpr size_t count = sizeof...(T);
    static void run(Archive& a, World& w) { (ioPool<T>(a, w), ...); }
    static void touch(World& w) { ((void)w.pool<T>(), ...); }
};
using kPools = PoolList<Transform, Owner, Unit, Orders, Mobile, Combat, Cargo, Production, Construction, ResourceStock, Extractor, Footprint,
                        Garrison, Projectile, Hero, Transit, Squad, Buffs, AbilityState, Statuses, Area, Channel, GroupTag, Capture, Guard, Summon,
                        Needs, Possessed, PortalState, AutoTrainState, ClaimState, Advancer, Inventory, Workshop, Carrier, LaneWalker,
                        Stance, Retreating, Transport, Boarded, Veteran, RallyPath, Shield, Eater, Formation, BridgeLaid, Ammo, Merging, Trader, ItemBag,
                        Suppression, Turret, GrowState, Morale,
                        Weathering, TrapState, TeleportBack, Belt, Crafter, TurnBudget, Altitude, ScriptData, ScriptStats>;
} // namespace

bool RtsModule::serializeState(Simulation& sim, Archive& a) {
    World& w = sim.world();
    GameState& gs = state(sim);
    if (a.writing()) {
        if (script_) script_->flush(sim);   // Lua state / entity data are written into the world lazily
        // every component type the world has must be in kPools (+1: the GameState resource is not a pool)
        kPools::touch(w);   // creates the listed pools that don't exist yet
    }
    uint32_t pools = uint32_t(kPools::count);
    io(a, pools);
    if (a.writing() && w.poolCount() != kPools::count) return false;   // an unlisted component type exists
    if (!a.writing() && pools != kPools::count) { a.fail(); return false; }
    kPools::run(a, w);
    ioState(a, gs);
    // Lua state of games with their own scripts (the standard library keeps its state in GameState)
    if (auto* ss = w.resource<ScriptStore>(); ss && ss->userScripts) {
        io(a, *ss);
        if (!a.writing()) ss->serial = 0;   // never a VM write stamp: the VM restores `state` from it
    }
    if (!a.writing()) {
        gs.grid.invalidatePathing();   // derived caches (clearance, HPA*) rebuild from the loaded cells
        gs.grid.clearSoft();
        gs.mech.groupSpeed.clear();
        // flow fields are a cache (pure functions of the grid): re-acquire the ones moving units follow, in ECS order,
        // so they keep following them instead of dropping to A* (sysFlowFields drops units whose field is gone)
        w.each<Mobile, Unit>([&](Entity, Mobile& m, Unit& u) {
            if (!m.flow) return;
            const UnitType& t = rules_.types[u.type];
            if (gs.grid.flowAcquire(m.goal, m.flowRadius, t.pathLayer, MapGrid::clearanceFor(t.radius), sim.tick()) < 0) m.flow = false;
        });
        slotClaims_.clear();
    }
    return a.ok();
}

} // namespace rts::game
