// RTS map editor entry point.
//   rts_editor [--project <content dir with game.json>] [--map <file.json>] [--engine <engine dir>]
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "Editor.h"

#ifndef RTS_SOURCE_DIR
#define RTS_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;
using namespace rts;

static std::unique_ptr<editor::Editor> g_editor;
static int g_demoSelect = -1;
static bool g_demoPlay = false;

static std::string exeDir(const char* argv0) {
    std::error_code ec;
#if defined(__linux__)
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path().string();
#endif
    fs::path p2 = fs::absolute(fs::path(argv0), ec);
    return p2.parent_path().string();
}

static std::string findBuildTool(const std::string& dir) {
    if (const char* env = std::getenv("RTS_BUILD_TOOL")) return env;
#if defined(_WIN32)
    const char* name = "rtsbuild.exe";
#else
    const char* name = "rtsbuild";
#endif
    // next to the editor, or next to the .app bundle on macOS (RTSEditor.app/Contents/MacOS/..)
    for (fs::path c : {fs::path(dir) / name, fs::path(dir) / ".." / ".." / ".." / name, fs::path(dir) / "Release" / name, fs::path(dir) / "Debug" / name}) {
        std::error_code ec;
        if (fs::exists(c, ec)) return fs::canonical(c, ec).string();
    }
    return name; // rely on PATH
}

int rtsMain(int argc, char** argv) {
    editor::Editor::Options opt;
    opt.engineDir = RTS_SOURCE_DIR;
    opt.projectDir = std::string(RTS_SOURCE_DIR) + "/games/skirmish";
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--play") g_demoPlay = true;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string a = argv[i];
        if (a == "--project") opt.projectDir = argv[++i];
        else if (a == "--map") opt.openMap = argv[++i];
        else if (a == "--engine") opt.engineDir = argv[++i];
        else if (a == "--select") g_demoSelect = std::atoi(argv[++i]); // used by automated screenshots
    }
    std::error_code ec;
    opt.projectDir = fs::absolute(opt.projectDir, ec).lexically_normal().string();
    if (!opt.projectDir.empty() && (opt.projectDir.back() == '/' || opt.projectDir.back() == '\\')) opt.projectDir.pop_back();
    if (!opt.openMap.empty()) opt.openMap = fs::absolute(opt.openMap, ec).string();
    opt.buildTool = findBuildTool(exeDir(argv[0]));

    g_editor = std::make_unique<editor::Editor>(opt);
    platform::AppConfig cfg;
    cfg.title = "RTS Editor";
    cfg.width = 1440; cfg.height = 900;
    platform::AppCallbacks cb;
    cb.init = [](gfx::Device& d) { g_editor->init(d); if (g_demoSelect >= 0) g_editor->selectForDemo(g_demoSelect); if (g_demoPlay) g_editor->playForDemo(); };
    cb.frame = [](gfx::Device& d, double dt) { g_editor->frame(d, dt); };
    cb.event = [](const platform::Event& e) { g_editor->event(e); };
    cb.shutdown = [] { g_editor->shutdown(); };
    return platform::run(cfg, cb);
}
