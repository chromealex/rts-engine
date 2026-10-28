// Parsing of the agent-mech blocks (see RtsMechData.h). Every key is optional; errors name the place.
#include "RtsData.h"
#include <algorithm>
#include <cstdlib>
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

static Fixed kOneFx() { return Fixed::fromInt(1); }

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
static void loadMechStage3(Rules& r, const Json& j);

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

// stage 3: cover, suppression, arcs, turrets, ruins, crush, craters, staff, growth, autonomy, power fields,
// infection, rounds (garrison-paid upgrades: RtsOriginalsData.cpp cost.garrison)
static void loadMechStage3(Rules& r, const Json& j) {
    MechRules& m = r.mech;
    if (const Json& cv = j["cover"]; cv.isObject()) {
        m.coverLevels.assign(10, Fixed::fromInt(1));
        for (auto& [lv, mul] : cv["levels"].members()) {
            int l = std::atoi(lv.c_str());
            if (l < 1 || l > 9) fail("cover.levels", "levels are 1..9");
            m.coverLevels[size_t(l)] = mul.fx();
        }
        m.coverTags = tagsOf(r, "cover.tags", cv["tags"]);
        m.any = true;
    }
    if (const Json& sp = j["suppression"]; sp.isObject()) {
        m.suppression = m.any = true;
        m.supDecay = sp.getFx("decay", m.supDecay);
        m.supSuppressed = sp.getFx("suppressed", m.supSuppressed);
        m.supPinned = sp.getFx("pinned", m.supPinned);
        m.supSpeed = sp.getFx("suppressedSpeed", m.supSpeed);
        m.supAttack = sp.getFx("suppressedAttack", m.supAttack);
        m.supTags = tagsOf(r, "suppression.tags", sp["tags"]);
    }
    if (const Json& rd = j["rounds"]; rd.isObject()) {
        m.rounds = m.any = true;
        m.prepareTime = rd.getFx("prepare", m.prepareTime);
        m.fightTime = rd.getFx("fight", m.fightTime);
        m.roundCount = int(rd.getInt("rounds", m.roundCount));
        m.roundReset = rd.getBool("reset", true);
        if (rd.has("lives")) { m.livesRes = int16_t(r.findResource(rd.getStr("lives"))); if (m.livesRes < 0) fail("rounds.lives", "unknown resource '" + rd.getStr("lives") + "'"); }
        m.damagePerUnit = rd.getFx("damagePerUnit", m.damagePerUnit);
        m.roundIncome = amountsOf(r, "rounds.income", rd["income"]);
    }
    size_t ti = 0;
    for (auto& [id, tj] : j["types"].members()) {
        UnitType& t = r.types[ti++];
        MechType& mt = t.mech;
        const std::string where = "type '" + id + "'";
        mt.givesCover = uint8_t(std::clamp<int64_t>(tj.getInt("givesCover", 0), 0, 9));
        if (mt.givesCover) m.any = true;
        if (const Json& ar = tj["armorArcs"]; ar.isObject()) { mt.arcs = true; mt.arcSide = ar.getFx("side", kOneFx()); mt.arcRear = ar.getFx("rear", kOneFx()); m.any = m.combat3 = true; }
        const Json& ws = tj["weapons"];
        const size_t nw = ws.size();
        auto perWeapon = [&](const char* key, std::vector<Fixed>& out) {
            for (size_t wi = 0; wi < nw; ++wi) if (ws[wi].has(key)) { out.resize(nw); out[wi] = ws[wi].getFx(key); m.any = true; }
        };
        perWeapon("suppress", mt.suppress);
        for (size_t wi = 0; wi < nw; ++wi) if (ws[wi].has("turret")) { mt.turretRate.resize(nw); mt.turretRate[wi] = ws[wi]["turret"].getFx("turnRate", Fixed::fromInt(180)); m.any = m.combat3 = true; }
        for (size_t wi = 0; wi < nw; ++wi) if (ws[wi].has("crater")) { mt.crater.resize(nw); mt.crater[wi] = ws[wi]["crater"].isObject() ? ws[wi]["crater"].getFx("radius", kOneFx()) : ws[wi].getFx("crater"); m.any = m.combat3 = true; }
        for (size_t wi = 0; wi < nw; ++wi) if (ws[wi].has("infect")) { mt.infect.resize(nw, 0xFFFF); mt.infect[wi] = typeRef(r, where + " weapon infect", ws[wi].getStr("infect")); m.any = m.infection = m.combat3 = true; }
        if (tj.has("ruins")) { mt.ruins = typeRef(r, where + " ruins", tj.getStr("ruins")); m.any = m.ruinsAny = true; }
        if (const Json& cr = tj["crush"]; cr.isObject()) { mt.crushDamage = cr.getFx("damage", Fixed::fromInt(1000)); mt.crushTags = tagsOf(r, where + " crush.tags", cr["tags"]); m.any = m.combat3 = true; }
        if (const Json& st = tj["staff"]; st.isObject()) {
            mt.staff = int(st.getInt("count", 1));
            if (mt.transport.slots == 0) { mt.transport.slots = mt.staff; mt.transport.killOnDeath = Fixed{}; mt.transport.killOnDeathSet = true; }
            mt.transport.acceptTags = tagsOf(r, where + " staff.accepts", st["accepts"]);
            m.any = m.staffing = m.transports = true;
        }
        if (const Json& gr = tj["grows"]; gr.isObject()) {
            mt.growUnit = typeRef(r, where + " grows.unit", gr.getStr("unit"));
            mt.growInterval = gr.getFx("interval", Fixed::fromInt(30));
            mt.growCost = amountsOf(r, where + " grows.cost", gr["cost"]);
            m.any = m.growth = true;
        }
        if (tj.getBool("autonomous", false)) { mt.autonomous = true; m.any = m.autonomy = true; }
        if (tj.has("powerField")) { mt.powerField = tj.getFx("powerField"); m.any = m.power = true; }
        if (tj.getBool("needsPower", false)) { mt.needsPower = true; m.any = m.power = true; }
        if (const Json& inf = tj["infectable"]; inf.isObject()) { mt.infectSpawn = typeRef(r, where + " infectable.spawns", inf.getStr("spawns")); mt.infectCount = int(inf.getInt("count", 1)); m.any = m.infection = true; }
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
    loadMechStage3(r, j);
    loadMechStage4(r, j);
    ti = 0;
    for (auto& [id, tj] : j["types"].members()) {
        UnitType& t = r.types[ti++];
        if (tj.has("xp")) t.mech.killXp = t.xpValue;
        else { for (auto& a : t.cost) t.mech.killXp += a.value; t.mech.killXp = t.mech.killXp * m.vetFromCost; }
    }
    for (auto& t : r.types)
        if (t.mech.transport.slots > 0 && !t.mech.transport.killOnDeathSet) t.mech.transport.killOnDeath = t.structure ? Fixed{} : Fixed::fromInt(1);
}

// ================================================================= UD: unit designs (chassis + parts)
namespace {
void deepMerge(Json& into, const Json& from) {
    if (!into.isObject() || !from.isObject()) { into = from; return; }
    for (auto& [k, v] : from.members()) {
        if (Json* cur = into.find(k); cur && cur->isObject() && v.isObject()) deepMerge(*cur, v);
        else into.set(k, v);
    }
}
// "add": {"health.max": 50, "move.speed": -0.5}: adds to a number at a dotted path (missing = 0)
void addAt(Json& t, const std::string& path, const Json& v, const std::string& where) {
    if (!v.isNumber()) fail(where, "add values are numbers");
    Json* cur = &t;
    size_t s = 0;
    for (;;) {
        size_t d = path.find('.', s);
        std::string k = path.substr(s, d == std::string::npos ? std::string::npos : d - s);
        if (k.empty()) fail(where, "bad path '" + path + "'");
        if (d == std::string::npos) {
            Json* n = cur->find(k);
            if (n && !n->isNumber() && !n->isNull()) fail(where, "'" + path + "' is not a number");
            const Fixed base = n ? n->fx() : Fixed{};
            cur->set(k, Json::number(base + v.fx()));
            return;
        }
        Json& next = cur->member(k);
        if (!next.isObject()) fail(where, "'" + path + "' crosses a non-object");
        cur = &next;
        s = d + 1;
    }
}
void applyPart(Json& t, const Json& part, const std::string& where) {
    for (auto& [k, v] : part.members()) {
        if (k == "add") { for (auto& [path, n] : v.members()) addAt(t, path, n, where + ".add"); continue; }
        if (k == "cost" || k == "upkeep") {   // summed per resource
            Json& c = t.member(k);
            for (auto& [res, n] : v.members()) addAt(c, res, n, where + "." + k);
            continue;
        }
        if (k == "weapons" || k == "tags" || k == "abilities" || k == "upgrades") {   // appended
            Json* arr = t.find(k);
            if (!arr || !arr->isArray()) { t.set(k, Json::array()); arr = t.find(k); }
            for (auto& x : v.items()) arr->push(x);
            continue;
        }
        if (k == "slot" || k == "name") continue;
        Json* cur = t.find(k);
        if (cur && cur->isObject() && v.isObject()) deepMerge(*cur, v);
        else t.set(k, v);
    }
}
} // namespace

Json expandDesigns(const Json& j) {
    const Json& ds = j["designs"];
    if (!ds.isObject()) fail("designs", "expected an object with chassis and parts");
    const Json& chassis = ds["chassis"];
    const Json& parts = ds["parts"];
    Json out = j;
    out.erase("designs");
    Json* types = out.find("types");
    if (!types || !types->isObject()) return out;
    for (auto& [id, tj] : types->membersMut()) {
        if (!tj.isObject() || !tj.has("design")) continue;
        const std::string where = "type '" + id + "' design";
        const Json& d = tj["design"];
        const std::string cid = d.getStr("chassis");
        if (!chassis.has(cid)) fail(where + ".chassis", "unknown chassis '" + cid + "'");
        Json t = chassis[cid];
        // slots: {"weapon": 2, "system": 1} or a plain count; parts may name their "slot" kind
        const Json slots = t["slots"];
        t.erase("slots");
        std::vector<std::pair<std::string, int64_t>> used;
        int64_t total = 0;
        for (auto& pn : d["parts"].items()) {
            const std::string pid = pn.str();
            if (!parts.has(pid)) fail(where + ".parts", "unknown part '" + pid + "'");
            const Json& part = parts[pid];
            ++total;
            if (slots.isObject()) {
                const std::string kind = part.getStr("slot", "any");
                int64_t n = 1;
                for (auto& u : used) if (u.first == kind) n = ++u.second;
                if (n == 1) used.push_back({kind, 1});
                if (n > slots.getInt(kind, 0)) fail(where + ".parts", "chassis '" + cid + "' has " + std::to_string(slots.getInt(kind, 0)) + " '" + kind + "' slot(s)");
            } else if (slots.isNumber() && total > slots.integer()) {
                fail(where + ".parts", "chassis '" + cid + "' has " + std::to_string(slots.integer()) + " slot(s)");
            }
            applyPart(t, part, where + " part '" + pid + "'");
        }
        for (auto& [k, v] : tj.members()) if (k != "design") { Json* cur = t.find(k); if (cur && cur->isObject() && v.isObject()) deepMerge(*cur, v); else t.set(k, v); }
        tj = std::move(t);
    }
    return out;
}

} // namespace rts::game
