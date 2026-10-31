// MapDoc: planet maps (docs/planets.md) — the `world` block with per-body face layers and corner heights,
// and the planet validation rules (seams equal, starts reachable). Scene / camera / brushes for spheres live
// with the rendering agent; generators fill terrain through the data.
#include "MapDoc.h"
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include "engine/core/Fixed.h"
#include "modules/rts/RtsMap.h"
#include "modules/rts/RtsSphere.h"

namespace rts::editor {

void MapDoc::loadWorld(const Json& w, MapData& d) {
    d.bodies.clear();
    d.worldExtra = Json::object();
    for (auto& [k, v] : w.members()) if (k != "bodies") d.worldExtra.set(k, v);
    for (const Json& bj : w["bodies"].items()) {
        DocBody b;
        b.id = bj.getStr("id", "body" + std::to_string(d.bodies.size()));
        const bool star = bj.getStr("kind", "planet") == "star";
        b.faceSize = star ? 0 : std::clamp<int>(int(bj.getInt("faceSize", 0)), 0, 512);
        for (auto& [k, v] : bj.members()) if (k != "id" && k != "faceSize" && k != "terrain") b.meta.set(k, v);
        b.faceExtra.assign(6, Json::object());
        const Json& t = bj["terrain"];
        for (auto& [k, v] : t.members()) if (k != "faces" && k != "corners" && k != "cornersFile" && k != "cornersBlob" && k != "detail") b.terrainExtra.set(k, v);
        if (b.surface()) {
            const int N = b.faceSize;
            b.cells.assign(size_t(6 * N * N), '.');
            b.levels.assign(size_t(6 * N * N), 0);
            b.corners.assign(size_t(6 * (N + 1) * (N + 1)), 0);
            for (int f = 0; f < 6 && f < int(t["faces"].size()); ++f) {
                const Json& fj = t["faces"][size_t(f)];
                for (int y = 0; y < N && y < int(fj["rows"].size()); ++y) {
                    const std::string r = fj["rows"][size_t(y)].str();
                    for (int x = 0; x < N && x < int(r.size()); ++x) b.cells[b.cellIndex(f, x, y)] = r[size_t(x)];
                }
                for (int y = 0; y < N && y < int(fj["levels"].size()); ++y) {
                    const std::string r = fj["levels"][size_t(y)].str();
                    for (int x = 0; x < N && x < int(r.size()); ++x) if (r[size_t(x)] >= '0' && r[size_t(x)] <= '9') b.levels[b.cellIndex(f, x, y)] = int8_t(r[size_t(x)] - '0');
                }
                for (auto& [k, v] : fj.members()) if (k != "rows" && k != "levels") b.faceExtra[size_t(f)].set(k, v);
            }
            if (t["corners"].size() > 0) {
                for (size_t i = 0; i < t["corners"].size() && i < b.corners.size(); ++i) b.corners[i] = int16_t(t["corners"][i].integer());
            } else if (t.has("cornersFile")) {
                b.cornersFile = t.getStr("cornersFile");
                if (!t["cornersBlob"].isString()) throw std::runtime_error("world body '" + b.id + "': terrain.cornersFile '" + b.cornersFile + "' was not loaded");
                std::string cerr;   // 6 faces of (N+1)^2 = a (6(N+1)) x (N+1) corner grid
                if (!game::decodeCorners(t, 6 * (N + 1) - 1, N, b.corners, &cerr)) throw std::runtime_error("world body '" + b.id + "': " + cerr);
            }
            // detail grid: load() attached the file bytes as "blob" (a missing / broken file drops the detail)
            if (const Json& dj = t["detail"]; dj.isObject()) {
                b.detailScale = std::clamp(int(dj.getInt("scale", 4)), 1, 16);
                b.detailFile = dj.getStr("file");
                const size_t M1 = size_t(N * b.detailScale + 1), n = 6 * M1 * M1;
                const Json& blob = dj["blob"];
                if (blob.isString() && blob.str().size() == n * 2) {
                    const std::string& s = blob.str();
                    b.detail.resize(n);
                    for (size_t i = 0; i < n; ++i) b.detail[i] = int16_t(uint16_t(uint8_t(s[2 * i])) | uint16_t(uint8_t(s[2 * i + 1])) << 8);
                } else { b.detailScale = 0; b.detailFile.clear(); }
            }
        }
        d.bodies.push_back(std::move(b));
    }
    // the flat layers are unused on planet maps: keep them minimal
    d.w = d.h = 4;
    d.cells.assign(16, '.'); d.levels.assign(16, 0); d.heights.assign(16, 0);
    d.fieldRes.assign(16, -1); d.fieldAmt.assign(16, 0); d.regions.assign(16, '.');
    d.corners.clear();
}

std::string MapDoc::bodyDetailFileName(const DocBody& b) const {
    if (!b.detailFile.empty()) return b.detailFile;
    std::string stem = path.empty() ? "map" : std::filesystem::path(path).stem().string();
    return "maps/" + stem + "." + b.id + ".detail.r16";
}

std::string MapDoc::bodyCornersFileName(const DocBody& b) const {
    if (!b.cornersFile.empty()) return b.cornersFile;
    std::string stem = path.empty() ? "map" : std::filesystem::path(path).stem().string();
    return "maps/" + stem + "." + b.id + ".r16";
}

Json MapDoc::worldJson() const {
    Json w = Json::object();
    Json bodies = Json::array();
    for (const DocBody& b : data.bodies) {
        Json bj = Json::object();
        bj.set("id", Json::string(b.id));
        for (auto& [k, v] : b.meta.members()) bj.set(k, v);
        if (b.surface()) {
            bj.set("faceSize", Json::number(int64_t(b.faceSize)));
            const int N = b.faceSize;
            Json t = Json::object();
            Json faces = Json::array();
            for (int f = 0; f < 6; ++f) {
                Json fj = Json::object();
                bool any = false, anyLv = false;
                Json rows = Json::array(), lv = Json::array();
                for (int y = 0; y < N; ++y) {
                    std::string r(size_t(N), '.'), l(size_t(N), '0');
                    for (int x = 0; x < N; ++x) {
                        r[size_t(x)] = b.cells[b.cellIndex(f, x, y)];
                        l[size_t(x)] = char('0' + std::clamp<int>(b.levels[b.cellIndex(f, x, y)], 0, 9));
                        any |= r[size_t(x)] != '.'; anyLv |= l[size_t(x)] != '0';
                    }
                    rows.push(Json::string(r)); lv.push(Json::string(l));
                }
                if (any) fj.set("rows", rows);
                if (anyLv) fj.set("levels", lv);
                for (auto& [k, v] : b.faceExtra[size_t(f)].members()) fj.set(k, v);
                faces.push(fj);
            }
            t.set("faces", faces);
            if (std::any_of(b.corners.begin(), b.corners.end(), [](int16_t v) { return v != 0; })) {
                if (b.corners.size() > kInlineCornersMax) {
                    t.set("cornersFile", Json::string(bodyCornersFileName(b)));
                    t.set("cornersBlob", Json::string(game::encodeCorners(b.corners)));
                } else {
                    Json cs = Json::array();
                    for (int16_t v : b.corners) cs.push(Json::number(int64_t(v)));
                    t.set("corners", cs);
                }
            }
            for (auto& [k, v] : b.terrainExtra.members()) t.set(k, v);
            if (b.detailScale > 0 && b.detail.size() == size_t(6) * size_t(b.detailSide() + 1) * size_t(b.detailSide() + 1)) {
                Json dj = Json::object();
                dj.set("scale", Json::number(int64_t(b.detailScale)));
                dj.set("file", Json::string(bodyDetailFileName(b)));
                dj.set("blob", Json::string(game::encodeCorners(b.detail)));   // the view reads it; save() writes the file
                t.set("detail", dj);
            }
            bj.set("terrain", t);
        }
        bodies.push(bj);
    }
    w.set("bodies", bodies);
    for (auto& [k, v] : data.worldExtra.members()) w.set(k, v);
    return w;
}

void MapDoc::validateWorld(const game::Rules& rules, std::vector<Issue>& out) const {
    const auto& d = data;
    // seams: corners duplicated on 2-3 faces must be equal (the loader averages them, tools must write them equal)
    for (const DocBody& b : d.bodies) {
        if (!b.surface()) continue;
        if (b.corners.size() != size_t(6 * (b.faceSize + 1) * (b.faceSize + 1))) { out.push_back({true, "Тело " + b.id + ": неверное число высот углов"}); continue; }
        const game::CubeSphere cs(b.faceSize);
        int bad = 0;
        for (int f = 0; f < 6; ++f)
            for (int j = 0; j <= b.faceSize; ++j)
                for (int i = 0; i <= b.faceSize; ++i) {
                    if (i != 0 && j != 0 && i != b.faceSize && j != b.faceSize) continue;
                    int ids[3];
                    const int k = cs.cornerDuplicates(f, i, j, ids);
                    for (int m = 1; m < k; ++m) if (b.corners[size_t(ids[m])] != b.corners[size_t(ids[0])]) ++bad;
                }
        if (bad) out.push_back({false, "Тело " + b.id + ": " + std::to_string(bad) + " высот на швах граней не совпадают (загрузчик усреднит)"});
    }
    // positions: known body with a surface, face 0..5, inside the face
    auto checkAt = [&](const Json& at, const std::string& nm, int ent) {
        const int bi = d.findBody(at.getStr("body"));
        if (bi < 0) { out.push_back({true, nm + ": неизвестное тело '" + at.getStr("body") + "'", ent}); return; }
        const DocBody& b = d.bodies[size_t(bi)];
        const int f = int(at.getInt("face", -1));
        const double x = at["x"].toDouble(-1), y = at["y"].toDouble(-1);
        if (!b.surface()) out.push_back({true, nm + ": у тела " + b.id + " нет поверхности", ent});
        else if (f < 0 || f > 5) out.push_back({true, nm + ": грань должна быть 0..5", ent});
        else if (x < 0 || y < 0 || x > b.faceSize || y > b.faceSize) out.push_back({true, nm + ": за пределами грани", ent});
    };
    for (size_t i = 0; i < d.entities.size(); ++i) {
        const auto& e = d.entities[i];
        if (!e.at.isObject()) { out.push_back({true, (e.id.empty() ? e.type : e.id) + ": на карте планет нужна позиция at {body, face, x, y}", int(i)}); continue; }
        checkAt(e.at, e.id.empty() ? e.type : e.id, int(i));
    }
    for (auto& p : d.players) if (p.hasStart) {
        if (p.startAt.isObject()) checkAt(p.startAt, "Старт " + p.name, -1);
        else out.push_back({true, "Старт " + p.name + ": на карте планет нужна позиция {body, face, x, y}"});
    }
    (void)rules;
}

} // namespace rts::editor
