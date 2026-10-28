#include "RtsData.h"
#include "RtsEffectsData.h"
#include <algorithm>
#include <stdexcept>

namespace rts::game {

namespace {
const Fixed kFxOne = Fixed::fromInt(1);
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
        if (s == "orbit") return uint8_t(kLayerAir | kLayerOrbit);
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
        {"productionSpeed", Stat::ProductionSpeed}, {"workRate", Stat::GatherRate}, {"energyRegen", Stat::EnergyRegen},
        {"defense", Stat::Defense}, {"garrisonCap", Stat::GarrisonCap}, {"cooldownRate", Stat::CooldownRate}, {"upgradeCost", Stat::UpgradeCost},
        {"lifesteal", Stat::Lifesteal}, {"damageTaken", Stat::DamageTaken}, {"scale", Stat::Scale},
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
int Rules::findRoom(const std::string& id) const {
    for (size_t i = 0; i < rooms.size(); ++i) if (rooms[i].id == id) return int(i);
    return -1;
}
int Rules::findDiggable(char c) const {
    for (size_t i = 0; i < diggables.size(); ++i) if (diggables[i].ch == c) return int(i);
    return -1;
}
int Rules::findStatus(const std::string& id) const {
    for (size_t i = 0; i < statuses.size(); ++i) if (statuses[i].id == id) return int(i);
    return -1;
}
int Rules::findPlayerAbility(const std::string& id) const {
    for (size_t i = 0; i < playerAbilities.size(); ++i) if (playerAbilities[i].id == id) return int(i);
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
    else if (mode == "territory") v.mode = VictoryMode::Territory;
    else if (!mode.empty()) throw std::runtime_error("victory: unknown mode '" + mode + "'");
    if (j.isObject()) {
        v.tickets = j.getFx("tickets", v.tickets);
        v.ticketDrain = j.getFx("drain", v.ticketDrain);
        v.surviveSeconds = j.getFx("seconds", v.surviveSeconds);
        v.keyLoses = j.getBool("keyLoses", v.keyLoses);
        v.share = j.getFx("share", v.share);
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
    if (const Json& f = j["fogOfWar"]; f.isObject()) {
        std::string m = f.getStr("mode", "full");
        if (m == "full") r.fog.mode = FogMode::Full;
        else if (m == "explored") r.fog.mode = FogMode::Explored;
        else if (m == "none" || m == "off") r.fog.mode = FogMode::None;
        else throw std::runtime_error("fogOfWar.mode: unknown '" + m + "' (full, explored, none)");
        r.fog.shareAllies = f.getBool("shareAllies", true);
        r.fog.lineOfSight = f.getBool("lineOfSight", false);
        r.fog.rockHeight = f.getFx("rockHeight", r.fog.rockHeight);
    }
    if (const Json& h = j["heights"]; h.isObject()) {
        r.heights.blockVision = h.getBool("blockVision", true);
        r.heights.missUphill = h.getFx("missUphill");
        r.heights.rangePerLevel = h.getFx("rangePerLevel");
        r.heights.damagePerLevel = h.getFx("damagePerLevel");
    }

    if (const Json& tr = j["terrain"]; tr.isObject()) { // other keys of the block are visual (view)
        std::string model = tr.getStr("model", "levels");
        if (model == "levels") r.terrain.model = TerrainModel::Levels;
        else if (model == "flat") r.terrain.model = TerrainModel::Flat;
        else if (model == "heightfield") r.terrain.model = TerrainModel::Heightfield;
        else fail("terrain.model", "expected 'flat', 'levels' or 'heightfield', got '" + model + "'");
        if (const Json& ms = tr["maxSlope"]; !ms.isNull()) {
            if (ms.isNumber()) r.terrain.maxSlopeGround = ms.fx();
            else if (ms.isObject()) {
                for (auto& [k, v] : ms.members()) {
                    if (k == "ground") r.terrain.maxSlopeGround = v.fx();
                    else if (k == "sand") r.terrain.maxSlopeSand = v.fx();
                    else fail("terrain.maxSlope", "unknown move layer '" + k + "' (ground, sand)");
                    if (v.fx().raw <= 0) fail("terrain.maxSlope." + k, "must be positive");
                }
            } else fail("terrain.maxSlope", "expected a number or {\"ground\": x, \"sand\": y}");
            if (r.terrain.maxSlopeGround.raw <= 0) fail("terrain.maxSlope", "must be positive");
        }
        auto nonNeg = [&](const char* key, Fixed& v) {
            v = tr.getFx(key, v);
            if (v.raw < 0) fail(std::string("terrain.") + key, "must not be negative");
        };
        nonNeg("buildSlope", r.terrain.buildSlope);
        nonNeg("uphillSpeed", r.terrain.uphillSpeed);
        nonNeg("downhillSpeed", r.terrain.downhillSpeed);
        nonNeg("eyeHeight", r.terrain.eyeHeight);
        nonNeg("targetHeight", r.terrain.targetHeight);
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
        d.global = rj.getBool("global", false);
        d.regen = rj.getFx("regen");
        r.resources.push_back(d);
    }
    if (r.resources.size() > 32) fail("resources", "at most 32 resources are supported");
    auto resRef = [&](const std::string& where, const std::string& id) -> uint16_t {
        int ri = r.findResource(id);
        if (ri < 0) fail(where, "unknown resource '" + id + "'");
        return uint16_t(ri);
    };
    for (auto& rd : r.resources) {
        const Json& cv = j["resources"][rd.id]["convertsTo"];
        if (!cv.isObject()) continue;
        for (auto& [to, rate] : cv.members()) {
            int ti = r.findResource(to);
            if (ti < 0) fail("resource '" + rd.id + "' convertsTo", "unknown resource '" + to + "'");
            if (r.resources[size_t(ti)].convertTo >= 0) fail("resource '" + rd.id + "' convertsTo", "'" + to + "' converts itself");
            rd.convertTo = int16_t(ti);
            rd.convertRate = rate.fx();
        }
    }
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
    for (auto& pa : j["playerAbilities"].items()) for (auto& e : pa["effects"].items()) damageIdx(e.getStr("damageType", "normal"));
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

    // ---- effects parser (abilities, statuses, payloads)
    EffectParser ep{r, typeRef, resRef, [&](const std::string& d) { return damageIdx(d); }, statMod, tagsOf,
                    [&](const std::string& where, const std::string& id) { Requirement q = requirement(where, id); return q.tech ? (0x8000 | q.index) : int(q.index); }};
    ep.namedStatuses(j["statuses"]);
    r.loadoutSlots = int(j.getInt("loadoutSlots", 0));

    // ---- dungeon: diggable terrain and rooms (types reference rooms)
    for (auto& [ch, dj] : j["diggable"].members()) {
        const std::string where = "diggable '" + ch + "'";
        if (ch.size() != 1 || std::string(".~#,=/R ").find(ch[0]) != std::string::npos || (ch[0] >= '0' && ch[0] <= '9'))
            fail(where, "key must be one character not used by built-in terrain (. ~ # , = / R digits)");
        DiggableDef d;
        d.ch = ch[0];
        d.hardness = dj.getFx("hardness", d.hardness);
        if (d.hardness.raw <= 0) fail(where, "hardness must be positive");
        amounts(where + " yields", dj["yields"], d.yields, {});
        d.infinite = dj.getBool("infinite", false);
        r.diggables.push_back(d);
    }
    for (auto& [id, rj] : j["rooms"].members()) {
        const std::string where = "room '" + id + "'";
        RoomDef d;
        d.id = id;
        d.name = rj.getStr("name", id);
        amounts(where + " cost", rj["cost"], d.cost, {});
        d.supply = rj.getFx("supply");
        amounts(where + " income", rj["income"], d.income, {});
        amounts(where + " storage", rj["storage"], d.storage, {});
        amounts(where + " provides", rj["provides"], d.provides, {});
        if (const Json& wk = rj["work"]; wk.isObject()) {
            amounts(where + " work.yields", wk["yields"], d.workYield, {});
            amounts(where + " work.cost", wk["cost"], d.workCost, {});
            d.workXp = wk.getFx("xp");
        }
        d.view = rj["view"];
        r.rooms.push_back(d);
    }
    if (r.rooms.size() > 250) fail("rooms", "at most 250 room types");
    auto roomRef = [&](const std::string& where, const std::string& id) -> int16_t {
        int k = r.findRoom(id);
        if (k < 0) fail(where, "unknown room '" + id + "'");
        return int16_t(k);
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
        if (const Json& vj = tj["vision"]; vj.isObject()) {
            t.visionBlock = fxMax(Fixed{}, vj.getFx("blocks"));
            t.eyeHeight = fxMax(Fixed{}, vj.getFx("eye"));
            t.seesOver = vj.getBool("seesOver", false);
        }
        if (const Json& m = tj["move"]; m.isObject()) {
            t.speed = m.getFx("speed", Fixed::fromInt(2));
            t.layer = parseLayers(m["layer"], kLayerGround);
            t.transitSpeed = m.getFx("transitSpeed", t.speed * 10);
            t.radius = m.getFx("radius", t.radius);
            if (m.has("maxSlope")) {
                t.maxSlope = m.getFx("maxSlope");
                if (t.maxSlope.raw <= 0) fail(where + " move.maxSlope", "must be positive");
            }
            t.mass = m.getFx("mass", t.radius * t.radius * 4);
            if (t.radius.raw <= 0 || t.mass.raw <= 0) fail(where + " move", "radius and mass must be positive");
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
            w.chargeTime = wj.getFx("chargeTime");
            if (wj.has("onHit")) {
                const Json& oh = wj["onHit"];
                w.onHit = int16_t(ep.payload(where + " weapon " + w.id + " onHit", oh.isObject() && oh.has("effects") ? oh["effects"] : oh));
                if (oh.isObject() && oh.has("effects")) w.onHitChance = oh.getFx("chance", w.onHitChance);
            }
            t.weapons.push_back(w);
        }
        if (const Json& e = tj["energy"]; e.isObject()) {
            t.maxEnergy = e.getFx("max", Fixed::fromInt(100));
            t.energyRegen = e.getFx("regen", Fixed::fromRatio(1, 2));
            t.startEnergy = e.getFx("start", t.maxEnergy / 2);
        }
        for (auto& aj : tj["abilities"].items()) {
            AbilityDef a;
            ep.ability(where, aj, a);
            t.abilities.push_back(std::move(a));
        }
        if (t.abilities.size() > 8) fail(where, "at most 8 abilities per type");
        for (auto& st : tj["statuses"].items()) t.statuses.push_back(int16_t(ep.status(where + " statuses", st)));
        t.level = int(tj.getInt("level", 1));
        t.blocksLanes = tj.getBool("blocksLanes", false);
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
        t.capturable = tj.getBool("capturable", t.capturable);
        if (const Json& d = tj["digs"]; d.isObject()) t.digRate = d.getFx("rate", kFxOne);
        if (const Json& c = tj["claimer"]; c.isObject()) t.claimerTime = c.getFx("time", Fixed::fromInt(2));
        if (const Json& c = tj["claim"]; c.isObject()) {
            t.claimRadius = c.getFx("radius", Fixed::fromInt(1));
            t.claimRate = c.getFx("rate", kFxOne);
        }
        t.claimedByTerritory = tj.getBool("claimedByTerritory", false);
        if (const Json& n = tj["needs"]; n.isObject()) {
            t.hasNeeds = true;
            NeedsDef& nd = t.needs;
            auto need = [&](const char* key, NeedDef& out) {
                const Json& q = n[key];
                if (!q.isObject()) return;
                out.enabled = true;
                out.interval = q.getFx("interval", out.interval);
                if (q.has("room")) out.room = roomRef(where + " needs." + key, q.getStr("room"));
                if (q.has("at")) out.structure = typeRef(where + " needs." + key + ".at", q.getStr("at"));
                out.starve = q.getFx("starve");
                out.time = q.getFx("time", out.time);
                amounts(where + " needs." + key + ".cost", q["cost"], out.cost, {});
                if (out.interval.raw <= 0) fail(where + " needs." + key, "interval must be positive");
            };
            need("hunger", nd.hunger);
            need("sleep", nd.sleep);
            if (const Json& py = n["pay"]; py.isObject()) {
                nd.payInterval = py.getFx("interval", Fixed::fromInt(120));
                amounts(where + " needs.pay.amount", py["amount"], nd.pay, {});
            }
            for (auto& wr : n["works"].items()) nd.works.push_back(uint16_t(roomRef(where + " needs.works", wr.str())));
            nd.workTime = n.getFx("workTime", nd.workTime);
            if (const Json& h = n["happiness"]; h.isObject()) {
                nd.happyStart = h.getFx("start", nd.happyStart);
                nd.unmetRate = h.getFx("unmetPerSecond", nd.unmetRate);
                nd.recovery = h.getFx("recovery", nd.recovery);
                nd.slapLoss = h.getFx("slap", nd.slapLoss);
                nd.unpaidLoss = h.getFx("unpaid", nd.unpaidLoss);
                nd.leaveBelow = h.getFx("leaveBelow", nd.leaveBelow);
                nd.leaveAfter = h.getFx("leaveAfter", nd.leaveAfter);
            }
        }
        if (const Json& a = tj["attractedBy"]; a.isObject()) {
            t.attractable = true;
            for (auto& [room, cells] : a["rooms"].members()) t.attract.rooms.push_back({uint16_t(roomRef(where + " attractedBy.rooms", room)), int(cells.integer(1))});
            amounts(where + " attractedBy.resources", a["resources"], t.attract.resources, {});
            t.attract.weight = a.getFx("weight", t.attract.weight);
        }
        if (const Json& p = tj["portal"]; p.isObject()) {
            t.portalInterval = p.getFx("interval", Fixed::fromInt(20));
            for (auto& u : p["pool"].items()) t.portalPool.push_back(typeRef(where + " portal.pool", u.str()));
            if (t.portalInterval.raw <= 0) fail(where + " portal", "interval must be positive");
        }
        if (const Json& sh = tj["storehouse"]; sh.isObject()) t.storeCapacity = sh.getFx("capacity", Fixed::fromInt(1000));
        if (const Json& rc = tj["recipe"]; rc.isObject()) {
            t.recipe.enabled = true;
            amounts(where + " recipe.in", rc["in"], t.recipe.in, {});
            amounts(where + " recipe.out", rc["out"], t.recipe.out, {});
            t.recipe.time = rc.getFx("time", t.recipe.time);
            if (rc.has("from")) t.recipe.fromRes = int16_t(resRef(where + " recipe.from", rc.getStr("from")));
            t.recipe.radius = rc.getFx("radius", t.recipe.radius);
            t.recipe.buffer = std::max<int>(1, int(rc.getInt("buffer", 2)));
            if (t.recipe.out.empty()) fail(where + " recipe", "needs 'out'");
            if (t.recipe.time.raw <= 0) fail(where + " recipe", "time must be positive");
        }
        if (const Json& cr = tj["carrier"]; cr.isObject()) t.carrierCapacity = cr.getFx("capacity", kFxOne);
        if (tj.has("trainFrom")) t.trainFrom = typeRef(where + " trainFrom", tj.getStr("trainFrom"));
        if (const Json& at = tj["autoTrain"]; at.isObject()) {
            t.autoTrainUnit = typeRef(where + " autoTrain.unit", at.getStr("unit"));
            t.autoTrainInterval = at.getFx("interval", t.autoTrainInterval);
            if (t.autoTrainInterval.raw <= 0) fail(where + " autoTrain", "interval must be positive");
        }
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
        t.levels = int(kj.getInt("levels", 1));
        if (t.levels < 1 || t.levels > 250) fail(where, "levels must be 1..250");
        t.costGrowth = kj.getFx("costGrowth");
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
    if (const Json& tr = j["territory"]; tr.isObject()) {
        r.territory.enabled = true;
        std::string by = tr.getStr("claimBy", "both");
        if (by != "both" && by != "units" && by != "workers") fail("territory.claimBy", "expected units, workers or both, got '" + by + "'");
        r.territory.claimBy = by == "units" ? ClaimBy::Units : by == "workers" ? ClaimBy::Workers : ClaimBy::Both;
        r.territory.startFromRegions = tr.getBool("startFromRegions", false);
        r.territory.startRadius = int(tr.getInt("startRadius", 0));
        r.territory.buildOnlyOwned = tr.getBool("buildOnlyOwned", false);
    }
    if (const Json& h = j["hand"]; h.isObject()) {
        r.hand.max = int(h.getInt("max", 8));
        r.hand.ownTerritoryOnly = h.getBool("ownTerritoryOnly", true);
        r.hand.slapDamage = h.getFx("slapDamage", r.hand.slapDamage);
        r.hand.slapDuration = h.getFx("slapDuration", r.hand.slapDuration);
        r.hand.slapSpeed = h.getFx("slapSpeed", r.hand.slapSpeed);
        r.hand.slapWork = h.getFx("slapWork", r.hand.slapWork);
    }
    if (const Json& ps = j["possession"]; ps.isObject()) {
        r.possession.enabled = true;
        amounts("possession.cost", ps["cost"], r.possession.cost, {});
        amounts("possession.drain", ps["drain"], r.possession.drain, {});
    }
    for (auto& pj : j["playerAbilities"].items()) {
        PlayerAbilityDef a;
        a.id = pj.getStr("id", "ability" + std::to_string(r.playerAbilities.size()));
        if (r.findPlayerAbility(a.id) >= 0) fail("playerAbility '" + a.id + "'", "defined twice");
        ep.playerAbility("playerAbility", pj, a);
        r.playerAbilities.push_back(std::move(a));
    }
    if (r.playerAbilities.size() > 64) fail("playerAbilities", "at most 64");
    std::string storage = j.getStr("storage", "player");
    if (storage != "player" && storage != "buildings") fail("storage", "expected 'player' or 'buildings', got '" + storage + "'");
    r.buildingStorage = storage == "buildings";
    if (const Json& rn = j["roadNetwork"]; rn.isObject()) {
        r.roadNetwork.enabled = true;
        amounts("roadNetwork.cost", rn["cost"], r.roadNetwork.cost, {});
        r.roadNetwork.speed = rn.getFx("speed", r.roadNetwork.speed);
        r.roadNetwork.required = rn.getBool("required", true);
    }
    if (const Json& ln = j["lanes"]; ln.isObject()) {
        r.lanes.enabled = true;
        r.lanes.left = std::clamp<int>(int(ln.getInt("left", 0)), 0, 8);
        r.lanes.right = std::clamp<int>(int(ln.getInt("right", 0)), 0, 8);
        r.lanes.spacing = ln.getFx("spacing", r.lanes.spacing);
    }
    if (const Json& mv = j["movement"]; mv.isObject()) {
        std::string av = mv.getStr("avoidance", "rvo");
        if (av != "rvo" && av != "simple") fail("movement.avoidance", "expected 'rvo' or 'simple', got '" + av + "'");
        r.movement.rvo = av == "rvo";
        r.movement.neighbors = std::clamp<int>(int(mv.getInt("neighbors", 8)), 1, 32);
        r.movement.timeHorizon = mv.getFx("timeHorizon", r.movement.timeHorizon);
    }
    // slope classes: types with their own move.maxSlope get a class (pathing and passability per class)
    for (auto& t : r.types) {
        t.pathLayer = uint8_t(t.layer & ~kLayerOrbit);   // bits 4..7 of pathLayer are the slope class
        if (!r.terrain.heightfield() || t.maxSlope.raw < 0 || (t.layer & kLayerAir)) continue;
        auto& cls = r.terrain.slopeClasses;
        auto it = std::find(cls.begin(), cls.end(), t.maxSlope);
        if (it == cls.end()) {
            if (cls.size() >= 15) fail("type '" + t.id + "' move.maxSlope", "at most 15 different maxSlope values");
            cls.push_back(t.maxSlope);
            it = cls.end() - 1;
        }
        t.pathLayer = uint8_t(t.layer | ((it - cls.begin() + 1) << 4));
    }
    bool anySettlers = r.buildingStorage || r.roadNetwork.enabled;
    for (auto& t : r.types) if (t.storeCapacity.raw > 0 || t.recipe.enabled || t.carrierCapacity.raw > 0 || t.trainFrom != kNoType) anySettlers = true;
    r.settlers = anySettlers;
    bool anyDungeonType = false;
    for (auto& t : r.types)
        if (t.digRate.raw > 0 || t.claimerTime.raw > 0 || t.claimRate.raw > 0 || t.hasNeeds || t.portalInterval.raw > 0 ||
            t.autoTrainUnit != kNoType || t.claimedByTerritory) anyDungeonType = true;
    for (auto& t : r.types) if (t.hasNeeds) anyDungeonType = true;
    if (anyDungeonType && !r.territory.enabled) {
        // claimers / portals imply a territory layer
        for (auto& t : r.types) if (t.claimerTime.raw > 0 || t.claimRate.raw > 0 || t.claimedByTerritory) r.territory.enabled = true;
    }
    // built-in statuses (internal ids start with '_')
    r.userStatuses = r.statuses.size();
    auto builtin = [&](const char* id, uint32_t flags) {
        StatusDef s; s.id = id; s.name = id; s.flags = flags; s.stacking = Stacking::Independent;
        r.statuses.push_back(s);
        return int16_t(r.statuses.size() - 1);
    };
    r.stTemp = builtin("_temporary", 0);
    r.stLink = builtin("_link", 0);
    r.stRedirect = builtin("_redirect", 0);
    r.stFake = builtin("_illusion", kStFake);
    if (r.statuses.size() > 60000) fail("statuses", "too many statuses");
    // anything beyond the original instant effects switches the effect engine on (old games stay bit-identical)
    bool adv = r.userStatuses > 0 || r.loadoutSlots > 0;
    for (auto& pl : r.payloads) for (auto& e : pl) if (e.kind > EffectKind::Blink || e.chance < Fixed::fromInt(1) || e.randomAmount || e.limit > 0 || e.filter.explicitShape) adv = true;
    for (auto& rs : r.resources) if (rs.regen.raw != 0) adv = true;
    for (auto& t : r.types) {
        if (!t.statuses.empty() || t.blocksLanes || t.level != 1) adv = true;
        for (auto& wd : t.weapons) if (wd.onHit >= 0 || wd.chargeTime.raw > 0) adv = true;
        for (auto& a : t.abilities)
            if (a.castTime.raw > 0 || a.backswing.raw > 0 || a.channel || a.chargesMax > 0 || !a.levels.empty() || a.initialCooldown.raw > 0 ||
                !a.cost.empty() || !a.requires_.empty() || a.smart != SmartRule::Auto || a.target > TargetKind::Point) adv = true;
    }
    for (auto& a : r.playerAbilities)
        if (!a.levels.empty() || a.passive || a.autoEvery.raw > 0 || a.castTime.raw > 0 || !a.requires_.empty() || a.initialCooldown.raw > 0 ||
            (a.target != TargetKind::Point && a.target != TargetKind::None) || a.smart != SmartRule::Auto) adv = true;
    r.advancedEffects = adv;
    r.dungeon = anyDungeonType || r.territory.enabled || !r.diggables.empty() || !r.rooms.empty() || r.hand.max > 0 ||
                r.possession.enabled || !r.playerAbilities.empty();
    loadMechRules(r, j);
    return r;
}

} // namespace rts::game
