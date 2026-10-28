// Parsing of the agent-mech blocks (see RtsMechData.h). Every key is optional; errors name the place.
#include "RtsData.h"
#include <algorithm>
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

static std::vector<Amount> amountsOf(const Rules& r, const std::string& where, const Json& j) {
    std::vector<Amount> out;
    for (auto& [res, v] : j.members()) {
        int ri = r.findResource(res);
        if (ri < 0) fail(where, "unknown resource '" + res + "'");
        out.push_back({uint16_t(ri), v.fx()});
    }
    return out;
}

static int16_t statusRef(const Rules& r, const std::string& where, const Json& j) {
    if (j.isNull()) return -1;
    int s = r.findStatus(j.str());
    if (s < 0) fail(where, "unknown status '" + j.str() + "' (rules statuses)");
    return int16_t(s);
}

static uint16_t typeRef(const Rules& r, const std::string& where, const std::string& id) {
    int t = r.findType(id);
    if (t < 0) fail(where, "unknown type '" + id + "'");
    return uint16_t(t);
}

// stage 2: walls, gates, bridges, tunnels, amphibious, ammo + docks, merge / split, market, trade, tribute,
// diplomacy, items, cycles, hazards, card deck
static void loadMechStage2(Rules& r, const Json& j) {
    MechRules& m = r.mech;
    // items first (types reference them)
    for (auto& [id, ij] : j["items"].members()) {
        ItemDef d;
        d.id = id;
        d.name = ij.getStr("name", id);
        d.cost = amountsOf(r, "item '" + id + "' cost", ij["cost"]);
        for (auto& pl : ij["mods"].items()) {
            bool ok = false;
            StatMod sm;
            sm.stat = parseStat(pl.getStr("stat"), &ok);
            if (!ok) fail("item '" + id + "' mods", "unknown stat '" + pl.getStr("stat") + "'");
            sm.add = pl.getFx("add");
            sm.mul = pl.getFx("mul", sm.mul);
            d.mods.push_back(sm);
        }
        const Json& use = ij["use"];
        d.heal = use.getFx("heal");
        d.energy = use.getFx("energy");
        d.status = statusRef(r, "item '" + id + "' use.status", use["status"]);
        d.statusDuration = use.getFx("duration", d.statusDuration);
        d.charges = int(ij.getInt("charges", use.isObject() ? 1 : 0));
        d.consumable = ij.getBool("consumable", true);
        m.items.push_back(d);
        m.any = m.hasItems = true;
    }
    if (const Json& mk = j["market"]; mk.isObject()) {
        m.currency = int16_t(r.findResource(mk.getStr("currency", "gold")));
        if (m.currency < 0) fail("market.currency", "unknown resource '" + mk.getStr("currency", "gold") + "'");
        m.basePrice.assign(r.resources.size(), Fixed{});
        for (auto& [res, v] : mk["prices"].members()) {
            int ri = r.findResource(res);
            if (ri < 0) fail("market.prices", "unknown resource '" + res + "'");
            m.basePrice[size_t(ri)] = v.fx();
        }
        m.marketFee = mk.getFx("fee", m.marketFee);
        m.priceStep = mk.getFx("step", m.priceStep);
        m.lot = mk.getFx("lot", m.lot);
        m.any = m.market = true;
    }
    if (const Json& tb = j["tribute"]; tb.isObject() || tb.boolean()) { m.tribute = m.any = true; m.tributeFee = tb.getFx("fee", m.tributeFee); }
    if (const Json& dp = j["diplomacy"]; dp.isObject() || dp.boolean()) m.diplomacy = m.any = true;
    for (auto& cj : j["cycles"].items()) {
        CycleDef c;
        c.id = cj.getStr("id", "cycle" + std::to_string(m.cycles.size()));
        c.length = cj.getFx("length", c.length);
        c.offset = cj.getFx("offset");
        if (c.length.raw <= 0) fail("cycles." + c.id + ".length", "must be > 0");
        Fixed prev{};
        for (auto& pj : cj["phases"].items()) {
            CyclePhase p;
            p.id = pj.getStr("id");
            p.until = pj.getFx("until", Fixed::fromInt(1));
            if (p.until <= prev || p.until > Fixed::fromInt(1)) fail("cycles." + c.id + " phase '" + p.id + "'", "until must grow up to 1");
            prev = p.until;
            p.status = statusRef(r, "cycles." + c.id + " phase '" + p.id + "' status", pj["status"]);
            c.phases.push_back(p);
        }
        if (c.phases.empty()) fail("cycles." + c.id, "needs phases");
        m.cycles.push_back(c);
        m.any = true;
    }
    for (auto& hj : j["hazards"].items()) {
        HazardDef h;
        if (hj.has("field")) { h.field = int16_t(r.findResource(hj.getStr("field"))); if (h.field < 0) fail("hazards.field", "unknown resource '" + hj.getStr("field") + "'"); }
        h.area = hj.getStr("area");
        if (h.field < 0 && h.area.empty()) fail("hazards", "needs a field or an area");
        h.status = statusRef(r, "hazards.status", hj["status"]);
        h.damage = hj.getFx("damage");
        h.tags = tagsOf(r, "hazards.tags", hj["tags"]);
        h.layers = layersOf("hazards.layers", hj["layers"], kLayerGround);
        m.hazards.push_back(h);
        m.any = true;
    }
    if (const Json& dk = j["deck"]; dk.isObject()) {
        m.deck = m.any = true;
        m.hand = int(dk.getInt("hand", 4));
        m.deployNear = dk.getFx("deployNear");
        m.deployOwnHalf = dk.getBool("ownHalf", false);
    }

    size_t ti = 0;
    for (auto& [id, tj] : j["types"].members()) {
        UnitType& t = r.types[ti++];
        MechType& mt = t.mech;
        const std::string where = "type '" + id + "'";
        if (tj.getBool("wall", false)) { mt.wall = true; m.any = m.walls = true; }
        if (const Json& g = tj["gate"]; !g.isNull()) {
            std::string v = g.isString() ? g.str() : (g.boolean() ? "own" : g.getStr("opensFor", "own"));
            if (v != "own" && v != "allies") fail(where + " gate", "expected own or allies");
            mt.gate = v == "own" ? 1 : 2;
            m.any = m.gates = true;
        }
        if (tj.getBool("bridge", false)) { mt.bridge = true; m.any = m.bridges = true; }
        if (const Json& tn = tj["tunnel"]; !tn.isNull()) {
            std::string net = tn.isString() ? tn.str() : tn.getStr("network", "tunnel");
            auto it = std::find(m.tunnelNets.begin(), m.tunnelNets.end(), net);
            if (it == m.tunnelNets.end()) { m.tunnelNets.push_back(net); it = m.tunnelNets.end() - 1; }
            mt.tunnel = int16_t(it - m.tunnelNets.begin());
            if (mt.transport.slots == 0) fail(where + " tunnel", "needs a transport block (slots)");
            m.any = m.tunnels = true;
        }
        if (tj["move"].getStr("layer") == "amphibious") { t.layer = uint8_t(kLayerGround | kLayerNaval); t.pathLayer = t.layer; }
        const Json& ws = tj["weapons"];
        for (size_t wi = 0; wi < ws.size(); ++wi)
            if (ws[wi].has("ammo")) { mt.ammo.resize(ws.size(), 0); mt.ammo[wi] = int(ws[wi].getInt("ammo", 0)); m.any = m.ammo = true; }
        if (const Json& dk = tj["dock"]; dk.isObject()) { mt.dockSlots = int(dk.getInt("slots", 1)); mt.rearmTime = dk.getFx("rearmTime", mt.rearmTime); m.any = true; }
        if (const Json& mg = tj["mergeInto"]; mg.isObject()) {
            mt.mergeWith = typeRef(r, where + " mergeInto.with", mg.getStr("with", id));
            mt.mergeResult = typeRef(r, where + " mergeInto.result", mg.getStr("result"));
            m.any = m.merge = true;
        }
        if (const Json& sp = tj["splitInto"]; sp.isObject()) {
            mt.splitType = typeRef(r, where + " splitInto.type", sp.getStr("type"));
            mt.splitCount = int(sp.getInt("count", 2));
            m.any = m.merge = true;
        }
        mt.market = tj.getBool("market", false);
        if (const Json& tr = tj["trader"]; !tr.isNull()) { mt.tradeRate = tr.isObject() ? tr.getFx("rate", Fixed::fromRatio(1, 2)) : tr.fx(); m.any = m.traders = true; }
        if (const Json& inv = tj["inventory"]; !inv.isNull()) { mt.inventorySlots = int(inv.isObject() ? inv.getInt("slots", 6) : inv.integer(6)); m.any = true; }
        if (const Json& sh = tj["shop"]; sh.isObject()) {
            for (auto& it : sh["sells"].items()) {
                int ii = m.findItem(it.str());
                if (ii < 0) fail(where + " shop.sells", "unknown item '" + it.str() + "'");
                mt.shopItems.push_back(uint16_t(ii));
            }
            mt.shopRadius = sh.getFx("radius", mt.shopRadius);
        }
        for (auto& dj : tj["drops"].items()) {
            int ii = m.findItem(dj.getStr("item"));
            if (ii < 0) fail(where + " drops", "unknown item '" + dj.getStr("item") + "'");
            mt.drops.push_back({uint16_t(ii), dj.getFx("chance", Fixed::fromInt(1))});
        }
    }
}

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
    loadMechStage2(r, j);
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
