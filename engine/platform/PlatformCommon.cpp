#include "PlatformCommon.h"

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

#if !defined(__ANDROID__)
bool readContentFile(const std::string& rel, std::string& out) {
    if (detail::readFileStd(state().contentRoot + "/" + rel, out)) return true;
    return detail::readFileStd(rel, out); // absolute path or relative to CWD (tools, editor)
}
#endif

#if !defined(__APPLE__) && !defined(_WIN32)
std::string clipboardText() { return state().clipboard; }
void setClipboardText(const std::string& s) { state().clipboard = s; }
#endif

} // namespace rts::platform
