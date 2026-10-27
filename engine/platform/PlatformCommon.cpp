#include "PlatformCommon.h"

#include <cstdio>
#include <mutex>

#include "engine/core/Hash.h"

namespace rts::platform {

namespace detail {
State& state() { static State s; return s; }
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
