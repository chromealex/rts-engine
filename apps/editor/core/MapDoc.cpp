#include "MapDoc.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <set>
#include "engine/sim/Simulation.h"
#include "modules/rts/RtsModule.h"
#include <filesystem>
#include <iterator>

namespace rts::editor {

namespace {
// keys the structured part owns; everything else round-trips through `extra`
const char* kOwnedKeys[] = {"name", "size", "roadsOnly", "players", "terrain", "fields", "regions", "entities", "roads", "world"};
bool owned(const std::string& k) {
    for (auto* o : kOwnedKeys) if (k == o) return true;
    return false;
}
float jf(const Json& j, float def = 0) { return j.isNumber() ? float(j.toDouble()) : def; }
} // namespace

void MapDoc::ensureLayers() {
    const size_t n = size_t(data.w * data.h);
    data.cells.resize(n, '.');
    data.levels.resize(n, 0);
    data.heights.resize(n, 0);
    data.fieldRes.resize(n, -1);
    data.fieldAmt.resize(n, 0);
    data.regions.resize(n, '.');
}

void MapDoc::reset(int w, int h) {
    data = MapData{};
    data.w = w; data.h = h;
    ensureLayers();
    path.clear();
    dirty = false;
    ++revision;
    undo_.clear(); redo_.clear();
}

std::string MapDoc::cornersPath(const std::string& mapFile, const std::string& rel) {
    namespace fs = std::filesystem;
    fs::path r(rel);
    if (r.is_absolute()) return rel;
    fs::path dir = fs::path(mapFile).parent_path();
    // "maps/x.r16" is relative to the game folder; a map outside maps/ keeps it next to itself
    if (dir.filename() == "maps" && !rel.empty() && rel.rfind("maps/", 0) == 0) return (dir.parent_path() / r).string();
    return (dir / r.filename()).string();
}

std::string MapDoc::cornersFileName() const {
    if (!data.cornersFile.empty()) return data.cornersFile;
    std::string stem = path.empty() ? "map" : std::filesystem::path(path).stem().string();
    return "maps/" + stem + ".r16";
}

bool MapDoc::load(const std::string& file, std::string* err) {
    try {
        Json j = Json::parseFile(file);
        auto read = [&](const std::string& rel, std::string& out) {
            std::ifstream f(cornersPath(file, rel), std::ios::binary);
            if (!f) return false;
            out.assign(std::istreambuf_iterator<char>(f), {});
            return true;
        };
        if (!game::applyCornersFile(j, read, err)) return false;
        if (!loadJson(j, err)) return false;
        path = file;
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

bool MapDoc::loadJson(const Json& j, std::string* err) {
    try {
        MapData d;
        d.name = j.getStr("name", "Untitled");
        d.w = std::clamp(int(j["size"][0].integer(64)), 4, game::kMaxMapSize);
        d.h = std::clamp(int(j["size"][1].integer(48)), 4, game::kMaxMapSize);
        d.roadsOnly = j.getBool("roadsOnly", false);
        const size_t n = size_t(d.w * d.h);
        d.cells.assign(n, '.'); d.levels.assign(n, 0); d.heights.assign(n, 0);
        d.fieldRes.assign(n, -1); d.fieldAmt.assign(n, 0); d.regions.assign(n, '.');
        const Json& t = j["terrain"];
        for (size_t y = 0; y < t["rows"].size() && int(y) < d.h; ++y) {
            std::string r = t["rows"][y].str();
            for (size_t x = 0; x < r.size() && int(x) < d.w; ++x) d.cells[d.idx(int(x), int(y))] = r[x];
        }
        for (auto& r : t["rects"].items()) { // legacy rectangles are baked into the rows
            std::string ty = r.getStr("type", ".");
            char c = ty == "water" ? '~' : ty == "blocked" ? '#' : ty == "nobuild" ? ',' : ty == "rock" ? 'R' : ty == "ramp" ? '/' : ty.size() == 1 ? ty[0] : '.';
            int x0 = int(r["rect"][0].integer()), y0 = int(r["rect"][1].integer()), rw = int(r["rect"][2].integer()), rh = int(r["rect"][3].integer());
            for (int y = y0; y < y0 + rh; ++y) for (int x = x0; x < x0 + rw; ++x) if (d.inside(x, y)) d.cells[d.idx(x, y)] = c;
        }
        for (size_t y = 0; y < t["levels"].size() && int(y) < d.h; ++y) {
            std::string r = t["levels"][y].str();
            for (size_t x = 0; x < r.size() && int(x) < d.w; ++x) if (r[x] >= '0' && r[x] <= '9') d.levels[d.idx(int(x), int(y))] = int8_t(r[x] - '0');
        }
        for (size_t i = 0; i < t["heights"].size() && i < n; ++i) d.heights[i] = int16_t(t["heights"][i].integer());
        if (t["corners"].size() > 0) {
            d.corners.assign(size_t((d.w + 1) * (d.h + 1)), 0);
            for (size_t i = 0; i < t["corners"].size() && i < d.corners.size(); ++i) d.corners[i] = int16_t(t["corners"][i].integer());
        } else if (t.has("cornersFile")) {
            d.cornersFile = t.getStr("cornersFile");
            std::string cerr;  // load() attached the bytes (cornersBlob); without them the heightfield is dropped
            if (!t["cornersBlob"].isString()) throw std::runtime_error("terrain.cornersFile '" + d.cornersFile + "' was not loaded");
            if (!game::decodeCorners(t, d.w, d.h, d.corners, &cerr)) throw std::runtime_error(cerr);
        }
        // fields: digit rows per resource, or legacy circles (converted to digits)
        for (auto& f : j["fields"].items()) {
            std::string res = f.getStr("resource");
            auto it = std::find(d.fieldResources.begin(), d.fieldResources.end(), res);
            int ri = int(it - d.fieldResources.begin());
            if (it == d.fieldResources.end()) d.fieldResources.push_back(res);
            if (f.has("rows")) {
                for (size_t y = 0; y < f["rows"].size() && int(y) < d.h; ++y) {
                    std::string r = f["rows"][y].str();
                    for (size_t x = 0; x < r.size() && int(x) < d.w; ++x)
                        if (r[x] >= '1' && r[x] <= '9') { d.fieldRes[d.idx(int(x), int(y))] = int8_t(ri); d.fieldAmt[d.idx(int(x), int(y))] = uint8_t(r[x] - '0'); }
                }
            } else {
                int cx = int(jf(f["center"][0])), cy = int(jf(f["center"][f["center"].size() >= 3 ? 2 : 1])), rad = int(f.getInt("radius", 3));
                for (int y = cy - rad; y <= cy + rad; ++y)
                    for (int x = cx - rad; x <= cx + rad; ++x) {
                        int d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                        if (!d.inside(x, y) || d2 > rad * rad) continue;
                        d.fieldRes[d.idx(x, y)] = int8_t(ri);
                        d.fieldAmt[d.idx(x, y)] = uint8_t(std::clamp(9 - d2 * 6 / std::max(1, rad * rad), 3, 9));
                    }
            }
        }
        if (const Json& rg = j["regions"]; rg.isObject()) {
            for (size_t y = 0; y < rg["rows"].size() && int(y) < d.h; ++y) {
                std::string r = rg["rows"][y].str();
                for (size_t x = 0; x < r.size() && int(x) < d.w; ++x) d.regions[d.idx(int(x), int(y))] = r[x] == ' ' ? '.' : r[x];
            }
            for (auto& [k, v] : rg["names"].members()) if (!k.empty()) d.regionNames[k[0]] = v.str();
        }
        d.players.clear();
        for (auto& p : j["players"].items()) {
            DocPlayer dp;
            dp.name = p.getStr("name", "Player");
            dp.team = int(p.getInt("team", int64_t(d.players.size() + 1)));
            dp.faction = p.getStr("faction");
            if (p["start"].isObject()) { dp.hasStart = true; dp.startAt = p["start"]; dp.startX = jf(p["start"]["x"]); dp.startZ = jf(p["start"]["y"]); }
            else if (p.has("start")) { dp.hasStart = true; dp.startX = jf(p["start"][0]); dp.startZ = jf(p["start"][p["start"].size() >= 3 ? 2 : 1]); }
            dp.waves = p.getBool("waves", false);
            for (auto& [r, v] : p["resources"].members()) dp.resources.push_back({r, jf(v)});
            for (auto& [k, v] : p.members())
                if (k != "name" && k != "team" && k != "faction" && k != "start" && k != "waves" && k != "resources") dp.extra.set(k, v);
            d.players.push_back(dp);
        }
        for (auto& e : j["entities"].items()) {
            DocEntity de;
            de.id = e.getStr("id");
            de.type = e.getStr("type");
            const Json& p = e["pos"];
            de.x = jf(p[0]);
            de.z = jf(p[p.size() >= 3 ? 2 : 1]);
            if (e["at"].isObject()) { de.at = e["at"]; de.x = jf(de.at["x"]); de.z = jf(de.at["y"]); }
            de.owner = int(e.getInt("owner", 0));
            de.garrison = e.has("garrison") ? int(e.getInt("garrison")) : -1;
            de.amount = e.has("amount") ? jf(e["amount"]) : -1;
            for (auto& [k, v] : e.members())
                if (k != "id" && k != "type" && k != "pos" && k != "at" && k != "owner" && k != "garrison" && k != "amount") de.extra.set(k, v);
            d.entities.push_back(de);
        }
        auto find = [&](const std::string& id) {
            for (size_t i = 0; i < d.entities.size(); ++i) if (d.entities[i].id == id) return int(i);
            return -1;
        };
        for (auto& r : j["roads"].items()) {
            int a = find(r[0].str()), b = find(r[1].str());
            if (a >= 0 && b >= 0 && a != b) d.roads.emplace_back(std::min(a, b), std::max(a, b));
        }
        d.extra = Json::object();
        for (auto& [k, v] : j.members()) if (!owned(k)) d.extra.set(k, v);
        if (j["world"].isObject()) loadWorld(j["world"], d);
        data = std::move(d);
        dirty = false;
        ++revision;
        undo_.clear(); redo_.clear();
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

Json MapDoc::toJson() const {
    const auto& d = data;
    Json j = Json::object();
    j.set("name", Json::string(d.name));
    Json size = Json::array();
    size.push(Json::number(int64_t(d.w))); size.push(Json::number(int64_t(d.h)));
    if (!d.planet()) j.set("size", size);
    if (d.roadsOnly) j.set("roadsOnly", Json::fromBool(true));
    Json players = Json::array();
    for (auto& p : d.players) {
        Json pj = Json::object();
        pj.set("name", Json::string(p.name));
        pj.set("team", Json::number(int64_t(p.team)));
        if (!p.faction.empty()) pj.set("faction", Json::string(p.faction));
        if (p.hasStart && p.startAt.isObject()) pj.set("start", p.startAt);
        else if (p.hasStart) { Json s = Json::array(); s.push(Json::number(double(p.startX), 2)); s.push(Json::number(double(p.startZ), 2)); pj.set("start", s); }
        if (p.waves) pj.set("waves", Json::fromBool(true));
        if (!p.resources.empty()) {
            Json r = Json::object();
            for (auto& [k, v] : p.resources) r.set(k, Json::number(double(v), 2));
            pj.set("resources", r);
        }
        for (auto& [k, v] : p.extra.members()) pj.set(k, v);
        players.push(pj);
    }
    j.set("players", players);
    Json terrain = Json::object();
    Json rows = Json::array();
    bool flat = std::all_of(d.cells.begin(), d.cells.end(), [](char c) { return c == '.'; });
    if (!flat) for (int y = 0; y < d.h; ++y) rows.push(Json::string(std::string(d.cells.begin() + y * d.w, d.cells.begin() + (y + 1) * d.w)));
    terrain.set("rows", rows);
    if (std::any_of(d.levels.begin(), d.levels.end(), [](int8_t v) { return v != 0; })) {
        Json lv = Json::array();
        for (int y = 0; y < d.h; ++y) {
            std::string r(size_t(d.w), '0');
            for (int x = 0; x < d.w; ++x) r[size_t(x)] = char('0' + std::clamp<int>(d.levels[d.idx(x, y)], 0, 9));
            lv.push(Json::string(r));
        }
        terrain.set("levels", lv);
    }
    if (std::any_of(d.heights.begin(), d.heights.end(), [](int16_t v) { return v != 0; })) {
        Json hs = Json::array();
        for (auto v : d.heights) hs.push(Json::number(int64_t(v)));
        terrain.set("heights", hs);
    }
    if (d.corners.size() > kInlineCornersMax) {
        terrain.set("cornersFile", Json::string(cornersFileName()));
        terrain.set("cornersBlob", Json::string(game::encodeCorners(d.corners)));
    } else if (!d.corners.empty()) {
        Json cs = Json::array();
        for (auto v : d.corners) cs.push(Json::number(int64_t(v)));
        terrain.set("corners", cs);
    }
    if (d.planet()) j.set("world", worldJson());
    else j.set("terrain", terrain);
    Json fields = Json::array();
    for (size_t ri = 0; ri < d.fieldResources.size(); ++ri) {
        bool any = false;
        Json fr = Json::array();
        for (int y = 0; y < d.h; ++y) {
            std::string r(size_t(d.w), '0');
            for (int x = 0; x < d.w; ++x)
                if (d.fieldRes[d.idx(x, y)] == int8_t(ri) && d.fieldAmt[d.idx(x, y)] > 0) { r[size_t(x)] = char('0' + d.fieldAmt[d.idx(x, y)]); any = true; }
            fr.push(Json::string(r));
        }
        if (!any) continue;
        Json f = Json::object();
        f.set("resource", Json::string(d.fieldResources[ri]));
        f.set("rows", fr);
        fields.push(f);
    }
    if (fields.size() && !d.planet()) j.set("fields", fields);
    if (std::any_of(d.regions.begin(), d.regions.end(), [](char c) { return c != '.'; })) {
        Json rg = Json::object(), rr = Json::array();
        for (int y = 0; y < d.h; ++y) rr.push(Json::string(std::string(d.regions.begin() + y * d.w, d.regions.begin() + (y + 1) * d.w)));
        rg.set("rows", rr);
        Json names = Json::object();
        for (auto& [c, nm] : d.regionNames) names.set(std::string(1, c), Json::string(nm));
        rg.set("names", names);
        j.set("regions", rg);
    }
    Json ents = Json::array();
    for (auto& e : d.entities) {
        Json ej = Json::object();
        if (!e.id.empty()) ej.set("id", Json::string(e.id));
        ej.set("type", Json::string(e.type));
        ej.set("owner", Json::number(int64_t(e.owner)));
        if (e.at.isObject()) ej.set("at", e.at);
        else {
            Json pos = Json::array();
            pos.push(Json::number(double(e.x), 2)); pos.push(Json::number(double(e.z), 2));
            ej.set("pos", pos);
        }
        if (e.garrison >= 0) ej.set("garrison", Json::number(int64_t(e.garrison)));
        if (e.amount >= 0) ej.set("amount", Json::number(double(e.amount), 1));
        for (auto& [k, v] : e.extra.members()) ej.set(k, v);
        ents.push(ej);
    }
    j.set("entities", ents);
    if (!d.roads.empty()) {
        Json roads = Json::array();
        for (auto [a, b] : d.roads) {
            Json r = Json::array();
            r.push(Json::string(d.entities[size_t(a)].id)); r.push(Json::string(d.entities[size_t(b)].id));
            roads.push(r);
        }
        j.set("roads", roads);
    }
    for (auto& [k, v] : d.extra.members()) j.set(k, v);
    return j;
}

bool MapDoc::save(const std::string& file, std::string* err) {
    for (auto [a, b] : data.roads) // roads need ids on their endpoints
        for (int i : {a, b}) if (data.entities[size_t(i)].id.empty()) data.entities[size_t(i)].id = uniqueId(data.entities[size_t(i)].type);
    const std::string oldPath = path, oldCorners = data.cornersFile;
    path = file;  // the default corners file name follows the map file
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!oldPath.empty() && !fs::equivalent(oldPath, file, ec)) data.cornersFile.clear(); // "save as": own .r16
    Json j = toJson();
    if (Json* t = j.find("terrain"); t && t->has("cornersBlob")) {
        const std::string cf = cornersPath(file, t->getStr("cornersFile"));
        std::ofstream c(cf, std::ios::binary);
        const std::string& blob = (*t)["cornersBlob"].str();
        c.write(blob.data(), std::streamsize(blob.size()));
        if (!c) { if (err) *err = "cannot write " + cf; path = oldPath; data.cornersFile = oldCorners; return false; }
        t->erase("cornersBlob");
        data.cornersFile = t->getStr("cornersFile");
    }
    if (Json* wj = j.find("world"); wj && wj->find("bodies"))
        for (size_t bi = 0; bi < wj->find("bodies")->size(); ++bi) {
            Json* t = wj->find("bodies")->at(bi).find("terrain");
            if (!t || !t->has("cornersBlob")) continue;
            const std::string cf = cornersPath(file, t->getStr("cornersFile"));
            std::ofstream c(cf, std::ios::binary);
            const std::string& blob = (*t)["cornersBlob"].str();
            c.write(blob.data(), std::streamsize(blob.size()));
            if (!c) { if (err) *err = "cannot write " + cf; path = oldPath; return false; }
            t->erase("cornersBlob");
            if (bi < data.bodies.size()) data.bodies[bi].cornersFile = t->getStr("cornersFile");
        }
    std::ofstream f(file, std::ios::binary);
    if (!f) { if (err) *err = "cannot write " + file; path = oldPath; return false; }
    f << j.dump() << "\n";
    if (!f) { if (err) *err = "write failed"; return false; }
    dirty = false;
    return true;
}

void MapDoc::beginEdit() {
    ensureLayers(); // a map that was never created/loaded still has layers sized to w*h
    undo_.push_back(data);
    // whole-document snapshots: keep ~256 MB at most (a 2048x2048 map is ~36 MB per snapshot)
    const size_t snap = size_t(data.w * data.h) * 8 + data.corners.size() * 2 + 1024;
    const size_t keep = std::clamp<size_t>((size_t(256) << 20) / snap, 4, 200);
    while (undo_.size() > keep) undo_.erase(undo_.begin());
    redo_.clear();
    touched();
}
void MapDoc::undo() { if (undo_.empty()) return; redo_.push_back(data); data = undo_.back(); undo_.pop_back(); touched(); }
void MapDoc::redo() { if (redo_.empty()) return; undo_.push_back(data); data = redo_.back(); redo_.pop_back(); touched(); }

std::string MapDoc::uniqueId(const std::string& base) const {
    for (int i = 1;; ++i) {
        std::string id = base + "_" + std::to_string(i);
        if (findEntity(id) < 0) return id;
    }
}

int MapDoc::findEntity(const std::string& id) const {
    for (size_t i = 0; i < data.entities.size(); ++i) if (data.entities[i].id == id) return int(i);
    return -1;
}

int MapDoc::addEntity(const DocEntity& e) {
    beginEdit();
    data.entities.push_back(e);
    return int(data.entities.size() - 1);
}

void MapDoc::removeEntity(int i) {
    if (i < 0 || i >= int(data.entities.size())) return;
    beginEdit();
    data.entities.erase(data.entities.begin() + i);
    std::vector<std::pair<int, int>> roads;
    for (auto [a, b] : data.roads) if (a != i && b != i) roads.emplace_back(a > i ? a - 1 : a, b > i ? b - 1 : b);
    data.roads = std::move(roads);
}

bool MapDoc::toggleRoad(int a, int b) {
    if (a == b || a < 0 || b < 0 || a >= int(data.entities.size()) || b >= int(data.entities.size())) return false;
    beginEdit();
    auto key = std::make_pair(std::min(a, b), std::max(a, b));
    auto it = std::find(data.roads.begin(), data.roads.end(), key);
    if (it != data.roads.end()) { data.roads.erase(it); return false; }
    data.roads.push_back(key);
    for (int i : {a, b}) if (data.entities[size_t(i)].id.empty()) data.entities[size_t(i)].id = uniqueId(data.entities[size_t(i)].type);
    return true;
}

void MapDoc::removeRoad(size_t i) {
    if (i >= data.roads.size()) return;
    beginEdit();
    data.roads.erase(data.roads.begin() + long(i));
}

void MapDoc::resize(int w, int h) {
    w = std::clamp(w, 8, game::kMaxMapSize); h = std::clamp(h, 8, game::kMaxMapSize);
    beginEdit();
    MapData& d = data;
    auto remap = [&](auto& v, auto fill) {
        std::remove_reference_t<decltype(v)> out(size_t(w * h), fill);
        for (int y = 0; y < std::min(h, d.h); ++y)
            for (int x = 0; x < std::min(w, d.w); ++x) out[size_t(y * w + x)] = v[size_t(y * d.w + x)];
        v.swap(out);
    };
    remap(d.cells, '.'); remap(d.levels, int8_t(0)); remap(d.heights, int16_t(0));
    remap(d.fieldRes, int8_t(-1)); remap(d.fieldAmt, uint8_t(0)); remap(d.regions, '.');
    if (!d.corners.empty()) {
        std::vector<int16_t> out(size_t((w + 1) * (h + 1)), 0);
        // keep the overlap; new rows / columns continue the old edge height (no cliff at the seam)
        for (int y = 0; y <= h; ++y)
            for (int x = 0; x <= w; ++x) out[size_t(y * (w + 1) + x)] = d.corners[size_t(std::min(y, d.h) * (d.w + 1) + std::min(x, d.w))];
        d.corners.swap(out);
    }
    d.w = w; d.h = h;
}

void MapDoc::setCorners(bool on) {
    if (on == !data.corners.empty()) return;
    beginEdit();
    if (on) data.corners.assign(size_t((data.w + 1) * (data.h + 1)), 0);
    else { data.corners.clear(); data.cornersFile.clear(); }
}

namespace {
template <class Fn> void brush(const MapData& d, int cx, int cy, int r, Fn&& fn) {
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x)
            if (d.inside(x, y) && (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r + r) fn(d.idx(x, y));
}
template <class Fn> void rect(const MapData& d, int x0, int y0, int x1, int y1, Fn&& fn) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    for (int y = std::max(0, y0); y <= std::min(d.h - 1, y1); ++y)
        for (int x = std::max(0, x0); x <= std::min(d.w - 1, x1); ++x) fn(d.idx(x, y));
}
} // namespace

void MapDoc::paint(int cx, int cy, int r, char c) { brush(data, cx, cy, r, [&](size_t i) { data.cells[i] = c; }); touched(); }
void MapDoc::paintRect(int x0, int y0, int x1, int y1, char c) { rect(data, x0, y0, x1, y1, [&](size_t i) { data.cells[i] = c; }); touched(); }
void MapDoc::setLevel(int cx, int cy, int r, int lv) { brush(data, cx, cy, r, [&](size_t i) { data.levels[i] = int8_t(std::clamp(lv, 0, 9)); }); touched(); }
void MapDoc::setLevelRect(int x0, int y0, int x1, int y1, int lv) { rect(data, x0, y0, x1, y1, [&](size_t i) { data.levels[i] = int8_t(std::clamp(lv, 0, 9)); }); touched(); }
void MapDoc::paintRegion(int cx, int cy, int r, char c) { brush(data, cx, cy, r, [&](size_t i) { data.regions[i] = c; }); touched(); }
void MapDoc::paintRegionRect(int x0, int y0, int x1, int y1, char c) { rect(data, x0, y0, x1, y1, [&](size_t i) { data.regions[i] = c; }); touched(); }

void MapDoc::paintField(int cx, int cy, int r, const std::string& res, int amount) {
    int ri = -1;
    if (amount > 0) {
        auto it = std::find(data.fieldResources.begin(), data.fieldResources.end(), res);
        ri = int(it - data.fieldResources.begin());
        if (it == data.fieldResources.end()) data.fieldResources.push_back(res);
    }
    brush(data, cx, cy, r, [&](size_t i) {
        if (amount <= 0) { data.fieldRes[i] = -1; data.fieldAmt[i] = 0; return; }
        data.fieldRes[i] = int8_t(ri);
        data.fieldAmt[i] = uint8_t(std::clamp(amount, 1, 9));
    });
    touched();
}

void MapDoc::raise(int cx, int cy, int r, int delta) {
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x) {
            if (!data.inside(x, y)) continue;
            float dd = std::sqrt(float((x - cx) * (x - cx) + (y - cy) * (y - cy)));
            if (dd > r + 0.5f) continue;
            float k = 1.0f - dd / float(r + 1);
            auto& hv = data.heights[data.idx(x, y)];
            hv = int16_t(std::clamp(int(hv) + int(std::lround(delta * k)), -160, 320));
        }
    touched();
}

void MapDoc::autoRamps() {
    // Every ramp cell must touch both levels; extend lone ramp cells across the cliff edge.
    std::vector<char> c = data.cells;
    for (int y = 0; y < data.h; ++y)
        for (int x = 0; x < data.w; ++x) {
            if (data.cell(x, y) != '/') continue;
            for (auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}})
                if (data.inside(x + dx, y + dy) && std::abs(data.level(x + dx, y + dy) - data.level(x, y)) == 1 && data.cell(x + dx, y + dy) == '.')
                    c[data.idx(x + dx, y + dy)] = '/';
        }
    data.cells.swap(c);
    touched();
}

void MapDoc::mirror(bool point) {
    beginEdit();
    auto& d = data;
    const int W = d.w, H = d.h;
    for (int y = 0; y < H; ++y)
        for (int x = W / 2; x < W; ++x) {
            size_t to = d.idx(x, y), from = d.idx(W - 1 - x, point ? H - 1 - y : y);
            d.cells[to] = d.cells[from]; d.levels[to] = d.levels[from]; d.heights[to] = d.heights[from];
            d.fieldRes[to] = d.fieldRes[from]; d.fieldAmt[to] = d.fieldAmt[from];
        }
    std::vector<DocEntity> left;
    std::vector<int> map(d.entities.size(), -1);
    for (size_t i = 0; i < d.entities.size(); ++i) if (d.entities[i].x < W * 0.5f) { map[i] = int(left.size()); left.push_back(d.entities[i]); }
    std::vector<std::pair<int, int>> roads;
    for (auto [a, b] : d.roads) if (map[size_t(a)] >= 0 && map[size_t(b)] >= 0) roads.push_back({map[size_t(a)], map[size_t(b)]});
    const size_t n = left.size();
    std::vector<int> twin(n);
    for (size_t i = 0; i < n; ++i) {
        DocEntity m = left[i];
        m.x = float(W) - m.x;
        if (point) m.z = float(H) - m.z;
        if (m.owner == 1) m.owner = 2; else if (m.owner == 2) m.owner = 1;
        if (!m.id.empty()) m.id += "_m";
        twin[i] = int(left.size());
        left.push_back(m);
    }
    size_t nr = roads.size();
    for (size_t i = 0; i < nr; ++i) roads.push_back({twin[size_t(roads[i].first)], twin[size_t(roads[i].second)]});
    d.entities = std::move(left);
    d.roads = std::move(roads);
    if (d.players.size() >= 2 && d.players[0].hasStart) {
        d.players[1].hasStart = true;
        d.players[1].startX = float(W) - d.players[0].startX;
        d.players[1].startZ = point ? float(H) - d.players[0].startZ : d.players[0].startZ;
    }
}
void MapDoc::mirrorHorizontal() { mirror(false); }
void MapDoc::mirrorPoint() { mirror(true); }

std::string MapDoc::ascii(int maxW) const {
    const auto& d = data;
    int step = std::max(1, (d.w + maxW - 1) / maxW);
    std::vector<std::string> rows;
    for (int y = 0; y < d.h; y += step) {
        std::string r;
        for (int x = 0; x < d.w; x += step) {
            size_t i = d.idx(x, y);
            char c = d.cells[i];
            if (c == '.' && d.levels[i] > 0) c = char('0' + d.levels[i]);
            if (d.fieldRes[i] >= 0 && d.fieldAmt[i] > 0) c = '$';
            r += c;
        }
        rows.push_back(r);
    }
    for (size_t k = 0; k < d.entities.size(); ++k) {
        const auto& e = d.entities[k];
        int x = int(e.x) / step, y = int(e.z) / step;
        if (y >= 0 && y < int(rows.size()) && x >= 0 && x < int(rows[size_t(y)].size()))
            rows[size_t(y)][size_t(x)] = e.owner > 0 && e.owner <= 26 ? char('A' + (e.owner - 1)) : e.owner < 0 ? 'x' : 'n';
    }
    std::string out = "legend: . ground ~ water # blocked , no-build = shallow / ramp R rock 1-9 cliff level $ resource field; "
                      "A/B/C.. objects of player 1/2/3.., n neutral, x hostile creeps; scale 1:" + std::to_string(step) + "\n";
    for (auto& r : rows) out += r + "\n";
    return out;
}

std::vector<Issue> MapDoc::validate(const game::Rules& rules, const Json& rulesJson) const {
    std::vector<Issue> out;
    const auto& d = data;
    if (d.planet()) validateWorld(rules, out);
    int real = 0;
    for (auto& p : d.players) if (!p.waves) ++real;
    if (real < 1 || d.players.size() < 2) out.push_back({true, "Нужно минимум 2 игрока"});
    std::set<std::string> ids;
    for (size_t i = 0; i < d.entities.size(); ++i) {
        const auto& e = d.entities[i];
        int ti = rules.findType(e.type);
        std::string nm = e.id.empty() ? e.type : e.id;
        if (ti < 0) { out.push_back({true, nm + ": неизвестный тип", int(i)}); continue; }
        if (!e.id.empty() && !ids.insert(e.id).second) out.push_back({true, "Повторяющийся id: " + e.id, int(i)});
        if (e.owner < -1 || e.owner > int(d.players.size())) out.push_back({true, nm + ": владелец вне диапазона", int(i)});
        if (d.planet()) continue;   // positions checked by validateWorld, terrain by the loader below
        if (e.x < 0 || e.z < 0 || e.x > d.w || e.z > d.h) out.push_back({true, nm + ": за пределами карты", int(i)});
        const auto& t = rules.types[size_t(ti)];
        char c = d.cell(int(std::floor(e.x)), int(std::floor(e.z)));
        bool naval = t.layer & game::kLayerNaval, air = t.layer & game::kLayerAir;
        if (!air && !d.roadsOnly && ((naval && c != '~' && c != '=') || (!naval && (c == '~' || c == '#'))))
            out.push_back({true, nm + ": стоит на непроходимой клетке", int(i)});
    }
    for (size_t p = 1; p <= d.players.size(); ++p) {
        if (d.players[p - 1].waves) continue;
        bool has = d.players[p - 1].hasStart;
        for (auto& e : d.entities) if (e.owner == int(p)) has = true;
        if (!has) out.push_back({true, "У игрока " + d.players[p - 1].name + " нет стартовых объектов или стартовой точки"});
        if (!d.players[p - 1].faction.empty() && rules.findFaction(d.players[p - 1].faction) < 0)
            out.push_back({true, "Игрок " + d.players[p - 1].name + ": неизвестная фракция " + d.players[p - 1].faction});
    }
    for (const auto& r : d.fieldResources)
        if (rules.findResource(r) < 0) out.push_back({true, "Поле ресурса: неизвестный ресурс " + r});
    for (size_t i = 0; i < d.entities.size() && !d.planet(); ++i) {
        int ti = rules.findType(d.entities[i].type);
        if (ti < 0 || !rules.types[size_t(ti)].structure) continue;
        for (size_t k = i + 1; k < d.entities.size(); ++k) {
            int tk = rules.findType(d.entities[k].type);
            if (tk < 0 || !rules.types[size_t(tk)].structure) continue;
            const auto& a = rules.types[size_t(ti)];
            const auto& b = rules.types[size_t(tk)];
            if (std::fabs(d.entities[i].x - d.entities[k].x) * 2 < a.footprintW + b.footprintW && std::fabs(d.entities[i].z - d.entities[k].z) * 2 < a.footprintH + b.footprintH)
                out.push_back({false, d.entities[i].type + " и " + d.entities[k].type + " перекрываются", int(i)});
        }
    }
    if (d.roadsOnly && d.roads.empty()) out.push_back({true, "Карта «только дороги», но дорог нет"});
    // heightfield terrain (rules terrain.model)
    if (rules.terrain.heightfield() && d.corners.empty() && !d.planet())
        out.push_back({false, "Правила: terrain.model = heightfield, но у карты нет высот углов (terrain.corners) - рельеф будет плоским"});
    if (!rules.terrain.heightfield() && !d.corners.empty() && !d.planet())
        out.push_back({false, "У карты есть высоты углов, но terrain.model не heightfield - рельеф только визуальный"});
    if (rules.terrain.heightfield() && !d.corners.empty() && d.corners.size() == size_t((d.w + 1) * (d.h + 1))) {
        game::MapGrid g;  // same slope math as the simulation
        g.init(d.w, d.h);
        g.corner = d.corners;
        g.slopeLimit[0] = game::MapGrid::slopeUnits(rules.terrain.maxSlopeGround);
        g.sandLimit = game::MapGrid::slopeUnits(rules.terrain.maxSlopeSand.raw < 0 ? rules.terrain.maxSlopeGround : rules.terrain.maxSlopeSand);
        g.buildLimit = game::MapGrid::slopeUnits(rules.terrain.buildSlope);
        for (size_t k = 0; k < rules.terrain.slopeClasses.size() && k < 15; ++k) g.slopeLimit[k + 1] = game::MapGrid::slopeUnits(rules.terrain.slopeClasses[k]);
        g.enableHeightfield(rules.levelHeight);
        for (size_t i = 0; i < d.entities.size(); ++i) {
            int ti = rules.findType(d.entities[i].type);
            if (ti < 0) continue;
            const auto& t = rules.types[size_t(ti)];
            int x = int(std::floor(d.entities[i].x)), y = int(std::floor(d.entities[i].z));
            if (!d.inside(x, y) || (t.layer & game::kLayerAir)) continue;
            std::string nm = d.entities[i].id.empty() ? d.entities[i].type : d.entities[i].id;
            if (t.structure) {
                int x0 = 0, y0 = 0;
                game::RtsModule::footprintAt(t, game::RtsModule::snapToFootprint(t, {Fixed::fromRaw(int64_t(std::lround(double(d.entities[i].x) * 65536))), Fixed{},
                                                                         Fixed::fromRaw(int64_t(std::lround(double(d.entities[i].z) * 65536)))}), x0, y0);
                bool steep = false;
                for (int yy = y0; yy < y0 + t.footprintH; ++yy)
                    for (int xx = x0; xx < x0 + t.footprintW; ++xx)
                        if (g.inside(xx, yy) && g.slope[size_t(g.index(xx, yy))] > g.buildLimit) steep = true;
                if (steep) out.push_back({false, nm + ": склон круче terrain.buildSlope под зданием", int(i)});
            } else if (!g.passable(x, y, t.pathLayer)) {
                out.push_back({true, nm + ": склон слишком крутой для этого юнита (maxSlope)", int(i)});
            }
        }
    }
    // the real loader is the final judge (waves, triggers, regions, factions, victory...)
    bool anyError = std::any_of(out.begin(), out.end(), [](const Issue& i) { return i.error; });
    if (!anyError) {
        try {
            SimConfig cfg;
            Simulation sim(cfg, std::make_unique<game::RtsModule>());
            sim.setup(rulesJson, toJson());
            for (int k = 0; k < 3; ++k) sim.step();
            if (d.planet()) {   // starts reachable: every pair of player key/first structures on one body by ground
                auto* m = dynamic_cast<game::RtsModule*>(&sim.module());
                auto& gs = m->state(sim);
                std::vector<std::pair<int, Vec3>> homes;   // player, position
                sim.world().each<game::Unit, Owner, Transform>([&](Entity, game::Unit& u, Owner& o, Transform& tr) {
                    if (o.player == 0 || o.player > d.players.size()) return;
                    for (auto& h : homes) if (h.first == o.player) return;
                    if (rules.types[u.type].layer & game::kLayerGround) homes.push_back({o.player, tr.pos});
                });
                for (size_t a = 0; a < homes.size(); ++a)
                    for (size_t b = a + 1; b < homes.size(); ++b) {
                        if (m->bodyOf(sim, homes[a].second) != m->bodyOf(sim, homes[b].second)) continue;
                        std::vector<Vec3> path;
                        if (!gs.grid.findPath(homes[a].second, homes[b].second, game::kLayerGround, path, 400000))
                            out.push_back({false, "Игроки " + std::to_string(homes[a].first) + " и " + std::to_string(homes[b].first) + " не связаны наземным путём (только орбита)"});
                    }
            }
        } catch (const std::exception& e) {
            out.push_back({true, std::string("Загрузчик: ") + e.what()});
        }
    }
    return out;
}

} // namespace rts::editor
