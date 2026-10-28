// Parsing of grid fields: rules "fields", type "field" emitter blocks, map "gridFields" (overrides + initial
// values) and the RLE text format of field layers. Errors name the field and the key.
#include "RtsData.h"
#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace rts::game {

namespace {
[[noreturn]] void fail(const std::string& where, const std::string& what) { throw std::runtime_error(where + ": " + what); }

int32_t sat32(int64_t v) { return int32_t(std::clamp<int64_t>(v, INT32_MIN, INT32_MAX)); }
int32_t q16(const Json& j, const std::string& where, bool clamp01 = true) {
    if (!j.isNumber()) fail(where, "number expected");
    int64_t r = j.fx().raw;
    if (clamp01 && (r < 0 || r > 65536)) fail(where, "must be between 0 and 1");
    return int32_t(r);
}
uint64_t tagList(const Rules& r, const Json& j, const std::string& where) {
    uint64_t m = 0;
    auto one = [&](const std::string& t) {
        uint64_t b = r.tagMask(t);
        if (!b) fail(where, "unknown tag '" + t + "'");
        m |= b;
    };
    if (j.isString()) one(j.str());
    for (auto& t : j.items()) one(t.str());
    return m;
}
int fieldRef(const std::vector<GridFieldDef>& all, const std::string& id, const std::string& self, const std::string& where) {
    for (size_t i = 0; i < all.size(); ++i) if (all[i].id == id) return int(i);
    if (id == self) return int(all.size());   // the field being defined (appended right after)
    fail(where, "unknown field '" + id + "'");
}
uint32_t periodOf(const Json& j, uint32_t def, const std::string& where) {
    int64_t p = j.getInt("period", def);
    if (p < 1 || p > 100000) fail(where, "period must be 1..100000 ticks");
    return uint32_t(p);
}
} // namespace

int32_t GridFieldDef::raw(Fixed v) const { return fixed ? sat32(v.raw) : sat32(v.floorInt()); }
Fixed GridFieldDef::toFixed(int32_t v) const { return fixed ? Fixed::fromRaw(v) : Fixed::fromInt(v); }

GridFieldDef parseGridField(const std::string& id, const Json& j, const Rules& r, const std::vector<GridFieldDef>& all, const GridFieldDef* base) {
    const std::string w = "field '" + id + "'";
    if (!j.isObject()) fail(w, "object expected");
    GridFieldDef d = base ? *base : GridFieldDef{};
    d.id = id;
    if (j.has("type")) {
        const std::string t = j.getStr("type");
        if (t != "int" && t != "fixed") fail(w, "type must be \"int\" or \"fixed\"");
        if (base && (t == "fixed") != base->fixed) fail(w, "a map can't change the type of a rules field");
        d.fixed = t == "fixed";
    }
    if (j.has("resolution")) {
        int res = int(j.getInt("resolution", 1));
        if (res != 1 && res != 2 && res != 4 && res != 8) fail(w, "resolution must be 1, 2, 4 or 8 map cells per field cell");
        d.resolution = res;
    }
    if (j.has("perPlayer")) d.perPlayer = j.getBool("perPlayer", false);
    auto val = [&](const char* k, int32_t def) { return j.has(k) ? d.raw(j[k].fx()) : def; };
    d.def = val("default", d.def);
    d.min = val("min", d.min);
    d.max = val("max", d.max);
    if (d.min > d.max) fail(w, "min > max");
    if (d.def < d.min || d.def > d.max) fail(w, "default outside min..max");
    if (j.has("view")) d.view = j["view"];
    if (j.has("kernels")) {
        d.kernels.clear();
        if (!j["kernels"].isArray()) fail(w, "kernels: array expected");
        for (size_t ki = 0; ki < j["kernels"].size(); ++ki) {
            const Json& kj = j["kernels"][ki];
            const std::string kt = kj.getStr("type");
            const std::string kw = w + " kernel " + std::to_string(ki) + " '" + kt + "'";
            FieldKernelDef k;
            if (kt == "decay") k.kind = FieldKernelKind::Decay;
            else if (kt == "diffuse") k.kind = FieldKernelKind::Diffuse;
            else if (kt == "spread") k.kind = FieldKernelKind::Spread;
            else if (kt == "flow") k.kind = FieldKernelKind::Flow;
            else if (kt == "consume") k.kind = FieldKernelKind::Consume;
            else fail(kw, "unknown kernel (decay, diffuse, spread, flow, consume)");
            k.period = periodOf(kj, 5, kw);
            k.phase = uint32_t(std::max<int64_t>(0, kj.getInt("phase", 0)));
            const int nb = int(kj.getInt("neighbours", kj.getInt("neighbors", 4)));
            if (nb != 4 && nb != 8) fail(kw, "neighbours must be 4 or 8");
            k.eight = nb == 8;
            switch (k.kind) {
            case FieldKernelKind::Decay:
                k.amount = kj.has("amount") ? d.raw(kj["amount"].fx()) : 0;
                if (k.amount < 0) fail(kw, "amount must be >= 0");
                k.percent = kj.has("percent") ? q16(kj["percent"], kw + " percent") : 0;
                if (!k.amount && !k.percent) fail(kw, "needs 'amount' and / or 'percent'");
                break;
            case FieldKernelKind::Diffuse:
                k.rate = q16(kj.has("rate") ? kj["rate"] : Json(), kw + " rate");
                break;
            case FieldKernelKind::Spread: {
                k.above = kj.has("above") ? d.raw(kj["above"].fx()) : (d.fixed ? 65536 : 1);
                k.below = kj.has("below") ? d.raw(kj["below"].fx()) : k.above;
                k.set = kj.has("set") ? d.raw(kj["set"].fx()) : k.above;
                k.chance = kj.has("chance") ? q16(kj["chance"], kw + " chance") : 0;
                k.fuelChance = kj.has("fuelChance") ? q16(kj["fuelChance"], kw + " fuelChance") : 0;
                if (const Json& fj = kj["fuel"]; fj.isObject()) {
                    if (fj.has("field")) {
                        k.fuel.field = int16_t(fieldRef(all, fj.getStr("field"), id, kw + " fuel"));
                        k.fuel.fieldMin = int32_t(fj.getInt("min", 1));
                    }
                    for (auto& m : fj["materials"].items()) {
                        int64_t mi = m.integer(-1);
                        if (mi < 0 || mi > 9) fail(kw, "fuel.materials: terrainMaterial layer indices 0..9");
                        k.fuel.materials.push_back(uint8_t(mi));
                    }
                    k.fuel.terrain = fj.getStr("terrain");
                    if (fj.has("tags")) k.fuel.tags = tagList(r, fj["tags"], kw + " fuel.tags");
                } else if (kj.has("fuel")) fail(kw, "fuel: object {field, min, materials, terrain, tags} expected");
                if (k.fuel.field >= 0 && k.fuel.field < int(all.size()) && all[size_t(k.fuel.field)].resolution != d.resolution)
                    fail(kw, "the fuel field must have the same resolution");
                break;
            }
            case FieldKernelKind::Flow:
                k.rate = q16(kj.has("rate") ? kj["rate"] : Json::number(Fixed::fromRatio(1, 2)), kw + " rate");
                k.unitsPerHeight = kj.has("unitsPerHeight") ? kj["unitsPerHeight"].integer(100) : (d.fixed ? 65536 : 100);
                if (d.fixed && kj.has("unitsPerHeight")) k.unitsPerHeight = kj["unitsPerHeight"].fx().raw;
                k.minDepth = kj.has("minDepth") ? d.raw(kj["minDepth"].fx()) : 0;
                if (d.perPlayer) fail(kw, "flow on a per-player field is not supported");
                break;
            case FieldKernelKind::Consume:
                k.fuelField = int16_t(fieldRef(all, kj.getStr("fuel"), "", kw + " fuel"));
                if (all[size_t(k.fuelField)].resolution != d.resolution) fail(kw, "the fuel field must have the same resolution");
                if (all[size_t(k.fuelField)].perPlayer || d.perPlayer) fail(kw, "consume works on plain (not per-player) fields");
                k.amount = kj.has("amount") ? all[size_t(k.fuelField)].raw(kj["amount"].fx()) : 1;
                k.above = kj.has("above") ? d.raw(kj["above"].fx()) : (d.fixed ? 65536 : 1);
                k.out = kj.has("out") ? d.raw(kj["out"].fx()) : d.def;
                break;
            }
            d.kernels.push_back(k);
        }
    }
    if (j.has("effects")) {
        d.effects.clear();
        if (!j["effects"].isArray()) fail(w, "effects: array expected");
        for (size_t ei = 0; ei < j["effects"].size(); ++ei) {
            const Json& ej = j["effects"][ei];
            const std::string et = ej.getStr("type");
            const std::string ew = w + " effect " + std::to_string(ei) + " '" + et + "'";
            FieldEffectDef e;
            if (et == "damage") e.kind = FieldEffectKind::Damage;
            else if (et == "speed") e.kind = FieldEffectKind::Speed;
            else if (et == "blockMove") e.kind = FieldEffectKind::BlockMove;
            else if (et == "blockVision") e.kind = FieldEffectKind::BlockVision;
            else if (et == "status") e.kind = FieldEffectKind::Status;
            else if (et == "yield") e.kind = FieldEffectKind::Yield;
            else if (et == "reveal") e.kind = FieldEffectKind::Reveal;
            else fail(ew, "unknown effect (damage, speed, blockMove, blockVision, status, yield, reveal)");
            e.above = ej.has("above") ? d.raw(ej["above"].fx()) : (d.fixed ? 65536 : 1);
            e.period = periodOf(ej, 10, ew);
            e.perSecond = ej.getFx("perSecond");
            e.scaled = ej.getBool("scaled", false);
            if (ej.has("damageType")) {
                const std::string dt = ej.getStr("damageType");
                auto it = std::find(r.damageTypes.begin(), r.damageTypes.end(), dt);
                if (it == r.damageTypes.end()) fail(ew, "unknown damageType '" + dt + "'");
                e.damageType = uint16_t(it - r.damageTypes.begin());
            }
            e.mul = ej.getFx("mul", e.mul);
            if (e.kind == FieldEffectKind::Speed && e.mul.raw < 0) fail(ew, "mul must be >= 0");
            e.height = ej.getFx("height", e.height);
            if (e.kind == FieldEffectKind::Status) {
                e.status = int16_t(r.findStatus(ej.getStr("status")));
                if (e.status < 0) fail(ew, "unknown status '" + ej.getStr("status") + "' (rules \"statuses\")");
                e.seconds = ej.getFx("seconds");
            }
            if (e.kind == FieldEffectKind::Yield) {
                e.resource = int16_t(r.findResource(ej.getStr("resource")));
                if (e.resource < 0) fail(ew, "unknown resource '" + ej.getStr("resource") + "'");
            }
            const std::string who = ej.getStr("who", "any");
            if (who == "any") e.who = FieldWho::Any;
            else if (who == "enemies") e.who = FieldWho::Enemies;
            else if (who == "own") e.who = FieldWho::Own;
            else fail(ew, "who must be any, enemies or own");
            if (ej.has("tags")) e.tags = tagList(r, ej["tags"], ew + " tags");
            if (ej.has("notTags")) e.notTags = tagList(r, ej["notTags"], ew + " notTags");
            e.units = ej.getBool("units", true);
            e.structures = ej.getBool("structures", true);
            e.all = ej.getBool("all", !d.perPlayer);
            d.effects.push_back(e);
        }
    }
    return d;
}

void loadGridFieldRules(Rules& r, const Json& j) {
    const Json& fj = j["fields"];
    if (fj.isNull()) return;
    if (!fj.isObject()) fail("fields", "object {id: {...}} expected");
    // names first (kernels may refer to fields defined later in the file)
    std::vector<GridFieldDef> stub;
    for (auto& [id, v] : fj.members()) {
        if (id.empty()) fail("fields", "empty field id");
        for (auto& s : stub) if (s.id == id) fail("field '" + id + "'", "defined twice");
        GridFieldDef d; d.id = id;
        d.fixed = v.getStr("type", "int") == "fixed";
        d.resolution = int(v.getInt("resolution", 1));
        d.perPlayer = v.getBool("perPlayer", false);
        stub.push_back(d);
    }
    r.gridFields = stub;
    for (size_t i = 0; i < stub.size(); ++i) r.gridFields[i] = parseGridField(stub[i].id, fj[stub[i].id], r, stub, nullptr);
    // emitters: type "field": {"<field>": {"emit", "radius", "mode", "period", "whileBuilding"}}
    for (auto& [tid, tj] : j["types"].members()) {
        const Json& em = tj["field"];
        if (em.isNull()) continue;
        const std::string w = "type '" + tid + "' field";
        if (!em.isObject()) fail(w, "object {field: {emit, radius}} expected");
        UnitType& t = r.types[size_t(r.findType(tid))];
        for (auto& [fid, e] : em.members()) {
            FieldEmitDef d;
            d.field = int16_t(fieldRef(r.gridFields, fid, "", w));
            const GridFieldDef& fd = r.gridFields[size_t(d.field)];
            d.amount = fd.raw(e.getFx("emit"));
            d.radius = e.getFx("radius");
            const std::string mode = e.getStr("mode", "add");
            if (mode != "add" && mode != "max" && mode != "set") fail(w + "." + fid, "mode must be add, max or set");
            d.mode = mode == "add" ? 0 : mode == "max" ? 1 : 2;
            d.period = periodOf(e, 10, w + "." + fid);
            d.whileBuilding = e.getBool("whileBuilding", false);
            t.fieldEmit.push_back(d);
        }
    }
    // effects {"type": "field", "field": id, "add" | "set" | "max": n, "radius": r}
    for (auto& list : r.payloads)
        for (auto& e : list)
            if (e.kind == EffectKind::Field) {
                e.field = int16_t(fieldRef(r.gridFields, e.fieldName, "", "effect 'field'"));
                e.fieldValue = r.gridFields[size_t(e.field)].raw(e.fieldAmount);
            }
}

void applyMapGridFields(std::vector<GridFieldDef>& defs, const Json& map, const Rules& r) {
    const Json* gj = &map["gridFields"];
    if (!gj->isObject() && map["fields"].isObject()) gj = &map["fields"];   // alias (map "fields" arrays are resource fields)
    if (!gj->isObject()) return;
    for (auto& [id, v] : gj->members()) {
        int at = -1;
        for (size_t i = 0; i < defs.size(); ++i) if (defs[i].id == id) at = int(i);
        if (at >= 0) defs[size_t(at)] = parseGridField(id, v, r, defs, &defs[size_t(at)]);
        else defs.push_back(parseGridField(id, v, r, defs, nullptr));
        GridFieldDef& d = defs[size_t(at >= 0 ? at : int(defs.size()) - 1)];
        d.initial = v;
    }
}

bool buildInitialField(const GridFieldDef& d, int mapW, int mapH, int layers, std::vector<int32_t>& v, std::string* err) {
    const int fw = (mapW + d.resolution - 1) / d.resolution, fh = (mapH + d.resolution - 1) / d.resolution;
    const size_t cells = size_t(fw) * size_t(fh);
    v.assign(cells * size_t(std::max(1, layers)), d.def);
    const Json& ini = d.initial;
    if (!ini.isObject()) return true;
    const std::string w = "map gridFields '" + d.id + "'";
    auto bad = [&](const std::string& m) { if (err) *err = w + ": " + m; return false; };
    auto clampV = [&](int64_t x) { return int32_t(std::clamp<int64_t>(x, d.min, d.max)); };
    auto layerRle = [&](int layer, const std::string& text) {
        if (layer < 0 || layer >= layers) return bad("layer " + std::to_string(layer) + " out of range");
        std::vector<int32_t> vals;
        if (!decodeFieldRle(text, cells, vals)) return bad("values must be RLE text of " + std::to_string(cells) + " cells (\"v*n v ...\")");
        for (size_t i = 0; i < cells; ++i) v[size_t(layer) * cells + i] = clampV(vals[i]);
        return true;
    };
    if (const Json& vv = ini["values"]; vv.isString()) { if (!layerRle(0, vv.str())) return false; }
    else if (vv.isObject()) { for (auto& [k, t] : vv.members()) if (!layerRle(int(std::strtol(k.c_str(), nullptr, 10)), t.str())) return false; }
    for (auto& fl : ini["fill"].items()) {
        const int32_t val = d.raw(fl.getFx("value"));
        const int layer = int(fl.getInt("layer", 0));
        if (layer < 0 || layer >= layers) return bad("fill layer out of range");
        const std::string mode = fl.getStr("mode", "set");
        auto put = [&](int x, int z) {
            if (x < 0 || z < 0 || x >= fw || z >= fh) return;
            int32_t& c = v[size_t(layer) * cells + size_t(z * fw + x)];
            c = clampV(mode == "add" ? int64_t(c) + val : mode == "max" ? std::max<int64_t>(c, val) : val);
        };
        if (const Json& r = fl["rect"]; r.isArray() && r.size() == 4) {
            const int x0 = int(r[size_t(0)].integer()), z0 = int(r[size_t(1)].integer());
            for (int z = z0; z < z0 + int(r[size_t(3)].integer()); ++z)
                for (int x = x0; x < x0 + int(r[size_t(2)].integer()); ++x) put(x, z);
        } else if (const Json& c = fl["circle"]; c.isArray() && c.size() == 3) {
            const Fixed cx = c[size_t(0)].fx(), cz = c[size_t(1)].fx(), rad = c[size_t(2)].fx(), r2 = rad * rad;
            const int res = d.resolution;
            for (int z = std::max<int>(0, int((cz - rad).floorInt()) / res - 1); z <= int((cz + rad).floorInt()) / res + 1; ++z)
                for (int x = std::max<int>(0, int((cx - rad).floorInt()) / res - 1); x <= int((cx + rad).floorInt()) / res + 1; ++x) {
                    const Fixed dx = Fixed::fromInt(x * res) + Fixed::fromRatio(res, 2) - cx, dz = Fixed::fromInt(z * res) + Fixed::fromRatio(res, 2) - cz;
                    if (dx * dx + dz * dz <= r2) put(x, z);
                }
        } else return bad("fill entries need \"rect\": [x, y, w, h] (field cells) or \"circle\": [x, z, r] (world)");
    }
    return true;
}

std::string encodeFieldRle(const std::vector<int32_t>& v) {
    std::string out;
    size_t i = 0;
    while (i < v.size()) {
        size_t j = i + 1;
        while (j < v.size() && v[j] == v[i]) ++j;
        if (!out.empty()) out += ' ';
        out += std::to_string(v[i]);
        if (j - i > 1) { out += '*'; out += std::to_string(j - i); }
        i = j;
    }
    return out;
}

bool decodeFieldRle(const std::string& s, size_t n, std::vector<int32_t>& out) {
    out.clear();
    out.reserve(n);
    size_t p = 0;
    auto num = [&](int64_t& x) {
        bool neg = false;
        if (p < s.size() && s[p] == '-') { neg = true; ++p; }
        if (p >= s.size() || s[p] < '0' || s[p] > '9') return false;
        x = 0;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') { x = x * 10 + (s[p] - '0'); if (x > (int64_t(1) << 40)) return false; ++p; }
        if (neg) x = -x;
        return true;
    };
    while (p < s.size()) {
        if (s[p] == ' ' || s[p] == '\n' || s[p] == ',') { ++p; continue; }
        int64_t v = 0, c = 1;
        if (!num(v)) return false;
        if (p < s.size() && s[p] == '*') { ++p; if (!num(c) || c < 1) return false; }
        if (out.size() + size_t(c) > n) return false;
        out.insert(out.end(), size_t(c), int32_t(std::clamp<int64_t>(v, INT32_MIN, INT32_MAX)));
    }
    return out.size() == n;
}

} // namespace rts::game
