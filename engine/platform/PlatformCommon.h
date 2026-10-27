// Internal helpers shared by platform implementations.
#pragma once
#include "Platform.h"
#include <cstdio>
#include <fstream>
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

// Map an ASCII letter/digit to Key.
inline Key keyFromAscii(int c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'Z') return Key(int(Key::A) + (c - 'A'));
    if (c >= '0' && c <= '9') return Key(int(Key::Num0) + (c - '0'));
    return Key::Unknown;
}

} // namespace rts::platform::detail
