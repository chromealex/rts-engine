#include "RtsTriggers.h"
#include <algorithm>
#include <stdexcept>
#include "RtsInternal.h"

namespace rts::game {
using namespace detail;

// ================================================================= parsing

bool TrigFilter::matches(const Rules& r, uint16_t t) const {
    if (type != kNoType && type != t) return false;
    if (tags && !(r.types[t].tags & tags)) return false;
    if (structures && !r.types[t].structure) return false;
    if (units && r.types[t].structure) return false;
    return true;
}

int TriggerSet::findArea(const std::string& n) const {
    for (size_t i = 0; i < areaNames.size(); ++i) if (areaNames[i] == n) return int(i);
    return -1;
}
int TriggerSet::findTrigger(const std::string& id) const {
    for (size_t i = 0; i < defs.size(); ++i) if (defs[i].id == id) return int(i);
    return -1;
}

namespace {
[[noreturn]] void fail(const std::string& where, const std::string& msg) { throw std::runtime_error("trigger " + where + ": " + msg); }

TrigPlayer parsePlayer(const Json& j, uint8_t defRel = 0) {
    TrigPlayer p;
    p.rel = defRel;
    if (j.isNumber()) { p.rel = 4; p.slot = int8_t(j.integer()); return p; }
    std::string s = j.str("");
    if (s == "all" || s == "any") p.rel = 1;
    else if (s == "enemies") p.rel = 2;
    else if (s == "allies") p.rel = 3;
    else if (s == "creeps" || s == "hostile") p.rel = 5;
    else if (s == "neutral") { p.rel = 4; p.slot = 0; }
    else if (s == "triggering" || s == "owner") p.rel = 0;
    return p;
}

bool parseCmp(const Json& j, TrigCmp& cmp, Fixed& v) {
    static const std::pair<const char*, TrigCmp> ops[] = {{">=", TrigCmp::Ge}, {">", TrigCmp::Gt}, {"<=", TrigCmp::Le}, {"<", TrigCmp::Lt}, {"==", TrigCmp::Eq}, {"!=", TrigCmp::Ne}};
    for (auto& [k, c] : ops) if (j.has(k)) { cmp = c; v = j[k].fx(); return true; }
    if (j.has("value")) { cmp = TrigCmp::Ge; v = j.getFx("value"); return true; }
    return false;
}


struct Parser {
    const Rules& r;
    TriggerSet& set;
    std::string where;

    uint16_t type(const std::string& id) {
        int k = r.findType(id);
        if (k < 0) fail(where, "unknown type '" + id + "'");
        return uint16_t(k);
    }
    TrigFilter filter(const Json& j) {
        TrigFilter f;
        if (j.has("unit")) f.type = type(j.getStr("unit"));
        for (auto& t : j["tags"].items()) f.tags |= r.tagMask(t.str());
        std::string k = j.getStr("kind", "");
        f.structures = k == "structure";
        f.units = k == "unit";
        return f;
    }
    int area(const Json& j) {
        const Json& a = j["area"];
        if (a.isNull()) return -1;
        if (a.isString()) {
            int i = set.findArea(a.str());
            if (i < 0) fail(where, "unknown area '" + a.str() + "'");
            return i;
        }
        TrigArea ar{a[0].fx(), a[1].fx(), a[2].fx(Fixed::fromInt(1)), a[3].fx(Fixed::fromInt(1))};
        set.areas.push_back(ar);
        set.areaNames.push_back("");
        return int(set.areas.size() - 1);
    }
    int16_t resource(const std::string& id) {
        int k = r.findResource(id);
        if (k < 0) fail(where, "unknown resource '" + id + "'");
        return int16_t(k);
    }
    int16_t tech(const std::string& id) {
        int k = r.findTech(id);
        if (k < 0) fail(where, "unknown tech '" + id + "'");
        return int16_t(k);
    }

    TrigCondition condition(const Json& c) {
        TrigCondition o;
        std::string t = c.getStr("type", "count");
        o.negate = c.getBool("not", false);
        o.player = parsePlayer(c["player"]);
        o.filter = filter(c);
        o.area = area(c);
        parseCmp(c, o.cmp, o.value);
        if (t == "count") o.kind = TrigCondition::Count;
        else if (t == "resource") { o.kind = TrigCondition::Resource; o.index = resource(c.getStr("resource")); }
        else if (t == "variable") { o.kind = TrigCondition::Variable; o.name = c.getStr("name"); }
        else if (t == "researched") { o.kind = TrigCondition::Researched; o.index = tech(c.getStr("tech")); }
        else if (t == "random") { o.kind = TrigCondition::Random; o.value = c.getFx("chance", Fixed::fromRatio(1, 2)); }
        else if (t == "time") o.kind = TrigCondition::Time;
        else if (t == "alive") o.kind = TrigCondition::Alive;
        else if (t == "regions") o.kind = TrigCondition::RegionsOwned;
        else if (t == "script") {
            o.kind = TrigCondition::Script; o.name = c.getStr("fn"); o.args = c;
            if (o.name.empty()) fail(where, "script condition needs 'fn'");
        }
        else fail(where, "unknown condition '" + t + "'");
        return o;
    }

    TrigAction action(const Json& a) {
        TrigAction o;
        std::string t = a.getStr("type", "");
        static const std::pair<const char*, TrigAction::Kind> kinds[] = {
            {"spawn", TrigAction::Spawn}, {"give", TrigAction::Give}, {"take", TrigAction::Take}, {"setOwner", TrigAction::SetOwner},
            {"kill", TrigAction::Kill}, {"remove", TrigAction::Remove}, {"damage", TrigAction::Damage}, {"heal", TrigAction::Heal},
            {"buff", TrigAction::Buff}, {"message", TrigAction::Message}, {"setVariable", TrigAction::SetVariable},
            {"addVariable", TrigAction::AddVariable}, {"research", TrigAction::Research}, {"enableType", TrigAction::EnableType},
            {"disableType", TrigAction::DisableType}, {"victory", TrigAction::Victory}, {"defeat", TrigAction::Defeat},
            {"enableTrigger", TrigAction::EnableTrigger}, {"disableTrigger", TrigAction::DisableTrigger}, {"addScore", TrigAction::AddScore},
            {"setRegionOwner", TrigAction::SetRegionOwner}, {"attackMove", TrigAction::AttackMove}, {"move", TrigAction::MoveUnits},
            {"setTickets", TrigAction::SetTickets}, {"castAbility", TrigAction::CastAbility}, {"script", TrigAction::Script},
        };
        bool known = false;
        for (auto& [k, v] : kinds) if (t == k) { o.kind = v; known = true; }
        if (!known) fail(where, "unknown action '" + t + "'");
        o.player = parsePlayer(a["player"]);
        o.filter = filter(a);
        o.area = area(a);
        o.triggeringUnit = a.getStr("target", "") == "triggeringUnit";
        if (a.has("unit")) o.type = type(a.getStr("unit"));
        o.count = int(a.getInt("count", 1));
        o.amount = a.getFx("amount", a.getFx("value"));
        o.duration = a.getFx("duration");
        o.text = a.getStr("text");
        o.name = a.getStr("name", a.getStr("trigger", a.getStr("region")));
        o.attackMove = a.getBool("attackMove", false);
        if (a.has("at")) { o.at = {a["at"][0].fx(), Fixed{}, a["at"][1].fx()}; o.hasAt = true; }
        if (a.has("resource")) o.index = resource(a.getStr("resource"));
        if (a.has("tech")) o.index = tech(a.getStr("tech"));
        if (o.kind == TrigAction::Script) {
            o.name = a.getStr("fn"); o.args = a;
            if (o.name.empty()) fail(where, "script action needs 'fn'");
        }
        if (o.kind == TrigAction::CastAbility) {
            int k = r.findPlayerAbility(a.getStr("ability"));
            if (k < 0) fail(where, "unknown player ability '" + a.getStr("ability") + "'");
            o.index = int16_t(k);
            o.attackMove = a.getBool("free", true);   // reused: free casts skip cost / cooldown / loadout
        }
        if (o.kind == TrigAction::Buff) {
            bool ok = false;
            o.mod.stat = parseStat(a.getStr("stat", "damage"), &ok);
            if (!ok) fail(where, "unknown stat '" + a.getStr("stat") + "'");
            o.mod.add = a.getFx("add");
            o.mod.mul = a.getFx("mul", Fixed::fromInt(1));
        }
        return o;
    }

    TriggerDef def(const Json& j, size_t n) {
        TriggerDef d;
        d.id = j.getStr("id", "trigger" + std::to_string(n));
        where = "'" + d.id + "'";
        d.once = j.getBool("once", true);
        d.enabled = j.getBool("enabled", true);
        d.forEachPlayer = j.getBool("forEachPlayer", false);
        const Json& e = j["event"];
        std::string t = e.isString() ? e.str() : e.getStr("type", "start");
        static const std::pair<const char*, TrigEvent> evs[] = {
            {"start", TrigEvent::Start}, {"time", TrigEvent::Time}, {"every", TrigEvent::Every}, {"enterArea", TrigEvent::EnterArea},
            {"leaveArea", TrigEvent::LeaveArea}, {"unitDied", TrigEvent::UnitDied}, {"built", TrigEvent::Built}, {"trained", TrigEvent::Trained},
            {"researched", TrigEvent::Researched}, {"captured", TrigEvent::Captured}, {"resource", TrigEvent::Resource},
            {"variable", TrigEvent::Variable}, {"wave", TrigEvent::Wave}, {"regionClaimed", TrigEvent::RegionClaimed},
            {"playerDefeated", TrigEvent::PlayerDefeated}, {"ability", TrigEvent::Ability}, {"levelUp", TrigEvent::LevelUp},
        };
        bool known = false;
        for (auto& [k, v] : evs) if (t == k) { d.event = v; known = true; }
        if (!known) fail(where, "unknown event '" + t + "'");
        if (e.isObject()) {
            d.seconds = e.getFx("seconds");
            d.area = area(e);
            d.filter = filter(e);
            d.player = parsePlayer(e["player"], 1);
            if (e.has("resource")) d.index = resource(e.getStr("resource"));
            if (e.has("tech")) d.index = tech(e.getStr("tech"));
            d.name = e.getStr("name");
            parseCmp(e, d.cmp, d.value);
        } else {
            d.player.rel = 1;
        }
        if ((d.event == TrigEvent::Time || d.event == TrigEvent::Every) && d.seconds.raw <= 0 && d.event == TrigEvent::Every) fail(where, "'every' needs seconds > 0");
        if ((d.event == TrigEvent::EnterArea || d.event == TrigEvent::LeaveArea) && d.area < 0) fail(where, "area event needs an area");
        // repeating events default to repeatable triggers
        if (!j.has("once") && (d.event == TrigEvent::Every || d.event == TrigEvent::UnitDied || d.event == TrigEvent::Trained ||
                               d.event == TrigEvent::Built || d.event == TrigEvent::EnterArea || d.event == TrigEvent::LeaveArea)) d.once = false;
        for (auto& c : j["conditions"].items()) d.conditions.push_back(condition(c));
        for (auto& a : j["actions"].items()) d.actions.push_back(action(a));
        return d;
    }
};
} // namespace

void loadTriggers(const Rules& rules, const Json& rulesJson, const Json& mapJson, TriggerSet& out) {
    out = {};
    for (auto& [name, a] : mapJson["areas"].members()) {
        out.areaNames.push_back(name);
        out.areas.push_back({a[0].fx(), a[1].fx(), a[2].fx(Fixed::fromInt(1)), a[3].fx(Fixed::fromInt(1))});
    }
    Parser p{rules, out, ""};
    size_t n = 0;
    for (auto& t : rulesJson["triggers"].items()) out.defs.push_back(p.def(t, n++));
    for (auto& t : mapJson["triggers"].items()) out.defs.push_back(p.def(t, n++));
    // trigger references are checked after all are known
    for (auto& d : out.defs)
        for (auto& a : d.actions)
            if ((a.kind == TrigAction::EnableTrigger || a.kind == TrigAction::DisableTrigger) && out.findTrigger(a.name) < 0)
                throw std::runtime_error("trigger '" + d.id + "': unknown trigger '" + a.name + "'");
}

// runtime: modules/rts/lua/triggers.lua (standard Lua library, std.triggers)

} // namespace rts::game
