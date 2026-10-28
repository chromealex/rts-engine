#include "PlatformCommon.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include "engine/core/Hash.h"
#include "engine/data/Json.h"
#include <cstdint>
#include <cstdlib>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

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

// ------------------------------------------------------------------ text input
void startTextInput(const TextInputArea& area) {
    auto& S = state();
    S.textArea = area;
    S.textInput = true;
    detail::showSoftKeyboard(true, area); // also when already active: the field may have moved
}
void stopTextInput() {
    auto& S = state();
    if (!S.textInput) return;
    S.textInput = false;
    detail::showSoftKeyboard(false, S.textArea);
}
bool textInputActive() { return state().textInput; }
float softKeyboardHeight() { return state().keyboardH; }

namespace detail {
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
void showSoftKeyboard(bool, const TextInputArea&) {} // desktop: the physical keyboard is always there
#endif

void setKeyboardHeight(float h) {
    auto& S = state();
    if (h < 0) h = 0;
    if (h == S.keyboardH) return;
    S.keyboardH = h;
    Event e; e.type = EventType::KeyboardChanged; dispatch(e);
}

void textInputEndedByUser() {
    auto& S = state();
    if (!S.textInput) return;
    S.textInput = false;
    Event e; e.type = EventType::TextInputEnded; dispatch(e);
}

void dispatchKeyPress(Key k) {
    Event e; e.type = EventType::KeyDown; e.key = k; dispatch(e);
    e.type = EventType::KeyUp; dispatch(e);
}

void dispatchText(const std::string& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const auto* end = p + s.size();
    while (p < end) {
        uint32_t cp = *p++;
        int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        if (extra) cp &= (0x3Fu >> extra);
        for (; extra > 0 && p < end && (*p & 0xC0) == 0x80; --extra) cp = (cp << 6) | (*p++ & 0x3Fu);
        if (extra) continue; // truncated sequence
        if (cp == '\n' || cp == '\r') dispatchKeyPress(Key::Enter);
        else if (cp == '\b') dispatchKeyPress(Key::Backspace);
        else if (cp == '\t') dispatchKeyPress(Key::Tab);
        else if (cp >= 32 && cp != 127) { Event e; e.type = EventType::Char; e.codepoint = cp; dispatch(e); }
    }
}
} // namespace detail

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
    std::fprintf(stderr, "[pack] mounted %s (%zu files%s)\n", path.c_str(), p->size(),
                 p->chunks().empty() ? "" : (", " + std::to_string(p->chunks().size()) + " downloadable chunks").c_str());
    pack::Pack& ref = *p;
    state().packs.mount(std::move(p));
    if (!ref.chunks().empty()) {
        auto& S = state();
        S.chunks.setCacheDir(S.userDir + "/content-cache");
        S.chunks.addPack(ref);
    }
    return true;
}

// Sources: $RTS_CONTENT_SOURCES first (testing, self-hosted mirrors), then game.json
// `packaging.sources`, unless the game set its own with setContentSources().
void initSources() {
    auto& S = state();
    if (S.sourcesSet) return;
    std::vector<std::string> urls;
    if (const char* env = std::getenv("RTS_CONTENT_SOURCES")) {
        std::string e = env;
        for (size_t a = 0; a <= e.size();) {
            size_t b = e.find(';', a);
            if (b == std::string::npos) b = e.size();
            if (b > a) urls.push_back(e.substr(a, b - a));
            a = b + 1;
        }
    }
    std::string text;
    bool autoDownload = true;
    if (S.packs.read("game.json", text)) {
        try {
            Json game = Json::parse(text);
            const Json& p = game["packaging"];
            for (auto& u : p["sources"].items()) urls.push_back(u.str());
            autoDownload = p.getBool("autoDownload", true);
            S.contentWait = double(p.getInt("waitSeconds", 30));
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "[pack] game.json: %s\n", ex.what());
        }
    }
    S.chunks.setSources(std::move(urls));
    // Default: fetch the rest of the content in the background right after start.
    if (autoDownload) S.chunks.requestAll();
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
    S.chunks.clear();
    std::string path = S.contentRoot + "/content.rpak";
    if (auto src = open(path)) mountSource(std::move(src), path); // no pack = normal dev setup
    initSources();
}

// A file in a chunk that isn't here yet: queue its download. Natively the caller then waits for
// it (up to packaging.waitSeconds) so code that loads files synchronously keeps working; the web
// can't block, so there it returns at once and the file is available on a later call.
void requestChunkFor(const std::string& rel) {
    pack::Pack* p = nullptr;
    const pack::Entry* e = nullptr;
    if (state().packs.state(rel, &p, &e) != pack::PackSet::State::NotDownloaded) return;
    auto& C = state().chunks;
    C.request(*p, e->chunk);
#if !defined(__EMSCRIPTEN__)
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(state().contentWait * 1000));
    for (;;) {
        C.poll();
        auto st = C.status(*p, e->chunk);
        if (st == pack::ChunkManager::Status::Ready || st == pack::ChunkManager::Status::Failed || st == pack::ChunkManager::Status::NotDownloaded) break;
        if (std::chrono::steady_clock::now() > until) {
            std::fprintf(stderr, "[pack] %s: still downloading after %.0f s\n", rel.c_str(), state().contentWait);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#endif
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
    if (!state().packs.empty()) {
        if (state().packs.read(rel, out)) return true;
        detail::requestChunkFor(rel);
        if (state().packs.read(rel, out)) return true;
    }
    if (detail::readFileStd(state().contentRoot + "/" + rel, out)) return true;
    return detail::readFileStd(rel, out); // absolute path or relative to CWD (tools, editor)
}
#endif

ContentState contentState(const std::string& rel) {
    auto st = state().packs.state(rel);
    if (st == pack::PackSet::State::Ready) return ContentState::Ready;
    if (st == pack::PackSet::State::NotDownloaded) return ContentState::NotDownloaded;
    std::string tmp;
    return detail::readFileStd(state().contentRoot + "/" + rel, tmp) ? ContentState::Ready : ContentState::Missing;
}

void requestContent(const std::vector<std::string>& paths) {
    for (auto& p : paths) detail::requestChunkFor(p);
    pollContent();
}

void requestAllContent() {
    state().chunks.requestAll();
    pollContent();
}

void pollContent() { state().chunks.poll(); }

ContentProgress contentProgress() {
    pollContent();
    auto pr = state().chunks.progress();
    ContentProgress c;
    c.chunks = pr.chunks; c.ready = pr.ready; c.active = pr.active; c.failed = pr.failed;
    c.bytesTotal = pr.bytesTotal; c.bytesReady = pr.bytesReady; c.busy = pr.busy;
    for (auto& v : state().chunks.chunks()) if (!v.error.empty() && v.status == pack::ChunkManager::Status::Failed) c.error = v.error;
    return c;
}

void setContentSources(const std::vector<std::string>& urls) {
    state().sourcesSet = true;
    state().chunks.setSources(urls);
}

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
