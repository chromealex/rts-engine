// Agent operations for game data that has no dedicated op: any text data file of the game folder
// (lang/*.json, audio.json, tilesets, fx, ui/*.rml, ...) and the map's per-cell territory layer.
// Engine code is out of reach on purpose: paths are confined to the open game's folder.
#include "DataOps.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace rts::editor {

namespace {
Json str(const std::string& s) { return Json::string(s); }
Json okWith(const std::string& summary) { Json r = Json::object(); r.set("ok", Json::fromBool(true)); r.set("summary", str(summary)); return r; }

const char* kTextExt[] = {".json", ".rml", ".rcss", ".txt", ".md", ".csv", ".po", ".lua", ".js"};
constexpr size_t kMaxText = 2u << 20;

// "lang/ru.json" -> absolute path inside the game folder; throws for anything outside it.
fs::path gamePath(const Workspace& ws, const std::string& rel) {
    if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
    if (rel.empty()) throw std::runtime_error("'path' is required (relative to the game folder)");
    fs::path p = fs::path(rel).lexically_normal();
    if (p.is_absolute() || p.empty() || *p.begin() == "..") throw std::runtime_error("path must stay inside the game folder: " + rel);
    return fs::path(ws.project.dir) / p;
}

bool textFile(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return std::any_of(std::begin(kTextExt), std::end(kTextExt), [&](const char* e) { return ext == e; });
}

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

int shapeInt(const Json& p, const char* k, int def = 0) { return p.has(k) ? int(std::lround(p[k].toDouble(def))) : def; }
} // namespace

void registerDataOps(Workspace& ws) {
    ws.registerOp({"game_file_read",
        "Read a text data file of the open game (path relative to the game folder): lang/ru.json, audio.json, "
        "tilesets/desert.json, ui/hud.rml, fx/*.json, ... Use rules_get / map_get for rules.json and maps.",
        Json::parse(R"({"properties": {"path": {"type": "string"}}, "required": ["path"]})"),
        [&ws](const Json& p) {
            fs::path f = gamePath(ws, p.getStr("path"));
            if (!textFile(f)) throw std::runtime_error("not a text data file (json, rml, rcss, txt, md, csv): " + p.getStr("path"));
            if (!fs::exists(f)) throw std::runtime_error("no such file: " + p.getStr("path") + " (asset_list shows the files)");
            if (fs::file_size(f) > kMaxText) throw std::runtime_error("file is larger than 2 MB");
            Json r = Json::object();
            r.set("path", str(p.getStr("path")));
            r.set("text", str(readAll(f)));
            return r;
        }});
    ws.registerOp({"game_file_write",
        "Create or replace a text data file of the open game (lang/*.json, audio.json, tilesets/*.json, fx/*.json, "
        "ui/*.rml|rcss, campaign texts). JSON files are checked before saving. rules.json, game.json and maps have "
        "their own ops (rules_set, set_manifest, map_*), which validate against the engine.",
        Json::parse(R"({"properties": {"path": {"type": "string"}, "text": {"type": "string"}, "json": {}},
            "required": ["path"]})"),
        [&ws](const Json& p) {
            const std::string rel = p.getStr("path");
            fs::path f = gamePath(ws, rel);
            if (!textFile(f)) throw std::runtime_error("only text data files can be written (json, rml, rcss, txt, md, csv)");
            fs::path norm = fs::path(rel).lexically_normal();
            if (norm == "rules.json" || norm == "game.json" || norm == fs::path(ws.project.rulesPath()).lexically_relative(ws.project.dir))
                throw std::runtime_error("use rules_set / rules_merge / set_manifest for " + rel);
            if (!ws.map.path.empty() && fs::path(ws.map.path).lexically_normal() == f.lexically_normal())
                throw std::runtime_error("this is the open map: edit it with the map ops and map_save");
            std::string text;
            if (p.has("json")) text = p["json"].dump(2) + "\n";
            else if (p.has("text")) text = p.getStr("text");
            else throw std::runtime_error("give 'text' or 'json'");
            if (text.size() > kMaxText) throw std::runtime_error("text is larger than 2 MB");
            if (f.extension() == ".json") {
                try { (void)Json::parse(text); }
                catch (const std::exception& e) { throw std::runtime_error(std::string("invalid JSON, not saved: ") + e.what()); }
            }
            std::error_code ec;
            fs::create_directories(f.parent_path(), ec);
            bool existed = fs::exists(f);
            {
                std::ofstream o(f, std::ios::binary);
                if (!o) throw std::runtime_error("cannot write " + rel);
                o << text;
            }
            ws.note({Change::Project, 0, 0, -1, -1, -1, (existed ? "Изменён файл " : "Создан файл ") + rel});
            Json r = okWith((existed ? "Изменён " : "Создан ") + rel);
            r.set("bytes", Json::number(int64_t(text.size())));
            return r;
        }});
    ws.registerOp({"territory_paint",
        "Paint the map's starting territory layer (dungeon / territory-war games): cells owned by player 1..9, "
        "0 clears. Shape: rect [x0,y0,x1,y1] or circle [cx,cy,r]. Only ground cells count in the game.",
        Json::parse(R"({"properties": {"player": {"type": "integer"},
            "rect": {"type": "array", "items": {"type": "integer"}}, "circle": {"type": "array", "items": {"type": "integer"}}},
            "required": ["player"]})"),
        [&ws](const Json& p) {
            MapData& d = ws.map.data;
            int pl = shapeInt(p, "player");
            if (pl < 0 || pl > 9) throw std::runtime_error("player must be 0..9");
            bool rect = p.has("rect");
            const Json& sh = rect ? p["rect"] : p["circle"];
            if (!rect && !p.has("circle")) throw std::runtime_error("give 'rect' or 'circle'");
            if (sh.size() < (rect ? 4u : 3u)) throw std::runtime_error(rect ? "rect is [x0,y0,x1,y1]" : "circle is [cx,cy,r]");
            auto v = [&](size_t i) { return int(std::lround(sh[i].toDouble())); };
            ws.map.beginEdit();
            if (!d.extra["territory"].isObject()) d.extra.set("territory", Json::object());
            Json* t = d.extra.find("territory");
            std::vector<std::string> rows(size_t(d.h), std::string(size_t(d.w), '.'));
            const Json& old = (*t)["rows"];
            for (size_t y = 0; y < rows.size() && y < old.size(); ++y) {
                std::string r = old[y].str();
                for (size_t x = 0; x < rows[y].size() && x < r.size(); ++x) rows[y][x] = r[x];
            }
            char c = pl == 0 ? '.' : char('0' + pl);
            int n = 0, x0, y0, x1, y1;
            if (rect) { x0 = std::min(v(0), v(2)); y0 = std::min(v(1), v(3)); x1 = std::max(v(0), v(2)); y1 = std::max(v(1), v(3)); }
            else { x0 = v(0) - v(2); y0 = v(1) - v(2); x1 = v(0) + v(2); y1 = v(1) + v(2); }
            for (int y = std::max(0, y0); y <= std::min(d.h - 1, y1); ++y)
                for (int x = std::max(0, x0); x <= std::min(d.w - 1, x1); ++x) {
                    if (!rect && (x - v(0)) * (x - v(0)) + (y - v(1)) * (y - v(1)) > v(2) * v(2) + v(2)) continue;
                    rows[size_t(y)][size_t(x)] = c; ++n;
                }
            Json arr = Json::array();
            for (auto& r : rows) arr.push(str(r));
            t->set("rows", arr);
            ws.map.touched();
            ws.note({Change::Cells, x0, y0, x1, y1, -1, pl ? "Территория игрока " + std::to_string(pl) : "Территория снята"});
            Json r = okWith("Территория: " + std::to_string(n) + " клеток → " + (pl ? "игрок " + std::to_string(pl) : "ничья"));
            return r;
        }});
}

} // namespace rts::editor
