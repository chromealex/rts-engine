#include "RtsOverlayData.h"
#include "../../../engine/platform/Platform.h"
#include "../RtsModule.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace rts::game {

using render::float4;

namespace {

[[noreturn]] void fail(const std::string& where, const std::string& msg) { throw std::runtime_error(where + ": " + msg); }

std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower((unsigned char)c)); return s; }

void onlyKeys(const Json& o, std::initializer_list<const char*> keys, const std::string& where) {
    for (auto& [k, v] : o.members()) {
        (void)v;
        bool ok = false;
        for (const char* key : keys) if (k == key) { ok = true; break; }
        if (!ok) {
            std::string list;
            for (const char* key : keys) { if (!list.empty()) list += ", "; list += key; }
            fail(where, "unknown key '" + k + "' (" + list + ")");
        }
    }
}

float number(const Json& j, const std::string& where) {
    if (!j.isNumber()) fail(where, "expected a number");
    return float(j.toDouble());
}
bool boolean(const Json& j, const std::string& where) {
    if (j.type() != Json::Type::Bool) fail(where, "expected true or false");
    return j.boolean();
}
std::vector<std::string> strings(const Json& j, const std::string& where) {
    std::vector<std::string> out;
    if (j.isString()) out.push_back(j.str());
    else if (j.isArray()) for (auto& e : j.items()) { if (!e.isString()) fail(where, "expected strings"); out.push_back(e.str()); }
    else fail(where, "expected a string or a list of strings");
    return out;
}
void pair2(const Json& j, float out[2], const std::string& where) {
    if (j.isNumber()) { out[0] = out[1] = number(j, where); return; }
    if (!j.isArray() || j.size() != 2) fail(where, "expected [a, b]");
    out[0] = number(j[size_t(0)], where); out[1] = number(j[size_t(1)], where);
}

// "barracks", "tag:defense", ["a", "tag:b"]
void parseTypes(const Rules& R, const Json& j, OvFilter& f, const std::string& where) {
    for (auto& s : strings(j, where)) {
        if (s.rfind("tag:", 0) == 0) {
            uint64_t m = R.tagMask(s.substr(4));
            if (!m) fail(where, "unknown tag '" + s.substr(4) + "'");
            f.tags |= m;
        } else {
            int t = R.findType(s);
            if (t < 0) fail(where, "unknown type '" + s + "'");
            f.types.push_back(uint16_t(t));
        }
    }
}
void parseTags(const Rules& R, const Json& j, OvFilter& f, const std::string& where) {
    for (auto& s : strings(j, where)) {
        uint64_t m = R.tagMask(s);
        if (!m) fail(where, "unknown tag '" + s + "'");
        f.tags |= m;
    }
}

uint8_t parseRelations(const Json& j, const std::string& where) {
    uint8_t m = 0;
    for (auto& s0 : strings(j, where)) {
        std::string s = lower(s0);
        if (s == "own" || s == "mine" || s == "self") m |= kRelOwn;
        else if (s == "ally" || s == "allied" || s == "allies") m |= kRelAlly;
        else if (s == "friendly") m |= kRelOwn | kRelAlly;
        else if (s == "enemy" || s == "enemies" || s == "hostile") m |= kRelEnemy;
        else if (s == "neutral") m |= kRelNeutral;
        else if (s == "any" || s == "all") m |= kRelOwn | kRelAlly | kRelEnemy | kRelNeutral;
        else fail(where, "unknown owner '" + s0 + "' (own, ally, friendly, enemy, neutral, any)");
    }
    return m;
}

struct FlagName { const char* name; uint32_t bit; };
const FlagName kFlags[] = {
    {"structure", kOvStructure}, {"unit", kOvUnit}, {"constructing", kOvConstructing}, {"damaged", kOvDamaged},
    {"garrison", kOvGarrison}, {"moving", kOvMoving}, {"producing", kOvProducing}, {"idle", kOvIdle},
    {"hero", kOvHero}, {"worker", kOvWorker}, {"invulnerable", kOvInvulnerable}, {"air", kOvAir},
    {"carrying", kOvCarrying}, {"attacking", kOvAttacking}, {"resource", kOvResource}, {"energy", kOvEnergy},
    {"ranged", kOvRanged}, {"aura", kOvAura},
};
void parseFlags(const Json& j, OvFilter& f, const std::string& where) {
    for (auto& s0 : strings(j, where)) {
        bool neg = !s0.empty() && s0[0] == '!';
        std::string s = lower(neg ? s0.substr(1) : s0);
        uint32_t bit = 0;
        for (auto& fl : kFlags) if (s == fl.name) bit = fl.bit;
        if (!bit) {
            std::string list;
            for (auto& fl : kFlags) { if (!list.empty()) list += ", "; list += fl.name; }
            fail(where, "unknown flag '" + s0 + "' (" + list + "; '!' negates)");
        }
        (neg ? f.flagsOff : f.flagsOn) |= bit;
    }
}

// One filter key; false when `k` is not a filter key.
bool filterKey(const Rules& R, const std::string& k, const Json& v, OvFilter& f, const std::string& where) {
    const std::string w = where + "." + k;
    if (k == "types" || k == "type") parseTypes(R, v, f, w);
    else if (k == "tags") parseTags(R, v, f, w);
    else if (k == "owner") f.relations |= parseRelations(v, w);
    else if (k == "flags") parseFlags(v, f, w);
    else if (k == "status") {
        for (auto& s : strings(v, w)) {
            int st = R.findStatus(s);
            if (st < 0) fail(w, "unknown status '" + s + "'");
            f.statuses.push_back(int16_t(st));
        }
    } else if (k == "hp") {
        float r[2];
        pair2(v, r, w);
        f.hpMin = r[0]; f.hpMax = r[1];
    } else return false;
    return true;
}
OvFilter parseFilter(const Rules& R, const Json& j, const std::string& where) {
    OvFilter f;
    if (j.isString() || j.isArray()) { parseTypes(R, j, f, where); return f; }
    if (!j.isObject()) fail(where, "expected an entity filter {types, tags, owner, flags, status, hp}");
    for (auto& [k, v] : j.members())
        if (!filterKey(R, k, v, f, where)) fail(where, "unknown key '" + k + "' (types, tags, owner, flags, status, hp)");
    return f;
}

struct KeyName { const char* name; uint16_t key; };
uint16_t keyCode(const std::string& s0, const std::string& where) {
    using platform::Key;
    std::string s = lower(s0);
    if (s.size() == 1 && s[0] >= 'a' && s[0] <= 'z') return uint16_t(int(Key::A) + (s[0] - 'a'));
    if (s.size() == 1 && s[0] >= '0' && s[0] <= '9') return uint16_t(int(Key::Num0) + (s[0] - '0'));
    if (s.size() >= 2 && s[0] == 'f' && std::isdigit((unsigned char)s[1])) {
        int n = std::atoi(s.c_str() + 1);
        if (n >= 1 && n <= 12) return uint16_t(int(Key::F1) + n - 1);
    }
    static const KeyName names[] = {
        {"shift", kOvKeyShift}, {"ctrl", kOvKeyCtrl}, {"control", kOvKeyCtrl}, {"alt", kOvKeyAlt}, {"option", kOvKeyAlt},
        {"super", kOvKeySuper}, {"cmd", kOvKeySuper}, {"space", uint16_t(Key::Space)}, {"tab", uint16_t(Key::Tab)},
        {"enter", uint16_t(Key::Enter)}, {"escape", uint16_t(Key::Escape)}, {"backspace", uint16_t(Key::Backspace)},
        {"left", uint16_t(Key::Left)}, {"right", uint16_t(Key::Right)}, {"up", uint16_t(Key::Up)}, {"down", uint16_t(Key::Down)},
        {"leftshift", uint16_t(Key::LeftShift)}, {"rightshift", uint16_t(Key::RightShift)}, {"leftctrl", uint16_t(Key::LeftCtrl)},
        {"rightctrl", uint16_t(Key::RightCtrl)}, {"leftalt", uint16_t(Key::LeftAlt)}, {"rightalt", uint16_t(Key::RightAlt)},
        {"grave", uint16_t(Key::GraveAccent)}, {"`", uint16_t(Key::GraveAccent)},
    };
    for (auto& k : names) if (s == k.name) return k.key;
    fail(where, "unknown key '" + s0 + "' (A-Z, 0-9, F1-F12, Shift, Ctrl, Alt, Super, Space, Tab, ...)");
}

OvCondition single(OvCondition::Kind k) { OvCondition c; c.kind = k; return c; }

OvCondition parseCond(const Rules& R, const Json& j, const std::string& where);

OvCondition condFromString(const std::string& s0, const std::string& where) {
    std::string s = lower(s0);
    if (s == "always") return single(OvCondition::Always);
    if (s == "never") return single(OvCondition::Never);
    if (s == "placing") return single(OvCondition::Placing);
    if (s == "selected") return single(OvCondition::Selected);
    if (s == "hovered") return single(OvCondition::Hovered);
    if (s == "cursor") return single(OvCondition::Cursor);
    OvCondition c = single(OvCondition::State);
    if (s == "playing") c.states = 1;
    else if (s == "paused") c.states = 2;
    else if (s == "over") c.states = 4;
    else fail(where, "unknown condition '" + s0 + "' (always, never, placing, selected, hovered, cursor, playing, paused, over or an object)");
    return c;
}

OvCondition parseCond(const Rules& R, const Json& j, const std::string& where) {
    if (j.isNull()) return single(OvCondition::Always);
    if (j.type() == Json::Type::Bool) return single(j.boolean() ? OvCondition::Always : OvCondition::Never);
    if (j.isString()) return condFromString(j.str(), where);
    OvCondition all = single(OvCondition::All);
    if (j.isArray()) {
        for (size_t i = 0; i < j.size(); ++i) all.kids.push_back(parseCond(R, j[i], where + "[" + std::to_string(i) + "]"));
        return all;
    }
    if (!j.isObject()) fail(where, "expected a condition (string, list or object)");
    OvCondition ent = single(OvCondition::Entity);
    bool hasEnt = false;
    for (auto& [k, v] : j.members()) {
        const std::string w = where + "." + k;
        if (filterKey(R, k, v, ent.filter, where)) { hasEnt = true; continue; }
        OvCondition c;
        if (k == "always") c = single(boolean(v, w) ? OvCondition::Always : OvCondition::Never);
        else if (k == "placing") {
            c = single(OvCondition::Placing);
            if (v.type() == Json::Type::Bool) { if (!v.boolean()) { OvCondition n = single(OvCondition::Not); n.kids.push_back(c); c = n; } }
            else parseTypes(R, v, c.filter, w);
        } else if (k == "selected" || k == "hovered") {
            c = single(k == "selected" ? OvCondition::Selected : OvCondition::Hovered);
            if (v.type() == Json::Type::Bool) c.want = v.boolean();
            else c.filter = parseFilter(R, v, w);
        } else if (k == "entity") { c = single(OvCondition::Entity); c.filter = parseFilter(R, v, w); }
        else if (k == "key" || k == "keys") { c = single(OvCondition::Key); for (auto& s : strings(v, w)) c.keys.push_back(keyCode(s, w)); }
        else if (k == "state") {
            c = single(OvCondition::State);
            for (auto& s : strings(v, w)) c.states |= condFromString(s, w).states;
            if (!c.states) fail(w, "expected playing, paused or over");
        } else if (k == "var" || k == "flag") {
            c = single(OvCondition::Var);
            if (v.isString()) c.names.push_back(v.str());
            else if (v.isObject()) {
                static const char* ops[] = {"", "==", "!=", "<", "<=", ">", ">="};
                for (auto& [vk, vv] : v.members()) {
                    if (vk == "name") { if (!vv.isString()) fail(w, "name: expected a string"); c.names.push_back(vv.str()); continue; }
                    bool found = false;
                    for (int o = 1; o < 7; ++o) if (vk == ops[o]) { c.cmp = uint8_t(o); c.value = number(vv, w + "." + vk); found = true; }
                    if (!found) fail(w, "unknown key '" + vk + "' (name, ==, !=, <, <=, >, >=)");
                }
                if (c.names.empty()) fail(w, "needs \"name\"");
            } else fail(w, "expected a variable name or {\"name\": ..., \">=\": 1}");
        } else if (k == "zoom") {
            c = single(OvCondition::Zoom);
            float r[2];
            pair2(v, r, w);
            c.lo = r[0]; c.hi = r[1];
        } else if (k == "cursor") { c = single(OvCondition::Cursor); c.want = boolean(v, w); }
        else if (k == "mode") {
            c = single(OvCondition::Mode);
            static const char* modes[] = {"normal", "attack", "patrol", "place", "rally", "ability", "claim"};
            for (auto& s : strings(v, w)) {
                bool ok = false;
                for (const char* m : modes) ok = ok || lower(s) == m;
                if (!ok) fail(w, "unknown mode '" + s + "' (normal, attack, patrol, place, rally, ability, claim)");
                c.names.push_back(lower(s));
            }
        } else if (k == "any" || k == "all") {
            c = single(k == "any" ? OvCondition::Any : OvCondition::All);
            if (!v.isArray()) fail(w, "expected a list of conditions");
            for (size_t i = 0; i < v.size(); ++i) c.kids.push_back(parseCond(R, v[i], w + "[" + std::to_string(i) + "]"));
        } else if (k == "not") { c = single(OvCondition::Not); c.kids.push_back(parseCond(R, v, w)); }
        else fail(where, "unknown key '" + k + "' (always, placing, selected, hovered, entity, types, tags, owner, flags, status, hp, "
                         "key, state, var, flag, zoom, cursor, mode, any, all, not)");
        all.kids.push_back(std::move(c));
    }
    if (hasEnt) all.kids.push_back(std::move(ent));
    if (all.kids.size() == 1) return all.kids[0];
    if (all.kids.empty()) return single(OvCondition::Always);
    return all;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = char(std::tolower((unsigned char)c));
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

float4 colorKeyed(const Json& v, const std::string& w) { return parseOverlayColor(v, w); }

OvShape shapeOf(const std::string& s0, const std::string& where) {
    static const std::pair<const char*, OvShape> names[] = {
        {"grid", OvShape::Grid}, {"circle", OvShape::Circle}, {"disc", OvShape::Circle}, {"ring", OvShape::Ring},
        {"rect", OvShape::Rect}, {"footprint", OvShape::Footprint}, {"line", OvShape::Line}, {"arrow", OvShape::Arrow},
        {"path", OvShape::Path}, {"cells", OvShape::Cells}, {"text", OvShape::Text}, {"label", OvShape::Text},
        {"icon", OvShape::Icon}, {"image", OvShape::Icon}, {"decal", OvShape::Icon}, {"bar", OvShape::Bar}, {"bars", OvShape::Bar},
    };
    for (auto& [n, s] : names) if (lower(s0) == n) return s;
    fail(where, "unknown shape '" + s0 + "' (grid, circle, ring, rect, footprint, line, arrow, path, cells, text, icon, bar)");
}

OvPred predOf(const std::string& s0, const std::string& where) {
    static const std::pair<const char*, OvPred> names[] = {
        {"ok", OvPred::Ok}, {"placeable", OvPred::Ok}, {"buildable", OvPred::Buildable}, {"blocked", OvPred::Blocked},
        {"buildRadius", OvPred::BuildRadius}, {"walkable", OvPred::Walkable}, {"water", OvPred::Water}, {"resource", OvPred::Resource},
        {"creep", OvPred::Creep}, {"road", OvPred::Road}, {"region", OvPred::Region}, {"territory", OvPred::Territory},
        {"enemyTerritory", OvPred::EnemyTerritory}, {"anyTerritory", OvPred::AnyTerritory}, {"territoryEdge", OvPred::TerritoryEdge},
        {"visible", OvPred::Visible}, {"explored", OvPred::Explored}, {"fogged", OvPred::Fogged}, {"unexplored", OvPred::Unexplored},
    };
    for (auto& [n, p] : names) if (s0 == n || lower(s0) == lower(n)) return p;
    fail(where, "unknown predicate '" + s0 + "' (ok, buildable, blocked, buildRadius, walkable, water, resource, creep, road, region, "
                "territory, enemyTerritory, anyTerritory, territoryEdge, visible, explored, fogged, unexplored)");
}

void parseRadius(const Json& v, OverlayDef& d, const std::string& w) {
    if (v.isNumber()) { d.radiusFrom = OvRadius::Fixed; d.radius = number(v, w); return; }
    if (!v.isString()) fail(w, "expected a number or a stat name");
    std::string s = v.str();
    size_t colon = s.find(':');
    if (colon != std::string::npos) { d.index = std::atoi(s.c_str() + colon + 1); s = s.substr(0, colon); }
    static const std::pair<const char*, OvRadius> names[] = {
        {"range", OvRadius::Range}, {"minRange", OvRadius::MinRange}, {"sight", OvRadius::Sight}, {"aura", OvRadius::Aura},
        {"buildRadius", OvRadius::BuildRadius}, {"selection", OvRadius::Selection}, {"capture", OvRadius::Capture},
        {"claim", OvRadius::Claim}, {"footprint", OvRadius::Footprint}, {"ability", OvRadius::Ability}, {"leash", OvRadius::Leash},
    };
    for (auto& [n, r] : names) if (s == n) { d.radiusFrom = r; return; }
    fail(w, "unknown radius '" + v.str() + "' (a number, range[:weapon], minRange, sight, aura, buildRadius, selection, capture, claim, "
            "footprint, ability:N, leash)");
}

void parseAnchor(const Rules& R, const Json& j, OverlayDef& d, const std::string& where) {
    std::string type = j.isString() ? j.str() : j.isObject() ? j.getStr("type") : "";
    if (type.empty()) fail(where, "expected a name or {\"type\": ...}");
    static const std::pair<const char*, OvAnchor> names[] = {
        {"cursor", OvAnchor::Cursor}, {"selected", OvAnchor::Selected}, {"hovered", OvAnchor::Hovered}, {"entities", OvAnchor::Entities},
        {"ghost", OvAnchor::Ghost}, {"area", OvAnchor::Area}, {"rect", OvAnchor::Area}, {"point", OvAnchor::Point},
        {"screen", OvAnchor::Screen}, {"orders", OvAnchor::Orders}, {"route", OvAnchor::Route}, {"path", OvAnchor::Route},
        {"rally", OvAnchor::Rally}, {"markers", OvAnchor::Markers}, {"map", OvAnchor::Map},
    };
    bool found = false;
    for (auto& [n, a] : names) if (lower(type) == n) { d.anchor = a; found = true; }
    if (!found) fail(where, "unknown anchor '" + type + "' (cursor, selected, hovered, entities, ghost, area, rect, point, screen, orders, route, rally, markers, map)");
    if (!j.isObject()) {
        if (d.anchor == OvAnchor::Area || d.anchor == OvAnchor::Point || d.anchor == OvAnchor::Screen)
            fail(where, "'" + type + "' needs coordinates: {\"type\": \"" + type + "\", ...}");
        return;
    }
    for (auto& [k, v] : j.members()) {
        const std::string w = where + "." + k;
        if (k == "type") continue;
        if (filterKey(R, k, v, d.anchorFilter, where)) continue;
        if (k == "snap") d.snap = boolean(v, w);
        else if (k == "area" || k == "name") { if (!v.isString()) fail(w, "expected an area name"); d.areaName = v.str(); }
        else if (k == "rect") {
            if (!v.isArray() || v.size() != 4) fail(w, "expected [x, z, w, h]");
            for (size_t i = 0; i < 4; ++i) d.rect[i] = number(v[i], w);
        } else if (k == "at" || k == "point") pair2(v, d.point, w);
        else if (k == "x") d.point[0] = number(v, w);
        else if (k == "z" || k == "y") d.point[1] = number(v, w);
        else fail(where, "unknown key '" + k + "' (type, types, tags, owner, flags, status, hp, snap, area, rect, at, x, z / y)");
    }
    if (d.anchor == OvAnchor::Area && d.areaName.empty() && d.rect[2] <= 0) fail(where, "area needs \"area\": name or \"rect\": [x, z, w, h]");
    if (!(d.anchor == OvAnchor::Entities || d.anchor == OvAnchor::Selected || d.anchor == OvAnchor::Hovered) &&
        (!d.anchorFilter.types.empty() || d.anchorFilter.tags || d.anchorFilter.relations || d.anchorFilter.flagsOn ||
         d.anchorFilter.flagsOff || !d.anchorFilter.statuses.empty()))
        fail(where, "entity filters only apply to the entities / selected / hovered anchors");
}

void shapeDefaults(OverlayDef& d) {
    OvStyle& s = d.style;
    switch (d.shape) {
    case OvShape::Circle: s.fill = true; s.outline = false; break;
    case OvShape::Grid: case OvShape::Cells: s.fill = true; s.outline = false; break;
    case OvShape::Text: case OvShape::Bar: s.space = OvSpace::Screen; break;
    case OvShape::Icon: s.fill = true; s.outline = false; break;
    default: break;
    }
}

void parseShape(const Json& j, OverlayDef& d, const std::string& where) {
    if (j.isString()) { d.shape = shapeOf(j.str(), where); shapeDefaults(d); return; }
    if (!j.isObject() || !j["type"].isString()) fail(where, "expected a shape name or {\"type\": ...}");
    d.shape = shapeOf(j.getStr("type"), where + ".type");
    shapeDefaults(d);
    onlyKeys(j, {"type", "radius", "radiusAdd", "radiusScale", "size", "margin", "square", "predicate", "text", "image", "value",
                 "width", "widthStructure", "height", "to", "head", "highlightFootprint", "index"}, where);
    for (auto& [k, v] : j.members()) {
        const std::string w = where + "." + k;
        if (k == "type") continue;
        if (k == "radius") parseRadius(v, d, w);
        else if (k == "radiusAdd") d.radiusAdd = number(v, w);
        else if (k == "radiusScale") d.radiusScale = number(v, w);
        else if (k == "size") pair2(v, d.size, w);
        else if (k == "margin") d.margin = number(v, w);
        else if (k == "square") d.square = boolean(v, w);
        else if (k == "highlightFootprint") d.highlightFootprint = boolean(v, w);
        else if (k == "predicate") { if (!v.isString()) fail(w, "expected a predicate name"); d.pred = predOf(v.str(), w); }
        else if (k == "text") { if (!v.isString()) fail(w, "expected a string"); d.text = v.str(); }
        else if (k == "image") { if (!v.isString()) fail(w, "expected a texture path"); d.image = v.str(); }
        else if (k == "value") {
            static const std::pair<const char*, OvValue> names[] = {{"hp", OvValue::Hp}, {"energy", OvValue::Energy}, {"build", OvValue::Build},
                {"garrison", OvValue::Garrison}, {"production", OvValue::Production}, {"carry", OvValue::Carry}, {"capture", OvValue::Capture}};
            bool ok = false;
            for (auto& [n, val] : names) if (v.str() == n) { d.value = val; ok = true; }
            if (!ok) fail(w, "unknown value '" + v.str() + "' (hp, energy, build, garrison, production, carry, capture)");
        } else if (k == "width") { d.barW = number(v, w); d.barWStructure = d.barW; }
        else if (k == "widthStructure") d.barWStructure = number(v, w);
        else if (k == "height") d.barH = number(v, w);
        else if (k == "head") d.head = number(v, w);
        else if (k == "index") d.index = int(number(v, w));
        else if (k == "to") {
            if (v.isArray()) { pair2(v, d.toPoint, w); d.to = OvTo::Point; }
            else {
                std::string s = v.str();
                if (s == "target") d.to = OvTo::Target;
                else if (s == "rally") d.to = OvTo::Rally;
                else if (s == "cursor") d.to = OvTo::Cursor;
                else if (s == "order") d.to = OvTo::Order;
                else fail(w, "expected target, rally, cursor, order or [x, z]");
            }
        }
    }
}

void parseStyle(const Json& j, OverlayDef& d, const std::string& where) {
    if (!j.isObject()) fail(where, "expected an object");
    OvStyle& s = d.style;
    for (auto& [k, v] : j.members()) {
        const std::string w = where + "." + k;
        if (k == "color") { bool pl = false; s.color = parseOverlayColor(v, w, &pl); s.playerColor = pl; }
        else if (k == "colors") {
            if (!v.isObject()) fail(w, "expected {\"true\": colour, \"false\": colour, \"own\": ..., \"enemy\": ...}");
            for (auto& [ck, cv] : v.members()) {
                int key = -1;
                if (ck == "true" || ck == "ok" || ck == "yes") key = kColTrue;
                else if (ck == "false" || ck == "bad" || ck == "no") key = kColFalse;
                else if (ck == "own") key = kColOwn;
                else if (ck == "ally") key = kColAlly;
                else if (ck == "enemy") key = kColEnemy;
                else if (ck == "neutral") key = kColNeutral;
                else fail(w, "unknown key '" + ck + "' (true, false, own, ally, enemy, neutral)");
                s.colors[key] = colorKeyed(cv, w + "." + ck);
                s.hasColor |= uint8_t(1u << key);
            }
        } else if (k == "stops") {
            if (!v.isArray()) fail(w, "expected [[value, colour], ...] (first stop >= value wins)");
            for (auto& st : v.items()) {
                if (!st.isArray() || st.size() != 2) fail(w, "expected [value, colour]");
                s.stops.push_back({number(st[size_t(0)], w), parseOverlayColor(st[size_t(1)], w)});
            }
            std::sort(s.stops.begin(), s.stops.end(), [](auto& a, auto& b) { return a.first < b.first; });
        }
        else if (k == "fillColor") { s.fillColor = parseOverlayColor(v, w); s.hasFill = true; }
        else if (k == "lineColor") { s.lineColor = parseOverlayColor(v, w); s.hasLine = true; }
        else if (k == "background") {
            if (v.type() == Json::Type::Bool && !v.boolean()) { s.hasBackground = false; continue; }
            bool pl = false;
            s.background = parseOverlayColor(v, w, &pl);
            s.backgroundPlayer = pl; s.hasBackground = true;
        }
        else if (k == "backgroundShade") s.backgroundShade = number(v, w);
        else if (k == "backgroundAlpha") s.backgroundAlpha = number(v, w);
        else if (k == "alpha") s.alpha = number(v, w);
        else if (k == "thickness" || k == "width") s.thickness = number(v, w);
        else if (k == "dash") {
            if (v.type() == Json::Type::Bool) { s.dashOn = v.boolean() ? 0.5f : 0; s.dashOff = s.dashOn * 0.6f; }
            else { float p[2]; pair2(v, p, w); s.dashOn = p[0]; s.dashOff = p[1]; }
        }
        else if (k == "scroll") s.scroll = number(v, w);
        else if (k == "pulse") s.pulse = v.type() == Json::Type::Bool ? (v.boolean() ? 1.0f : 0.0f) : number(v, w);
        else if (k == "pulseMin") s.pulseMin = number(v, w);
        else if (k == "rotate") s.rotate = number(v, w);
        else if (k == "fadeIn") s.fadeIn = number(v, w);
        else if (k == "fadeOut") s.fadeOut = number(v, w);
        else if (k == "fade") s.fadeIn = s.fadeOut = number(v, w);
        else if (k == "lift") s.lift = number(v, w);
        else if (k == "rounding") s.rounding = number(v, w);
        else if (k == "padding") { float p[2]; pair2(v, p, w); s.padX = p[0]; s.padY = p[1]; }
        else if (k == "border") s.border = number(v, w);
        else if (k == "offset") { float p[2]; pair2(v, p, w); s.offsetX = p[0]; s.offsetY = p[1]; }
        else if (k == "segments") s.segments = std::clamp(int(number(v, w)), 3, 128);
        else if (k == "fill") s.fill = boolean(v, w);
        else if (k == "outline") s.outline = boolean(v, w);
        else if (k == "onTop") s.onTop = boolean(v, w);
        else if (k == "depthTest") s.onTop = !boolean(v, w);
        else if (k == "additive") s.additive = boolean(v, w);
        else if (k == "blend") {
            if (v.str() == "add" || v.str() == "additive") s.additive = true;
            else if (v.str() == "alpha") s.additive = false;
            else fail(w, "expected alpha or add");
        }
        else if (k == "shadow") s.shadow = boolean(v, w);
        else if (k == "falloff") s.falloff = boolean(v, w);
        else if (k == "outerAlpha") s.outerAlpha = number(v, w);
        else if (k == "space") {
            std::string sp = v.str();
            if (sp == "ground") s.space = OvSpace::Ground;
            else if (sp == "billboard") s.space = OvSpace::Billboard;
            else if (sp == "screen") s.space = OvSpace::Screen;
            else if (sp == "minimap") s.space = OvSpace::Minimap;
            else fail(w, "expected ground, billboard, screen or minimap");
        } else if (k == "ground") {
            std::string g = v.str();
            if (g == "follow") s.ground = OvGround::Follow;
            else if (g == "flat") s.ground = OvGround::Flat;
            else if (g == "zero") s.ground = OvGround::Zero;
            else fail(w, "expected follow (drape over the terrain), flat (height under the anchor) or zero");
        } else fail(where, "unknown key '" + k + "' (color, colors, stops, fillColor, lineColor, background, backgroundShade, backgroundAlpha, "
                           "alpha, thickness, dash, scroll, pulse, pulseMin, rotate, fadeIn, fadeOut, fade, lift, rounding, padding, border, "
                           "offset, segments, fill, outline, onTop, additive, blend, shadow, falloff, outerAlpha, space, ground)");
    }
}

void parseEvent(const Rules& R, const Json& j, OverlayDef& d, const std::string& where) {
    if (j.isString()) {
        uint16_t k = overlayEventKind(j.str());
        if (!k) fail(where, "unknown event '" + j.str() + "'");
        d.event.kinds.push_back(k);
        d.hasEvent = true;
        return;
    }
    if (!j.isObject()) fail(where, "expected an event name or {\"on\": ...}");
    onlyKeys(j, {"on", "lifetime", "rise", "grow", "spread", "by", "owner", "types", "max"}, where);
    if (!j.has("on")) fail(where, "needs \"on\": event name(s)");
    for (auto& s : strings(j["on"], where + ".on")) {
        uint16_t k = overlayEventKind(s);
        if (!k) fail(where + ".on", "unknown event '" + s + "' (hit, died, spawned, attack, gathered, delivered, produced, ability, cast, "
                                    "impact, miss, built, placed, trained, researched, levelup, captured, sold, upgraded, alert, status, "
                                    "area, claimed, wave, message, ping)");
        d.event.kinds.push_back(k);
    }
    for (auto& [k, v] : j.members()) {
        const std::string w = where + "." + k;
        if (k == "lifetime") d.event.lifetime = std::max(0.05f, number(v, w));
        else if (k == "rise") d.event.rise = number(v, w);
        else if (k == "grow") d.event.grow = number(v, w);
        else if (k == "spread") d.event.spread = number(v, w);
        else if (k == "by") d.event.by = parseRelations(v, w);
        else if (k == "owner") d.event.on = parseRelations(v, w);
        else if (k == "types") parseTypes(R, v, d.event.types, w);
        else if (k == "max") d.event.max = std::clamp(int(number(v, w)), 1, 512);
    }
    d.hasEvent = true;
}

bool entityAnchor(OvAnchor a) { return a == OvAnchor::Selected || a == OvAnchor::Hovered || a == OvAnchor::Entities; }

void deepMerge(Json& dst, const Json& src) {
    if (!src.isObject() || !dst.isObject()) { dst = src; return; }
    for (auto& [k, v] : src.members()) {
        if (Json* d = dst.find(k)) deepMerge(*d, v);
        else dst.set(k, v);
    }
}

} // namespace

bool OvCondition::entityScoped() const {
    switch (kind) {
    case Selected: case Hovered: case Entity: return true;
    case All: case Any: case Not: for (auto& k : kids) if (k.entityScoped()) return true; return false;
    default: return false;
    }
}

render::float4 overlayPlayerColor(int p) {
    static const float4 colors[] = {
        {0.70f, 0.70f, 0.68f, 1}, // neutral
        {0.20f, 0.45f, 0.95f, 1}, // 1 blue
        {0.92f, 0.22f, 0.20f, 1}, // 2 red
        {0.25f, 0.80f, 0.30f, 1}, // 3 green
        {0.97f, 0.80f, 0.15f, 1}, // 4 yellow
        {0.68f, 0.35f, 0.90f, 1}, // 5 purple
        {0.15f, 0.82f, 0.85f, 1}, // 6 teal
        {0.98f, 0.55f, 0.15f, 1}, // 7 orange
        {0.95f, 0.45f, 0.72f, 1}, // 8 pink
    };
    return colors[p >= 0 && p < 9 ? p : 0];
}

render::float4 parseOverlayColor(const Json& j, const std::string& where, bool* player) {
    if (player) *player = false;
    if (j.isArray()) {
        if (j.size() != 3 && j.size() != 4) fail(where, "expected [r, g, b] or [r, g, b, a]");
        float c[4] = {1, 1, 1, 1};
        bool bytes = false;
        for (size_t i = 0; i < j.size(); ++i) { c[i] = number(j[i], where); bytes = bytes || c[i] > 1.0f; }
        if (bytes) for (size_t i = 0; i < j.size(); ++i) c[i] /= 255.0f;
        return {c[0], c[1], c[2], c[3]};
    }
    if (!j.isString()) fail(where, "expected a colour: [r, g, b, a] (0..1), \"#rrggbb\", \"#rrggbbaa\", a name or \"player\"");
    std::string s = lower(j.str());
    if (s == "player" || s == "owner") {
        if (!player) fail(where, "\"player\" colour is not allowed here");
        *player = true;
        return {1, 1, 1, 1};
    }
    if (!s.empty() && s[0] == '#') {
        std::string h = s.substr(1);
        if (h.size() == 3 || h.size() == 4) { std::string e; for (char c : h) { e += c; e += c; } h = e; }
        if (h.size() != 6 && h.size() != 8) fail(where, "bad hex colour '" + j.str() + "'");
        float c[4] = {1, 1, 1, 1};
        for (size_t i = 0; i < h.size() / 2; ++i) {
            int a = hexDigit(h[2 * i]), b = hexDigit(h[2 * i + 1]);
            if (a < 0 || b < 0) fail(where, "bad hex colour '" + j.str() + "'");
            c[i] = float(a * 16 + b) / 255.0f;
        }
        return {c[0], c[1], c[2], c[3]};
    }
    static const std::pair<const char*, float4> names[] = {
        {"white", {1, 1, 1, 1}}, {"black", {0, 0, 0, 1}}, {"red", {0.95f, 0.25f, 0.2f, 1}}, {"green", {0.3f, 0.9f, 0.35f, 1}},
        {"blue", {0.25f, 0.5f, 1, 1}}, {"yellow", {1, 0.9f, 0.25f, 1}}, {"cyan", {0.3f, 0.9f, 1, 1}}, {"magenta", {0.9f, 0.3f, 0.9f, 1}},
        {"orange", {1, 0.6f, 0.15f, 1}}, {"gray", {0.6f, 0.6f, 0.6f, 1}}, {"grey", {0.6f, 0.6f, 0.6f, 1}}, {"gold", {1, 0.8f, 0.2f, 1}},
    };
    for (auto& [n, c] : names) if (s == n) return c;
    fail(where, "unknown colour '" + j.str() + "'");
}

namespace {
struct EvName { const char* name; uint16_t kind; };
const EvName kEvents[] = {
    {"hit", kEvHit}, {"died", kEvDied}, {"spawned", kEvSpawned}, {"attack", kEvAttack}, {"gathered", kEvGathered},
    {"delivered", kEvDelivered}, {"produced", kEvProduced}, {"ability", kEvAbility}, {"cast", kEvCastStart}, {"impact", kEvImpact},
    {"miss", kEvMiss}, {"built", kEvBuildDone}, {"placed", kEvBuildPlaced}, {"trained", kEvTrainDone}, {"researched", kEvResearched},
    {"levelup", kEvLevelUp}, {"captured", kEvCaptured}, {"sold", kEvSold}, {"upgraded", kEvUpgraded}, {"alert", kEvAlert},
    {"status", kEvStatusStart}, {"area", kEvAreaStart}, {"claimed", kEvRegionClaimed}, {"wave", kEvWave}, {"message", kEvMessage},
    {"ping", kOvEventPing},
};
} // namespace

uint16_t overlayEventKind(const std::string& name) {
    for (auto& e : kEvents) if (name == e.name) return e.kind;
    return 0;
}
const char* overlayEventName(uint16_t kind) {
    for (auto& e : kEvents) if (kind == e.kind) return e.name;
    return "";
}

OverlayDef parseOverlay(const Rules& R, const Json& j) {
    OverlayDef d;
    if (!j.isObject()) throw std::runtime_error("overlay: expected an object with \"id\"");
    d.id = j.getStr("id");
    if (d.id.empty()) throw std::runtime_error("overlay: missing \"id\"");
    const std::string where = "overlay '" + d.id + "'";
    onlyKeys(j, {"id", "enabled", "order", "when", "anchor", "shape", "style", "event", "replace", "name", "description", "builtin"}, where);
    d.source = j;
    d.enabled = j.getBool("enabled", true);
    d.builtin = j.getBool("builtin", false);
    d.order = int(j.getInt("order", 0));
    if (!j.has("shape")) fail(where, "missing \"shape\"");
    parseShape(j["shape"], d, where + ".shape");
    if (j.has("event")) parseEvent(R, j["event"], d, where + ".event");
    if (d.hasEvent) {   // popups sit where the event happened
        if (j.has("anchor") && j["anchor"].str() != "event") fail(where, "event overlays are placed at the event: omit \"anchor\"");
        d.anchor = OvAnchor::Event;
    } else if (j.has("anchor")) parseAnchor(R, j["anchor"], d, where + ".anchor");
    else if (d.shape == OvShape::Cells) d.anchor = OvAnchor::Map;
    if (j.has("style")) parseStyle(j["style"], d, where + ".style");
    d.when = parseCond(R, j["when"], where + ".when");
    // shape / anchor / space combinations that can't be drawn
    const bool ent = entityAnchor(d.anchor);
    if (d.shape == OvShape::Bar && !ent) fail(where, "a bar needs an entity anchor (selected, hovered, entities)");
    if ((d.shape == OvShape::Text || d.shape == OvShape::Bar) && d.style.space != OvSpace::Screen)
        fail(where, "text and bars are drawn in screen space (style.space \"screen\")");
    if (d.shape == OvShape::Footprint && !ent && d.anchor != OvAnchor::Ghost) fail(where, "a footprint needs the ghost or an entity anchor");
    if (d.shape == OvShape::Path && d.anchor != OvAnchor::Orders && d.anchor != OvAnchor::Route && d.anchor != OvAnchor::Rally)
        fail(where, "a path needs the orders, route or rally anchor");
    if ((d.anchor == OvAnchor::Orders || d.anchor == OvAnchor::Route || d.anchor == OvAnchor::Rally) &&
        d.shape != OvShape::Path && d.shape != OvShape::Line && d.shape != OvShape::Arrow)
        fail(where, "the orders / route / rally anchors draw a path, line or arrow");
    if ((d.shape == OvShape::Line || d.shape == OvShape::Arrow) && d.to == OvTo::None &&
        d.anchor != OvAnchor::Orders && d.anchor != OvAnchor::Route && d.anchor != OvAnchor::Rally)
        fail(where, "a line / arrow needs \"to\": target, rally, cursor, order or [x, z]");
    if (d.pred == OvPred::TerritoryEdge && d.shape != OvShape::Cells) fail(where, "the territoryEdge predicate is for the cells shape");
    if (d.anchor == OvAnchor::Screen) d.style.space = OvSpace::Screen;
    if (d.style.space == OvSpace::Minimap && d.shape != OvShape::Circle && d.shape != OvShape::Ring && d.shape != OvShape::Rect)
        fail(where, "the minimap shows circles, rings and rects");
    if (d.style.space == OvSpace::Screen && (d.shape == OvShape::Grid || d.shape == OvShape::Cells || d.shape == OvShape::Footprint ||
                                           d.shape == OvShape::Path || d.shape == OvShape::Line || d.shape == OvShape::Arrow))
        fail(where, "grids, cells, footprints, lines and paths are drawn in the world (space ground)");
    if (d.shape == OvShape::Icon && d.image.empty()) fail(where, "an icon needs \"image\" (texture path)");
    if (d.hasEvent && (d.shape == OvShape::Grid || d.shape == OvShape::Cells || d.shape == OvShape::Path || d.shape == OvShape::Line ||
                       d.shape == OvShape::Arrow || d.shape == OvShape::Footprint))
        fail(where, "event popups are text, icons, circles, rings or rects");
    if (d.radiusFrom == OvRadius::Range || d.radiusFrom == OvRadius::MinRange) {
        if (d.index < 0 || d.index > 7) fail(where, "range weapon index 0..7");
    }
    return d;
}

void mergeOverlayEntries(Json& list, const Json& more) {
    if (!list.isArray()) list = Json::array();
    if (!more.isArray()) return;
    for (auto& e : more.items()) {
        const std::string id = e.getStr("id");
        Json* dst = nullptr;
        if (!id.empty())
            for (auto& x : list.itemsMut()) if (x.getStr("id") == id) { dst = &x; break; }
        if (!dst) { list.push(e); continue; }
        if (e.getBool("replace")) { *dst = e; dst->erase("replace"); }
        else {
            const bool builtin = dst->getBool("builtin");
            deepMerge(*dst, e);
            if (builtin) dst->set("builtin", Json::fromBool(true));
        }
    }
}

OverlaySet loadOverlaySet(const Rules& R, const Json& rulesOverlays, const Json& gameOverlays) {
    OverlaySet out;
    Json list = builtinOverlays();
    if (!rulesOverlays.isNull() && !rulesOverlays.isArray()) out.warnings.push_back("rules.json overlays: expected a list");
    if (!gameOverlays.isNull() && !gameOverlays.isArray()) out.warnings.push_back("game.json overlays: expected a list");
    mergeOverlayEntries(list, rulesOverlays);
    mergeOverlayEntries(list, gameOverlays);
    for (auto& e : list.items()) {
        try { out.defs.push_back(parseOverlay(R, e)); }
        catch (const std::exception& ex) { out.warnings.push_back(ex.what()); }
    }
    std::stable_sort(out.defs.begin(), out.defs.end(), [](const OverlayDef& a, const OverlayDef& b) { return a.order < b.order; });
    return out;
}

void validateOverlays(const Rules& R, const Json& list) {
    if (!list.isArray()) throw std::runtime_error("overlays: expected a list");
    std::vector<std::string> ids;
    const Json builtins = builtinOverlays();
    for (auto& e : list.items()) {
        const std::string id = e.getStr("id");
        if (id.empty()) throw std::runtime_error("overlay: missing \"id\"");
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) throw std::runtime_error("overlay '" + id + "': duplicate id");
        ids.push_back(id);
        bool builtin = false;   // overrides of built-ins may be partial: checked merged below
        for (auto& b : builtins.items()) builtin = builtin || b.getStr("id") == id;
        if (!builtin || e.getBool("replace")) parseOverlay(R, e);
    }
    // the merged result must parse too (partial overrides of built-ins)
    Json merged = builtinOverlays();
    mergeOverlayEntries(merged, list);
    for (auto& e : merged.items()) parseOverlay(R, e);
}

Json builtinOverlays() {
    // The look the engine always had; exact colours (hex = the old byte values). Games restyle by id.
    static const char* kText = R"([
  {"id": "selection", "builtin": true, "anchor": "selected",
   "shape": {"type": "ring", "radius": "selection"},
   "style": {"color": "#78ff78e6", "colors": {"neutral": "#ffffa0e6"}, "ground": "flat", "lift": 0.05, "segments": 32}},
  {"id": "hover", "builtin": true, "anchor": "hovered", "when": {"selected": false},
   "shape": {"type": "ring", "radius": "selection"},
   "style": {"color": "#ffffff82", "ground": "flat", "lift": 0.05, "segments": 32}},
  {"id": "move_markers", "builtin": true, "anchor": "markers",
   "shape": {"type": "ring", "radius": 0.35},
   "style": {"color": "#78ff78c8", "ground": "zero", "lift": 0.06, "segments": 16}},
  {"id": "placement", "builtin": true, "anchor": "ghost",
   "shape": {"type": "footprint", "predicate": "ok"},
   "style": {"fill": true, "outline": false, "colors": {"true": "#50ff786e", "false": "#ff463c6e"}, "ground": "zero", "lift": 0.08}},
  {"id": "garrison_count", "builtin": true, "anchor": {"type": "entities", "flags": ["garrison"]},
   "shape": {"type": "text", "text": "{garrison}", "size": 1.2},
   "style": {"color": "#ffffffff", "background": "player", "backgroundShade": 0.45, "backgroundAlpha": 0.85,
             "rounding": 7, "padding": [6, 1], "shadow": false}},
  {"id": "health_bars", "builtin": true, "anchor": {"type": "entities", "flags": ["!invulnerable", "!garrison"]},
   "when": {"any": [{"selected": true}, {"hovered": true}, {"flags": ["damaged"]}]},
   "shape": {"type": "bar", "value": "hp", "width": 26, "widthStructure": 46, "height": 4},
   "style": {"background": "#000000aa", "stops": [[0.3, "#e6463cff"], [0.6, "#f0c83cff"], [1, "#50dc5aff"]]}},
  {"id": "build_progress", "builtin": true, "anchor": {"type": "entities", "flags": ["constructing", "!garrison"]},
   "shape": {"type": "bar", "value": "build", "width": 46, "height": 3},
   "style": {"offset": [0, 7], "background": "#000000aa", "color": "#78beffff"}}
])";
    return Json::parse(kText);
}

Json overlayTemplates() {
    static const char* kText = R"([
  {"id": "build_grid", "name": "Build grid while placing",
   "description": "Cell grid around the building ghost: green buildable, red blocked; cells under the footprint brighter.",
   "overlay": {"id": "build_grid", "when": "placing", "anchor": "ghost",
     "shape": {"type": "grid", "radius": 4, "predicate": "buildable"},
     "style": {"colors": {"true": "#3cff6450", "false": "#ff3c3c60"}, "lineColor": "#ffffff38", "outerAlpha": 0.55, "lift": 0.07}}},
  {"id": "attack_range_on_select", "name": "Attack range of selected units",
   "description": "Dashed ring at weapon range around each selected own unit or tower.",
   "overlay": {"id": "attack_range", "anchor": {"type": "selected", "owner": "own", "flags": ["ranged"]},
     "shape": {"type": "ring", "radius": "range"},
     "style": {"color": "#ff6a4ac0", "thickness": 0.06, "dash": [0.6, 0.35], "scroll": 0.4, "segments": 64}}},
  {"id": "tower_range_on_place", "name": "Range preview while placing a tower",
   "description": "Ring at the weapon range of the structure being placed (works for any type with a weapon).",
   "overlay": {"id": "placing_range", "when": "placing", "anchor": "ghost",
     "shape": {"type": "ring", "radius": "range"},
     "style": {"color": "#ffd24ad0", "thickness": 0.05, "segments": 64}}},
  {"id": "aura_radius", "name": "Aura radius",
   "description": "Soft disc showing the aura radius of selected units with an aura ability.",
   "overlay": {"id": "aura_radius", "anchor": {"type": "selected", "flags": ["aura"]},
     "shape": {"type": "circle", "radius": "aura"},
     "style": {"color": "#7ac8ff40", "outline": true, "lineColor": "#9ad8ffc0", "pulse": 0.5, "pulseMin": 0.6}}},
  {"id": "sight_range_alt", "name": "Sight range while Alt is held",
   "description": "Rings at sight range of own units while Alt is held.",
   "overlay": {"id": "sight_alt", "when": {"key": "Alt"}, "anchor": {"type": "entities", "owner": "own", "flags": ["unit"]},
     "shape": {"type": "ring", "radius": "sight"}, "style": {"color": "#ffffff50", "segments": 48}}},
  {"id": "rally_line", "name": "Rally line",
   "description": "Dashed animated line from selected producers to their rally point, with a marker.",
   "overlay": {"id": "rally_line", "anchor": "rally", "shape": {"type": "arrow"},
     "style": {"color": "#78ff78d0", "thickness": 0.07, "dash": [0.5, 0.3], "scroll": 1.5, "lift": 0.12}}},
  {"id": "order_path", "name": "Queued orders path",
   "description": "Line through the queued order points of selected units (shift-queue).",
   "overlay": {"id": "order_path", "anchor": "orders", "shape": "path",
     "style": {"color": "#ffffffa0", "thickness": 0.05, "dash": [0.4, 0.25], "scroll": 1}}},
  {"id": "damage_numbers", "name": "Damage numbers",
   "description": "Floating numbers above hit units, rising and fading.",
   "overlay": {"id": "damage_numbers", "event": {"on": "hit", "lifetime": 1.0, "rise": 1.6, "spread": 0.5},
     "shape": {"type": "text", "text": "-{value}", "size": 1.1},
     "style": {"color": "#ffe070ff", "colors": {"own": "#ff7060ff"}, "fadeOut": 0.4, "lift": 1.4}}},
  {"id": "resource_popup", "name": "Resource gained",
   "description": "'+N gold' floating up where an own worker delivers resources.",
   "overlay": {"id": "resource_popup", "event": {"on": "gathered", "by": "own", "lifetime": 1.4, "rise": 1.2},
     "shape": {"type": "text", "text": "+{amount} {resource}", "size": 1.0},
     "style": {"color": "#ffd84aff", "fadeOut": 0.5, "lift": 1.6}}},
  {"id": "territory_border_on_hover", "name": "Territory borders on hover",
   "description": "Owner-coloured borders of territory cells near the hovered structure.",
   "overlay": {"id": "territory_hover", "when": {"hovered": {"flags": ["structure"]}}, "anchor": "cursor",
     "shape": {"type": "cells", "predicate": "territoryEdge", "radius": 14},
     "style": {"color": "player", "thickness": 0.08, "lift": 0.08}}},
  {"id": "build_radius_on_place", "name": "Build radius while placing",
   "description": "Tints the cells within the build radius of own structures while placing.",
   "overlay": {"id": "build_radius", "when": "placing", "anchor": "ghost",
     "shape": {"type": "cells", "predicate": "buildRadius", "radius": 18},
     "style": {"colors": {"true": "#60b0ff22"}, "lift": 0.05}}},
  {"id": "ping_marker", "name": "Ping marker",
   "description": "Expanding pulsing ring where a ping or an under-attack alert happens.",
   "overlay": {"id": "ping_marker", "event": {"on": ["ping", "alert"], "lifetime": 2.0, "grow": 1.5},
     "shape": {"type": "ring", "radius": 0.6},
     "style": {"color": "#ffd040ff", "thickness": 0.1, "fadeOut": 0.8, "pulse": 3, "onTop": true}}},
  {"id": "minimap_ping", "name": "Minimap ping",
   "description": "Ring on the minimap where an alert / ping happens (client minimap).",
   "overlay": {"id": "minimap_ping", "event": {"on": ["ping", "alert"], "lifetime": 3.0, "grow": 2},
     "shape": {"type": "ring", "radius": 2},
     "style": {"space": "minimap", "color": "#ffe040ff", "pulse": 2, "fadeOut": 1}}},
  {"id": "low_hp_warning", "name": "Low hp warning",
   "description": "Pulsing red ring under own units below 30% hp.",
   "overlay": {"id": "low_hp", "anchor": {"type": "entities", "owner": "own", "hp": [0, 0.3]},
     "shape": {"type": "ring", "radius": "selection", "radiusAdd": 0.15},
     "style": {"color": "#ff3030ff", "thickness": 0.06, "pulse": 2}}},
  {"id": "energy_bar", "name": "Energy bar",
   "description": "Blue energy bar under the health bar of selected casters.",
   "overlay": {"id": "energy_bar", "anchor": {"type": "selected", "flags": ["energy"]},
     "shape": {"type": "bar", "value": "energy", "width": 26, "height": 3},
     "style": {"offset": [0, 6], "background": "#000000aa", "color": "#4aa0ffff"}}},
  {"id": "target_line", "name": "Attack target line",
   "description": "Red line from selected units to what they attack.",
   "overlay": {"id": "target_line", "anchor": {"type": "selected", "flags": ["attacking"]},
     "shape": {"type": "line", "to": "target"}, "style": {"color": "#ff4040a0", "thickness": 0.04, "lift": 0.3}}},
  {"id": "cursor_cell", "name": "Cell under the cursor",
   "description": "Outline of the cell under the cursor, green when buildable.",
   "overlay": {"id": "cursor_cell", "when": "cursor", "anchor": {"type": "cursor", "snap": true},
     "shape": {"type": "grid", "radius": 0, "square": true, "predicate": "buildable"},
     "style": {"colors": {"true": "#60ff6040", "false": "#ff606040"}, "lineColor": "#ffffffa0"}}},
  {"id": "hud_label", "name": "HUD label",
   "description": "Text pinned to the screen (top-left), e.g. a hint; {selected} = selected count.",
   "overlay": {"id": "hud_label", "when": "selected", "anchor": {"type": "screen", "at": [16, 64]},
     "shape": {"type": "text", "text": "Selected: {selected}", "size": 1.0},
     "style": {"color": "#ffffffff", "background": "#00000090", "padding": [6, 3], "rounding": 4}}}
])";
    return Json::parse(kText);
}

} // namespace rts::game
