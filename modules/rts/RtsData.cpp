#include "RtsData.h"
#include <algorithm>
#include <stdexcept>

namespace rts::game {

int Rules::findType(const std::string& id) const {
    for (size_t i = 0; i < types.size(); ++i) if (types[i].id == id) return int(i);
    return -1;
}
int Rules::findResource(const std::string& id) const {
    for (size_t i = 0; i < resources.size(); ++i) if (resources[i].id == id) return int(i);
    return -1;
}
Fixed Rules::damageMult(uint16_t d, uint16_t a) const {
    if (d < damageTable.size() && a < damageTable[d].size()) return damageTable[d][a];
    return Fixed::fromInt(1);
}

namespace {
[[noreturn]] void fail(const std::string& where, const std::string& what) { throw std::runtime_error(where + ": " + what); }

uint8_t parseLayers(const Json& j, uint8_t def) {
    if (j.isString()) {
        std::string s = j.str();
        return s == "air" ? uint8_t(kLayerAir) : s == "naval" ? uint8_t(kLayerNaval) : s == "all" ? uint8_t(7) : uint8_t(kLayerGround);
    }
    if (!j.isArray()) return def;
    uint8_t m = 0;
    for (auto& v : j.items()) m |= parseLayers(v, 0);
    return m ? m : def;
}

int indexOf(const std::vector<std::string>& v, const std::string& s) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == s) return int(i);
    return -1;
}
} // namespace

Rules loadRules(const Json& j) {
    Rules r;
    r.supplyCap = j.getFx("supplyCap", r.supplyCap);
    r.victory = j.getStr("victory", r.victory);
    r.maxQueue = int(j.getInt("maxQueue", r.maxQueue));

    for (auto& [id, rj] : j["resources"].members()) {
        ResourceDef d;
        d.id = id;
        d.name = rj.getStr("name", id);
        d.kind = rj.getStr("kind", "stock") == "flow" ? ResourceKind::Flow : ResourceKind::Stock;
        d.start = rj.getFx("start");
        d.storage = rj.getFx("storage");
        r.resources.push_back(d);
    }
    if (r.resources.size() > 32) fail("resources", "at most 32 resources are supported");

    // armor / damage tables: "damage": { "normal": { "light": 1.0, "heavy": 0.7 }, ... }
    r.armorTypes = {"none"};
    r.damageTypes = {"normal"};
    for (auto& a : j["armorTypes"].items()) if (indexOf(r.armorTypes, a.str()) < 0) r.armorTypes.push_back(a.str());
    for (auto& [dt, row] : j["damageTable"].members()) {
        if (indexOf(r.damageTypes, dt) < 0) r.damageTypes.push_back(dt);
        for (auto& [at, v] : row.members()) if (indexOf(r.armorTypes, at) < 0) r.armorTypes.push_back(at);
    }
    r.damageTable.assign(r.damageTypes.size(), std::vector<Fixed>(r.armorTypes.size(), Fixed::fromInt(1)));
    for (auto& [dt, row] : j["damageTable"].members())
        for (auto& [at, v] : row.members()) r.damageTable[size_t(indexOf(r.damageTypes, dt))][size_t(indexOf(r.armorTypes, at))] = v.fx();

    // pass 1: names, so types can reference each other in any order
    for (auto& [id, tj] : j["types"].members()) { UnitType t; t.id = id; r.types.push_back(t); }
    auto typeRef = [&](const std::string& where, const Json& v) -> uint16_t {
        int k = r.findType(v.str());
        if (k < 0) fail(where, "unknown type '" + v.str() + "'");
        return uint16_t(k);
    };
    auto amounts = [&](const std::string& where, const Json& obj, std::vector<Amount>& out, std::initializer_list<const char*> skip) {
        for (auto& [k, v] : obj.members()) {
            bool skipped = false;
            for (auto* s : skip) if (k == s) skipped = true;
            if (skipped) continue;
            int ri = r.findResource(k);
            if (ri < 0) fail(where, "unknown resource '" + k + "'");
            out.push_back({uint16_t(ri), v.fx()});
        }
    };

    size_t ti = 0;
    for (auto& [id, tj] : j["types"].members()) {
        UnitType& t = r.types[ti++];
        const std::string where = "type '" + id + "'";
        t.name = tj.getStr("name", id);
        if (const Json& h = tj["health"]; h.isObject()) {
            t.maxHp = h.getFx("max", t.maxHp);
            t.armor = h.getFx("armor");
            t.regen = h.getFx("regen");
            std::string at = h.getStr("armorType", "none");
            int ai = indexOf(r.armorTypes, at);
            if (ai < 0) { r.armorTypes.push_back(at); for (auto& row : r.damageTable) row.push_back(Fixed::fromInt(1)); ai = int(r.armorTypes.size() - 1); }
            t.armorType = uint16_t(ai);
        }
        t.invulnerable = tj.getBool("invulnerable", false);
        if (const Json& m = tj["move"]; m.isObject()) {
            t.speed = m.getFx("speed", Fixed::fromInt(2));
            t.layer = parseLayers(m["layer"], kLayerGround);
            t.radius = m.getFx("radius", t.radius);
        }
        if (const Json& s = tj["structure"]; s.isObject()) {
            t.structure = true;
            t.footprintW = int(s["footprint"][0].integer(2));
            t.footprintH = int(s["footprint"][1].integer(2));
            t.radius = Fixed::fromInt(std::max(t.footprintW, t.footprintH)) / 2;
            t.speed = Fixed{};
        }
        t.sight = tj.getFx("sight", t.sight);
        for (auto& wj : tj["weapons"].items()) {
            WeaponDef w;
            w.id = wj.getStr("id", "weapon");
            w.range = wj.getFx("range", w.range);
            w.minRange = wj.getFx("minRange");
            w.damage = wj.getFx("damage", w.damage);
            w.cooldown = wj.getFx("cooldown", w.cooldown);
            w.targets = parseLayers(wj["targets"], kLayerGround);
            w.projectileSpeed = wj.getFx("projectileSpeed");
            w.splash = wj.getFx("splash");
            std::string dt = wj.getStr("damageType", "normal");
            int di = indexOf(r.damageTypes, dt);
            if (di < 0) { r.damageTypes.push_back(dt); r.damageTable.push_back(std::vector<Fixed>(r.armorTypes.size(), Fixed::fromInt(1))); di = int(r.damageTypes.size() - 1); }
            w.damageType = uint16_t(di);
            t.weapons.push_back(w);
        }
        if (const Json& c = tj["cost"]; c.isObject()) {
            amounts(where + " cost", c, t.cost, {"time", "supply"});
            t.buildTime = c.getFx("time", t.buildTime);
            t.supplyUsed = c.getFx("supply");
        }
        t.supplyProvided = tj.getFx("supply");
        amounts(where + " income", tj["income"], t.income, {});
        amounts(where + " storage", tj["storage"], t.storage, {});
        for (auto& p : tj["produces"].items()) t.produces.push_back(typeRef(where + " produces", p));
        for (auto& b : tj["builds"].items()) t.builds.push_back(typeRef(where + " builds", b));
        for (auto& rq : tj["requires"].items()) t.requires_.push_back(typeRef(where + " requires", rq));
        for (auto& [res, g] : tj["gather"].members()) {
            int ri = r.findResource(res);
            if (ri < 0) fail(where + " gather", "unknown resource '" + res + "'");
            t.gathers.push_back({uint16_t(ri), g.getFx("carry", Fixed::fromInt(10)), g.getFx("time", Fixed::fromInt(2))});
        }
        if (const Json& rn = tj["resourceNode"]; rn.isObject()) {
            int ri = r.findResource(rn.getStr("resource"));
            if (ri < 0) fail(where + " resourceNode", "unknown resource '" + rn.getStr("resource") + "'");
            t.resourceNode = int16_t(ri);
            t.resourceAmount = rn.getFx("amount", Fixed::fromInt(1000));
        }
        for (auto& d : tj["dropoff"].items()) {
            int ri = r.findResource(d.str());
            if (ri < 0) fail(where + " dropoff", "unknown resource '" + d.str() + "'");
            t.dropoffMask |= 1u << ri;
        }
        if (const Json& g = tj["garrison"]; g.isObject()) {
            t.garrisonMax = int(g.getInt("max", 20));
            if (g.has("unit")) t.spawnUnit = typeRef(where + " garrison.unit", g["unit"]);
            t.spawnInterval = g.getFx("interval");
            t.capturable = g.getBool("capturable", true);
            t.garrisonDefense = g.getFx("defense", Fixed::fromInt(1));
            t.garrisonDecay = g.getFx("decay", Fixed::fromInt(1));
        }
        t.entersGarrison = tj.getBool("entersGarrison", false);
        t.worker = tj.getBool("worker", !t.gathers.empty() || !t.builds.empty());
        t.countsForVictory = tj.getBool("countsForVictory", t.resourceNode < 0 && (!t.invulnerable || t.garrisonMax > 0));
        t.view = tj["view"];
        if (t.structure && t.footprintW <= 0) fail(where, "structure footprint must be positive");
    }
    if (r.types.empty()) fail("rules", "no types defined");
    return r;
}

} // namespace rts::game
