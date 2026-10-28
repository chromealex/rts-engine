// Campaign trigger actions (objective, dialog, camera, endMission) and mission-to-mission carry. See RtsCampaign.h.
#include "RtsCampaign.h"
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

void RtsModule::campaignAction(Simulation& sim, const TrigAction& a, const std::vector<uint8_t>& players, Entity unit, Vec3 at) {
    GameState& gs = state(sim);
    MechState& m = gs.mech;
    gs.mechActive = true;
    const uint8_t who = players.empty() ? 0 : players[0];
    switch (a.kind) {
    case TrigAction::Objective: {
        auto it = std::find_if(m.objectives.begin(), m.objectives.end(), [&](const MechState::Objective& o) { return o.id == a.name; });
        if (a.op == "add") {
            if (it == m.objectives.end()) { m.objectives.push_back({a.name, a.text, 0, who, a.flag}); it = m.objectives.end() - 1; }
            else { it->state = 0; if (!a.text.empty()) it->text = a.text; }
        } else {
            if (it == m.objectives.end()) { m.objectives.push_back({a.name, a.text, 0, who, a.flag}); it = m.objectives.end() - 1; }
            it->state = a.op == "done" ? 1 : 2;
        }
        sim.emit({kEvObjective, who, kNullEntity, kNullEntity, int64_t(it - m.objectives.begin()) | (int64_t(it->state) << 16), at});
        break;
    }
    case TrigAction::Dialog:
        m.dialogs.push_back({a.speaker, a.portrait, a.voice, a.text, a.duration});
        sim.emit({kEvDialog, a.player.rel == 1 ? uint8_t(0) : who, unit, kNullEntity, int64_t(m.dialogs.size() - 1), at});
        break;
    case TrigAction::Camera: {
        Vec3 p = a.hasAt ? a.at : at;
        if (a.area >= 0) { const TrigArea& ar = triggers_.areas[size_t(a.area)]; p = {ar.x + ar.w / 2, Fixed{}, ar.z + ar.h / 2}; }
        int64_t ms = (a.duration * 1000).floorInt();
        sim.emit({kEvCamera, who, kNullEntity, kNullEntity, std::min<int64_t>(ms, 0x7FFFFFFF) | (a.flag ? (int64_t(1) << 31) : 0), p});
        break;
    }
    case TrigAction::EndMission: {
        const bool won = a.op == "victory";
        m.missionResult = int8_t(won ? 1 : -1);
        m.nextMission = a.name;
        if (won) gs.winnerTeam = who ? teamOf(sim, who) : 1;
        else for (uint8_t p : players) if (auto* pi = sim.player(p); pi && pi->alive) { pi->alive = false; sim.emit({kEvPlayerDefeated, p, kNullEntity, kNullEntity, 0, {}}); }
        sim.emit({kEvMissionEnd, who, kNullEntity, kNullEntity, won ? 1 : 0, at});
        break;
    }
    default: break;
    }
}

// ================================================================= carry between missions

Json collectCarry(Simulation& sim, const Json& spec, uint8_t player) {
    auto* mod = dynamic_cast<RtsModule*>(&sim.module());
    Json out = Json::object();
    if (!mod) return out;
    const Rules& R = mod->rules();
    GameState& gs = mod->state(sim);
    Json res = Json::object();
    for (auto& rj : spec["resources"].items()) {
        int ri = R.findResource(rj.str());
        if (ri >= 0 && player < gs.players.size()) res.set(rj.str(), Json::number(gs.players[player].stock[size_t(ri)]));
    }
    out.set("resources", res);
    Json units = Json::array();
    const int64_t max = spec.getInt("max", 64);
    World& w = sim.world();
    w.each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
        if (o.player != player || u.hp.raw <= 0 || int64_t(units.size()) >= max) return;
        const UnitType& t = R.types[u.type];
        if (t.structure) return;
        bool keep = false;
        for (auto& ty : spec["types"].items()) if (ty.str() == t.id) keep = true;
        for (auto& tg : spec["tags"].items()) if (t.tags & R.tagMask(tg.str())) keep = true;
        if (!keep) return;
        Json uj = Json::object();
        uj.set("type", Json::string(t.id));
        uj.set("hp", Json::number(u.hp));
        if (auto* v = w.tryGet<Veteran>(e)) { uj.set("rank", Json::number(int64_t(v->level))); uj.set("xp", Json::number(v->xp)); }
        if (auto* h = w.tryGet<Hero>(e)) { uj.set("level", Json::number(int64_t(h->level))); uj.set("heroXp", Json::number(h->xp)); }
        units.push(uj);
    });
    out.set("units", units);
    return out;
}

void applyCarry(Json& map, const Json& carry, uint8_t player) {
    if (player == 0 || map["players"].size() < player) return;
    Json& pl = map.member("players").at(player - 1);
    Json start = pl["start"];
    Fixed sx = start.isArray() ? start[size_t(0)].fx() : Fixed::fromInt(4), sz = start.isArray() ? start[size_t(1)].fx() : Fixed::fromInt(4);
    int k = 0;
    for (auto& uj : carry["units"].items()) {
        Json e = Json::object();
        e.set("type", uj["type"]);
        e.set("owner", Json::number(int64_t(player)));
        Json pos = Json::array();
        pos.push(Json::number(sx + Fixed::fromInt(2 + k % 4)));
        pos.push(Json::number(sz + Fixed::fromInt(k / 4)));
        e.set("pos", pos);
        map.member("entities").push(e);
        ++k;
    }
}

void restoreCarry(Simulation& sim, const Json& carry, uint8_t player) {
    auto* mod = dynamic_cast<RtsModule*>(&sim.module());
    if (!mod) return;
    World& w = sim.world();
    GameState& gs = mod->state(sim);
    for (auto& [id, v] : carry["resources"].members()) {   // on top of the mission's own starting stock
        int ri = mod->rules().findResource(id);
        if (ri >= 0 && player < gs.players.size()) gs.players[player].stock[size_t(ri)] += v.fx();
    }
    // carried units were appended last, in order: match them by type among the player's units, newest first
    std::vector<Entity> mine;
    w.each<Unit, Owner>([&](Entity e, Unit&, Owner& o) { if (o.player == player) mine.push_back(e); });
    size_t next = mine.size();
    const auto& items = carry["units"].items();
    for (size_t i = items.size(); i-- > 0;) {
        const int t = mod->rules().findType(items[i].getStr("type"));
        while (next > 0 && int(w.get<Unit>(mine[next - 1]).type) != t) --next;
        if (next == 0) break;
        Entity e = mine[--next];
        Unit& u = w.get<Unit>(e);
        if (items[i].has("hp")) u.hp = fxMin(items[i].getFx("hp"), mod->stat(sim, e, Stat::MaxHp));
        if (auto* h = w.tryGet<Hero>(e); h && items[i].has("level")) { h->level = int(items[i].getInt("level", 1)); h->xp = items[i].getFx("heroXp"); }
        if (items[i].has("rank")) {
            if (!w.has<Veteran>(e)) w.add<Veteran>(e, Veteran{});
            w.get<Veteran>(e).level = int(items[i].getInt("rank", 1));
            w.get<Veteran>(e).xp = items[i].getFx("xp");
            mod->state(sim).mechActive = true;
        }
    }
}

MissionOutcome missionOutcome(Simulation& sim, const Json& mission, uint8_t player) {
    MissionOutcome o;
    auto* mod = dynamic_cast<RtsModule*>(&sim.module());
    if (!mod) return o;
    const MechState& m = mod->state(sim).mech;
    const int res = sim.result();
    if (m.missionResult == 0 && res == 0) return o;
    o.finished = true;
    auto* pi = sim.player(player);
    o.won = m.missionResult != 0 ? m.missionResult > 0 : (res > 0 && pi && pi->team == res);
    o.next = !m.nextMission.empty() ? m.nextMission : mission.getStr("next");
    if (!o.won) o.next.clear();
    return o;
}

} // namespace rts::game
