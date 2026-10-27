#include "RtsData.h"
#include <algorithm>
#include <stdexcept>

namespace rts::game {

namespace {
[[noreturn]] void fail(const std::string& where, const std::string& what) { throw std::runtime_error(where + ": " + what); }

int indexOf(const std::vector<std::string>& v, const std::string& s) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == s) return int(i);
    return -1;
}

uint8_t parseLayers(const Json& j, uint8_t def) {
    if (j.isString()) {
        std::string s = j.str();
        if (s == "air") return kLayerAir;
        if (s == "naval") return kLayerNaval;
        if (s == "sand") return kLayerSandOnly;
        if (s == "all") return uint8_t(kLayerGround | kLayerAir | kLayerNaval);
        return kLayerGround;
    }
    if (!j.isArray()) return def;
    uint8_t m = 0;
    for (auto& v : j.items()) m |= parseLayers(v, 0);
    return m ? m : def;
}
} // namespace

Stat parseStat(const std::string& s, bool* ok) {
    static const struct { const char* n; Stat s; } t[] = {
        {"maxHp", Stat::MaxHp}, {"hp", Stat::MaxHp}, {"armor", Stat::Armor}, {"damage", Stat::Damage}, {"range", Stat::Range},
        {"speed", Stat::Speed}, {"sight", Stat::Sight}, {"gatherRate", Stat::GatherRate}, {"attackSpeed", Stat::AttackSpeed},
    };
    for (auto& e : t) if (s == e.n) { if (ok) *ok = true; return e.s; }
    if (ok) *ok = false;
    return Stat::Damage;
}

int Rules::findType(const std::string& id) const {
    for (size_t i = 0; i < types.size(); ++i) if (types[i].id == id) return int(i);
    return -1;
}
int Rules::findResource(const std::string& id) const {
    for (size_t i = 0; i < resources.size(); ++i) if (resources[i].id == id) return int(i);
    return -1;
}
int Rules::findTech(const std::string& id) const {
    for (size_t i = 0; i < techs.size(); ++i) if (techs[i].id == id) return int(i);
    return -1;
}
int Rules::findFaction(const std::string& id) const {
    for (size_t i = 0; i < factions.size(); ++i) if (factions[i].id == id) return int(i);
    return -1;
}
uint64_t Rules::tagMask(const std::string& name) const {
    int i = indexOf(tagNames, name);
    return i < 0 ? 0 : (uint64_t(1) << i);
}
Fixed Rules::damageMult(uint16_t d, uint16_t a) const {
    if (d < damageTable.size() && a < damageTable[d].size()) return damageTable[d][a];
    return Fixed::fromInt(1);
}
bool Rules::matches(const ModTarget& t, uint16_t type) const {
    if (t.all) return true;
    if (t.type != kNoType) return t.type == type;
    return (types[type].tags & t.tags) != 0;
}

VictoryRules loadVictory(const Json& j, const VictoryRules& def) {
    VictoryRules v = def;
    std::string mode = j.isString() ? j.str() : j.getStr("mode", "");
    if (mode == "noStructures") v.mode = VictoryMode::NoStructures;
    else if (mode == "noUnits") v.mode = VictoryMode::NoUnits;
    else if (mode == "noEntities") v.mode = VictoryMode::NoEntities;
    else if (mode == "keyUnits" || mode == "assassination") v.mode = VictoryMode::KeyUnits;
    else if (mode == "tickets") v.mode = VictoryMode::Tickets;
    else if (mode == "survive") v.mode = VictoryMode::Survive;
    else if (mode == "score") v.mode = VictoryMode::Score;
    else if (!mode.empty()) throw std::runtime_error("victory: unknown mode '" + mode + "'");
    if (j.isObject()) {
        v.tickets = j.getFx("tickets", v.tickets);
        v.ticketDrain = j.getFx("drain", v.ticketDrain);
        v.surviveSeconds = j.getFx("seconds", v.surviveSeconds);
        v.keyLoses = j.getBool("keyLoses", v.keyLoses);
    }
    return v;
}

Rules loadRules(const Json& j) {
    Rules r;
    r.supplyCap = j.getFx("supplyCap", r.supplyCap);
    r.maxQueue = int(j.getInt("maxQueue", r.maxQueue));
    r.costModel = j.getStr("costModel", "upfront") == "progressive" ? CostModel::Progressive : CostModel::Upfront;
    r.sellRefund = j.getFx("sellRefund", r.sellRefund);
    r.buildRadius = int(j.getInt("buildRadius", 0));
    r.levelHeight = j.getFx("levelHeight", r.levelHeight);
    r.waveClock = j.getFx("waveClock");
    r.victory = loadVictory(j["victory"], r.victory);
    if (const Json& h = j["heights"]; h.isObject()) {
        r.heights.blockVision = h.getBool("blockVision", true);
        r.heights.missUphill = h.getFx("missUphill");
        r.heights.rangePerLevel = h.getFx("rangePerLevel");
        r.heights.damagePerLevel = h.getFx("damagePerLevel");
    }

    // ---- resources
    for (auto& [id, rj] : j["resources"].members()) {
        ResourceDef d;
        d.id = id;
        d.name = rj.getStr("name", id);
        std::string kind = rj.getStr("kind", "stock");
        d.kind = kind == "flow" ? ResourceKind::Flow : kind == "capacity" ? ResourceKind::Capacity : ResourceKind::Stock;
        d.start = rj.getFx("start");
        d.storage = rj.getFx("storage");
        d.capped = rj.getBool("capped", false);
        d.field = rj.getBool("field", false);
        d.fieldRegrow = rj.getFx("regrow");
        d.fieldCellMax = rj.getFx("cellMax", d.fieldCellMax);
        r.resources.push_back(d);
    }
    if (r.resources.size() > 32) fail("resources", "at most 32 resources are supported");
    auto resRef = [&](const std::string& where, const std::string& id) -> uint16_t {
        int ri = r.findResource(id);
        if (ri < 0) fail(where, "unknown resource '" + id + "'");
        return uint16_t(ri);
    };
    auto amounts = [&](const std::string& where, const Json& obj, std::vector<Amount>& out, std::initializer_list<const char*> skip) {
        for (auto& [k, v] : obj.members()) {
            bool skipped = false;
            for (auto* s : skip) if (k == s) skipped = true;
            if (!skipped) out.push_back({resRef(where, k), v.fx()});
        }
    };

    // ---- tags (collected from types and tech targets so they can be used anywhere)
    auto addTag = [&](const std::string& t) {
        if (indexOf(r.tagNames, t) < 0) {
            if (r.tagNames.size() >= 64) fail("tags", "at most 64 distinct tags");
            r.tagNames.push_back(t);
        }
    };
    for (auto& [id, tj] : j["types"].members()) for (auto& t : tj["tags"].items()) addTag(t.str());
    for (auto& [id, tj] : j["techs"].members()) for (auto& e : tj["effects"].items()) for (auto& t : e["tags"].items()) addTag(t.str());
    for (auto& [id, rj] : j["resources"].members()) for (auto& t : rj["deficitDisables"].items()) addTag(t.str());
    for (size_t i = 0; i < r.resources.size(); ++i) {
        const Json& rj = j["resources"][r.resources[i].id];
        r.resources[i].deficitProductionMult = rj.getFx("deficitProduction", r.resources[i].deficitProductionMult);
        for (auto& t : rj["deficitDisables"].items()) r.resources[i].deficitDisableTags |= r.tagMask(t.str());
    }

    // ---- armor / damage tables
    r.armorTypes = {"none"};
    r.damageTypes = {"normal"};
    for (auto& a : j["armorTypes"].items()) if (indexOf(r.armorTypes, a.str()) < 0) r.armorTypes.push_back(a.str());
    for (auto& [dt, row] : j["damageTable"].members()) {
        if (indexOf(r.damageTypes, dt) < 0) r.damageTypes.push_back(dt);
        for (auto& [at, v] : row.members()) if (indexOf(r.armorTypes, at) < 0) r.armorTypes.push_back(at);
    }
    auto armorIdx = [&](const std::string& at) {
        int ai = indexOf(r.armorTypes, at);
        if (ai < 0) { r.armorTypes.push_back(at); ai = int(r.armorTypes.size() - 1); }
        return uint16_t(ai);
    };
    auto damageIdx = [&](const std::string& dt) {
        int di = indexOf(r.damageTypes, dt);
        if (di < 0) { r.damageTypes.push_back(dt); di = int(r.damageTypes.size() - 1); }
        return uint16_t(di);
    };
    // pre-register armor/damage types used by types so the table has the final size
    for (auto& [id, tj] : j["types"].members()) {
        armorIdx(tj["health"].getStr("armorType", "none"));
        for (auto& w : tj["weapons"].items()) damageIdx(w.getStr("damageType", "normal"));
        for (auto& a : tj["abilities"].items()) for (auto& e : a["effects"].items()) damageIdx(e.getStr("damageType", "normal"));
    }
    r.damageTable.assign(r.damageTypes.size(), std::vector<Fixed>(r.armorTypes.size(), Fixed::fromInt(1)));
    for (auto& [dt, row] : j["damageTable"].members())
        for (auto& [at, v] : row.members()) r.damageTable[size_t(indexOf(r.damageTypes, dt))][size_t(indexOf(r.armorTypes, at))] = v.fx();

    // ---- names first (types and techs may reference each other in any order)
    for (auto& [id, tj] : j["types"].members()) { UnitType t; t.id = id; r.types.push_back(t); }
    for (auto& [id, tj] : j["techs"].members()) { TechDef t; t.id = id; r.techs.push_back(t); }
    if (r.types.empty()) fail("rules", "no types defined");
    auto typeRef = [&](const std::string& where, const std::string& id) -> uint16_t {
        int k = r.findType(id);
        if (k < 0) fail(where, "unknown type '" + id + "'");
        return uint16_t(k);
    };
    auto requirement = [&](const std::string& where, const std::string& id) -> Requirement {
        int k = r.findType(id);
        if (k >= 0) return {false, uint16_t(k)};
        k = r.findTech(id);
        if (k >= 0) return {true, uint16_t(k)};
        fail(where, "unknown type or tech '" + id + "'");
    };
    auto statMod = [&](const std::string& where, const Json& e) {
        StatMod m;
        bool ok = false;
        m.stat = parseStat(e.getStr("stat", "damage"), &ok);
        if (!ok) fail(where, "unknown stat '" + e.getStr("stat") + "'");
        m.add = e.getFx("add");
        m.mul = e.getFx("mul", Fixed::fromInt(1));
        return m;
    };
    auto tagsOf = [&](const Json& arr) {
        uint64_t m = 0;
        for (auto& t : arr.items()) m |= r.tagMask(t.str());
        return m;
    };

    // ---- types
    size_t ti = 0;
    for (auto& [id, tj] : j["types"].members()) {
        UnitType& t = r.types[ti++];
        const std::string where = "type '" + id + "'";
        t.name = tj.getStr("name", id);
        t.tags = tagsOf(tj["tags"]);
        if (const Json& h = tj["health"]; h.isObject()) {
            t.maxHp = h.getFx("max", t.maxHp);
            t.armor = h.getFx("armor");
            t.regen = h.getFx("regen");
            t.armorType = armorIdx(h.getStr("armorType", "none"));
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
            if (s.has("on")) t.placeOn = typeRef(where + " structure.on", s.getStr("on"));
            t.takesOverNode = s.getBool("takesOverNode", false);
            std::string tr = s.getStr("terrain", "any");
            t.terrainReq = tr == "rock" ? TerrainReq::Rock : tr == "creep" ? TerrainReq::Creep : TerrainReq::Any;
            t.extendsBase = s.getBool("extendsBase", true);
            if (t.footprintW <= 0 || t.footprintH <= 0) fail(where, "structure footprint must be positive");
        }
        t.sight = tj.getFx("sight", t.sight);
        for (auto& wj : tj["weapons"].items()) {
            WeaponDef w;
            w.id = wj.getStr("id", "weapon");
            w.range = wj.getFx("range", w.range);
            w.minRange = wj.getFx("minRange");
            w.damage = wj.getFx("damage", w.damage);
            w.cooldown = wj.getFx("cooldown", w.cooldown);
            w.targets = parseLayers(wj["targets"], kLayerGround | kLayerNaval | kLayerSandOnly);
            if (w.targets & kLayerGround) w.targets |= kLayerSandOnly | kLayerNaval;
            w.projectileSpeed = wj.getFx("projectileSpeed");
            w.splash = wj.getFx("splash");
            w.damageType = damageIdx(wj.getStr("damageType", "normal"));
            t.weapons.push_back(w);
        }
        if (const Json& e = tj["energy"]; e.isObject()) {
            t.maxEnergy = e.getFx("max", Fixed::fromInt(100));
            t.energyRegen = e.getFx("regen", Fixed::fromRatio(1, 2));
            t.startEnergy = e.getFx("start", t.maxEnergy / 2);
        }
        for (auto& aj : tj["abilities"].items()) {
            AbilityDef a;
            a.id = aj.getStr("id", "ability");
            a.name = aj.getStr("name", a.id);
            std::string tg = aj.getStr("target", "none");
            a.target = tg == "unit" ? TargetKind::Unit : tg == "point" ? TargetKind::Point : tg == "self" ? TargetKind::Self : TargetKind::None;
            a.range = aj.getFx("range", a.range);
            a.cooldown = aj.getFx("cooldown", a.cooldown);
            a.energyCost = aj.getFx("energy");
            a.requiredLevel = int(aj.getInt("level", 1));
            a.autocast = aj.getBool("autocast", true);
            a.aura = aj.getBool("aura", false);
            a.auraRadius = aj.getFx("radius", Fixed::fromInt(6));
            for (auto& ej : aj["effects"].items()) {
                EffectDef e;
                std::string k = ej.getStr("type", "damage");
                e.kind = k == "heal" ? EffectKind::Heal : k == "stun" ? EffectKind::Stun : k == "buff" ? EffectKind::Buff
                       : k == "spawn" ? EffectKind::Spawn : k == "blink" ? EffectKind::Blink : EffectKind::Damage;
                e.amount = ej.getFx("amount");
                e.radius = ej.getFx("radius");
                e.duration = ej.getFx("duration");
                e.damageType = damageIdx(ej.getStr("damageType", "normal"));
                if (e.kind == EffectKind::Buff) e.mod = statMod(where + " ability " + a.id, ej);
                if (e.kind == EffectKind::Spawn) e.spawnType = typeRef(where + " ability " + a.id, ej.getStr("unit"));
                e.count = int(ej.getInt("count", 1));
                std::string aff = ej.getStr("affects", e.kind == EffectKind::Heal || (e.kind == EffectKind::Buff && e.mod.mul >= Fixed::fromInt(1) && e.mod.add.raw >= 0) ? "allies" : "enemies");
                e.enemies = aff == "enemies" || aff == "all";
                e.allies = aff == "allies" || aff == "all";
                a.effects.push_back(e);
            }
            t.abilities.push_back(a);
        }
        if (const Json& h = tj["hero"]; h.isObject()) {
            t.hero = true;
            t.unique = true;
            for (auto& x : h["levels"].items()) t.xpLevels.push_back(int(x.integer()));
            for (auto& m : h["perLevel"].items()) t.perLevel.push_back(statMod(where + " hero.perLevel", m));
        }
        t.unique = tj.getBool("unique", t.unique);
        Fixed costSum{};
        if (const Json& c = tj["cost"]; c.isObject()) {
            amounts(where + " cost", c, t.cost, {"time", "supply"});
            t.buildTime = c.getFx("time", t.buildTime);
            t.supplyUsed = c.getFx("supply");
            for (auto& a : t.cost) costSum += a.value;
        }
        t.xpValue = tj.getFx("xp", fxMax(Fixed::fromInt(10), t.maxHp / 10));
        amounts(where + " bounty", tj["bounty"], t.bounty, {});
        if (const Json& s = tj["squad"]; s.isObject()) {
            t.members = std::max<int>(1, int(s.getInt("members", 4)));
            amounts(where + " squad.reinforceCost", s["reinforceCost"], t.reinforceCost, {});
            t.reinforceTime = s.getFx("reinforceTime", t.reinforceTime);
        }
        t.supplyProvided = tj.getFx("supply");
        amounts(where + " income", tj["income"], t.income, {});
        amounts(where + " storage", tj["storage"], t.storage, {});
        amounts(where + " provides", tj["provides"], t.provides, {});
        amounts(where + " consumes", tj["consumes"], t.consumes, {});
        for (auto& p : tj["produces"].items()) t.produces.push_back(typeRef(where + " produces", p.str()));
        for (auto& b : tj["builds"].items()) t.builds.push_back(typeRef(where + " builds", b.str()));
        for (auto& rq : tj["requires"].items()) t.requires_.push_back(requirement(where + " requires", rq.str()));
        for (auto& rs : tj["researches"].items()) {
            int k = r.findTech(rs.str());
            if (k < 0) fail(where + " researches", "unknown tech '" + rs.str() + "'");
            t.researches.push_back(uint16_t(k));
        }
        std::string bm = tj.getStr("buildMode", "walk");
        t.buildMode = bm == "warp" ? BuildMode::Warp : bm == "morph" ? BuildMode::Morph : BuildMode::Walk;
        t.buildPower = tj.getFx("buildPower", t.buildPower);
        for (auto& [res, g] : tj["gather"].members())
            t.gathers.push_back({resRef(where + " gather", res), g.getFx("carry", Fixed::fromInt(10)), g.getFx("time", Fixed::fromInt(2))});
        if (const Json& rn = tj["resourceNode"]; rn.isObject()) {
            t.resourceNode = int16_t(resRef(where + " resourceNode", rn.getStr("resource")));
            t.resourceAmount = rn.getFx("amount", Fixed::fromInt(1000));
            t.ownerOnly = rn.getBool("ownerOnly", false);
        }
        for (auto& d : tj["dropoff"].items()) t.dropoffMask |= 1u << resRef(where + " dropoff", d.str());
        if (const Json& c = tj["charges"]; c.isObject()) {
            t.chargesMax = int(c.getInt("max", 3));
            t.chargeInterval = c.getFx("interval", Fixed::fromInt(10));
        }
        t.creepRadius = int(tj.getInt("creep", 0));
        if (const Json& fs = tj["fieldSpawner"]; fs.isObject()) {
            t.fieldSpawnRes = int16_t(resRef(where + " fieldSpawner", fs.getStr("resource")));
            t.fieldSpawnRate = fs.getFx("rate", Fixed::fromInt(10));
            t.fieldSpawnRadius = int(fs.getInt("radius", 3));
        }
        if (tj.has("deploysTo")) t.deploysTo = typeRef(where + " deploysTo", tj.getStr("deploysTo"));
        amounts(where + " wreck", tj["wreck"], t.wreck, {});
        t.sellable = tj.getBool("sellable", t.structure);
        if (const Json& g = tj["garrison"]; g.isObject()) {
            t.garrisonMax = int(g.getInt("max", 20));
            if (g.has("unit")) t.spawnUnit = typeRef(where + " garrison.unit", g.getStr("unit"));
            t.spawnInterval = g.getFx("interval");
            t.capturable = g.getBool("capturable", true);
            t.garrisonDefense = g.getFx("defense", Fixed::fromInt(1));
            t.garrisonDecay = g.getFx("decay", Fixed::fromInt(1));
        }
        t.entersGarrison = tj.getBool("entersGarrison", false);
        if (const Json& c = tj["capture"]; c.isObject()) {
            t.captureTime = c.getFx("time", Fixed::fromInt(10));
            t.captureRadius = c.getFx("radius", t.captureRadius);
            t.victoryPoint = c.getBool("victoryPoint", false);
            t.capturesRegion = c.getBool("region", false);
        }
        if (const Json& n = tj["neutral"]; n.isObject()) {
            t.leash = n.getFx("leash", t.leash);
            t.wanders = n.getBool("wander", false);
            t.wanderRadius = n.getFx("wanderRadius", t.wanderRadius);
        }
        t.worker = tj.getBool("worker", !t.gathers.empty() || !t.builds.empty());
        t.key = tj.getBool("key", false);
        bool defaultCounts = t.resourceNode < 0 && t.captureTime.raw == 0 && t.placeOn == kNoType && (!t.invulnerable || t.garrisonMax > 0);
        t.countsForVictory = tj.getBool("countsForVictory", defaultCounts);
        if (const Json& ws = tj["waveSpawn"]; ws.isObject()) {
            t.waveUnit = typeRef(where + " waveSpawn.unit", ws.getStr("unit"));
            t.waveCount = std::max<int>(1, int(ws.getInt("count", 1)));
        }
        for (auto& u : tj["upgrades"].items()) t.upgrades.push_back(typeRef(where + " upgrades", u.str()));
        t.score = tj.getFx("score", t.structure ? Fixed::fromInt(5) : Fixed::fromInt(1));
        t.view = tj["view"];
        if (const Json& sp = tj["sprite"]; !sp.isNull()) { // 2D games: presentation only, kept with the view block
            if (!sp.isObject() || sp.getStr("sheet").empty()) throw std::runtime_error(where + ": sprite needs \"sheet\" (image path)");
            t.view.set("sprite", sp);
        }
    }
    if (const Json& ab = j["autoBuild"]; ab.isObject()) {
        for (auto& p : ab["pool"].items()) r.autoBuildPool.push_back(typeRef("autoBuild.pool", p.str()));
        amounts("autoBuild.cost", ab["cost"], r.autoBuildCost, {});
        r.autoBuildGrowth = ab.getFx("growth");
    }

    // ---- techs
    size_t ki = 0;
    for (auto& [id, kj] : j["techs"].members()) {
        TechDef& t = r.techs[ki++];
        const std::string where = "tech '" + id + "'";
        t.name = kj.getStr("name", id);
        if (const Json& c = kj["cost"]; c.isObject()) {
            amounts(where + " cost", c, t.cost, {"time"});
            t.time = c.getFx("time", t.time);
        }
        for (auto& rq : kj["requires"].items()) t.requires_.push_back(requirement(where + " requires", rq.str()));
        for (auto& e : kj["effects"].items()) {
            ModTarget mt;
            if (e.has("type")) mt.type = typeRef(where + " effect", e.getStr("type"));
            else if (e.has("tags")) mt.tags = tagsOf(e["tags"]);
            else mt.all = true;
            t.effects.push_back({mt, statMod(where + " effect", e)});
        }
    }

    // ---- factions
    for (auto& [id, fj] : j["factions"].members()) {
        FactionDef f;
        f.id = id;
        f.name = fj.getStr("name", id);
        for (auto& s : fj["start"].items()) {
            FactionStart st;
            st.type = typeRef("faction '" + id + "' start", s.getStr("type"));
            st.dx = s["offset"][0].fx();
            st.dz = s["offset"][1].fx();
            st.count = int(s.getInt("count", 1));
            f.start.push_back(st);
        }
        r.factions.push_back(f);
    }

    for (auto& u : j["upkeep"].items()) r.upkeep.push_back({u.getFx("supply"), u.getFx("gatherMult", Fixed::fromInt(1))});
    if (const Json& rg = j["regions"]; rg.isObject()) {
        r.regions.enabled = true;
        amounts("regions.claimCost", rg["claimCost"], r.regions.claimCost, {});
        r.regions.claimCostGrowth = rg.getFx("claimCostGrowth");
        r.regions.buildOnlyOwned = rg.getBool("buildOnlyOwned", true);
        r.regions.productionPerRegion = rg.getFx("productionPerRegion");
    }
    return r;
}

} // namespace rts::game
