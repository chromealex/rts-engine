// Parsing of abilities, effects, statuses and delivery blocks (see RtsAbilityData.h). Called from loadRules.
#include "RtsEffectsData.h"
#include <algorithm>
#include <stdexcept>

namespace rts::game {

namespace {
[[noreturn]] void fail(const std::string& where, const std::string& what) { throw std::runtime_error(where + ": " + what); }

// cos(deg) by a Taylor series in fixed point (deterministic; plenty for cone angles).
Fixed cosDeg(Fixed deg) {
    Fixed x = deg * Fixed::fromRaw(1144);  // pi / 180 in 16.16
    Fixed x2 = x * x, term = Fixed::fromInt(1), sum = term;
    for (int k = 1; k <= 6; ++k) { term = -term * x2 / int64_t((2 * k - 1) * (2 * k)); sum += term; }
    return fxClamp(sum, Fixed::fromInt(-1), Fixed::fromInt(1));
}

// Level overrides: objects merge key by key, arrays of objects merge element-wise, anything else replaces.
Json merge(const Json& base, const Json& over) {
    if (base.isObject() && over.isObject()) {
        Json out = base;
        for (auto& [k, v] : over.members()) out.set(k, merge(base[k], v));
        return out;
    }
    if (base.isArray() && over.isArray() && base.size() == over.size()) {
        Json out = Json::array();
        for (size_t i = 0; i < base.size(); ++i) out.push(merge(base[i], over[i]));
        return out;
    }
    return over;
}
} // namespace

uint64_t EffectParser::statusTags(const Json& arr) {
    uint64_t m = 0;
    for (auto& t : arr.items()) {
        auto& names = r.statusTagNames;
        auto it = std::find(names.begin(), names.end(), t.str());
        if (it == names.end()) {
            if (names.size() >= 64) fail("status tags", "at most 64 distinct status tags");
            names.push_back(t.str());
            it = names.end() - 1;
        }
        m |= uint64_t(1) << (it - names.begin());
    }
    return m;
}

int EffectParser::payload(const std::string& where, const Json& arr) {
    if (arr.isNull()) return -1;
    std::vector<EffectDef> list;
    if (arr.isObject()) list.push_back(effect(where, arr));
    else for (auto& e : arr.items()) list.push_back(effect(where, e));
    r.payloads.push_back(std::move(list));
    if (r.payloads.size() > 30000) fail(where, "too many effect lists");
    return int(r.payloads.size() - 1);
}

void EffectParser::filter(const std::string& where, const Json& j, EffectDef& e) {
    TargetFilter& f = e.filter;
    f.enemies = e.enemies;
    f.allies = e.allies;
    std::string aff = j.getStr("affects", "");
    if (aff == "self") { f.self = true; f.enemies = f.allies = false; }
    if (aff == "own") { f.allies = true; f.enemies = false; }
    f.neutral = j.getBool("neutral", aff == "all" || aff == "any");
    if (aff == "any") f.self = true;
    std::string sh = j.getStr("shape", "");
    if (!sh.empty()) {
        static const std::pair<const char*, Shape> shapes[] = {{"target", Shape::Target}, {"circle", Shape::Circle}, {"line", Shape::Line}, {"cone", Shape::Cone},
                                                               {"ring", Shape::Ring}, {"map", Shape::Map}, {"group", Shape::Group}};
        bool ok = false;
        for (auto& [n, s] : shapes) if (sh == n) { f.shape = s; ok = true; }
        if (!ok) fail(where, "unknown shape '" + sh + "' (target, circle, line, cone, ring, map, group)");
        f.explicitShape = true;
    } else if (e.radius.raw > 0) f.shape = Shape::Circle;
    f.width = j.getFx("width", f.width);
    if (j.has("angle")) f.angle = cosDeg(j.getFx("angle") / 2);
    f.ring = j.getFx("ring");
    if (j.has("layers")) {
        f.layers = 0;
        for (auto& l : j["layers"].items()) {
            std::string s = l.str();
            f.layers |= s == "air" ? 2 : s == "naval" ? 4 : s == "ground" ? (1 | 8) : 0;
        }
    }
    std::string kind = j.getStr("kind", "");
    if (kind == "structure") f.units = false;
    else if (kind == "unit") f.structures = false;
    else if (!kind.empty() && kind != "any") fail(where, "kind must be unit, structure or any");
    f.garrisons = j.getBool("garrisons", false);
    f.inTransit = j.getBool("inTransit", false);
    f.tags = tagsOf(j["tags"]);
    if (j.has("ofType")) f.type = typeRef(where, j.getStr("ofType"));
    if (const Json& lv = j["level"]; lv.isArray()) { f.minLevel = int(lv[0].integer(0)); f.maxLevel = int(lv[1].integer(1 << 20)); }
    f.maxTargets = int(j.getInt("maxTargets", 0));
    std::string sort = j.getStr("sort", "");
    if (!sort.empty()) {
        static const std::pair<const char*, SortBy> sorts[] = {{"nearest", SortBy::Nearest}, {"farthest", SortBy::Farthest}, {"lowestHp", SortBy::LowestHp},
            {"highestHp", SortBy::HighestHp}, {"levelLow", SortBy::LevelLow}, {"levelHigh", SortBy::LevelHigh}, {"garrisonLow", SortBy::GarrisonLow},
            {"garrisonHigh", SortBy::GarrisonHigh}, {"random", SortBy::Random}};
        bool ok = false;
        for (auto& [n, s] : sorts) if (sort == n) { f.sort = s; ok = true; }
        if (!ok) fail(where, "unknown sort '" + sort + "'");
    }
    f.ownFirst = j.getBool("ownFirst", false);
    f.area = j.getStr("area", "");
}

void EffectParser::delivery(const std::string& where, const Json& j, DeliveryDef& d) {
    d.speed = j.getFx("speed", d.speed);
    d.homing = j.getBool("homing", false);
    d.arc = j.getBool("arc", false);
    d.arcHeight = j.getFx("arcHeight", d.arcHeight);
    d.collide = j.getBool("collide", false);
    d.scatter = j.getFx("scatter");
    d.falloff = j.getFx("falloff");
    d.delay = j.getFx("delay");
    d.lifetime = j.getFx("lifetime");
    d.interval = j.getFx("interval", d.interval);
    if (d.interval.raw <= 0) fail(where, "interval must be positive");
    std::string tr = j.getStr("trigger", "");
    if (tr == "periodic") d.trigger = AreaTrigger::Periodic;
    else if (tr == "contact") d.trigger = AreaTrigger::Contact;
    else if (!tr.empty() && tr != "once") fail(where, "trigger must be once, periodic or contact");
    std::string mo = j.getStr("motion", "");
    if (mo == "toward") d.motion = AreaMotion::Toward;
    else if (mo == "lane") d.motion = AreaMotion::Lane;
    else if (mo == "wander") d.motion = AreaMotion::Wander;
    else if (mo == "follow") d.motion = AreaMotion::Follow;
    else if (!mo.empty() && mo != "static") fail(where, "motion must be static, toward, lane, wander or follow");
    d.maxTriggers = int(j.getInt("maxTriggers", 0));
    d.sight = j.getFx("sight");
    d.blocksVision = j.getFx("blocksVision");
    if (const Json& tp = j["teleportTo"]; tp.isArray()) { d.teleport = true; d.teleportOffset = {tp[0].fx(), Fixed{}, tp[1].fx()}; }
    if (j.has("deathSpawn")) d.deathSpawn = typeRef(where + " deathSpawn", j.getStr("deathSpawn"));
    d.jumps = int(j.getInt("jumps", 0));
    d.jumpRadius = j.getFx("jumpRadius", d.jumpRadius);
    d.jumpFalloff = j.getFx("jumpFalloff", d.jumpFalloff);
}

int EffectParser::status(const std::string& where, const Json& j) {
    if (j.isString()) {
        int k = r.findStatus(j.str());
        if (k < 0) fail(where, "unknown status '" + j.str() + "'");
        return k;
    }
    if (!j.isObject()) fail(where, "status must be an id or an object");
    StatusDef s;
    s.id = j.getStr("id", "status" + std::to_string(r.statuses.size()));
    r.statuses.push_back(s);
    int idx = int(r.statuses.size() - 1);
    statusBody(where + " status '" + s.id + "'", j, idx);
    return idx;
}

void EffectParser::statusBody(const std::string& where, const Json& j, int idx) {
    StatusDef s = r.statuses[size_t(idx)];
    s.name = j.getStr("name", s.id);
    s.tags = statusTags(j["tags"]);
    static const std::pair<const char*, uint32_t> flags[] = {{"stun", kStStun}, {"silence", kStSilence}, {"disarm", kStDisarm}, {"root", kStRoot},
        {"invulnerable", kStInvulnerable}, {"invisible", kStInvisible}, {"lock", kStLock}, {"blind", kStBlind}, {"hidden", kStHidden},
        {"flying", kStFlying}, {"fake", kStFake}, {"ignoreDefense", kStIgnoreDefense}, {"spreads", kStSpreads}, {"freezeRegen", kStFreezeRegen},
        {"alert", kStAlert}, {"revealed", kStRevealed}};
    for (auto& f : j["flags"].items()) {
        bool ok = false;
        for (auto& [n, b] : flags) if (f.str() == n) { s.flags |= b; ok = true; }
        if (!ok) fail(where, "unknown status flag '" + f.str() + "'");
    }
    for (auto& m : j["mods"].items()) s.mods.push_back(statMod(where + " mods", m));
    if (const Json& t = j["tick"]; t.isObject()) {
        s.tickAmount = t.getFx("damage") - t.getFx("heal");
        s.tickDamageType = damageIdx(t.getStr("damageType", "normal"));
        s.tickInterval = t.getFx("interval", s.tickInterval);
        if (s.tickInterval.raw <= 0) fail(where + " tick", "interval must be positive");
        if (t.has("effects")) s.tickPayload = int16_t(payload(where + " tick", t["effects"]));
    }
    s.shield = j.getFx("shield");
    std::string st = j.getStr("stacking", "refresh");
    s.stacking = st == "stack" ? Stacking::Stack : st == "independent" ? Stacking::Independent : Stacking::Refresh;
    if (st != "refresh" && st != "stack" && st != "independent") fail(where, "stacking must be refresh, stack or independent");
    s.maxStacks = std::max<int>(1, int(j.getInt("maxStacks", s.stacking == Stacking::Refresh ? 1 : 5)));
    if (j.has("onExpire")) s.onExpire = int16_t(payload(where + " onExpire", j["onExpire"]));
    if (j.has("onCaptured")) s.onCaptured = int16_t(payload(where + " onCaptured", j["onCaptured"]));
    if (j.has("onHit")) s.onHit = int16_t(payload(where + " onHit", j["onHit"]));
    for (auto& [res, v] : j["incomeMult"].members()) s.incomeMult.push_back({resRef(where + " incomeMult", res), v.fx()});
    s.garrisonPerSecond = j.getFx("garrisonPerSecond");
    s.view = j["view"];
    r.statuses[size_t(idx)] = s;
}

void EffectParser::namedStatuses(const Json& j) {
    for (auto& [id, sj] : j.members()) {
        if (r.findStatus(id) >= 0) fail("status '" + id + "'", "defined twice");
        StatusDef s;
        s.id = id;
        r.statuses.push_back(s);
    }
    for (auto& [id, sj] : j.members()) statusBody("status '" + id + "'", sj, r.findStatus(id));
}

EffectDef EffectParser::effect(const std::string& where, const Json& ej) {
    EffectDef e;
    static const std::pair<const char*, EffectKind> kinds[] = {
        {"damage", EffectKind::Damage}, {"heal", EffectKind::Heal}, {"stun", EffectKind::Stun}, {"buff", EffectKind::Buff}, {"spawn", EffectKind::Spawn},
        {"blink", EffectKind::Blink}, {"status", EffectKind::Status}, {"dispel", EffectKind::Dispel}, {"knockback", EffectKind::Knockback},
        {"teleport", EffectKind::Teleport}, {"resource", EffectKind::Resource}, {"energyBurn", EffectKind::EnergyBurn}, {"reveal", EffectKind::Reveal},
        {"kill", EffectKind::Kill}, {"area", EffectKind::Area}, {"projectile", EffectKind::Projectile}, {"chain", EffectKind::Chain}, {"cast", EffectKind::Cast},
        {"garrisonKill", EffectKind::GarrisonKill}, {"garrisonAdd", EffectKind::GarrisonAdd}, {"garrisonConvert", EffectKind::GarrisonConvert},
        {"garrisonPanic", EffectKind::GarrisonPanic}, {"capture", EffectKind::Capture}, {"swap", EffectKind::Swap}, {"transform", EffectKind::Transform},
        {"level", EffectKind::Level}, {"equalize", EffectKind::Equalize}, {"link", EffectKind::Link}, {"redirect", EffectKind::Redirect},
        {"sendBack", EffectKind::SendBack}, {"clone", EffectKind::Clone}, {"formation", EffectKind::Formation}, {"convert", EffectKind::Convert},
        {"playerStatus", EffectKind::PlayerStatus}, {"equalizeResource", EffectKind::EqualizeResource}, {"sacrifice", EffectKind::Sacrifice},
        {"stealSkill", EffectKind::StealSkill}, {"cooldown", EffectKind::Cooldown}, {"script", EffectKind::Script}};
    std::string k = ej.getStr("type", "damage");
    bool known = false;
    for (auto& [n, kk] : kinds) if (k == n) { e.kind = kk; known = true; }
    if (!known) fail(where, "unknown effect type '" + k + "'");
    const std::string w = where + " effect '" + k + "'";
    if (const Json& a = ej["amount"]; a.isArray()) {
        e.amount = a[0].fx(); e.amountMax = a[1].fx(); e.randomAmount = true;
        if (e.amountMax < e.amount) fail(w, "amount range must be [min, max]");
    }
    else e.amount = ej.getFx("amount");
    if (e.kind == EffectKind::Script) {
        e.scriptFn = ej.getStr("fn");
        if (e.scriptFn.empty()) fail(w, "needs 'fn' (a function of the game's scripts)");
        e.scriptArgs = ej;
    }
    e.percent = ej.getFx("percent");
    e.radius = ej.getFx("radius");
    e.duration = ej.getFx("duration");
    e.chance = ej.getFx("chance", e.chance);
    e.damageType = damageIdx(ej.getStr("damageType", "normal"));
    if (e.kind == EffectKind::Buff) e.mod = statMod(w, ej);
    if (ej.has("unit")) e.spawnType = typeRef(w, ej.getStr("unit"));
    if (e.kind == EffectKind::Spawn && e.spawnType == kNoType) fail(w, "needs 'unit'");
    e.count = int(ej.getInt("count", 1));
    e.limit = int(ej.getInt("limit", 0));
    bool friendly = e.kind == EffectKind::Heal || e.kind == EffectKind::GarrisonAdd || e.kind == EffectKind::Equalize ||
                    (e.kind == EffectKind::Buff && e.mod.mul >= Fixed::fromInt(1) && e.mod.add.raw >= 0);
    std::string aff = ej.getStr("affects", friendly ? "allies" : "enemies");
    e.enemies = aff == "enemies" || aff == "all" || aff == "any";
    e.allies = aff == "allies" || aff == "all" || aff == "any" || aff == "own";
    filter(w, ej, e);
    if (ej.has("status")) e.status = int16_t(status(w, ej["status"]));
    if ((e.kind == EffectKind::Status || e.kind == EffectKind::PlayerStatus) && e.status < 0) fail(w, "needs 'status'");
    delivery(w, ej, e.delivery);
    if (ej.has("effects")) e.payload = int16_t(payload(w, ej["effects"]));
    if ((e.kind == EffectKind::Projectile || e.kind == EffectKind::Chain || e.kind == EffectKind::Cast) && e.payload < 0) fail(w, "needs 'effects'");
    if (ej.has("resource")) e.resource = int16_t(resRef(w, ej.getStr("resource")));
    if ((e.kind == EffectKind::Resource || e.kind == EffectKind::EqualizeResource) && e.resource < 0) fail(w, "needs 'resource'");
    e.transfer = ej.getBool("transfer", false);
    for (auto& [res, v] : ej["gain"].members()) e.gain.push_back({resRef(w + " gain", res), v.fx()});
    if (ej.has("to")) e.toType = typeRef(w, ej.getStr("to"));
    if (e.kind == EffectKind::Transform && e.toType == kNoType) fail(w, "needs 'to'");
    e.keepChance = ej.getFx("keepChance");
    e.levelDelta = int(ej.getInt("delta", 1));
    e.dispelTags = statusTags(ej["dispel"]);
    std::string from = ej.getStr("from", "any");
    e.dispelFrom = from == "enemies" ? 1 : from == "allies" ? 2 : 0;
    std::string mode = ej.getStr("mode", "");
    if (e.kind == EffectKind::SendBack) e.mode = mode == "reverse" ? 1 : mode == "throwBack" ? 2 : 0;
    if (e.kind == EffectKind::Teleport) e.mode = mode == "caster" ? 1 : 0;
    if (e.kind == EffectKind::Spawn) {
        std::string ow = ej.getStr("owner", "caster");
        if (ow != "caster" && ow != "hostile" && ow != "neutral") fail(w, "owner must be caster, hostile or neutral");
        e.mode = ow == "hostile" ? 1 : ow == "neutral" ? 2 : 0;
    }
    e.fake = ej.getBool("fake", false);
    e.ofLost = ej.getBool("ofLost", false);
    e.fromCaster = ej.getBool("fromCaster", false);
    return e;
}

namespace {
SmartRule smartRule(const std::string& where, const std::string& s) {
    static const std::pair<const char*, SmartRule> rules[] = {{"auto", SmartRule::Auto}, {"lowestHpAlly", SmartRule::LowestHpAlly},
        {"densestEnemy", SmartRule::DensestEnemy}, {"strongestEnemyStructure", SmartRule::StrongestEnemyStructure},
        {"weakestOwnStructure", SmartRule::WeakestOwnStructure}, {"largestEnemyGroup", SmartRule::LargestEnemyGroup},
        {"ownStructureUnderAttack", SmartRule::OwnStructureUnderAttack}, {"self", SmartRule::Self}, {"enemyPlayer", SmartRule::EnemyPlayer},
        {"debuffedOwn", SmartRule::DebuffedOwn}};
    for (auto& [n, r] : rules) if (s == n) return r;
    throw std::runtime_error(where + ": unknown smart rule '" + s + "'");
}
TargetKind targetKind(const std::string& where, const std::string& tg) {
    if (tg == "none") return TargetKind::None;
    if (tg == "self") return TargetKind::Self;
    if (tg == "unit") return TargetKind::Unit;
    if (tg == "point") return TargetKind::Point;
    if (tg == "structure") return TargetKind::Structure;
    if (tg == "group") return TargetKind::Group;
    if (tg == "player") return TargetKind::Player;
    throw std::runtime_error(where + ": unknown target '" + tg + "' (none, self, unit, point, structure, group, player)");
}
} // namespace

// "delivery": {"type": "projectile" | "area" | "chain" | "channel" | "line" | "cone" | "instant", ...}
int EffectParser::deliveredPayload(const std::string& where, const Json& aj, bool* channel) {
    const Json& d = aj["delivery"];
    int inner = payload(where, aj["effects"]);
    if (inner < 0) { r.payloads.emplace_back(); inner = int(r.payloads.size() - 1); }
    if (!d.isObject()) return inner;
    std::string type = d.getStr("type", "instant");
    if (type == "instant") return inner;
    if (type == "channel") { if (channel) *channel = true; return inner; }
    if (type == "line" || type == "cone") {
        for (auto& e : r.payloads[size_t(inner)])
            if (!e.filter.explicitShape) {
                e.filter.shape = type == "line" ? Shape::Line : Shape::Cone;
                e.filter.width = d.getFx("width", e.filter.width);
                if (d.has("angle")) e.filter.angle = cosDeg(d.getFx("angle") / 2);
                if (e.radius.raw <= 0) e.radius = d.getFx("length", aj.getFx("range", Fixed::fromInt(6)));
                e.filter.explicitShape = true;
            }
        return inner;
    }
    EffectDef w;
    if (type == "projectile") w.kind = EffectKind::Projectile;
    else if (type == "area") w.kind = EffectKind::Area;
    else if (type == "chain") w.kind = EffectKind::Chain;
    else fail(where + " delivery", "unknown type '" + type + "' (instant, projectile, area, chain, channel, line, cone)");
    w.payload = int16_t(inner);
    w.filter.shape = Shape::Target;
    w.enemies = w.filter.enemies = true;
    w.filter.neutral = d.getBool("neutral", false);
    w.radius = d.getFx("radius");
    delivery(where + " delivery", d, w.delivery);
    if (w.kind == EffectKind::Chain && w.delivery.jumps <= 0) w.delivery.jumps = 3;
    r.payloads.push_back({w});
    return int(r.payloads.size() - 1);
}

void EffectParser::ability(const std::string& where, const Json& aj0, AbilityDef& a, bool withLevels) {
    const Json& aj = aj0;
    a.id = aj.getStr("id", "ability");
    const std::string w = where + " ability '" + a.id + "'";
    a.name = aj.getStr("name", a.id);
    a.target = targetKind(w, aj.getStr("target", "none"));
    a.range = aj.getFx("range", a.range);
    a.cooldown = aj.getFx("cooldown", a.cooldown);
    a.initialCooldown = aj.getFx("initialCooldown");
    a.energyCost = aj.getFx("energy");
    for (auto& [res, v] : aj["cost"].members()) a.cost.push_back({resRef(w + " cost", res), v.fx()});
    a.requiredLevel = int(aj.getInt("level", 1));
    for (auto& rq : aj["requires"].items()) a.requires_.push_back(requirement(w + " requires", rq.str()));
    a.autocast = aj.getBool("autocast", true);
    a.aura = aj.getBool("aura", false);
    a.auraRadius = aj.getFx("radius", Fixed::fromInt(6));
    a.castTime = aj.getFx("castTime");
    a.backswing = aj.getFx("backswing");
    a.chargesMax = int(aj.getInt("charges", 0));
    if (aj.has("smart")) a.smart = smartRule(w, aj.getStr("smart"));
    for (auto& ej : aj["effects"].items()) a.effects.push_back(effect(w, ej)); // kept for auras / inspection
    a.payload = int16_t(deliveredPayload(w, aj, &a.channel));
    if (const Json& d = aj["delivery"]; d.isObject() && a.channel) {
        a.channelTime = d.getFx("duration", Fixed::fromInt(3));
        a.channelInterval = d.getFx("interval", a.channelInterval);
        if (a.channelInterval.raw <= 0) fail(w + " delivery", "interval must be positive");
    }
    if (withLevels)
        for (auto& lv : aj["levels"].items()) {
            AbilityDef l;
            ability(where, merge(aj, lv), l, false);
            a.levels.push_back(std::move(l));
        }
}

void EffectParser::playerAbility(const std::string& where, const Json& pj, PlayerAbilityDef& a, bool withLevels) {
    a.id = pj.getStr("id", a.id.empty() ? "ability" : a.id);
    const std::string w = where + " '" + a.id + "'";
    a.name = pj.getStr("name", a.id);
    for (auto& [res, v] : pj["cost"].members()) a.cost.push_back({resRef(w + " cost", res), v.fx()});
    a.cooldown = pj.getFx("cooldown");
    a.initialCooldown = pj.getFx("initialCooldown");
    if (const Json& c = pj["charges"]; c.isObject()) {
        a.chargesMax = int(c.getInt("max", 1));
        for (auto& [res, v] : c["rearmCost"].members()) a.rearmCost.push_back({resRef(w + " charges.rearmCost", res), v.fx()});
        a.rearmTime = c.getFx("rearmTime", a.rearmTime);
    }
    a.target = targetKind(w, pj.getStr("target", "point"));
    if (a.target == TargetKind::Unit || a.target == TargetKind::Self) fail(w, "player abilities target none, point, structure, group or player");
    for (auto& ej : pj["effects"].items()) {
        a.effects.push_back(effect(w, ej));
        if (a.effects.back().kind == EffectKind::Blink) fail(w, "blink needs a caster");
    }
    a.payload = int16_t(deliveredPayload(w, pj, nullptr));
    a.castTime = pj.getFx("castTime");
    for (auto& rq : pj["requires"].items()) a.requires_.push_back(requirement(w + " requires", rq.str()));
    a.passive = pj.getBool("passive", false);
    if (pj.has("status")) a.status = int16_t(status(w, pj["status"]));
    if (a.passive && a.status < 0) fail(w, "a passive ability needs 'status'");
    a.autoEvery = pj.getFx("autoEvery");
    if (pj.has("smart")) a.smart = smartRule(w, pj.getStr("smart"));
    if (withLevels)
        for (auto& lv : pj["levels"].items()) {
            PlayerAbilityDef l;
            l.id = a.id;
            playerAbility(where, merge(pj, lv), l, false);
            a.levels.push_back(std::move(l));
        }
}

} // namespace rts::game
