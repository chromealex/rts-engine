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

VeterancyTable vetTable(const std::string& where, const Json& v) {
    VeterancyTable t;
    t.set = true;
    Fixed prev{};
    for (auto& x : v["xp"].items()) {
        if (x.fx() <= prev) fail(where + ".xp", "must increase");
        prev = x.fx();
        t.xp.push_back(x.fx());
    }
    for (auto& pl : v["perLevel"].items()) {
        bool ok = false;
        StatMod m;
        m.stat = parseStat(pl.getStr("stat"), &ok);
        if (!ok) fail(where + ".perLevel", "unknown stat '" + pl.getStr("stat") + "'");
        m.add = pl.getFx("add");
        m.mul = pl.getFx("mul", m.mul);
        t.perLevel.push_back(m);
    }
    return t;
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
    if (const Json& v = j["veterancy"]; v.isObject()) {
        m.vet = vetTable("veterancy", v);
        m.vetHeal = v.getBool("healOnLevel", true);
        m.vetFromCost = v.getFx("fromCost", m.vetFromCost);
        if (m.vet.xp.empty()) fail("veterancy.xp", "needs at least one rank");
        m.any = m.veterancy = true;
    }
    // ---- combat models
    auto classIndex = [&](const std::string& name) -> uint16_t {
        for (size_t i = 0; i < m.armorClasses.size(); ++i) if (m.armorClasses[i] == name) return uint16_t(i);
        if (m.armorClasses.size() >= 64) fail("armor classes", "at most 64");
        m.armorClasses.push_back(name);
        return uint16_t(m.armorClasses.size() - 1);
    };
    if (const Json& am = j["armorModel"]; !am.isNull()) {
        const std::string type = am.isString() ? am.str() : am.getStr("type", "subtract");
        if (type == "subtract") m.armorModel = ArmorModel::Subtract;
        else if (type == "percent") m.armorModel = ArmorModel::Percent;
        else if (type == "classes") m.armorModel = ArmorModel::Classes;
        else fail("armorModel", "expected subtract, percent or classes, got '" + type + "'");
        m.minDamageFraction = am.getFx("minFraction", m.armorModel == ArmorModel::Classes ? Fixed{} : m.minDamageFraction);
        m.minDamage = am.getFx("min", m.armorModel == ArmorModel::Classes ? Fixed::fromInt(1) : Fixed{});
        m.percentFactor = am.getFx("factor", m.percentFactor);
        for (auto& b : am["base"].items()) classIndex(b.str());
        m.baseClasses = int(m.armorClasses.size());
        if (m.armorModel == ArmorModel::Classes && m.baseClasses == 0) fail("armorModel.base", "the classes model needs base classes, e.g. [\"melee\", \"pierce\"]");
        m.any = true;
    }
    for (auto& rs : j["upkeepResources"].items()) {
        int ri = r.findResource(rs.str());
        if (ri < 0) fail("upkeepResources", "unknown resource '" + rs.str() + "'");
        m.upkeepMask |= 1u << ri;
    }
    size_t ti = 0;
    for (auto& [id, tj] : j["types"].members()) {
        UnitType& t = r.types[ti++];
        const std::string where = "type '" + id + "'";
        if (m.armorModel == ArmorModel::Classes) {
            for (auto& [cls, v] : tj["armors"].members()) {
                uint16_t ci = classIndex(cls);
                if (t.mech.armors.size() <= ci) t.mech.armors.resize(size_t(ci) + 1);
                t.mech.armors[ci] = v.fx();
                t.mech.armorClassMask |= uint64_t(1) << ci;
            }
            const Json& ws = tj["weapons"];
            t.mech.attacks.resize(ws.size());
            for (size_t wi = 0; wi < ws.size(); ++wi)
                for (auto& [cls, v] : ws[wi]["attacks"].members()) t.mech.attacks[wi].push_back({classIndex(cls), v.fx()});
        }
        if (const Json& sh = tj["shields"]; sh.isObject()) {
            t.mech.shieldMax = sh.getFx("max");
            if (t.mech.shieldMax.raw <= 0) fail(where + " shields.max", "must be > 0");
            t.mech.shieldRegen = sh.getFx("regen", Fixed::fromInt(2));
            t.mech.shieldDelay = sh.getFx("delay", t.mech.shieldDelay);
            t.mech.shieldArmor = sh.getFx("armor");
            m.any = m.shields = true;
        }
        if (const Json& rn = tj["resourceNode"]; rn.isObject() && rn.has("maxWorkers")) {
            t.mech.maxWorkers = int(rn.getInt("maxWorkers", 0));
            if (t.mech.maxWorkers > 0) m.any = m.workerCaps = true;
        }
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
        if (const Json& dj = tj["detects"]; !dj.isNull()) {
            mt.detects = dj.isObject() ? dj.getFx("radius") : dj.fx();
            if (mt.detects.raw <= 0) fail(where + " detects", "expected a radius > 0");
            m.any = m.detection = true;
            if (mt.detects > m.maxDetect) m.maxDetect = mt.detects;
        }
        const Json& abs = tj["abilities"];
        for (size_t ai = 0; ai < abs.size(); ++ai) {
            const Json& tg = abs[ai]["toggle"];
            if (tg.isNull()) continue;
            ToggleDef d;
            d.ability = int(ai);
            int st = r.findStatus(tg.getStr("status"));
            if (st < 0) fail(where + " ability " + std::to_string(ai) + " toggle.status", "unknown status '" + tg.getStr("status") + "'");
            d.status = int16_t(st);
            d.drain = tg.getFx("drain");
            mt.toggles.push_back(d);
            m.any = m.toggles = true;
        }
        if (const Json& v = tj["veterancy"]; !v.isNull()) {
            if (v.type() == Json::Type::Bool) mt.veterancy = v.boolean() ? -1 : 0;
            else { mt.veterancy = 1; mt.vetTable = vetTable(where + " veterancy", v); m.any = m.veterancy = true; }
        }
    }
    ti = 0;
    for (auto& [id, tj] : j["types"].members()) {
        UnitType& t = r.types[ti++];
        if (tj.has("xp")) t.mech.killXp = t.xpValue;
        else { for (auto& a : t.cost) t.mech.killXp += a.value; t.mech.killXp = t.mech.killXp * m.vetFromCost; }
    }
    for (auto& t : r.types)
        if (t.mech.transport.slots > 0 && !t.mech.transport.killOnDeathSet) t.mech.transport.killOnDeath = t.structure ? Fixed{} : Fixed::fromInt(1);
}

} // namespace rts::game
