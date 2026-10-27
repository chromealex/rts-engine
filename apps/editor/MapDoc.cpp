#include "MapDoc.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>

namespace rts::editor {

namespace {
std::string num(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", double(v));
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}
std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if (c == '\n') o += "\\n"; else o += c; }
    return o;
}
} // namespace

void MapDoc::reset(int w, int h) {
    data = MapData{};
    data.w = w; data.h = h;
    data.cells.assign(size_t(w * h), '.');
    data.heights.assign(size_t(w * h), 0);
    path.clear();
    dirty = false;
    undo_.clear(); redo_.clear();
}

bool MapDoc::load(const std::string& file, std::string* err) {
    try {
        Json j = Json::parseFile(file);
        MapData d;
        d.name = j.getStr("name", "Untitled");
        d.w = int(j["size"][0].integer(64)); d.h = int(j["size"][1].integer(48));
        d.roadsOnly = j.getBool("roadsOnly", false);
        d.cells.assign(size_t(d.w * d.h), '.');
        d.heights.assign(size_t(d.w * d.h), 0);
        const Json& rows = j["terrain"]["rows"];
        for (size_t y = 0; y < rows.size() && int(y) < d.h; ++y) {
            std::string r = rows[y].str();
            for (size_t x = 0; x < r.size() && int(x) < d.w; ++x) d.cells[y * size_t(d.w) + x] = r[x];
        }
        const Json& hs = j["terrain"]["heights"];
        for (size_t i = 0; i < hs.size() && i < d.heights.size(); ++i) d.heights[i] = int16_t(hs[i].integer());
        d.players.clear();
        for (auto& p : j["players"].items()) {
            DocPlayer dp{p.getStr("name", "Player"), int(p.getInt("team", int64_t(d.players.size() + 1))), {}};
            for (auto& [r, v] : p["resources"].members()) dp.resources.push_back({r, float(v.fx().toDouble())});
            d.players.push_back(dp);
        }
        for (auto& e : j["entities"].items()) {
            DocEntity de;
            de.id = e.getStr("id");
            de.type = e.getStr("type");
            const Json& p = e["pos"];
            de.x = float(p[0].fx().toDouble());
            de.z = float(p[p.size() >= 3 ? 2 : 1].fx().toDouble());
            de.owner = int(e.getInt("owner", 0));
            de.garrison = e.has("garrison") ? int(e.getInt("garrison")) : -1;
            de.amount = e.has("amount") ? float(e.getFx("amount").toDouble()) : -1;
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
        data = std::move(d);
        path = file;
        dirty = false;
        undo_.clear(); redo_.clear();
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

std::string MapDoc::toJsonText() const {
    const auto& d = data;
    std::string s = "{\n";
    s += "  \"name\": \"" + esc(d.name) + "\",\n";
    s += "  \"size\": [" + std::to_string(d.w) + ", " + std::to_string(d.h) + "],\n";
    if (d.roadsOnly) s += "  \"roadsOnly\": true,\n";
    s += "  \"players\": [\n";
    for (size_t i = 0; i < d.players.size(); ++i) {
        const auto& p = d.players[i];
        s += "    { \"name\": \"" + esc(p.name) + "\", \"team\": " + std::to_string(p.team);
        if (!p.resources.empty()) {
            s += ", \"resources\": {";
            for (size_t k = 0; k < p.resources.size(); ++k) s += (k ? ", " : " ") + std::string("\"") + esc(p.resources[k].first) + "\": " + num(p.resources[k].second);
            s += " }";
        }
        s += " }" + std::string(i + 1 < d.players.size() ? ",\n" : "\n");
    }
    s += "  ],\n  \"terrain\": {\n    \"rows\": [\n";
    bool flat = std::all_of(d.cells.begin(), d.cells.end(), [](char c) { return c == '.'; });
    if (!flat)
        for (int y = 0; y < d.h; ++y) s += "      \"" + std::string(d.cells.begin() + y * d.w, d.cells.begin() + (y + 1) * d.w) + "\"" + (y + 1 < d.h ? ",\n" : "\n");
    s += "    ]";
    if (std::any_of(d.heights.begin(), d.heights.end(), [](int16_t v) { return v != 0; })) {
        s += ",\n    \"heights\": [";
        for (size_t i = 0; i < d.heights.size(); ++i) s += (i ? "," : "") + std::to_string(d.heights[i]);
        s += "]";
    }
    s += "\n  },\n  \"entities\": [\n";
    for (size_t i = 0; i < d.entities.size(); ++i) {
        const auto& e = d.entities[i];
        s += "    {";
        if (!e.id.empty()) s += " \"id\": \"" + esc(e.id) + "\",";
        s += " \"type\": \"" + esc(e.type) + "\", \"owner\": " + std::to_string(e.owner) + ", \"pos\": [" + num(e.x) + ", " + num(e.z) + "]";
        if (e.garrison >= 0) s += ", \"garrison\": " + std::to_string(e.garrison);
        if (e.amount >= 0) s += ", \"amount\": " + num(e.amount);
        s += " }" + std::string(i + 1 < d.entities.size() ? ",\n" : "\n");
    }
    s += "  ]";
    if (!d.roads.empty()) {
        s += ",\n  \"roads\": [\n";
        for (size_t i = 0; i < d.roads.size(); ++i)
            s += "    [\"" + esc(d.entities[size_t(d.roads[i].first)].id) + "\", \"" + esc(d.entities[size_t(d.roads[i].second)].id) + "\"]" + (i + 1 < d.roads.size() ? ",\n" : "\n");
        s += "  ]";
    }
    s += "\n}\n";
    return s;
}

bool MapDoc::save(const std::string& file, std::string* err) {
    // roads need ids on their endpoints
    for (auto [a, b] : data.roads)
        for (int i : {a, b}) if (data.entities[size_t(i)].id.empty()) data.entities[size_t(i)].id = uniqueId(data.entities[size_t(i)].type);
    std::ofstream f(file, std::ios::binary);
    if (!f) { if (err) *err = "cannot write " + file; return false; }
    f << toJsonText();
    if (!f) { if (err) *err = "write failed"; return false; }
    path = file;
    dirty = false;
    return true;
}

void MapDoc::beginEdit() {
    undo_.push_back(data);
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    redo_.clear();
    dirty = true;
}
void MapDoc::undo() { if (undo_.empty()) return; redo_.push_back(data); data = undo_.back(); undo_.pop_back(); dirty = true; }
void MapDoc::redo() { if (redo_.empty()) return; undo_.push_back(data); data = redo_.back(); redo_.pop_back(); dirty = true; }

std::string MapDoc::uniqueId(const std::string& base) const {
    for (int i = 1;; ++i) {
        std::string id = base + "_" + std::to_string(i);
        bool used = false;
        for (auto& e : data.entities) if (e.id == id) { used = true; break; }
        if (!used) return id;
    }
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
    if (a == b || a < 0 || b < 0) return false;
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
    w = std::clamp(w, 8, 512); h = std::clamp(h, 8, 512);
    beginEdit();
    std::vector<char> cells(size_t(w * h), '.');
    std::vector<int16_t> hs(size_t(w * h), 0);
    for (int y = 0; y < std::min(h, data.h); ++y)
        for (int x = 0; x < std::min(w, data.w); ++x) {
            cells[size_t(y * w + x)] = data.cells[size_t(y * data.w + x)];
            hs[size_t(y * w + x)] = data.heights[size_t(y * data.w + x)];
        }
    data.w = w; data.h = h; data.cells = std::move(cells); data.heights = std::move(hs);
}

void MapDoc::paint(int cx, int cy, int r, char c) {
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x)
            if (x >= 0 && y >= 0 && x < data.w && y < data.h && (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r + r)
                data.cells[size_t(y * data.w + x)] = c;
    dirty = true;
}

void MapDoc::raise(int cx, int cy, int r, int delta) {
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x) {
            if (x < 0 || y < 0 || x >= data.w || y >= data.h) continue;
            float d = std::sqrt(float((x - cx) * (x - cx) + (y - cy) * (y - cy)));
            if (d > r + 0.5f) continue;
            float k = 1.0f - d / float(r + 1);
            auto& hv = data.heights[size_t(y * data.w + x)];
            hv = int16_t(std::clamp(int(hv) + int(std::lround(delta * k)), -160, 320));
        }
    dirty = true;
}

void MapDoc::mirror(bool point) {
    beginEdit();
    auto& d = data;
    const int W = d.w, H = d.h;
    for (int y = 0; y < H; ++y)
        for (int x = W / 2; x < W; ++x) {
            int sx = W - 1 - x, sy = point ? H - 1 - y : y;
            d.cells[size_t(y * W + x)] = d.cells[size_t(sy * W + sx)];
            d.heights[size_t(y * W + x)] = d.heights[size_t(sy * W + sx)];
        }
    // drop objects on the right half, copy the left half
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
}
void MapDoc::mirrorHorizontal() { mirror(false); }
void MapDoc::mirrorPoint() { mirror(true); }

std::vector<Issue> MapDoc::validate(const game::Rules& rules) const {
    std::vector<Issue> out;
    const auto& d = data;
    if (d.players.size() < 2) out.push_back({true, "Нужно минимум 2 игрока"});
    std::set<std::string> ids;
    for (size_t i = 0; i < d.entities.size(); ++i) {
        const auto& e = d.entities[i];
        int ti = rules.findType(e.type);
        std::string nm = e.id.empty() ? e.type : e.id;
        if (ti < 0) { out.push_back({true, nm + ": неизвестный тип", int(i)}); continue; }
        if (!e.id.empty() && !ids.insert(e.id).second) out.push_back({true, "Повторяющийся id: " + e.id, int(i)});
        if (e.owner < 0 || e.owner > int(d.players.size())) out.push_back({true, nm + ": владелец вне диапазона", int(i)});
        if (e.x < 0 || e.z < 0 || e.x > d.w || e.z > d.h) out.push_back({true, nm + ": за пределами карты", int(i)});
        const auto& t = rules.types[size_t(ti)];
        char c = d.cell(int(std::floor(e.x)), int(std::floor(e.z)));
        bool naval = t.layer & game::kLayerNaval, air = t.layer & game::kLayerAir;
        if (!air && !d.roadsOnly && ((naval && c != '~' && c != '=') || (!naval && (c == '~' || c == '#'))))
            out.push_back({true, nm + ": стоит на непроходимой клетке", int(i)});
    }
    for (size_t p = 1; p <= d.players.size(); ++p) {
        bool has = false;
        for (auto& e : d.entities) if (e.owner == int(p)) has = true;
        if (!has) out.push_back({true, "У игрока " + d.players[p - 1].name + " нет стартовых объектов"});
    }
    // overlapping structures
    for (size_t i = 0; i < d.entities.size(); ++i) {
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
    return out;
}

} // namespace rts::editor
