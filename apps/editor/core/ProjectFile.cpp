#include "ProjectFile.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>
#include "Projects.h"
#include "engine/data/Json.h"

#ifndef RTS_ENGINE_VERSION
#define RTS_ENGINE_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;

namespace rts::editor {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
std::string today() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}
} // namespace

std::string engineVersion() { return RTS_ENGINE_VERSION; }

bool isProjectFilePath(const std::string& path) { return lower(fs::path(path).extension().string()) == kProjectExt; }

std::string findProjectFile(const std::string& dir) {
    std::error_code ec;
    std::vector<fs::path> found;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && isProjectFilePath(it->path().string())) found.push_back(it->path());
    }
    if (found.empty()) return {};
    std::sort(found.begin(), found.end());
    return found.front().string();
}

bool readProjectFile(const std::string& path, ProjectFile& out, std::string* err) {
    try {
        Json j = Json::parseFile(path);
        if (!j.isObject()) { if (err) *err = path + ": expected a JSON object"; return false; }
        out = {};
        out.format = int(j.getInt("format", kProjectFormat));
        out.name = j.getStr("name", fs::path(path).stem().string());
        out.engine = j.getStr("engine");
        out.manifest = j.getStr("manifest", "game.json");
        out.created = j.getStr("created");
        if (out.format > kProjectFormat) {
            if (err) *err = path + ": format " + std::to_string(out.format) + " is newer than this editor (" + std::to_string(kProjectFormat) + ")";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

bool writeProjectFile(const std::string& path, const ProjectFile& pf, std::string* err) {
    Json j = Json::object();
    std::error_code ec;
    if (fs::exists(path, ec)) try { Json old = Json::parseFile(path); if (old.isObject()) j = old; } catch (...) {}   // keep unknown keys
    j.set("format", Json::number(int64_t(pf.format)));
    j.set("name", Json::string(pf.name));
    j.set("engine", Json::string(pf.engine.empty() ? engineVersion() : pf.engine));
    j.set("manifest", Json::string(pf.manifest.empty() ? "game.json" : pf.manifest));
    j.set("created", Json::string(pf.created.empty() ? today() : pf.created));
    std::ofstream f(fs::path(path), std::ios::binary);
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    f << j.dump(2) << "\n";
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    return true;
}

std::string projectFileName(const std::string& title) { return projectFolderName(title) + kProjectExt; }

std::string resolveProjectDir(const std::string& path, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return std::string(); };
    if (path.empty()) return fail("empty project path");
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(path), ec).lexically_normal();
    while (p.has_relative_path() && p.filename().empty()) p = p.parent_path();   // trailing separator
    std::string manifest = "game.json";
    fs::path dir;
    if (fs::is_directory(p, ec)) dir = p;
    else if (fs::is_regular_file(p, ec)) {
        if (isProjectFilePath(p.string())) {
            ProjectFile pf;
            std::string e;
            if (!readProjectFile(p.string(), pf, &e)) return fail(e);
            manifest = pf.manifest;
        } else if (lower(p.filename().string()) != "game.json") return fail("not a project file (.rtsproj), folder or game.json: " + p.string());
        dir = p.parent_path();
    } else return fail("not found: " + p.string());
    if (!fs::exists(dir / manifest, ec)) return fail("no " + manifest + " in " + dir.string());
    return dir.string();
}

std::string ensureProjectFile(const std::string& dir, bool* created) {
    if (created) *created = false;
    if (std::string f = findProjectFile(dir); !f.empty()) return f;
    std::string name;
    try { name = Json::parseFile((fs::path(dir) / "game.json").string()).getStr("name"); } catch (...) {}
    if (name.empty()) name = fs::path(dir).filename().string();
    ProjectFile pf;
    pf.name = name;
    const std::string path = (fs::path(dir) / projectFileName(name)).string();
    if (!writeProjectFile(path, pf, nullptr)) return {};
    if (created) *created = true;
    return path;
}

} // namespace rts::editor
