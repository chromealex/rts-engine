// Data of the foundation / trap / group-teleport / charge mechanics (RtsFidelity.cpp): concrete slabs and
// weathering (Dune II), traps (Dungeon Keeper, Stronghold, They Are Billions). The two effect kinds
// (addCharges, teleportGroup) are parsed with the other effects in RtsEffectsData.cpp.
// Every block is optional; without them a game loads and plays exactly as before.
#include "RtsEffectsData.h"
#include <stdexcept>

namespace rts::game {

namespace {
[[noreturn]] void fail(const std::string& where, const std::string& what) { throw std::runtime_error(where + ": " + what); }
} // namespace

void loadFidelity(Rules& r, const Json& j, EffectParser& ep) {
    bool any = false;
    // ---- concrete foundations
    if (const Json& f = j["foundation"]; f.isObject() || f.boolean(false)) {
        FoundationRules& fr = r.foundation;
        fr.enabled = true;
        if (f.isObject()) {
            fr.startHp = f.getFx("startHp", fr.startHp);
            fr.decay = f.getFx("decay", fr.decay);
            fr.floor = f.getFx("floor", fr.floor);
            fr.lostWithBuilding = f.getBool("lostWithBuilding", fr.lostWithBuilding);
        }
        if (fr.startHp.raw <= 0 || fr.startHp > Fixed::fromInt(1)) fail("foundation.startHp", "must be in (0, 1]");
        if (fr.floor.raw < 0 || fr.floor > Fixed::fromInt(1)) fail("foundation.floor", "must be in [0, 1]");
        if (fr.decay.raw < 0) fail("foundation.decay", "must be >= 0");
        any = true;
    }
    size_t ti = 0;
    for (auto& [id, tj] : j["types"].members()) {
        if (ti >= r.types.size()) break;
        UnitType& t = r.types[ti++];
        const std::string where = "type '" + id + "'";
        if (tj.has("foundation")) {
            t.foundation = tj.getBool("foundation", false);
            if (t.foundation && !t.structure) fail(where, "foundation: only structures can be concrete slabs");
            if (t.foundation && !r.foundation.enabled) r.foundation.enabled = true;
            any = any || t.foundation;
        }
        t.weathers = tj.getBool("weathers", true);
        if (const Json& tr = tj["trap"]; tr.isObject()) {
            if (!t.structure) fail(where, "trap: only structures can be traps");
            const std::string tw = where + " trap";
            TrapDef d;
            d.radius = tr.getFx("radius", d.radius);
            if (d.radius.raw <= 0) fail(tw, "radius must be positive");
            EffectDef holder;              // filter parsed like an effect's: enemy units by default
            holder.enemies = true; holder.allies = false;
            const Json& fj = tr["trigger"];
            ep.filter(tw + " trigger", fj.isObject() ? fj : tr, holder);
            if (!(fj.isObject() ? fj : tr).has("kind")) holder.filter.structures = false;
            holder.filter.shape = Shape::Circle;
            d.filter = holder.filter;
            d.payload = int16_t(ep.payload(tw, tr["effects"]));
            if (d.payload < 0) fail(tw, "needs 'effects'");
            d.charges = int(tr.getInt("charges", 0));
            if (d.charges < 0) fail(tw, "charges must be >= 0");
            d.rearm = tr.getFx("rearm", d.rearm);
            d.armTime = tr.getFx("armTime");
            d.hidden = tr.getBool("hidden", true);
            d.revealFor = tr.getFx("revealFor", d.revealFor);
            d.removeWhenEmpty = tr.getBool("removeWhenEmpty", true);
            if (r.traps.size() >= 30000) fail(tw, "too many traps");
            t.trap = int16_t(r.traps.size());
            r.traps.push_back(d);
            any = true;
        } else if (tj.has("trap")) fail(where, "trap must be an object");
    }
    for (auto& pl : r.payloads)
        for (auto& e : pl)
            if (e.kind == EffectKind::AddCharges || e.kind == EffectKind::TeleportGroup) any = true;
    for (auto& a : r.playerAbilities) if (a.target == TargetKind::PointPair) any = true;
    r.fidelity = any;
}

} // namespace rts::game
