// Data of the fidelity mechanics: blocks that make sample games play like their originals
// (Warcraft III armor and heroes, StarCraft shields and pylon fields, Age of Empires armor classes and
// bonuses, Z drivers, They Are Billions infection, Mushroom Wars 2 building levels, ...).
// Every block is optional; without them a game loads and plays exactly as before.
#include "RtsEffectsData.h"
#include <stdexcept>

namespace rts::game {

namespace {
[[noreturn]] void fail(const std::string& where, const std::string& what) { throw std::runtime_error(where + ": " + what); }

uint16_t typeId(const Rules& r, const std::string& where, const std::string& id) {
    int k = r.findType(id);
    if (k < 0) fail(where, "unknown type '" + id + "'");
    return uint16_t(k);
}
uint16_t resId(const Rules& r, const std::string& where, const std::string& id) {
    int k = r.findResource(id);
    if (k < 0) fail(where, "unknown resource '" + id + "'");
    return uint16_t(k);
}
uint64_t tagsOf(const Rules& r, const std::string& where, const Json& arr) {
    uint64_t m = 0;
    for (auto& t : arr.items()) {
        uint64_t b = r.tagMask(t.str());
        if (!b) fail(where, "unknown tag '" + t.str() + "' (no type has it)");
        m |= b;
    }
    return m;
}
StatMod statMod(const std::string& where, const Json& e) {
    StatMod m;
    bool ok = false;
    m.stat = parseStat(e.getStr("stat", "damage"), &ok);
    if (!ok) fail(where, "unknown stat '" + e.getStr("stat") + "'");
    m.add = e.getFx("add");
    m.mul = e.getFx("mul", Fixed::fromInt(1));
    return m;
}
ModTarget modTarget(const Rules& r, const std::string& where, const Json& e) {
    ModTarget mt;
    if (e.has("type")) mt.type = typeId(r, where, e.getStr("type"));
    else if (e.has("tags")) mt.tags = tagsOf(r, where, e["tags"]);
    else mt.all = true;
    return mt;
}
} // namespace

void loadOriginals(Rules& r, const Json& j, EffectParser& ep) {
    bool any = false;
    // ---- damage model
    // "armor": {"model", "perPoint", "minDamage", "minShare", "negative", "base"}; alias "armorModel" (agent-mech):
    // "percent" | {"type", "factor", "min", "minFraction", "base"} — same meaning, percent there uses the exact
    // exponential curve for negative armor
    ArmorRules& ar = r.armor;
    auto classIndex = [&](const std::string& name) -> uint16_t {
        for (size_t i = 0; i < ar.classes.size(); ++i) if (ar.classes[i] == name) return uint16_t(i);
        if (ar.classes.size() >= 64) fail("armor classes", "at most 64");
        ar.classes.push_back(name);
        return uint16_t(ar.classes.size() - 1);
    };
    const Json& aj = j["armor"].isObject() ? j["armor"] : j["armorModel"];
    const bool alias = !j["armor"].isObject() && !aj.isNull();
    if (!aj.isNull()) {
        const char* where = alias ? "armorModel" : "armor.model";
        const std::string model = aj.isString() ? aj.str() : aj.getStr(alias ? "type" : "model", "subtract");
        if (model == "subtract") ar.model = ArmorModel::Subtract;
        else if (model == "percent") ar.model = ArmorModel::Percent;
        else if (model == "classes") ar.model = ArmorModel::Classes;
        else fail(where, "expected 'subtract', 'percent' or 'classes', got '" + model + "'");
        const bool cls = ar.model == ArmorModel::Classes;
        ar.perPoint = aj.getFx(alias ? "factor" : "perPoint", ar.perPoint);
        ar.minDamage = aj.getFx(alias ? "min" : "minDamage", cls ? Fixed::fromInt(1) : ar.minDamage);
        ar.minShare = aj.getFx(alias ? "minFraction" : "minShare", cls ? Fixed{} : ar.minShare);
        const std::string neg = aj.getStr("negative", alias ? "exponential" : "linear");
        if (neg != "linear" && neg != "exponential") fail(std::string(where) + " negative", "expected 'linear' or 'exponential', got '" + neg + "'");
        ar.negativeExp = neg == "exponential";
        for (auto& b : aj["base"].items()) classIndex(b.str());
        ar.baseClasses = int(ar.classes.size());
        if (cls && ar.baseClasses == 0) fail(std::string(where) + " base", "the classes model needs base classes, e.g. [\"melee\", \"pierce\"]");
    }
    if (ar.model == ArmorModel::Classes) {
        size_t ci = 0;
        for (auto& [id, tj] : j["types"].members()) {
            UnitType& t = r.types[ci++];
            for (auto& [c, v] : tj["armors"].members()) {
                uint16_t k = classIndex(c);
                if (t.classArmors.size() <= k) t.classArmors.resize(size_t(k) + 1);
                t.classArmors[k] = v.fx();
                t.classMask |= uint64_t(1) << k;
            }
            size_t wi = 0;
            for (auto& wj : tj["weapons"].items()) {
                if (wi >= t.weapons.size()) break;
                for (auto& [c, v] : wj["attacks"].members()) t.weapons[wi].attacks.push_back({classIndex(c), v.fx()});
                ++wi;
            }
        }
    }
    // ---- upkeep with resource masks and income multipliers; "upkeepResources" (agent-mech alias): the resources of
    // every tier without its own "resources"
    uint32_t upkeepDefault = 0xFFFFFFFFu;
    if (j.has("upkeepResources")) {
        upkeepDefault = 0;
        for (auto& rs : j["upkeepResources"].items()) upkeepDefault |= 1u << resId(r, "upkeepResources", rs.str());
    }
    if (j["upkeep"].isArray()) {
        r.upkeep.clear();
        for (auto& u : j["upkeep"].items()) {
            UpkeepTier t;
            t.supply = u.getFx("supply");
            t.gatherMult = u.getFx("gatherMult", t.gatherMult);
            t.incomeMult = u.getFx("incomeMult", t.incomeMult);
            t.resMask = upkeepDefault;
            if (u.has("resources")) {
                t.resMask = 0;
                for (auto& rs : u["resources"].items()) t.resMask |= 1u << resId(r, "upkeep.resources", rs.str());
            }
            r.upkeep.push_back(t);
        }
    }
    // ---- heroes
    if (const Json& h = j["heroes"]; h.isObject()) {
        any = true;
        r.heroes.skills = h.getBool("skills", false);
        r.heroes.max = int(h.getInt("max", 0));
        r.heroes.freeFirst = h.getBool("freeFirst", false);
        for (auto& t : h["tiers"].items()) r.heroes.tiers.push_back(typeId(r, "heroes.tiers", t.str()));
        if (const Json& rv = h["revive"]; rv.isObject()) {
            r.heroes.revive = true;
            r.heroes.reviveShare = rv.getFx("costShare", r.heroes.reviveShare);
            r.heroes.revivePerLevel = rv.getFx("perLevel", r.heroes.revivePerLevel);
            r.heroes.reviveTimeShare = rv.getFx("timeShare", r.heroes.reviveTimeShare);
            r.heroes.reviveMaxTime = rv.getFx("maxTime");
            for (auto& [k, v] : rv["maxCost"].members()) r.heroes.reviveMaxCost.push_back({resId(r, "heroes.revive.maxCost", k), v.fx()});
        }
    }
    // ---- regions / territory extras
    if (const Json& rg = j["regions"]; rg.isObject()) {
        r.regions.requireExplored = rg.getBool("requireExplored", false);
        r.regions.supplyLines = rg.getBool("supplyLines", false);
        r.regions.clearFirst = rg.getBool("clearFirst", false);
        if (r.regions.clearFirst) any = true;
        if (r.regions.supplyLines) any = true;
    }
    if (const Json& tr = j["territory"]; tr.isObject())
        for (auto& [k, v] : tr["incomePerCell"].members()) r.territory.incomePerCell.push_back({resId(r, "territory.incomePerCell", k), v.fx()});
    // ---- resources: event gains and per-unit player mods (morale)
    for (auto& rd : r.resources) {
        const Json& rj = j["resources"][rd.id];
        const std::string where = "resource '" + rd.id + "'";
        if (const Json& g = rj["gain"]; g.isObject()) {
            any = true;
            rd.gainKill = g.getFx("kill");
            rd.gainCapture = g.getFx("capture");
            rd.gainCaptured = g.getFx("captured");
            rd.gainUpgrade = g.getFx("upgrade");
            rd.gainLost = g.getFx("lost");
            rd.gainDefended = g.getFx("defended");
        }
        rd.min = rj.getFx("min");
        for (auto& m : rj["perUnit"].items()) {
            any = true;
            uint64_t tags = m.has("tags") ? tagsOf(r, where + " perUnit", m["tags"]) : 0;
            rd.perUnitMods.push_back({tags, statMod(where + " perUnit", m)});
        }
    }
    // ---- types (same order as loadRules)
    size_t ti = 0;
    for (auto& [id, tj] : j["types"].members()) {
        UnitType& t = r.types[ti++];
        const std::string where = "type '" + id + "'";
        if (const Json& h = tj["health"]; h.isObject()) {
            const Json& a = h["armor"];
            if (a.isObject()) {   // {"melee": 1, "pierce": 2}
                t.armor = a.getFx("melee", a.getFx("normal"));
                t.armor2 = a.getFx("pierce");
            } else t.armor2 = t.armor;
            if (h.has("pierceArmor")) t.armor2 = h.getFx("pierceArmor");
            t.shieldMax = h.getFx("shields");
            t.shieldRegen = h.getFx("shieldRegen", Fixed::fromRatio(2, 1));
            t.shieldDelay = h.getFx("shieldDelay", t.shieldDelay);
            t.shieldArmor = h.getFx("shieldArmor");
            if (t.shieldMax.raw > 0) any = true;
        }
        if (const Json& sh = tj["shields"]; sh.isObject()) {   // alias (agent-mech): {"max", "regen", "delay", "armor"}
            t.shieldMax = sh.getFx("max");
            if (t.shieldMax.raw <= 0) fail(where + " shields.max", "must be > 0");
            t.shieldRegen = sh.getFx("regen", Fixed::fromInt(2));
            t.shieldDelay = sh.getFx("delay", t.shieldDelay);
            t.shieldArmor = sh.getFx("armor");
            any = true;
        }
        size_t wi = 0;
        for (auto& wj : tj["weapons"].items()) {
            WeaponDef& w = t.weapons[wi++];
            std::string ac = wj.getStr("armorClass", "melee");
            if (ac != "melee" && ac != "normal" && ac != "pierce") fail(where + " weapon armorClass", "expected 'melee' or 'pierce'");
            w.armorClass = ac == "pierce" ? 1 : 0;
            for (auto& [tag, v] : wj["bonus"].members()) {
                uint64_t b = r.tagMask(tag);
                if (!b) fail(where + " weapon bonus", "unknown tag '" + tag + "' (no type has it)");
                w.bonus.push_back({b, v.fx()});
            }
            w.killsCrew = wj.getFx("killsCrew");
        }
        if (const Json& rn = tj["resourceNode"]; rn.isObject()) {   // "maxWorkers": agent-mech alias
            t.maxGatherers = int(rn.getInt("maxGatherers", rn.getInt("maxWorkers", 0)));
            if (t.maxGatherers) any = true;
        }
        if (const Json& pf = tj["powerField"]; pf.isObject()) {
            any = true;
            t.fieldId = int16_t(pf.getInt("id", 0));
            t.fieldRadius = pf.getFx("radius", Fixed::fromInt(6));
        }
        if (const Json& s = tj["structure"]; s.isObject() && s.has("needsField")) { any = true; t.needsField = int16_t(s.getInt("needsField", 0)); }
        t.queueSlots = std::max<int>(1, int(tj.getInt("queueSlots", 1)));
        // "crew": "type" / {"unit": type}: manned by units with "crews"; {"accepts": tags} / true (agent-mech alias):
        // manned by any mobile unit with one of the tags (any without tags)
        if (const Json& c = tj["crew"]; c.isString() || c.isObject() || c.boolean()) {
            any = true;
            t.crewed = true;
            if (c.isString() || c.has("unit")) t.crew = typeId(r, where + " crew", c.isString() ? c.str() : c.getStr("unit"));
            if (!c.isString() && (c.boolean() || c.has("accepts") || !c.has("unit"))) {
                t.crewOpen = true;
                t.crewAccepts = tagsOf(r, where + " crew.accepts", c["accepts"]);
            }
        }
        t.crews = tj.getBool("crews", false);
        if (const Json& inf = tj["infect"]; inf.isObject()) {
            any = true;
            t.infectBy = tagsOf(r, where + " infect.by", inf["by"]);
            t.infectSpawn = typeId(r, where + " infect.spawn", inf.getStr("spawn"));
            t.infectCount = std::max<int>(1, int(inf.getInt("count", 1)));
        }
        if (const Json& n = tj["neutral"]; n.isObject()) {
            t.eats = int(n.getInt("eats", 0));
            t.resurface = n.getFx("resurface", t.resurface);
            if (t.eats > 0) any = true;
        }
        if (tj.has("onDeath")) { any = true; t.onDeath = int16_t(ep.payload(where + " onDeath", tj["onDeath"])); }
        if (const Json& c = tj["cost"]; c.isObject()) {
            t.upgradeGarrison = int(c.getInt("garrison", 0));
            t.costGrowth = c.getFx("growth");
            if (t.upgradeGarrison > 0 || t.costGrowth.raw != 0) any = true;
        }
        if (const Json& g = tj["garrison"]; g.isObject()) {
            if (g.has("onCapture")) t.captureAs = typeId(r, where + " garrison.onCapture", g.getStr("onCapture"));
            t.garrisonKeep = int(g.getInt("keep", 0));
            if (t.captureAs != kNoType || t.garrisonKeep > 0) any = true;
        }
        if (const Json& c = tj["crush"]; c.isObject()) { any = true; t.crushTags = tagsOf(r, where + " crush.tags", c["tags"]); }
        if (const Json& df = tj["deathField"]; df.isObject()) {
            any = true;
            t.deathFieldRes = int16_t(resId(r, where + " deathField", df.getStr("resource")));
            t.deathFieldRadius = df.getFx("radius", t.deathFieldRadius);
            t.deathFieldAmount = df.getFx("amount", t.deathFieldAmount);
            if (!r.resources[size_t(t.deathFieldRes)].field) fail(where + " deathField", "resource must be a field resource");
        }
        for (auto& f : tj["factions"].items()) {
            int fi = r.findFaction(f.str());
            if (fi < 0 || fi >= 32) fail(where + " factions", "unknown faction '" + f.str() + "'");
            t.factionMask |= 1u << fi;
            any = true;
        }
        if (const Json& at = tj["autoTrain"]; at.isObject() && at.has("advance")) { t.autoTrainAdvance = at.getBool("advance", true); any = true; }
        t.ownsRegion = tj.getBool("ownsRegion", false);
        if (t.ownsRegion) any = true;
        t.requiresRegions = int(tj.getInt("requiresRegions", 0));
        if (t.requiresRegions > 0) any = true;
        t.clash = tj.getBool("clash", false);
        if (t.clash || t.crews || t.queueSlots > 1) any = true;
        if (const Json& pm = tj["playerMods"]; !pm.isNull()) {
            any = true;
            const Json& list = pm.isObject() ? pm["mods"] : pm;
            if (pm.isObject()) t.playerModsMax = int(pm.getInt("max", 0));
            for (auto& m : list.items()) t.playerMods.push_back({modTarget(r, where + " playerMods", m), statMod(where + " playerMods", m)});
        }
        size_t ai = 0;
        for (auto& aj : tj["abilities"].items()) {
            std::vector<int> la;
            for (auto& x : aj["learnAt"].items()) la.push_back(int(x.integer(1)));
            if (ai < t.abilities.size()) t.learnAt.push_back(la);
            ++ai;
        }
    }
    size_t ki = 0;
    for (auto& [id, kj] : j["techs"].members()) {
        TechDef& t = r.techs[ki++];
        if (const Json& ra = kj["requiresAny"]; ra.isObject()) {
            any = true;
            t.requiresAnyCount = int(ra.getInt("count", 1));
            for (auto& x : ra["of"].items()) t.requiresAny.push_back(typeId(r, "tech '" + id + "' requiresAny", x.str()));
        }
    }
    for (size_t k = 0; k < r.types.size(); ++k) if (!r.types[k].playerMods.empty()) r.playerModTypes.push_back(uint16_t(k));
    for (size_t k = 0; k < r.resources.size(); ++k) if (!r.resources[k].perUnitMods.empty()) r.perUnitResources.push_back(uint16_t(k));
    for (auto& t : r.types) {
        if (t.armor2 != t.armor) any = true;
        for (auto& w : t.weapons) if (!w.bonus.empty() || w.armorClass || w.killsCrew.raw > 0) any = true;
    }
    if (r.armor.minDamage.raw > 0 || r.armor.minShare != Fixed::fromRatio(1, 5)) any = true;
    r.originals = any || r.armor.model == ArmorModel::Percent || !r.territory.incomePerCell.empty() || r.regions.requireExplored;
    for (auto& u : r.upkeep) if (u.resMask != 0xFFFFFFFFu || u.incomeMult != Fixed::fromInt(1)) r.originals = true;
}

} // namespace rts::game
