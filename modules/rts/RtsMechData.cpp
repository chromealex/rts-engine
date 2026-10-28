// Parsing of the agent-mech blocks (see RtsMechData.h). Every key is optional; errors name the place.
#include "RtsData.h"
#include <stdexcept>

namespace rts::game {

namespace {
[[noreturn]] void fail(const std::string& where, const std::string& what) { throw std::runtime_error(where + ": " + what); }

uint8_t layersOf(const std::string& where, const Json& j, uint8_t def) {
    if (j.isNull()) return def;
    auto one = [&](const std::string& s) -> uint8_t {
        if (s == "ground") return kLayerGround;
        if (s == "air") return kLayerAir;
        if (s == "naval") return kLayerNaval;
        if (s == "sand") return kLayerSandOnly;
        fail(where, "unknown layer '" + s + "' (ground, air, naval, sand)");
    };
    if (j.isString()) return one(j.str());
    uint8_t m = 0;
    for (auto& v : j.items()) m |= one(v.str());
    return m ? m : def;
}

uint64_t tagsOf(const Rules& r, const std::string& where, const Json& j) {
    uint64_t m = 0;
    for (auto& t : j.items()) {
        uint64_t b = r.tagMask(t.str());
        if (!b) fail(where, "unknown tag '" + t.str() + "' (no type has it)");
        m |= b;
    }
    return m;
}

StanceMode stanceOf(const std::string& where, const std::string& s) {
    if (s == "aggressive") return StanceMode::Aggressive;
    if (s == "defensive") return StanceMode::Defensive;
    if (s == "hold" || s == "standGround") return StanceMode::Hold;
    if (s == "passive" || s == "noAttack") return StanceMode::Passive;
    fail(where, "unknown stance '" + s + "' (aggressive, defensive, hold, passive)");
}
} // namespace

void loadMechRules(Rules& r, const Json& j) {
    MechRules& m = r.mech;
    if (const Json& st = j["stances"]; st.isObject()) {
        m.any = true;
        m.defendRadius = st.getFx("defendRadius", m.defendRadius);
    }
    if (const Json& rt = j["retreat"]; rt.isObject() || rt.boolean()) {
        m.any = m.retreat = true;
        m.retreatSpeed = rt.getFx("speed", m.retreatSpeed);
        m.retreatDamageTaken = rt.getFx("damageTaken", m.retreatDamageTaken);
    }
    size_t ti = 0;
    for (auto& [id, tj] : j["types"].members()) {
        UnitType& t = r.types[ti++];
        const std::string where = "type '" + id + "'";
        MechType& mt = t.mech;
        if (tj.has("stance")) {
            mt.stance = stanceOf(where + " stance", tj.getStr("stance"));
            mt.stanceSet = true;
            m.any = true;
        }
        mt.retreatPoint = tj.getBool("retreatPoint", false);
        mt.cargoSize = int(tj.getInt("cargoSize", 1));
        if (mt.cargoSize < 1) fail(where + " cargoSize", "must be at least 1");
        if (const Json& tr = tj["transport"]; tr.isObject()) {
            TransportDef& d = mt.transport;
            d.slots = int(tr.getInt("slots", 0));
            if (d.slots < 1 || d.slots > 64) fail(where + " transport.slots", "expected 1..64");
            d.acceptTags = tagsOf(r, where + " transport.accepts", tr["accepts"]);
            d.acceptLayers = layersOf(where + " transport.layers", tr["layers"], kLayerGround);
            d.fireFrom = int(tr.getInt("fireFrom", 0));
            d.loadRange = tr.getFx("loadRange", d.loadRange);
            d.unloadInterval = tr.getFx("unloadInterval", d.unloadInterval);
            d.killOnDeathSet = tr.has("killOnDeath");
            d.killOnDeath = tr.getFx("killOnDeath", d.killOnDeath);
            d.heal = tr.getFx("heal");
            d.damagePerCargo = tr.getFx("damagePerCargo");
            d.weaponFromCargo = tr.getBool("weaponFromCargo", false);
            m.any = m.transports = true;
        }
    }
    for (auto& t : r.types)
        if (t.mech.transport.slots > 0 && !t.mech.transport.killOnDeathSet) t.mech.transport.killOnDeath = t.structure ? Fixed{} : Fixed::fromInt(1);
}

} // namespace rts::game
