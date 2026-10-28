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
    bool textInput = false;   // startTextInput() called and not stopped
    TextInputArea textArea;
    float keyboardH = 0;      // logical points covered by the on-screen keyboard
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
// Per-platform on-screen keyboard (PlatformCommon.cpp has the no-op for desktops).
void showSoftKeyboard(bool show, const TextInputArea& area);
// Called by the platform when the OS keyboard changes (any thread on Android: queued, see there).
void setKeyboardHeight(float logicalPoints);
// The user dismissed the keyboard (Done / Back / hide key) while a field was focused.
void textInputEndedByUser();
// Send a typed string (UTF-8) as Char events; '\n' becomes Enter, '\b' Backspace (KeyDown + KeyUp).
void dispatchText(const std::string& utf8);
void dispatchKeyPress(Key k);

// Frame timing shared by all backends. Clamps dt so a frame after a long stall (app in background,
// tab hidden, window dragged, debugger break) doesn't teleport the camera or flood the sim with
// catch-up ticks. In capture mode (RTS_EXIT_AFTER_FRAMES) frames advance by exactly 1/60 s so CI
// screenshots show the same moment of the match on every platform regardless of GPU speed.
class FrameClock {
public:
    static constexpr double kMaxDt = 0.25;
    explicit FrameClock(bool fixedStep = false) : fixed_(fixedStep) {}
    double tick(double nowSeconds) {
        double dt = started_ ? nowSeconds - last_ : 1.0 / 60.0;
        last_ = nowSeconds; started_ = true;
        if (fixed_) return 1.0 / 60.0;
        return dt < 0 ? 0 : dt > kMaxDt ? kMaxDt : dt;
    }
    void reset() { started_ = false; } // call when rendering resumes after a pause
private:
    double last_ = 0;
    bool started_ = false, fixed_ = false;
};

// RTS_SCREENSHOT=out.ppm RTS_EXIT_AFTER_FRAMES=N: render N frames, save the last one, quit.
struct CaptureRequest {
    std::string path;
    long frames = 0;
    bool active() const { return frames > 0; }
};
CaptureRequest captureRequestFromEnv();
// rgba: bottom-up rows (as glReadPixels returns them); written as a top-down binary PPM.
bool writePPM(const std::string& path, int w, int h, const unsigned char* rgba);

// Map an ASCII letter/digit to Key.
inline Key keyFromAscii(int c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'Z') return Key(int(Key::A) + (c - 'A'));
    if (c >= '0' && c <= '9') return Key(int(Key::Num0) + (c - '0'));
    return Key::Unknown;
}

} // namespace rts::platform::detail
