#include "PlatformCommon.h"

#include <cstdio>
#include <mutex>

#include "engine/core/Hash.h"
#include <cstdint>
#include <cstdlib>

namespace rts::platform {

namespace detail {
State& state() { static State s; return s; }

CaptureRequest captureRequestFromEnv() {
    CaptureRequest c;
#if defined(_WIN32)
    // _wgetenv keeps non-ASCII paths intact; convert to UTF-8 like the rest of the engine
    auto get = [](const wchar_t* n) -> std::string {
        const wchar_t* v = _wgetenv(n);
        if (!v) return {};
        std::string s;
        for (; *v; ++v) {
            uint32_t ch = uint32_t(*v);
            if (ch >= 0xD800 && ch < 0xDC00 && v[1]) { ch = 0x10000 + ((ch - 0xD800) << 10) + (uint32_t(v[1]) - 0xDC00); ++v; }
            if (ch < 0x80) s += char(ch);
            else if (ch < 0x800) { s += char(0xC0 | (ch >> 6)); s += char(0x80 | (ch & 63)); }
            else if (ch < 0x10000) { s += char(0xE0 | (ch >> 12)); s += char(0x80 | ((ch >> 6) & 63)); s += char(0x80 | (ch & 63)); }
            else { s += char(0xF0 | (ch >> 18)); s += char(0x80 | ((ch >> 12) & 63)); s += char(0x80 | ((ch >> 6) & 63)); s += char(0x80 | (ch & 63)); }
        }
        return s;
    };
    c.path = get(L"RTS_SCREENSHOT");
    c.frames = std::atol(get(L"RTS_EXIT_AFTER_FRAMES").c_str());
#else
    if (const char* p = std::getenv("RTS_SCREENSHOT")) c.path = p;
    if (const char* n = std::getenv("RTS_EXIT_AFTER_FRAMES")) c.frames = std::atol(n);
#endif
    return c;
}

bool writePPM(const std::string& path, int w, int h, const unsigned char* rgba) {
    if (path.empty() || w <= 0 || h <= 0) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "P6\n" << w << ' ' << h << "\n255\n";
    std::string row(size_t(w) * 3, '\0');
    for (int y = h - 1; y >= 0; --y) {
        const unsigned char* src = rgba + size_t(y) * size_t(w) * 4;
        for (int x = 0; x < w; ++x) { row[size_t(x) * 3] = char(src[x * 4]); row[size_t(x) * 3 + 1] = char(src[x * 4 + 1]); row[size_t(x) * 3 + 2] = char(src[x * 4 + 2]); }
        f.write(row.data(), std::streamsize(row.size()));
    }
    return bool(f);
}
} // namespace detail

using detail::state;

float windowWidth() { return state().winW; }
float windowHeight() { return state().winH; }
int framebufferWidth() { return state().fbW; }
int framebufferHeight() { return state().fbH; }
float dpiScale() { return state().scale; }
bool isTouchDevice() { return state().touch; }
void requestQuit() { state().quit = true; }
std::string contentRoot() { return state().contentRoot; }
std::string userDataDir() { return state().userDir; }

namespace detail {
namespace {
std::mutex& packMutex() { static std::mutex m; return m; }

bool mountSource(std::unique_ptr<pack::Source> src, const std::string& path) {
    std::string err = "cannot open";
    auto p = src ? pack::Pack::open(std::move(src), &err) : nullptr;
    if (!p) {
        std::fprintf(stderr, "[pack] %s: %s\n", path.c_str(), err.c_str());
        return false;
    }
    p->setName(path);
    std::fprintf(stderr, "[pack] mounted %s (%zu files)\n", path.c_str(), p->size());
    state().packs.mount(std::move(p));
    return true;
}
} // namespace

bool mountPackWith(const PackOpener& open, const std::string& path) { return mountSource(open(path), path); }

void ensurePacks(const PackOpener& open) {
    auto& S = state();
    if (S.packsInit && S.packsRoot == S.contentRoot) return;
    std::lock_guard<std::mutex> lock(packMutex());
    if (S.packsInit && S.packsRoot == S.contentRoot) return;
    S.packs.clear();
    S.packsRoot = S.contentRoot;
    S.packsInit = true;
    std::string path = S.contentRoot + "/content.rpak";
    if (auto src = open(path)) mountSource(std::move(src), path); // no pack = normal dev setup
}
} // namespace detail

#if !defined(__ANDROID__)
namespace {
std::unique_ptr<pack::Source> openStdPack(const std::string& path) { return pack::openFileSource(path); }
} // namespace

bool mountPack(const std::string& path) {
    detail::ensurePacks(openStdPack);
    std::string full = (!path.empty() && (path[0] == '/' || path.find(':') != std::string::npos)) ? path : state().contentRoot + "/" + path;
    return detail::mountPackWith(openStdPack, full);
}

bool readContentFile(const std::string& rel, std::string& out) {
    detail::ensurePacks(openStdPack);
    if (!state().packs.empty() && state().packs.read(rel, out)) return true;
    if (detail::readFileStd(state().contentRoot + "/" + rel, out)) return true;
    return detail::readFileStd(rel, out); // absolute path or relative to CWD (tools, editor)
}
#endif

uint64_t contentPacksHash() {
    auto& P = state().packs;
    if (P.empty()) return 0;
    Hasher h;
    for (size_t i = 0; i < P.count(); ++i) h.u64(P.at(i).contentHash());
    return h.value();
}

#if !defined(__APPLE__) && !defined(_WIN32)
std::string clipboardText() { return state().clipboard; }
void setClipboardText(const std::string& s) { state().clipboard = s; }
#endif

} // namespace rts::platform
