// Platform layer: window/surface, main loop, input, file access.
// One implementation per OS lives next to this file; the game/editor only sees this API.
//
// Entry point: the application defines   int rtsMain(int argc, char** argv);
// and calls platform::run() from it. Each platform provides the real main()/android_main/WinMain.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rts::gfx { class Device; }

namespace rts::platform {

enum class Key : uint16_t {
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Escape, Enter, Tab, Backspace, Delete, Insert, Space,
    Left, Right, Up, Down, Home, End, PageUp, PageDown,
    LeftShift, RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt, LeftSuper, RightSuper,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Minus, Equal, LeftBracket, RightBracket, Semicolon, Apostrophe, Comma, Period, Slash, Backslash, GraveAccent,
    Count
};

enum Mod : uint8_t { kModShift = 1, kModCtrl = 2, kModAlt = 4, kModSuper = 8 };

enum class EventType : uint8_t {
    MouseDown, MouseUp, MouseMove, MouseWheel,
    KeyDown, KeyUp, Char,
    TouchBegin, TouchMove, TouchEnd, TouchCancel,
    Resized, FocusGained, FocusLost, QuitRequested,
    TextInputEnded,   // the user closed the on-screen keyboard (Done, Back, hide key): blur the field
    KeyboardChanged,  // on-screen keyboard shown/hidden/resized: see softKeyboardHeight()
};

struct Event {
    EventType type{};
    float x = 0, y = 0;          // logical points, origin top-left
    float wheelX = 0, wheelY = 0;
    int button = 0;              // 0 left, 1 right, 2 middle
    Key key = Key::Unknown;
    uint32_t codepoint = 0;      // Char
    int64_t touchId = 0;
    uint8_t mods = 0;
    bool repeat = false;
};

struct AppConfig {
    std::string title = "RTSEngine";
    int width = 1280, height = 720;  // logical size of the initial window (desktop)
    bool highDpi = true;
    float clearColor[3] = {0.08f, 0.09f, 0.11f};
};

struct AppCallbacks {
    std::function<void(gfx::Device&)> init;          // device ready (after window creation)
    std::function<void(gfx::Device&, double dt)> frame;  // called every display refresh; must call beginFrame/endFrame
    std::function<void(const Event&)> event;
    std::function<void()> shutdown;
};

// Runs the main loop. Returns the exit code (on iOS/web it may never return).
int run(const AppConfig& cfg, AppCallbacks cb);

// --- queries valid while running ---
float windowWidth();      // logical points
float windowHeight();
int framebufferWidth();   // pixels
int framebufferHeight();
float dpiScale();         // framebuffer pixels per logical point
bool isTouchDevice();
const char* name();       // "macos", "ios", "android", "web", "windows", "linux"
void requestQuit();
void setTitle(const std::string& title);

// --- text input / on-screen keyboard ---
// Call startTextInput when a text field gets focus and stopTextInput when it loses it. Phones and
// tablets (iOS, Android, mobile browsers) show the on-screen keyboard; on desktops it is a no-op,
// the physical keyboard already sends KeyDown/Char. Typed text arrives as Char events in any
// language (after IME composition), Backspace and Enter as KeyDown + KeyUp.
// `area` is the focused field in logical points; it keeps the field where the user expects it
// (web places its hidden input there, so the browser doesn't scroll or zoom).
// Web: call it while handling the touch/mouse event that focused the field — mobile browsers open
// the keyboard only inside a user gesture.
struct TextInputArea { float x = 0, y = 0, w = 0, h = 0; };
void startTextInput(const TextInputArea& area = {});
void stopTextInput();
bool textInputActive();
// Height of the bottom part of the window covered by the on-screen keyboard, in logical points
// (0 when hidden or on desktop). Move the focused field above it. Changes: EventType::KeyboardChanged.
float softKeyboardHeight();

// --- files ---
// Content (rules, maps, textures) shipped with the app: bundle Resources on Apple, APK assets on
// Android, preloaded virtual FS on web, folder next to the executable elsewhere.
std::string contentRoot();
bool readContentFile(const std::string& relativePath, std::string& out);
// Resource packs: `<content>/content.rpak` is mounted automatically; files in packs win over
// loose files. mountPack adds another pack on top (patch, DLC); path is relative to the content
// root or absolute. Returns false (and logs why) if the pack can't be opened.
bool mountPack(const std::string& path);
// Content hash of the mounted packs (0 when running from loose files) — identifies the data set.
uint64_t contentPacksHash();
// Writable per-user directory (settings, saved replays).
std::string userDataDir();
// Text clipboard (editor copy/paste). May be a no-op on mobile.
std::string clipboardText();
void setClipboardText(const std::string& s);

} // namespace rts::platform

int rtsMain(int argc, char** argv);
