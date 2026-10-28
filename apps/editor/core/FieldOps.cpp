#include "FieldOps.h"
#include "modules/rts/view/RtsView.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rts::editor {

namespace {
Json str(const std::string& s) { return Json::string(s); }
void needProject(const Workspace& ws) { if (ws.project.dir.empty()) throw std::runtime_error("no game is open: call open_game first"); }

Json mapView(const MapDoc& doc) {   // the map keys the field parser reads
    Json m = Json::object();
    if (const Json& g = doc.data.extra["gridFields"]; g.isObject()) m.set("gridFields", g);
    return m;
}
int findDef(const std::vector<game::GridFieldDef>& defs, const std::string& id) {
    for (size_t i = 0; i < defs.size(); ++i) if (defs[i].id == id) return int(i);
    return -1;
}
double units(const game::GridFieldDef& d, int32_t raw) { return d.fixed ? double(raw) / 65536.0 : double(raw); }
Json num(const game::GridFieldDef& d, int32_t raw) { return d.fixed ? Json::number(double(raw) / 65536.0, 4) : Json::number(int64_t(raw)); }
int32_t rawOf(const game::GridFieldDef& d, double v) {
    const double r = d.fixed ? v * 65536.0 : v;
    return int32_t(std::clamp<double>(std::floor(r + (d.fixed ? 0.5 : 0.0)), double(d.min), double(d.max)));
}
// decoded values of one field (all layers) from the map; throws on bad data
std::vector<int32_t> decode(const game::GridFieldDef& d, const MapDoc& doc, int layers) {
    std::vector<int32_t> v;
    std::string err;
    if (!game::buildInitialField(d, doc.data.w, doc.data.h, layers, v, &err)) throw std::runtime_error(err);
    return v;
}
void store(MapDoc& doc, const game::GridFieldDef& d, const std::vector<int32_t>& v, int layers) {
    Json& g = doc.data.extra.member("gridFields");
    Json& e = g.member(d.id);
    e.erase("fill");   // baked into the values
    const size_t cells = v.size() / size_t(std::max(1, layers));
    bool any = false;
    for (int32_t x : v) any = any || x != d.def;
    if (!any) { e.erase("values"); if (e.size() == 0) g.erase(d.id); if (g.size() == 0) doc.data.extra.erase("gridFields"); return; }
    if (!d.perPlayer) { e.set("values", str(game::encodeFieldRle(v))); return; }
    Json per = Json::object();
    for (int l = 0; l < layers; ++l) {
        std::vector<int32_t> one(v.begin() + ptrdiff_t(size_t(l) * cells), v.begin() + ptrdiff_t(size_t(l + 1) * cells));
        if (std::any_of(one.begin(), one.end(), [&](int32_t x) { return x != d.def; })) per.set(std::to_string(l), str(game::encodeFieldRle(one)));
    }
    e.set("values", per);
}
} // namespace

std::vector<game::GridFieldDef> mapFieldDefs(const game::Rules& rules, const MapDoc& doc) {
    std::vector<game::GridFieldDef> defs = rules.gridFields;
    game::applyMapGridFields(defs, mapView(doc), rules);
    return defs;
}

int fieldLayers(const game::GridFieldDef& d, const MapDoc& doc) { return d.perPlayer ? int(doc.data.players.size()) + 1 : 1; }

int paintMapField(MapDoc& doc, const game::Rules& rules, const std::string& id, int mx, int mz, int radius, double value, int mode, int player, std::string* err, const int* rect) {
    try {
        auto defs = mapFieldDefs(rules, doc);
        const int fi = findDef(defs, id);
        if (fi < 0) throw std::runtime_error("unknown field '" + id + "' (field_list)");
        const game::GridFieldDef& d = defs[size_t(fi)];
        const int layers = fieldLayers(d, doc);
        const int layer = d.perPlayer ? player : 0;
        if (layer < 0 || layer >= layers || (d.perPlayer && layer == 0)) throw std::runtime_error("field '" + id + "' is per player: pass player 1.." + std::to_string(layers - 1));
        std::vector<int32_t> v = decode(d, doc, layers);
        const int fw = game::fieldCellsW(d, doc.data.w), fh = (doc.data.h + d.resolution - 1) / d.resolution;
        const size_t cells = size_t(fw) * size_t(fh);
        const int cx = mx / d.resolution, cz = mz / d.resolution, r = std::max(0, radius / d.resolution);
        const int32_t raw = rawOf(d, value);
        int n = 0;
        int x0 = cx - r, z0 = cz - r, x1 = cx + r, z1 = cz + r;
        if (rect) { x0 = rect[0] / d.resolution; z0 = rect[1] / d.resolution; x1 = (rect[0] + rect[2] - 1) / d.resolution; z1 = (rect[1] + rect[3] - 1) / d.resolution; }
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                if (x < 0 || z < 0 || x >= fw || z >= fh || (!rect && (x - cx) * (x - cx) + (z - cz) * (z - cz) > r * r)) continue;
                int32_t& c = v[size_t(layer) * cells + size_t(z * fw + x)];
                const int64_t nv = mode == 3 ? d.def : mode == 1 ? int64_t(c) + raw : mode == 2 ? std::max<int64_t>(c, raw) : raw;
                const int32_t cl = int32_t(std::clamp<int64_t>(nv, d.min, d.max));
                if (cl != c) { c = cl; ++n; }
            }
        if (n) { store(doc, d, v, layers); doc.touched(); }
        return n;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return -1;
    }
}

void FieldPreview::fill(const game::Rules& rules, const MapDoc& doc, game::RtsScene& scene) {
    scene.fields.clear();
    scene.fieldDefs = nullptr;
    if (rules.gridFields.empty() && !doc.data.extra["gridFields"].isObject()) return;
    size_t sig = rules.gridFields.size();
    for (auto& d : rules.gridFields) sig = sig * 31 + d.kernels.size() * 7 + d.effects.size() + d.view.dump(-1).size();
    if (doc.revision != revision_ || &rules != rules_ || sig != ruleFields_) {
        revision_ = doc.revision; rules_ = &rules; ruleFields_ = sig;
        try {
            defs_ = mapFieldDefs(rules, doc);
            values_.clear();
            for (auto& d : defs_) values_.push_back(decode(d, doc, fieldLayers(d, doc)));
        } catch (const std::exception&) { defs_.clear(); values_.clear(); }
        ++version_;
    }
    if (defs_.empty()) return;
    scene.fieldDefs = &defs_;
    for (size_t i = 0; i < defs_.size(); ++i) {
        const auto& d = defs_[i];
        const int fw = game::fieldCellsW(d, doc.data.w), fh = (doc.data.h + d.resolution - 1) / d.resolution;
        scene.fields.push_back({int(i), fw, fh, d.resolution, fieldLayers(d, doc), values_[i].data(), version_});
    }
}

void registerFieldOps(Workspace& ws) {
    Workspace* w = &ws;
    auto add = [&ws](const char* name, const char* desc, const char* schema, Workspace::OpFn fn) {
        ws.registerOp({name, desc, Json::parse(schema), std::move(fn), false});
    };
    add("field_list",
        "Grid fields of the open game (rules.json \"fields\" + map \"gridFields\" overrides, docs/fields.md): per-cell int / fixed "
        "layers updated by C++ kernels (decay, diffuse, spread, flow, consume) with per-cell effects (damage, speed, blockMove, "
        "blockVision, status, yield, reveal). For each: type, resolution, perPlayer, default/min/max, kernels, effects, size in "
        "field cells and the map's initial values (non-default cells, max).",
        "{}", [w](const Json&) {
            needProject(*w);
            auto defs = mapFieldDefs(w->project.rules, w->map);
            Json arr = Json::array();
            static const char* kk[] = {"decay", "diffuse", "spread", "flow", "consume"};
            static const char* ek[] = {"damage", "speed", "blockMove", "blockVision", "status", "yield", "reveal"};
            for (auto& d : defs) {
                Json j = Json::object();
                j.set("id", str(d.id));
                j.set("type", str(d.fixed ? "fixed" : "int"));
                j.set("resolution", Json::number(int64_t(d.resolution)));
                j.set("perPlayer", Json::fromBool(d.perPlayer));
                j.set("default", num(d, d.def));
                if (d.min != INT32_MIN) j.set("min", num(d, d.min));
                if (d.max != INT32_MAX) j.set("max", num(d, d.max));
                Json k = Json::array(); for (auto& x : d.kernels) k.push(str(kk[int(x.kind)]));
                Json e = Json::array(); for (auto& x : d.effects) e.push(str(ek[int(x.kind)]));
                j.set("kernels", k); j.set("effects", e);
                j.set("origin", str(findDef(w->project.rules.gridFields, d.id) >= 0 ? (w->map.data.extra["gridFields"].has(d.id) ? "rules+map" : "rules") : "map"));
                const int layers = fieldLayers(d, w->map);
                auto v = decode(d, w->map, layers);
                int64_t nonDef = 0; int32_t mx = d.def;
                for (int32_t x : v) { if (x != d.def) ++nonDef; mx = std::max(mx, x); }
                Json sz = Json::array();
                sz.push(Json::number(int64_t(game::fieldCellsW(d, w->map.data.w)))); sz.push(Json::number(int64_t((w->map.data.h + d.resolution - 1) / d.resolution)));
                j.set("size", sz);
                j.set("layers", Json::number(int64_t(layers)));
                j.set("initialCells", Json::number(nonDef));
                j.set("initialMax", num(d, mx));
                arr.push(j);
            }
            Json r = Json::object();
            r.set("fields", arr);
            return r;
        });
    add("field_get",
        "Initial values of a grid field on the open map: 'rect' [x, z, w, h] in field cells (at most 64 x 64) gives 'rows' of numbers; "
        "without rect an ASCII preview of the whole field ('ascii': '.' default, 1-9 by value, '#' max), at most 96 columns. "
        "player = layer of a per-player field. With 'live': true and a running play-test, the current simulation values.",
        R"({"properties": {"id": {"type": "string"}, "rect": {"type": "array", "items": {"type": "integer"}}, "player": {"type": "integer"},
            "live": {"type": "boolean"}}, "required": ["id"]})",
        [w](const Json& p) {
            needProject(*w);
            auto defs = mapFieldDefs(w->project.rules, w->map);
            const int fi = findDef(defs, p.getStr("id"));
            if (fi < 0) throw std::runtime_error("unknown field '" + p.getStr("id") + "' (field_list)");
            const auto& d = defs[size_t(fi)];
            const int layers = fieldLayers(d, w->map);
            const int layer = d.perPlayer ? int(p.getInt("player", 1)) : 0;
            if (layer < 0 || layer >= layers) throw std::runtime_error("player out of range");
            auto v = decode(d, w->map, layers);
            const int fw = game::fieldCellsW(d, w->map.data.w), fh = (w->map.data.h + d.resolution - 1) / d.resolution;
            const size_t cells = size_t(fw) * size_t(fh);
            Json r = Json::object();
            r.set("id", str(d.id));
            Json sz = Json::array(); sz.push(Json::number(int64_t(fw))); sz.push(Json::number(int64_t(fh)));
            r.set("size", sz);
            if (const Json& rc = p["rect"]; rc.isArray() && rc.size() == 4) {
                const int x0 = std::max<int>(0, int(rc[size_t(0)].integer())), z0 = std::max<int>(0, int(rc[size_t(1)].integer()));
                const int x1 = std::min<int>(fw, x0 + std::min<int>(64, int(rc[size_t(2)].integer()))), z1 = std::min<int>(fh, z0 + std::min<int>(64, int(rc[size_t(3)].integer())));
                Json rows = Json::array();
                for (int z = z0; z < z1; ++z) {
                    Json row = Json::array();
                    for (int x = x0; x < x1; ++x) row.push(num(d, v[size_t(layer) * cells + size_t(z * fw + x)]));
                    rows.push(row);
                }
                r.set("rows", rows);
                return r;
            }
            const int step = std::max(1, (fw + 95) / 96);
            const double lo = units(d, d.def), hi = std::max(lo + 1e-9, d.max != INT32_MAX ? units(d, d.max) : lo + 100);
            std::string ascii;
            for (int z = 0; z < fh; z += step) {
                for (int x = 0; x < fw; x += step) {
                    int32_t best = d.def;
                    for (int dz = 0; dz < step && z + dz < fh; ++dz) for (int dx = 0; dx < step && x + dx < fw; ++dx)
                        best = std::max(best, v[size_t(layer) * cells + size_t((z + dz) * fw + x + dx)]);
                    if (best == d.def) { ascii += '.'; continue; }
                    const double t = (units(d, best) - lo) / (hi - lo);
                    ascii += t >= 0.999 ? '#' : char('1' + std::clamp(int(t * 9), 0, 8));
                }
                ascii += '\n';
            }
            r.set("ascii", str(ascii));
            return r;
        });
    add("field_paint",
        "Paint initial values of a grid field on the open map (saved in the map's \"gridFields\".<id>.values as RLE; undoable). "
        "x, z (map cells) + radius (map cells, 0 = one field cell) or rect [x, z, w, h] (map cells); value in field units "
        "(fixed fields: decimals); mode set | add | max | erase (back to the default). player = layer of per-player fields.",
        R"({"properties": {"id": {"type": "string"}, "x": {"type": "integer"}, "z": {"type": "integer"}, "radius": {"type": "integer"},
            "rect": {"type": "array", "items": {"type": "integer"}}, "value": {"type": "number"},
            "mode": {"type": "string", "enum": ["set", "add", "max", "erase"]}, "player": {"type": "integer"}}, "required": ["id"]})",
        [w](const Json& p) {
            needProject(*w);
            const std::string ms = p.getStr("mode", "set");
            const int mode = ms == "add" ? 1 : ms == "max" ? 2 : ms == "erase" ? 3 : 0;
            if (ms != "set" && ms != "add" && ms != "max" && ms != "erase") throw std::runtime_error("mode: set, add, max or erase");
            const double value = p["value"].toDouble(0);
            const int player = int(p.getInt("player", 1));
            w->map.beginEdit();
            int n = 0;
            std::string err;
            if (const Json& rc = p["rect"]; rc.isArray() && rc.size() == 4) {
                const int rect[4] = {int(rc[size_t(0)].integer()), int(rc[size_t(1)].integer()), std::max<int>(1, int(rc[size_t(2)].integer())),
                                     std::max<int>(1, int(rc[size_t(3)].integer()))};
                n = paintMapField(w->map, w->project.rules, p.getStr("id"), 0, 0, 0, value, mode, player, &err, rect);
                if (n < 0) throw std::runtime_error(err);
            } else {
                n = paintMapField(w->map, w->project.rules, p.getStr("id"), int(p.getInt("x")), int(p.getInt("z")), int(p.getInt("radius", 0)), value, mode, player, &err);
                if (n < 0) throw std::runtime_error(err);
            }
            const int x = int(p.getInt("x")), z = int(p.getInt("z")), rr = int(p.getInt("radius", 0));
            w->note({Change::Cells, x - rr, z - rr, x + rr, z + rr, -1, "Поле " + p.getStr("id") + ": " + std::to_string(n) + " клеток"});
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("cells", Json::number(int64_t(n)));
            r.set("summary", str("Поле " + p.getStr("id") + ": изменено клеток " + std::to_string(n)));
            return r;
        });
    add("field_set_rules",
        "Add / replace / delete a grid field definition (docs/fields.md): target 'rules' (default) writes rules.json \"fields\".<id>, "
        "target 'map' writes the open map's \"gridFields\".<id> parameters (overrides of a rules field, or a map-only field; "
        "initial values are kept). 'field' = the definition {type, resolution, default, min, max, perPlayer, kernels, effects, view}; "
        "merge=true merges keys into the existing entry; delete=true removes it. Validated against the game's rules before saving.",
        R"({"properties": {"id": {"type": "string"}, "field": {"type": "object"}, "target": {"type": "string", "enum": ["rules", "map"]},
            "merge": {"type": "boolean"}, "delete": {"type": "boolean"}}, "required": ["id"]})",
        [w](const Json& p) {
            needProject(*w);
            const std::string id = p.getStr("id");
            if (id.empty()) throw std::runtime_error("id expected");
            const bool toMap = p.getStr("target", "rules") == "map";
            const bool del = p.getBool("delete");
            const Json& f = p["field"];
            if (!del && !f.isObject()) throw std::runtime_error("pass 'field' (object) or delete=true");
            auto merged = [&](const Json* old) {
                Json e = old && p.getBool("merge") ? *old : Json::object();
                if (f.isObject()) for (auto& [k, v] : f.members()) if (!(toMap && k == "values")) e.set(k, v);
                if (toMap && old && old->has("values")) e.set("values", (*old)["values"]);
                if (toMap && old && old->has("fill")) e.set("fill", (*old)["fill"]);
                return e;
            };
            if (toMap) {
                w->map.beginEdit();
                Json& g = w->map.data.extra.member("gridFields");
                if (del) g.erase(id);
                else { const Json* old = g.find(id); Json e = merged(old); g.set(id, e); }
                if (g.size() == 0) w->map.data.extra.erase("gridFields");
                mapFieldDefs(w->project.rules, w->map);   // validate (throws: the snapshot is still there to undo)
                w->map.touched();
                w->note({Change::Map, 0, 0, -1, -1, -1, "Поле карты: " + id});
            } else {
                Json rules = w->project.rulesJson;
                Json& fields = rules.member("fields");
                if (del) fields.erase(id);
                else { const Json* old = fields.find(id); Json e = merged(old); fields.set(id, e); }
                if (fields.size() == 0) rules.erase("fields");
                game::loadRules(rules);   // throws a readable error, nothing saved
                w->project.rulesJson = rules;
                std::string err;
                if (!w->project.saveRules(&err)) throw std::runtime_error(err);
                w->project.reloadRules();
                w->note({Change::Rules, 0, 0, -1, -1, -1, "Поле: " + id});
            }
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("summary", str(std::string(del ? "Поле удалено: " : "Поле сохранено: ") + id + (toMap ? " (карта)" : " (rules.json)")));
            return r;
        });
}

} // namespace rts::editor
