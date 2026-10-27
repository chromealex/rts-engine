// Internal helpers shared by platform implementations.
#pragma once
#include "Platform.h"
#include "engine/pack/Pack.h"
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>

namespace rts::platform::detail {

struct State {
    AppConfig cfg;
    AppCallbacks cb;
    float winW = 0, winH = 0;
    int fbW = 0, fbH = 0;
    float scale = 1.0f;
    bool quit = false;
    bool touch = false;
    std::string contentRoot = ".";
    std::string userDir = ".";
    std::string clipboard; // fallback clipboard
    pack::PackSet packs;   // mounted .rpak files, searched before loose content files
    std::string packsRoot; // content root the auto-mounted packs belong to
    bool packsInit = false;
};
State& state();

inline void dispatch(const Event& e) { if (state().cb.event) state().cb.event(e); }

inline bool readFileStd(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Auto-mounts `<contentRoot>/content.rpak` (once per content root) using the platform's opener.
using PackOpener = std::function<std::unique_ptr<pack::Source>(const std::string& path)>;
void ensurePacks(const PackOpener& open);
bool mountPackWith(const PackOpener& open, const std::string& path);

// Map an ASCII letter/digit to Key.
inline Key keyFromAscii(int c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'Z') return Key(int(Key::A) + (c - 'A'));
    if (c >= '0' && c <= '9') return Key(int(Key::Num0) + (c - '0'));
    return Key::Unknown;
}

} // namespace rts::platform::detail
