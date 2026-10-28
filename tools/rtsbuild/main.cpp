// rtsbuild — packages a game (content folder with game.json) for every platform.
//
//   rtsbuild --project <content dir> --target <macos|ios|android|web|windows|linux|all>
//            [--engine <engine dir>] [--out <dir>] [--config Debug|Release]
//            [--generate-only] [--team <Apple team id>] [--jobs N]
//            [--no-pack] [--pack-level N] [--no-atlas] [--audio auto|off|desktop|mobile|web]
//            [--split | --no-split] [--chunk-size 8MB] [--base-size 40MB] [--total-size 1GB] [--fill-base]
//
// Split builds (game.json "packaging", see splitConfig): the app ships only the base pack with
// what the game needs to start; the rest goes to <out>/<target>/chunks/ for upload, downloaded
// at runtime from packaging.sources (engine/pack/Chunks.h).
//
// Content (game folder + engine/assets) ships as one compressed resource pack
// content/content.rpak; --no-pack ships the loose folder instead (debugging). Every build prints
// a size report and writes it to <out>/<target>/size-report.txt (+ size-report.json for the
// editor). Small 2D images are packed into texture atlases per platform profile (= target; page
// size limit from game.json "atlas", see docs/game-format.md); --no-atlas ships them as is.
// Sounds and music are re-encoded per target the same way (audio.json "build" profiles, see
// tools/audiopack): desktop targets use "desktop", ios/android "mobile", web "web"; --audio
// <profile> forces one, --audio off ships the files as they are.
//
// Outputs (default <project>/../dist/<target>):
//   macos    <Name>.app                     (universal arm64+x86_64, ad-hoc signed)
//   ios      Xcode project (self-contained export) + simulator build check
//   android  Gradle project (self-contained export) + APK when the Android SDK is found
//   web      index.html / .js / .wasm / .data
//   windows  <Name>.exe + content (MSVC on Windows; MinGW cross-compiler elsewhere),
//            otherwise a source package with build.bat
//   linux    <Name> + content
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "engine/data/Json.h"
#include "engine/pack/PackWriter.h"
#include "engine/pack/Unpacker.h"
#include "tools/atlas/AtlasBuild.h"
#include "tools/audiopack/AudioPack.h"
#include "tools/rtsbuild/FileRules.h"
#include "tools/rtsbuild/Plugins.h"

#ifndef RTS_SOURCE_DIR
#define RTS_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;
using rts::Json;

namespace {

// ------------------------------------------------------------------ utils
#if defined(_WIN32)
constexpr const char* kHost = "windows";
constexpr char kPathSep = ';';
#elif defined(__APPLE__)
constexpr const char* kHost = "macos";
constexpr char kPathSep = ':';
#else
constexpr const char* kHost = "linux";
constexpr char kPathSep = ':';
#endif

struct Options {
    fs::path project, engine, out;
    std::string target, config = "Release", team;
    bool generateOnly = false;
    bool estimate = false;    // --estimate: content packs and size-report.json only, no app build (editor Build window)
    bool pack = true;
    int packLevel = 0;        // 0 = by config: 19 for Release, 3 for Debug
    bool atlas = true;
    std::string profile;      // current target: selects the atlas page size
    Json gameJson;
    std::map<std::string, fs::path> packFiles; // atlas + audio + split settings key -> built pack (shared by targets with equal settings)
    std::string audio = "auto";                 // auto (by target) | off | audio profile name
    std::map<std::string, fs::path> staged;     // game folder with audio re-encoded, per audio profile
    // Split builds: command-line overrides of game.json "packaging" (-1 / empty = not given).
    int split = -1, fillBase = -1;
    std::string chunkSize, baseSize, totalSize;
    std::string plugins;      // --plugins a,b: native plugins instead of game.json "plugins" (tools/rtsbuild/Plugins.h)
    unsigned jobs = std::max(2u, std::thread::hardware_concurrency());
};

struct Game {
    std::string name = "Game", id = "com.example.game", version = "0.1.0", orientation = "landscape";
    int build = 1;
    std::string exe;   // file-system friendly name
    std::string icon;  // relative to project
};

void step(const std::string& s) { std::cout << "\n==> " << s << std::endl; }
void info(const std::string& s) { std::cout << "    " << s << std::endl; }
[[noreturn]] void die(const std::string& s) { std::cout << "ОШИБКА: " << s << std::endl; std::exit(1); }

std::string quote(const fs::path& p) {
#if defined(_WIN32)
    return "\"" + p.string() + "\"";
#else
    std::string s = p.string(), o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
#endif
}

int run(const std::string& cmd) {
    std::cout << "    $ " << cmd << std::endl;
    std::cout.flush();
    int rc = std::system(cmd.c_str());
#if !defined(_WIN32)
    if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
    return rc;
}
void mustRun(const std::string& cmd) { if (run(cmd) != 0) die("команда завершилась с ошибкой"); }

fs::path findTool(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (!path) return {};
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, kPathSep)) {
        for (const char* ext : {"", ".exe", ".bat", ".cmd"}) {
            fs::path p = fs::path(dir) / (name + ext);
            std::error_code ec;
            if (fs::exists(p, ec) && !fs::is_directory(p, ec)) return p;
        }
    }
    return {};
}

std::string readText(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
void writeText(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << s;
    if (!f) die("не удалось записать " + p.string());
}
std::string replaceAll(std::string s, const std::string& a, const std::string& b) {
    for (size_t pos = 0; (pos = s.find(a, pos)) != std::string::npos; pos += b.size()) s.replace(pos, a.size(), b);
    return s;
}
void copyDir(const fs::path& from, const fs::path& to) {
    fs::create_directories(to);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}
std::string sanitize(const std::string& s) {
    std::string o;
    for (char c : s) o += (std::isalnum(static_cast<unsigned char>(c)) ? c : (c == ' ' || c == '-' || c == '_') ? '_' : '_');
    while (o.find("__") != std::string::npos) o = replaceAll(o, "__", "_");
    if (o.empty()) o = "Game";
    return o;
}

// ------------------------------------------------------------------ content pack
// "~/x", "${HOME}/x" and paths relative to the project.
fs::path expandPath(const std::string& p, const fs::path& base) {
    std::string s = p;
    for (size_t a; (a = s.find("${")) != std::string::npos;) {
        size_t b = s.find('}', a);
        if (b == std::string::npos) break;
        const char* v = std::getenv(s.substr(a + 2, b - a - 2).c_str());
        s.replace(a, b - a + 1, v ? v : "");
    }
    if (!s.empty() && s[0] == '~') if (const char* h = std::getenv("HOME")) s = h + s.substr(1);
    fs::path r(s);
    return r.is_relative() ? base / r : r;
}

// game.json "imports": third-party archives unpacked into the pack at build time, e.g.
//   {"source": "~/Games/war2/DATA/MAINDAT.WAR", "prefix": "imported/war2", "optional": true}
// Optional sources may be missing (CI, other machines); the build then goes without them.
void addImports(const Options& o, rts::pack::PackWriter& w, rts::pack::IgnoreRules& rules, std::vector<std::string>& notes) {
    Json m;
    try { m = Json::parseFile((o.project / "game.json").string()); } catch (const std::exception&) { return; }
    const Json& imports = m["imports"];
    if (imports.size() == 0) return;
    rts::pack::UnpackerRegistry reg;
    reg.addDefaultDirs(o.engine.string(), o.project.string());
    for (auto& e : reg.errors()) info("предупреждение: плагин пропущен: " + e);
    for (auto& im : imports.items()) {
        std::string src = im.getStr("source");
        if (src.empty()) die("game.json: imports: у элемента нет \"source\"");
        fs::path path = expandPath(src, o.project);
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            if (im.getBool("optional")) { info("импорт пропущен (нет файла): " + path.string()); notes.push_back("пропущен импорт: " + src); continue; }
            die("game.json: imports: не найден " + path.string() + " (добавьте \"optional\": true, если его может не быть)");
        }
        // An archive kept inside the game folder must not also land in the pack as a raw file.
        fs::path rel = fs::relative(path, o.project, ec);
        if (!ec && !rel.empty() && rel.native()[0] != '.') rules.add(rel.generic_string());
        rts::pack::ImportOptions io;
        io.format = im.getStr("format");
        io.prefix = im.getStr("prefix");
        io.external = im.getBool("external");
        for (auto& g : im["include"].items()) io.include.push_back(g.str());
        rts::pack::ImportResult r;
        std::string err;
        if (!rts::pack::importArchive(reg, path.string(), io, w, &r, &err)) die("импорт " + src + ": " + err);
        info("импорт: " + src + " (" + r.format + ") -> " + (io.prefix.empty() ? "/" : io.prefix + "/") + ", файлов " + std::to_string(r.files) +
             (r.external ? ", внешний контент" : ""));
    }
}

rts::atlas::AtlasConfig atlasConfig(const Options& o) {
    try {
        rts::atlas::AtlasConfig c = rts::atlas::AtlasConfig::fromGame(o.gameJson, o.profile);
        if (!o.atlas) c.enabled = false;
        return c;
    } catch (const std::exception& e) {
        die(e.what());
    }
}

// Machine-readable build report (editor Build window): pack totals, heaviest entries, atlases.
Json packReportJson(const rts::pack::WriteStats& st, const rts::atlas::AtlasStats& as) {
    Json j = Json::object();
    j.set("rawBytes", Json::number(int64_t(st.rawBytes)));
    j.set("packBytes", Json::number(int64_t(st.packBytes)));
    j.set("entries", Json::number(int64_t(st.items.size())));
    j.set("duplicates", Json::number(int64_t(st.duplicates)));
    std::vector<const rts::pack::WriteStats::Item*> top;
    for (auto& it : st.items) top.push_back(&it);
    std::stable_sort(top.begin(), top.end(), [](auto* a, auto* b) { return a->storedSize > b->storedSize; });
    Json t = Json::array();
    for (size_t i = 0; i < top.size() && i < 15; ++i) {
        Json e = Json::object();
        e.set("path", Json::string(top[i]->path));
        e.set("rawBytes", Json::number(int64_t(top[i]->rawSize)));
        e.set("storedBytes", Json::number(int64_t(top[i]->storedSize)));
        t.push(std::move(e));
    }
    j.set("top", std::move(t));
    Json r = Json::object();
    r.set("pack", std::move(j));
    r.set("atlas", as.toJson());
    return r;
}

// ------------------------------------------------------------------ split builds
// game.json "packaging" (all optional, the user decides):
//   {"split": true, "chunkSize": "8MB", "baseSize": "40MB", "totalSize": "1GB", "fillBase": false,
//    "base": ["maps/intro.json"], "order": ["maps/scena00*", "sprites/*"],
//    "sources": ["https://cdn.example.com/mygame/{hash}/"],
//    "targets": {"web": {"split": true, "baseSize": "15MB"}, "windows": {"split": false}}}
// Per-target blocks override the common settings; command-line flags override both.
uint64_t parseSize(const Json& j, const std::string& where) {
    if (j.isNumber()) return uint64_t(std::max<int64_t>(0, j.integer()));
    std::string s = j.str();
    if (s.empty()) return 0;
    size_t i = 0;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
    if (i == 0) die(where + ": размер \"" + s + "\" — ожидается число с KB/MB/GB");
    double v = std::atof(s.substr(0, i).c_str());
    std::string unit;
    for (char c : s.substr(i)) if (!std::isspace(static_cast<unsigned char>(c))) unit += char(std::toupper(static_cast<unsigned char>(c)));
    double mul = unit.empty() || unit == "B" ? 1 : unit == "K" || unit == "KB" ? 1024.0 : unit == "M" || unit == "MB" ? 1048576.0
               : unit == "G" || unit == "GB" ? 1073741824.0 : -1;
    if (mul < 0) die(where + ": неизвестная единица \"" + unit + "\" (B, KB, MB, GB)");
    return uint64_t(v * mul);
}

struct SplitConfig {
    bool enabled = false;
    rts::pack::SplitOptions opt;
    std::vector<std::string> sources;
    std::string key() const {
        if (!enabled) return "";
        std::string k = std::to_string(opt.chunkSize) + "/" + std::to_string(opt.baseSize) + "/" + std::to_string(opt.totalBudget) + "/" + (opt.fillBase ? "f" : "-");
        for (auto& g : opt.base) k += "|b:" + g;
        for (auto& g : opt.priority) k += "|o:" + g;
        char buf[24];
        std::snprintf(buf, sizeof buf, "split-%08llx", static_cast<unsigned long long>(rts::pack::hashBytes(k.data(), k.size()) & 0xffffffffu));
        return buf;
    }
};

SplitConfig splitConfig(const Options& o) {
    SplitConfig c;
    const Json& p = o.gameJson["packaging"];
    const Json& t = p["targets"][o.profile];
    auto pick = [&](const char* key) -> const Json& { return t.has(key) ? t[key] : p[key]; };
    c.enabled = pick("split").boolean(false);
    if (o.split >= 0) c.enabled = o.split == 1;
    if (!c.enabled) return c;
    if (pick("chunkSize").isNull() && o.chunkSize.empty()) c.opt.chunkSize = 8u << 20;
    else c.opt.chunkSize = o.chunkSize.empty() ? parseSize(pick("chunkSize"), "packaging.chunkSize") : parseSize(Json::string(o.chunkSize), "--chunk-size");
    c.opt.baseSize = o.baseSize.empty() ? parseSize(pick("baseSize"), "packaging.baseSize") : parseSize(Json::string(o.baseSize), "--base-size");
    c.opt.totalBudget = o.totalSize.empty() ? parseSize(pick("totalSize"), "packaging.totalSize") : parseSize(Json::string(o.totalSize), "--total-size");
    c.opt.fillBase = o.fillBase >= 0 ? o.fillBase == 1 : pick("fillBase").boolean(false);
    // What a game needs to start always ships: manifest, rules, languages, UI, engine assets,
    // sounds table, atlas manifest, the first map with its tileset and the icon.
    c.opt.base = {"game.json", "audio.json", "lang/*", "ui/*", "engine/*", "_atlas/*"};   // atlas pages hold the sprites
    c.opt.base.push_back(o.gameJson.getStr("rules", "rules.json"));
    if (std::string icon = o.gameJson.getStr("icon"); !icon.empty()) c.opt.base.push_back(icon);
    const Json& maps = o.gameJson["maps"];
    if (maps.size() > 0) {
        std::string first = maps[size_t(0)].getStr("file");
        c.opt.base.push_back(first);
        try {   // its tileset and atlas image
            Json m = Json::parseFile((o.project / first).string());
            if (std::string ts = m["tiles"].getStr("tileset"); !ts.empty()) {
                c.opt.base.push_back(ts);
                Json tj = Json::parseFile((o.project / ts).string());
                if (std::string at = tj.getStr("atlas"); !at.empty()) c.opt.base.push_back(at);
            }
        } catch (const std::exception&) {}
    }
    // Sprite sheets of all types: any unit may appear on screen at any time.
    try {
        Json rules = Json::parseFile((o.project / o.gameJson.getStr("rules", "rules.json")).string());
        for (auto& [id, t] : rules["types"].members())
            if (std::string sheet = t["sprite"].getStr("sheet"); !sheet.empty()) c.opt.base.push_back(sheet);
    } catch (const std::exception&) {}
    for (auto& g : p["base"].items()) c.opt.base.push_back(g.str());
    for (auto& g : t["base"].items()) c.opt.base.push_back(g.str());
    // Download order: user's order first, then small data files, then the rest.
    for (auto& g : pick("order").items()) c.opt.priority.push_back(g.str());
    c.opt.priority.push_back("*.json");
    for (auto& s : pick("sources").items()) c.sources.push_back(s.str());
    return c;
}

// ------------------------------------------------------------------ audio
std::string audioProfile(const Options& o) {
    if (o.audio == "off") return "off";
    return o.audio == "auto" ? rts::audiopack::profileForTarget(o.profile) : o.audio;
}

// The game folder as it ships for the current target: a copy with sounds and music re-encoded
// for its audio profile (once per profile), or the project itself with --audio off.
fs::path gameContent(Options& o) {
    const std::string profile = audioProfile(o);
    if (profile == "off") return o.project;
    if (auto it = o.staged.find(profile); it != o.staged.end()) return it->second;
    step("Звук: сжатие для профиля " + profile);
    const fs::path dir = o.out / "_audio" / profile;
    rts::audiopack::Report report;
    std::string err;
    if (!rts::audiopack::stageGame(o.project.string(), dir.string(), profile, report, &err)) die(err);
    std::stringstream ss(report.items.empty() ? std::string("без изменений\n") : report.text());
    for (std::string line; std::getline(ss, line);) info(line);
    writeText(o.out / "_audio" / (profile + "-report.txt"), report.text());
    o.staged[profile] = dir;
    return dir;
}

// Builds <out>/_pack[/<atlas key>][/audio-<profile>][/<split key>]/content.rpak (+ chunks/) from the
// game folder and engine assets, once per distinct atlas + audio + split setting (targets that agree
// share one pack).
// Per-file rules (packaging.files) of the current target, as a short key for the pack cache.
std::string fileRulesKey(const Options& o) {
    const Json& p = o.gameJson["packaging"];
    std::string k = p["files"].dump(-1) + "|" + p["targets"][o.profile]["files"].dump(-1);
    if (k == "null|null") return "";
    char buf[24];
    std::snprintf(buf, sizeof buf, "files-%08llx", static_cast<unsigned long long>(rts::pack::hashBytes(k.data(), k.size()) & 0xffffffffu));
    return buf;
}

fs::path packDir(const Options& o) {
    std::string key = atlasConfig(o).key(), audio = audioProfile(o), split = splitConfig(o).key();
    fs::path dir = key == "off" ? o.out / "_pack" : o.out / "_pack" / key;
    if (std::string fk = fileRulesKey(o); !fk.empty()) dir /= fk;
    if (!(audio == "desktop" || audio == "off")) dir /= "audio-" + audio;
    return split.empty() ? dir : dir / split;
}

fs::path contentPack(Options& o) {
    rts::atlas::AtlasConfig ac = atlasConfig(o);
    SplitConfig sc = splitConfig(o);
    const std::string audio = audioProfile(o), key = ac.key() + "|" + audio + "|" + sc.key() + "|" + fileRulesKey(o);
    if (auto it = o.packFiles.find(key); it != o.packFiles.end()) return it->second;
    const fs::path game = gameContent(o);
    step("Контент: сборка ресурс-пака" + (ac.key() == "off" ? std::string() : " (атласы: " + o.profile + ", страница до " + std::to_string(ac.pack.maxPage) + ")") +
         (audio == "off" ? std::string() : " (звук: " + audio + ")"));
    rts::pack::PackWriter w;
    rts::pack::IgnoreRules rules;
    rules.add("unpackers/"); // the game's own unpacker plugins are build tools, not content
    std::vector<std::string> notes;
    addImports(o, w, rules, notes);
    // Game folders produced by importers (tools/importers) carry converted third-party data.
    try {
        if (Json::parseFile((o.project / "game.json").string()).getBool("external")) w.markExternal();
    } catch (const std::exception&) {}
    std::string err;
    if (w.addDirectory(game.string(), "", rules, &err) < 0 || w.addDirectory((o.engine / "engine" / "assets").string(), "engine", rts::pack::IgnoreRules{}, &err) < 0)
        die(err);
    // per-file settings from the Build window (exclude, zstd level, image size) before atlases
    std::string ruleErr;
    std::vector<rts::build::FileRule> fileRules = rts::build::readFileRules(o.gameJson, o.profile, &ruleErr);
    if (!ruleErr.empty()) die(ruleErr);
    rts::build::FileRulesReport fr;
    std::vector<std::pair<std::string, int>> levelRules;
    std::vector<std::string> fixedImages;   // frames addressed in pixels: sprite sheets, tile atlases
    try {
        Json rules = Json::parseFile((o.project / o.gameJson.getStr("rules", "rules.json")).string());
        for (auto& [id, t] : rules["types"].members())
            if (std::string sheet = t["sprite"].getStr("sheet"); !sheet.empty()) fixedImages.push_back(sheet);
        std::error_code e3;
        for (auto it = fs::directory_iterator(o.project / "tilesets", e3); !e3 && it != fs::directory_iterator(); it.increment(e3))
            if (std::string at = Json::parseFile(it->path().string()).getStr("atlas"); !at.empty()) fixedImages.push_back(at);
    } catch (const std::exception&) {}
    if (!rts::build::applyFileRules(w, fileRules, levelRules, fr, &err, fixedImages)) die(err);
    for (auto& m : fr.warnings) info("предупреждение: " + m);
    for (auto& it : fr.items) info("файл " + it.path + ": " + it.what + (it.after ? " (" + rts::pack::formatBytes(it.before) + " → " + rts::pack::formatBytes(it.after) + ")" : ""));
    rts::atlas::AtlasStats as;
    if (!rts::atlas::applyAtlas(w, ac, as, &err)) die(err);
    for (auto& m : as.warnings) info("предупреждение: " + m);
    rts::pack::WriteOptions wo;
    wo.level = o.packLevel ? o.packLevel : (o.config == "Debug" ? 3 : 19);
    wo.threads = o.jobs;
    wo.levelRules = levelRules;
    rts::pack::WriteStats st;
    fs::path dir = packDir(o), out = dir / "content.rpak";
    rts::pack::SplitResult sr;
    std::error_code ec;
    fs::remove_all(dir / "chunks", ec);
    if (sc.enabled) {
        if (!w.writeSplitFiles(out.string(), (dir / "chunks").string(), wo, sc.opt, &st, &sr, &err)) die(err);
    } else if (!w.write(out.string(), wo, &st, &err)) {
        die(err);
    }
    rts::atlas::finishStats(as, st);
    for (auto& m : st.warnings) info("предупреждение: " + m);
    std::string report = rts::pack::sizeReport(st, 15) + as.report();
    if (sc.enabled) {
        report += "разбиение: в сборке " + rts::pack::formatBytes(sr.baseBytes) + ", докачка " + std::to_string(sr.chunks.size()) +
                  " част. (до " + rts::pack::formatBytes(sc.opt.chunkSize) + "), всего " + rts::pack::formatBytes(sr.totalBytes) + "\n";
        for (auto& c : sr.chunks) report += "  " + c.file + "  " + rts::pack::formatBytes(c.size) + ", файлов " + std::to_string(c.files) + "\n";
        if (sr.chunks.empty()) report += "  всё поместилось в сборку — докачивать нечего\n";
        else if (sc.sources.empty())
            report += "ВНИМАНИЕ: в game.json нет packaging.sources — игре неоткуда скачивать части (если игра не задаёт источник сама)\n";
        else {
            report += "выложите папку chunks/ по адресу:";
            for (auto& s : sc.sources) report += " " + s;
            report += "  ({hash} = " + [&] { char b[17]; std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(st.contentHash)); return std::string(b); }() + ")\n";
        }
    }
    for (auto& n : notes) report += n + "\n";
    if (w.external())
        report += "ВНИМАНИЕ: сборка содержит внешний контент (импорт из сторонних архивов) — только для личного использования, не распространять.\n";
    std::stringstream ss(report);
    for (std::string line; std::getline(ss, line);) info(line);
    writeText(dir / "size-report.txt", report);
    {
        Json rep = packReportJson(st, as);
        {   // every entry (the editor's Build window lists and sorts them) + what the file rules did
            Json files = Json::array();
            for (auto& it : st.items) {
                Json e = Json::object();
                e.set("path", Json::string(it.path));
                e.set("rawBytes", Json::number(int64_t(it.rawSize)));
                e.set("storedBytes", Json::number(int64_t(it.storedSize)));
                e.set("codec", Json::string(it.codec == rts::pack::Codec::Zstd ? "zstd" : "store"));
                if (it.duplicate) e.set("duplicate", Json::fromBool(true));
                files.push(std::move(e));
            }
            rep.set("files", std::move(files));
            Json rules = Json::array();
            for (auto& it : fr.items) {
                Json e = Json::object();
                e.set("path", Json::string(it.path)); e.set("what", Json::string(it.what));
                e.set("before", Json::number(int64_t(it.before))); e.set("after", Json::number(int64_t(it.after)));
                rules.push(std::move(e));
            }
            rep.set("fileRules", std::move(rules));
            rep.set("target", Json::string(o.profile));
            rep.set("level", Json::number(int64_t(wo.level)));
        }
        if (sc.enabled) {
            Json s = Json::object();
            s.set("baseBytes", Json::number(int64_t(sr.baseBytes)));
            s.set("totalBytes", Json::number(int64_t(sr.totalBytes)));
            s.set("chunkSize", Json::number(int64_t(sc.opt.chunkSize)));
            Json arr = Json::array();
            for (auto& c : sr.chunks) {
                Json e = Json::object();
                e.set("file", Json::string(c.file));
                e.set("bytes", Json::number(int64_t(c.size)));
                e.set("files", Json::number(int64_t(c.files)));
                e.set("sha256", Json::string(c.sha256));
                arr.push(std::move(e));
            }
            s.set("chunks", std::move(arr));
            Json src = Json::array();
            for (auto& u : sc.sources) src.push(Json::string(u));
            s.set("sources", std::move(src));
            rep.set("split", std::move(s));
        }
        writeText(dir / "size-report.json", rep.dump(2) + "\n");
    }
    o.packFiles[key] = out;
    return out;
}

// Places the content next to a packaged game: content.rpak, or the loose folders with --no-pack.
void placeContent(Options& o, const fs::path& contentDir) {
    if (o.pack) {
        fs::create_directories(contentDir);
        fs::copy_file(contentPack(o), contentDir / "content.rpak", fs::copy_options::overwrite_existing);
    } else {
        copyDir(gameContent(o), contentDir);
        copyDir(o.engine / "engine" / "assets", contentDir / "engine");
    }
}

// Size of a built artifact (file or folder), for the report at the end of each target.
uint64_t treeSize(const fs::path& p) {
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) return fs::file_size(p, ec);
    uint64_t n = 0;
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) n += it->file_size(ec);
    return n;
}
void reportSize(const Options& o, const std::string& target, const fs::path& artifact) {
    std::error_code ec;
    if (!fs::exists(artifact, ec)) return;
    std::string line = target + ": " + artifact.filename().string() + " — " + rts::pack::formatBytes(treeSize(artifact));
    info("размер: " + line);
    // Split build: the chunks to upload go next to the artifact, not inside it.
    if (o.pack && fs::is_directory(packDir(o) / "chunks", ec)) {
        fs::remove_all(o.out / target / "chunks", ec);
        copyDir(packDir(o) / "chunks", o.out / target / "chunks");
        line += ", докачка: " + rts::pack::formatBytes(treeSize(o.out / target / "chunks")) + " в " + (o.out / target / "chunks").string();
        info("части для докачки: " + (o.out / target / "chunks").string());
    }
    std::string report = line + "\n";
    if (o.pack && fs::exists(packDir(o) / "size-report.txt", ec)) report += "\nконтент (content.rpak):\n" + readText(packDir(o) / "size-report.txt");
    if (const std::string p = audioProfile(o); p != "off" && fs::exists(o.out / "_audio" / (p + "-report.txt"), ec))
        report += "\nзвук (профиль " + p + "):\n" + readText(o.out / "_audio" / (p + "-report.txt"));
    writeText(o.out / target / "size-report.txt", report);
    Json j = Json::object();
    j.set("target", Json::string(target));
    j.set("artifact", Json::string(artifact.filename().string()));
    j.set("artifactBytes", Json::number(int64_t(treeSize(artifact))));
    if (o.pack && fs::exists(packDir(o) / "size-report.json", ec)) {
        try {
            Json c = Json::parse(readText(packDir(o) / "size-report.json"));
            j.set("pack", c["pack"]);
            j.set("atlas", c["atlas"]);
            if (c.has("split")) j.set("split", c["split"]);
        } catch (const std::exception&) {}
    }
    writeText(o.out / target / "size-report.json", j.dump(2) + "\n");
}

std::string packVar(Options& o) { return o.pack ? " -DRTS_CONTENT_PACK=" + quote(contentPack(o)) : std::string(" -DRTS_CONTENT_PACK="); }

std::string cmakeVars(const Game& g, const Options& o, const fs::path& content, const fs::path& gen) {
    std::string s;
    s += " -DCMAKE_BUILD_TYPE=" + o.config;
    s += " -DRTS_CONTENT_DIR=" + quote(content);
    s += " -DRTS_GAME_NAME=" + quote(g.name);
    s += " -DRTS_GAME_ID=" + g.id;
    s += " -DRTS_GAME_VERSION=" + g.version;
    s += " -DRTS_GAME_BUILD=" + std::to_string(g.build);
    s += " -DRTS_GAME_EXECUTABLE=" + g.exe;
    s += " -DRTS_BUILD_EDITOR=OFF -DRTS_BUILD_TOOLS=OFF -DRTS_BUILD_TESTS=OFF -DRTS_BUILD_HEADLESS=OFF";
    if (!gen.empty()) s += " -DRTS_GEN_DIR=" + quote(gen);
    return s;
}

// ------------------------------------------------------------------ PNG (for default icons)
uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0xffffffffu) {
    for (size_t i = 0; i < n; ++i) { c ^= d[i]; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return c;
}
void writePNG(const fs::path& path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; ++y) { raw.push_back(0); raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4); }
    std::vector<uint8_t> z = {0x78, 0x01}; // zlib, stored blocks
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size(); off += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n >= raw.size() ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8)); z.push_back(uint8_t(~n)); z.push_back(uint8_t((~n) >> 8));
        z.insert(z.end(), raw.begin() + long(off), raw.begin() + long(off + n));
    }
    uint32_t adler = (b << 16) | a;
    for (int i = 3; i >= 0; --i) z.push_back(uint8_t(adler >> (8 * i)));
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        uint32_t n = uint32_t(data.size());
        for (int i = 3; i >= 0; --i) out.push_back(uint8_t(n >> (8 * i)));
        std::vector<uint8_t> td(type, type + 4); td.insert(td.end(), data.begin(), data.end());
        out.insert(out.end(), td.begin(), td.end());
        uint32_t c = ~crc32(td.data(), td.size());
        for (int i = 3; i >= 0; --i) out.push_back(uint8_t(c >> (8 * i)));
    };
    std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w), uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 6, 0, 0, 0};
    chunk("IHDR", ihdr); chunk("IDAT", z); chunk("IEND", {});
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
}
// Default icon: a mushroom on a green tile.
void writeDefaultIcon(const fs::path& path, int size) {
    std::vector<uint8_t> px(size_t(size) * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            float r = 0.27f, g = 0.47f, b = 0.30f;                              // background
            float cx = u - 0.5f, cy = v - 0.47f;
            bool cap = cy < 0.02f && (cx * cx) / 0.13f + (cy * cy) / 0.06f < 1.0f; // cap ellipse (upper half)
            bool stem = std::fabs(cx) < 0.11f && v > 0.47f && v < 0.80f;
            if (stem) { r = 0.93f; g = 0.88f; b = 0.76f; }
            if (cap) {
                r = 0.88f; g = 0.22f; b = 0.20f;
                float sx = cx + 0.12f, sy = cy + 0.12f, tx = cx - 0.12f, ty = cy + 0.16f, qx = cx, qy = cy + 0.25f;
                if (sx * sx + sy * sy < 0.0025f || tx * tx + ty * ty < 0.002f || qx * qx + qy * qy < 0.0018f) { r = g = b = 0.97f; }
            }
            size_t i = (size_t(y) * size + x) * 4;
            px[i] = uint8_t(r * 255); px[i + 1] = uint8_t(g * 255); px[i + 2] = uint8_t(b * 255); px[i + 3] = 255;
        }
    writePNG(path, size, size, px);
}

// Master 1024x1024 icon: the game's icon.png or the generated default.
fs::path masterIcon(const Game& g, const Options& o, const fs::path& gen) {
    fs::path custom = o.project / g.icon;
    std::error_code ec;
    if (!g.icon.empty() && fs::exists(custom, ec)) return custom;
    fs::path def = gen / "icon_1024.png";
    writeDefaultIcon(def, 1024);
    info("иконка: icon.png не найден, сгенерирована стандартная");
    return def;
}

// ------------------------------------------------------------------ source export
// Copies everything the runtime needs into `dst` so the generated project is self-contained.
void exportSources(const Options& o, const fs::path& dst) {
    for (const char* d : {"engine", "modules", "third_party", "platform"}) copyDir(o.engine / d, dst / d);
    copyDir(o.engine / "apps" / "client", dst / "apps" / "client");
    fs::copy_file(o.engine / "CMakeLists.txt", dst / "CMakeLists.txt", fs::copy_options::overwrite_existing);
}

// Native plugins for a target: report what goes in and write plugins.cmake into `dir`.
rts::build::PluginSet pluginsFor(const Options& o, const std::string& platform) {
    rts::build::PluginSet set = rts::build::loadPlugins(o.engine, o.project, readText(o.project / "game.json"), platform, o.plugins);
    for (auto& n : set.notes) info("плагин: " + n);
    return set;
}
std::string pluginsVar(const Options& o, const std::string& platform, const fs::path& dir) {
    return " -DRTS_PLUGINS_FILE=" + quote(rts::build::writePluginsCMake(pluginsFor(o, platform), dir));
}

// ------------------------------------------------------------------ targets
void buildMac(const Game& g, Options& o) {
    if (std::string(kHost) != "macos") { info("macOS-сборка возможна только на Mac — пропускаю"); return; }
    fs::path out = o.out / "macos", bdir = o.out / "_build" / "macos", gen = bdir / "gen";
    fs::create_directories(gen);
    step("macOS: иконка");
    fs::path icon = masterIcon(g, o, gen);
    fs::path iconset = gen / "AppIcon.iconset";
    fs::create_directories(iconset);
    for (int s : {16, 32, 128, 256, 512}) {
        run("sips -z " + std::to_string(s) + " " + std::to_string(s) + " " + quote(icon) + " --out " + quote(iconset / ("icon_" + std::to_string(s) + "x" + std::to_string(s) + ".png")) + " >/dev/null");
        run("sips -z " + std::to_string(s * 2) + " " + std::to_string(s * 2) + " " + quote(icon) + " --out " + quote(iconset / ("icon_" + std::to_string(s) + "x" + std::to_string(s) + "@2x.png")) + " >/dev/null");
    }
    run("iconutil -c icns " + quote(iconset) + " -o " + quote(gen / "AppIcon.icns"));

    step("macOS: конфигурация CMake");
    std::string gen_ = findTool("ninja").empty() ? "" : " -G Ninja";
    mustRun("cmake -S " + quote(o.engine) + " -B " + quote(bdir) + gen_ + cmakeVars(g, o, gameContent(o), gen) + packVar(o) + pluginsVar(o, "macos", gen) + " -DCMAKE_OSX_ARCHITECTURES=\"arm64;x86_64\" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0");
    step("macOS: сборка");
    mustRun("cmake --build " + quote(bdir) + " --target rts_client --config " + o.config + " -j " + std::to_string(o.jobs));
    fs::path app = bdir / (g.exe + ".app");
    if (!fs::exists(app)) app = bdir / o.config / (g.exe + ".app");
    if (!fs::exists(app)) die("не найден " + app.string());
    fs::remove_all(out / (g.exe + ".app"));
    fs::create_directories(out);
    mustRun("ditto " + quote(app) + " " + quote(out / (g.exe + ".app")));
    step("macOS: подпись (ad-hoc)");
    run("codesign --force --deep --sign - " + quote(out / (g.exe + ".app")));
    info("Готово: " + (out / (g.exe + ".app")).string());
    reportSize(o, "macos", out / (g.exe + ".app"));
}

void buildIOS(const Game& g, Options& o) {
    if (std::string(kHost) != "macos") { info("Xcode-проект для iOS генерируется только на Mac — пропускаю"); return; }
    fs::path out = o.out / "ios", src = out / "src", xc = out / "xcode", gen = out / "gen";
    step("iOS: экспорт исходников и контента");
    fs::remove_all(src);
    exportSources(o, src);
    placeContent(o, src / "content");
    rts::build::PluginSet plugins = pluginsFor(o, "ios");
    rts::build::writePluginsCMake(plugins, src); // src/plugins.cmake travels with the exported project
    step("iOS: иконка (asset catalog)");
    fs::path icon = masterIcon(g, o, gen);
    fs::path set = gen / "Assets.xcassets" / "AppIcon.appiconset";
    fs::create_directories(set);
    run("sips -z 1024 1024 -s format png " + quote(icon) + " --out " + quote(set / "icon_1024.png") + " >/dev/null");
    writeText(gen / "Assets.xcassets" / "Contents.json", "{ \"info\": { \"author\": \"rtsbuild\", \"version\": 1 } }\n");
    writeText(set / "Contents.json",
              "{\n  \"images\": [ { \"filename\": \"icon_1024.png\", \"idiom\": \"universal\", \"platform\": \"ios\", \"size\": \"1024x1024\" } ],\n"
              "  \"info\": { \"author\": \"rtsbuild\", \"version\": 1 }\n}\n");

    step("iOS: генерация Xcode-проекта");
    std::string vars = cmakeVars(g, o, src / "content", gen);
    vars += o.pack ? " -DRTS_CONTENT_PACK=" + quote(src / "content" / "content.rpak") : std::string(" -DRTS_CONTENT_PACK=");
    if (!o.team.empty()) vars += " -DRTS_APPLE_TEAM_ID=" + o.team;
    mustRun("cmake -S " + quote(src) + " -B " + quote(xc) + " -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0" + vars);
    fs::path proj = xc / "RTSEngine.xcodeproj";
    std::string xcodeTarget = "-project " + quote(proj);
    if (std::string pods = rts::build::podfile(plugins, "15.0"); !pods.empty()) {
        writeText(xc / "Podfile", pods);
        if (!findTool("pod").empty()) {
            step("iOS: CocoaPods (SDK плагинов)");
            mustRun("cd " + quote(xc) + " && pod install --repo-update");
            xcodeTarget = "-workspace " + quote(xc / "RTSEngine.xcworkspace");
            info("Открывайте RTSEngine.xcworkspace (не .xcodeproj): в нём SDK плагинов.");
        } else {
            info("Нужен CocoaPods для SDK плагинов: brew install cocoapods, затем pod install в " + xc.string());
            if (!o.generateOnly) { info("Проверочная сборка пропущена (нет pod)"); return; }
        }
    }
    info("Проект: " + proj.string());
    if (o.generateOnly) return;
    step("iOS: проверочная сборка для симулятора (без подписи)");
    int rc = run("xcodebuild " + xcodeTarget + " -scheme rts_client -configuration " + o.config +
                 " -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath " + quote(out / "derived") +
                 " CODE_SIGNING_ALLOWED=NO -quiet build");
    if (rc != 0) die("сборка для симулятора не прошла");
    info("Симулятор: OK. Для устройства откройте проект в Xcode, выберите Team в Signing & Capabilities и запустите.");
}

fs::path androidSdk() {
    for (const char* v : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) if (const char* e = std::getenv(v)) if (fs::exists(e)) return e;
    if (const char* home = std::getenv("HOME")) {
        for (const char* rel : {"Library/Android/sdk", "Android/Sdk", "Android/sdk"}) {
            fs::path p = fs::path(home) / rel;
            if (fs::exists(p)) return p;
        }
    }
    if (const char* la = std::getenv("LOCALAPPDATA")) { fs::path p = fs::path(la) / "Android" / "Sdk"; if (fs::exists(p)) return p; }
    return {};
}
std::string newestSubdir(const fs::path& dir) {
    std::vector<std::string> v;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) if (e.is_directory()) v.push_back(e.path().filename().string());
    if (v.empty()) return {};
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) {
        auto parts = [](const std::string& s) { std::vector<long> r; std::stringstream ss(s); std::string t; while (std::getline(ss, t, '.')) r.push_back(std::atol(t.c_str())); return r; };
        return parts(a) < parts(b);
    });
    return v.back();
}

void buildAndroid(const Game& g, Options& o) {
    fs::path out = o.out / "android", proj = out / g.exe;
    step("Android: генерация Gradle-проекта");
    fs::remove_all(proj);
    fs::path tpl = o.engine / "platform" / "android" / "template";
    fs::path sdk = androidSdk();
    std::string ndkLine, cmakeLine;
    if (!sdk.empty()) {
        std::string ndk = newestSubdir(sdk / "ndk");
        if (!ndk.empty()) ndkLine = "    ndkVersion '" + ndk + "'";
        std::string cm = newestSubdir(sdk / "cmake");
        if (!cm.empty()) cmakeLine = "            version '" + cm + "'";
        info("Android SDK: " + sdk.string() + (ndk.empty() ? "  (NDK не найден)" : "  NDK " + ndk));
    } else {
        info("Android SDK не найден (ANDROID_HOME) — будет только проект");
    }
    std::string orientation = g.orientation == "portrait" ? "sensorPortrait" : g.orientation == "any" ? "fullUser" : "sensorLandscape";
    rts::build::PluginSet plugins = pluginsFor(o, "android");
    const auto pluginText = rts::build::androidPlaceholders(plugins);
    for (auto& e : fs::recursive_directory_iterator(tpl)) {
        if (!e.is_regular_file()) continue;
        fs::path rel = fs::relative(e.path(), tpl);
        std::string t = readText(e.path());
        t = replaceAll(t, "@PROJECT_NAME@", g.exe);
        t = replaceAll(t, "@APP_ID@", g.id);
        t = replaceAll(t, "@APP_NAME@", g.name);
        t = replaceAll(t, "@VERSION@", g.version);
        t = replaceAll(t, "@VERSION_CODE@", std::to_string(g.build));
        t = replaceAll(t, "@ORIENTATION@", orientation);
        t = replaceAll(t, "@AGP_VERSION@", "8.7.3");
        t = replaceAll(t, "@NDK_VERSION_LINE@", ndkLine);
        t = replaceAll(t, "@CMAKE_VERSION_LINE@", cmakeLine);
        for (auto& [key, value] : pluginText) t = replaceAll(t, key, value);
        writeText(proj / rel, t);
    }
    exportSources(o, proj / "app" / "src" / "main" / "cpp");
    rts::build::writePluginsCMake(plugins, proj / "app" / "src" / "main" / "cpp");
    rts::build::copyAndroidJava(plugins, proj / "app" / "src" / "main" / "java");
    placeContent(o, proj / "app" / "src" / "main" / "assets" / "content");
    fs::path gen = out / "_gen";
    fs::create_directories(gen);
    fs::path icon = masterIcon(g, o, gen);
    // A single high-res launcher icon; Android scales it for other densities.
    fs::create_directories(proj / "app" / "src" / "main" / "res" / "mipmap-xxxhdpi");
    fs::copy_file(icon, proj / "app" / "src" / "main" / "res" / "mipmap-xxxhdpi" / "ic_launcher.png", fs::copy_options::overwrite_existing);
    if (!sdk.empty()) {
        std::string sdkPath = sdk.generic_string();
        writeText(proj / "local.properties", "sdk.dir=" + sdkPath + "\n");
    }
    writeText(proj / ".gitignore", ".gradle/\nbuild/\napp/build/\napp/.cxx/\nlocal.properties\n");
    info("Проект: " + proj.string() + "  (можно открыть в Android Studio)");
    if (o.generateOnly) return;

    fs::path gradle = findTool("gradle");
    if (sdk.empty()) { info("APK не собран: нет Android SDK"); return; }
    if (gradle.empty()) {
        info("APK не собран: нет gradle в PATH. Откройте проект в Android Studio или установите gradle (brew install gradle).");
        return;
    }
    step("Android: gradle wrapper");
    mustRun("cd " + quote(proj) + " && " + quote(gradle) + " wrapper --gradle-version 8.10.2 -q");
    step("Android: сборка APK");
    std::string task = o.config == "Debug" ? "assembleDebug" : "assembleRelease";
#if defined(_WIN32)
    mustRun("cd /d " + quote(proj) + " && gradlew.bat " + task);
#else
    mustRun("cd " + quote(proj) + " && ./gradlew " + task + " --console=plain");
#endif
    std::string variant = o.config == "Debug" ? "debug" : "release";
    fs::path apk = proj / "app" / "build" / "outputs" / "apk" / variant / ("app-" + variant + ".apk");
    if (fs::exists(apk)) {
        fs::copy_file(apk, out / (g.exe + "-" + variant + ".apk"), fs::copy_options::overwrite_existing);
        info("APK: " + (out / (g.exe + "-" + variant + ".apk")).string());
        info("Установка: adb install -r " + quote(out / (g.exe + "-" + variant + ".apk")));
        reportSize(o, "android", out / (g.exe + "-" + variant + ".apk"));
    }
}

fs::path findEmcmake() {
    fs::path p = findTool("emcmake");
    if (!p.empty()) return p;
    if (const char* e = std::getenv("EMSDK")) { fs::path c = fs::path(e) / "upstream" / "emscripten" / "emcmake"; if (fs::exists(c)) return c; }
    if (const char* home = std::getenv("HOME")) { fs::path c = fs::path(home) / "emsdk" / "upstream" / "emscripten" / "emcmake"; if (fs::exists(c)) return c; }
    return {};
}

void buildWeb(const Game& g, Options& o) {
    fs::path emcmake = findEmcmake();
    if (emcmake.empty()) { info("Emscripten не найден (emsdk). Установите: https://emscripten.org/docs/getting_started — пропускаю"); return; }
    fs::path out = o.out / "web", bdir = o.out / "_build" / "web";
    step("Web: конфигурация (Emscripten)");
    std::string gen_ = findTool("ninja").empty() ? "" : " -G Ninja";
    rts::build::PluginSet plugins = pluginsFor(o, "web");
    std::string pvar = " -DRTS_PLUGINS_FILE=" + quote(rts::build::writePluginsCMake(plugins, bdir / "gen"));
    mustRun(quote(emcmake) + " cmake -S " + quote(o.engine) + " -B " + quote(bdir) + gen_ + cmakeVars(g, o, gameContent(o), {}) + packVar(o) + pvar);
    step("Web: сборка");
    mustRun("cmake --build " + quote(bdir) + " --target rts_client -j " + std::to_string(o.jobs));
    fs::create_directories(out);
    for (const char* f : {"index.html", "index.js", "index.wasm", "index.data"}) {
        if (fs::exists(bdir / f)) fs::copy_file(bdir / f, out / f, fs::copy_options::overwrite_existing);
    }
    std::string html = readText(out / "index.html");
    writeText(out / "index.html", replaceAll(html, "<title>RTSEngine</title>", "<title>" + g.name + "</title>"));
    rts::build::installWebPlugins(plugins, out);
    info("Готово: " + out.string() + "  (проверка: python3 -m http.server -d " + quote(out) + ")");
    reportSize(o, "web", out);
}

void buildWindows(const Game& g, Options& o) {
    fs::path out = o.out / "windows";
    if (std::string(kHost) == "windows") {
        fs::path bdir = o.out / "_build" / "windows";
        step("Windows: конфигурация (Visual Studio)");
        mustRun("cmake -S " + quote(o.engine) + " -B " + quote(bdir) + " -A x64" + cmakeVars(g, o, gameContent(o), {}) + packVar(o) + pluginsVar(o, "windows", bdir / "gen"));
        step("Windows: сборка");
        mustRun("cmake --build " + quote(bdir) + " --target rts_client --config " + o.config);
        fs::path dir = bdir / o.config;
        fs::remove_all(out / g.exe);
        copyDir(dir, out / g.exe);
        info("Готово: " + (out / g.exe / (g.exe + ".exe")).string());
        reportSize(o, "windows", out / g.exe);
        return;
    }
    // Cross-compile with MinGW-w64 if present (brew install mingw-w64 / apt install mingw-w64).
    std::string cxx;
    for (const char* c : {"x86_64-w64-mingw32-g++", "x86_64-w64-mingw32-clang++"}) if (!findTool(c).empty()) { cxx = c; break; }
    if (!cxx.empty()) {
        fs::path bdir = o.out / "_build" / "windows-mingw";
        std::string cc = replaceAll(replaceAll(cxx, "g++", "gcc"), "clang++", "clang");
        fs::path tc = bdir / "mingw-toolchain.cmake";
        writeText(tc, "set(CMAKE_SYSTEM_NAME Windows)\nset(CMAKE_SYSTEM_PROCESSOR x86_64)\nset(CMAKE_C_COMPILER " + cc + ")\nset(CMAKE_CXX_COMPILER " + cxx +
                      ")\nset(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)\nset(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n"
                      "set(CMAKE_EXE_LINKER_FLAGS_INIT \"-static -static-libgcc -static-libstdc++\")\n");
        step("Windows: кросс-компиляция (" + cxx + ")");
        std::string gen_ = findTool("ninja").empty() ? "" : " -G Ninja";
        mustRun("cmake -S " + quote(o.engine) + " -B " + quote(bdir) + gen_ + " -DCMAKE_TOOLCHAIN_FILE=" + quote(tc) + cmakeVars(g, o, gameContent(o), {}) + packVar(o) + pluginsVar(o, "windows", bdir / "gen"));
        mustRun("cmake --build " + quote(bdir) + " --target rts_client -j " + std::to_string(o.jobs));
        fs::remove_all(out / g.exe);
        fs::create_directories(out / g.exe);
        fs::copy_file(bdir / (g.exe + ".exe"), out / g.exe / (g.exe + ".exe"), fs::copy_options::overwrite_existing);
        placeContent(o, out / g.exe / "content");
        info("Готово: " + (out / g.exe / (g.exe + ".exe")).string());
        reportSize(o, "windows", out / g.exe);
        return;
    }
    step("Windows: экспорт исходников (компилятор для Windows не найден)");
    fs::path src = out / (g.exe + "-src");
    fs::remove_all(src);
    exportSources(o, src);
    rts::build::writePluginsCMake(pluginsFor(o, "windows"), src);
    placeContent(o, src / "content");
    std::string packArg = o.pack ? " -DRTS_CONTENT_PACK=%~dp0content\\content.rpak" : "";
    writeText(src / "build.bat",
              "@echo off\r\nrem Requires Visual Studio 2022 (Desktop C++) and CMake.\r\n"
              "cmake -S . -B build -A x64 -DRTS_CONTENT_DIR=%~dp0content -DRTS_GAME_NAME=\"" + g.name + "\" -DRTS_GAME_ID=" + g.id +
              " -DRTS_GAME_EXECUTABLE=" + g.exe + packArg + " -DRTS_BUILD_EDITOR=OFF -DRTS_BUILD_TOOLS=OFF -DRTS_BUILD_TESTS=OFF -DRTS_BUILD_HEADLESS=OFF || exit /b 1\r\n"
              "cmake --build build --target rts_client --config Release || exit /b 1\r\n"
              "echo Done: build\\Release\\" + g.exe + ".exe\r\n");
    info("Исходники: " + src.string() + " — на Windows запустите build.bat.");
    info("Или установите кросс-компилятор: brew install mingw-w64");
}

void buildLinux(const Game& g, Options& o) {
    if (std::string(kHost) != "linux") { info("Linux-сборка выполняется на Linux — пропускаю"); return; }
    fs::path out = o.out / "linux", bdir = o.out / "_build" / "linux";
    step("Linux: сборка");
    std::string gen_ = findTool("ninja").empty() ? "" : " -G Ninja";
    mustRun("cmake -S " + quote(o.engine) + " -B " + quote(bdir) + gen_ + cmakeVars(g, o, gameContent(o), {}) + packVar(o) + pluginsVar(o, "linux", bdir / "gen"));
    mustRun("cmake --build " + quote(bdir) + " --target rts_client -j " + std::to_string(o.jobs));
    fs::remove_all(out / g.exe);
    fs::create_directories(out / g.exe);
    fs::copy_file(bdir / g.exe, out / g.exe / g.exe, fs::copy_options::overwrite_existing);
    placeContent(o, out / g.exe / "content");
    info("Готово: " + (out / g.exe / g.exe).string());
    reportSize(o, "linux", out / g.exe);
}

void usage() {
    std::cout << "usage: rtsbuild --project <content dir> --target <macos|ios|android|web|windows|linux|all>\n"
                 "                [--engine <dir>] [--out <dir>] [--config Debug|Release] [--generate-only] [--team <AppleTeamID>] [--jobs N]\n"
                 "                [--no-pack] [--pack-level 1..22] [--no-atlas] [--audio auto|off|desktop|mobile|web]\n"
              "                [--plugins name,name]   (native plugins; default: game.json \"plugins\")\n"
                 "                [--split | --no-split] [--chunk-size 8MB] [--base-size 40MB] [--total-size 1GB] [--fill-base]\n"
                 "                [--estimate]  (content pack + size-report.json per target only, for the editor)\n";
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    o.engine = RTS_SOURCE_DIR;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
        if (a == "--project") o.project = next();
        else if (a == "--engine") o.engine = next();
        else if (a == "--out") o.out = next();
        else if (a == "--target") o.target = next();
        else if (a == "--config") o.config = next();
        else if (a == "--team") o.team = next();
        else if (a == "--jobs") o.jobs = unsigned(std::atoi(next().c_str()));
        else if (a == "--generate-only") o.generateOnly = true;
        else if (a == "--estimate") o.estimate = true;
        else if (a == "--no-pack") o.pack = false;
        else if (a == "--pack-level") o.packLevel = std::atoi(next().c_str());
        else if (a == "--no-atlas") o.atlas = false;
        else if (a == "--plugins") o.plugins = next();
        else if (a == "--split") o.split = 1;
        else if (a == "--no-split") o.split = 0;
        else if (a == "--chunk-size") o.chunkSize = next();
        else if (a == "--base-size") o.baseSize = next();
        else if (a == "--total-size") o.totalSize = next();
        else if (a == "--fill-base") o.fillBase = 1;
        else if (a == "--audio") o.audio = next();
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { usage(); return 2; }
    }
    if (o.project.empty() || o.target.empty()) { usage(); return 2; }
    std::error_code ec;
    o.project = fs::canonical(o.project, ec);
    if (ec) die("папка проекта не найдена");
    o.engine = fs::canonical(o.engine, ec);
    if (ec || !fs::exists(o.engine / "engine" / "core" / "Fixed.h")) die("папка движка не найдена: " + o.engine.string());
    if (o.out.empty()) o.out = o.project.parent_path() / "dist";
    o.out = fs::absolute(o.out);
    if (o.config != "Debug" && o.config != "Release") die("--config должен быть Debug или Release");

    Game g;
    try {
        Json m = Json::parseFile((o.project / "game.json").string());
        o.gameJson = m;
        g.name = m.getStr("name", g.name);
        g.id = m.getStr("id", g.id);
        g.version = m.getStr("version", g.version);
        g.build = int(m.getInt("build", 1));
        g.orientation = m.getStr("orientation", g.orientation);
        g.icon = m.getStr("icon", "icon.png");
        if (o.team.empty()) o.team = m.getStr("appleTeamId", "");
        if (!o.packLevel) o.packLevel = int(m["packaging"]["level"].integer(0));   // Build window setting
    } catch (const std::exception& e) {
        die(std::string("game.json: ") + e.what());
    }
    g.exe = sanitize(g.name);
    for (char c : g.id) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_')) die("id в game.json может содержать только латиницу, цифры, точки и _");

    std::cout << "rtsbuild: " << g.name << " " << g.version << " (" << g.id << ")\n"
              << "  проект: " << o.project.string() << "\n  движок: " << o.engine.string() << "\n  вывод:  " << o.out.string()
              << "\n  хост:   " << kHost << ", конфигурация " << o.config << std::endl;
    if (o.estimate) { // editor Build window: only the content as it would ship, per target
        std::vector<std::string> ts = o.target == "all" ? std::vector<std::string>{"macos", "ios", "android", "web", "windows", "linux"} : std::vector<std::string>{o.target};
        for (auto& t : ts) {
            o.profile = t;
            fs::path pack = contentPack(o);
            std::error_code e2;
            fs::create_directories(o.out / t, e2);
            fs::copy_file(pack.parent_path() / "size-report.json", o.out / t / "size-report.json", fs::copy_options::overwrite_existing, e2);
            std::cout << "оценка " << t << ": " << (o.out / t / "size-report.json").string() << std::endl;
        }
        return 0;
    }
    if (findTool("cmake").empty()) die("cmake не найден в PATH");

    std::vector<std::string> targets;
    if (o.target == "all") targets = {"macos", "ios", "android", "web", "windows", "linux"};
    else targets = {o.target};
    for (auto& t : targets) {
        o.profile = t;
        if (t == "macos") buildMac(g, o);
        else if (t == "ios") buildIOS(g, o);
        else if (t == "android") buildAndroid(g, o);
        else if (t == "web") buildWeb(g, o);
        else if (t == "windows") buildWindows(g, o);
        else if (t == "linux") buildLinux(g, o);
        else die("неизвестная платформа: " + t);
    }
    std::cout << "\nСборка завершена." << std::endl;
    return 0;
}
