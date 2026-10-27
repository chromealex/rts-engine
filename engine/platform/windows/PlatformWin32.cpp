// Windows: Win32 window + WGL OpenGL 3.3 core (GL backend). A D3D11 backend can replace WGL later
// without touching anything above the RHI.
#include "../PlatformCommon.h"
#include "../../gfx/Gfx.h"
#include "../../gfx/gl/GLApi.h"

#include <shellapi.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace rts::platform {
using detail::state;

namespace {
HWND g_hwnd = nullptr;
HDC g_hdc = nullptr;
HGLRC g_rc = nullptr;
bool g_surrogatePending = false;
wchar_t g_highSurrogate = 0;

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

uint8_t currentMods() {
    uint8_t m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= kModShift;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= kModCtrl;
    if (GetKeyState(VK_MENU) & 0x8000) m |= kModAlt;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= kModSuper;
    return m;
}

Key mapVK(WPARAM vk, LPARAM lp) {
    if (vk >= 'A' && vk <= 'Z') return Key(int(Key::A) + int(vk - 'A'));
    if (vk >= '0' && vk <= '9') return Key(int(Key::Num0) + int(vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F12) return Key(int(Key::F1) + int(vk - VK_F1));
    const bool ext = (lp >> 24) & 1;
    switch (vk) {
        case VK_ESCAPE: return Key::Escape; case VK_RETURN: return Key::Enter; case VK_TAB: return Key::Tab;
        case VK_BACK: return Key::Backspace; case VK_DELETE: return Key::Delete; case VK_INSERT: return Key::Insert;
        case VK_SPACE: return Key::Space; case VK_LEFT: return Key::Left; case VK_RIGHT: return Key::Right;
        case VK_UP: return Key::Up; case VK_DOWN: return Key::Down; case VK_HOME: return Key::Home; case VK_END: return Key::End;
        case VK_PRIOR: return Key::PageUp; case VK_NEXT: return Key::PageDown;
        case VK_SHIFT: return MapVirtualKeyW(UINT((lp >> 16) & 0xff), MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? Key::RightShift : Key::LeftShift;
        case VK_CONTROL: return ext ? Key::RightCtrl : Key::LeftCtrl;
        case VK_MENU: return ext ? Key::RightAlt : Key::LeftAlt;
        case VK_LWIN: return Key::LeftSuper; case VK_RWIN: return Key::RightSuper;
        case VK_OEM_MINUS: return Key::Minus; case VK_OEM_PLUS: return Key::Equal; case VK_OEM_COMMA: return Key::Comma;
        case VK_OEM_PERIOD: return Key::Period; case VK_OEM_2: return Key::Slash; case VK_OEM_1: return Key::Semicolon;
        default: return Key::Unknown;
    }
}

void updateSize() {
    auto& S = state();
    RECT r; GetClientRect(g_hwnd, &r);
    UINT dpi = 96;
    if (auto fn = reinterpret_cast<UINT(WINAPI*)(HWND)>(GetProcAddress(GetModuleHandleW(L"user32"), "GetDpiForWindow"))) dpi = fn(g_hwnd);
    S.scale = S.cfg.highDpi ? float(dpi) / 96.0f : 1.0f;
    S.fbW = r.right - r.left; S.fbH = r.bottom - r.top;
    S.winW = float(S.fbW) / S.scale; S.winH = float(S.fbH) / S.scale;
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto& S = state();
    Event e;
    auto mouse = [&](EventType t, int button) {
        e.type = t; e.button = button;
        e.x = float(short(LOWORD(lp))) / S.scale; e.y = float(short(HIWORD(lp))) / S.scale;
        e.mods = currentMods();
        detail::dispatch(e);
    };
    switch (msg) {
    case WM_CLOSE: e.type = EventType::QuitRequested; detail::dispatch(e); S.quit = true; return 0;
    case WM_SIZE: updateSize(); e.type = EventType::Resized; detail::dispatch(e); return 0;
    case WM_DPICHANGED: {
        RECT* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        updateSize(); return 0;
    }
    case WM_MOUSEMOVE: mouse(EventType::MouseMove, 0); return 0;
    case WM_LBUTTONDOWN: SetCapture(h); mouse(EventType::MouseDown, 0); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); mouse(EventType::MouseUp, 0); return 0;
    case WM_RBUTTONDOWN: SetCapture(h); mouse(EventType::MouseDown, 1); return 0;
    case WM_RBUTTONUP: ReleaseCapture(); mouse(EventType::MouseUp, 1); return 0;
    case WM_MBUTTONDOWN: SetCapture(h); mouse(EventType::MouseDown, 2); return 0;
    case WM_MBUTTONUP: ReleaseCapture(); mouse(EventType::MouseUp, 2); return 0;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        POINT p{short(LOWORD(lp)), short(HIWORD(lp))}; ScreenToClient(h, &p);
        e.type = EventType::MouseWheel; e.x = float(p.x) / S.scale; e.y = float(p.y) / S.scale;
        float d = float(GET_WHEEL_DELTA_WPARAM(wp)) / float(WHEEL_DELTA);
        if (msg == WM_MOUSEWHEEL) e.wheelY = d; else e.wheelX = -d;
        e.mods = currentMods();
        detail::dispatch(e); return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        e.type = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) ? EventType::KeyDown : EventType::KeyUp;
        e.key = mapVK(wp, lp); e.mods = currentMods(); e.repeat = (lp >> 30) & 1;
        detail::dispatch(e);
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) break; // keep Alt+F4
        return 0;
    case WM_CHAR: {
        wchar_t c = wchar_t(wp);
        uint32_t cp = c;
        if (c >= 0xD800 && c <= 0xDBFF) { g_highSurrogate = c; g_surrogatePending = true; return 0; }
        if (c >= 0xDC00 && c <= 0xDFFF && g_surrogatePending) { cp = 0x10000 + ((uint32_t(g_highSurrogate) - 0xD800) << 10) + (c - 0xDC00); g_surrogatePending = false; }
        if (cp >= 32 && cp != 127) { e.type = EventType::Char; e.codepoint = cp; detail::dispatch(e); }
        return 0;
    }
    case WM_SETFOCUS: e.type = EventType::FocusGained; detail::dispatch(e); return 0;
    case WM_KILLFOCUS: e.type = EventType::FocusLost; detail::dispatch(e); return 0;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool createGLContext() {
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof pfd; pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 24; pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(g_hdc, &pfd);
    if (!pf || !SetPixelFormat(g_hdc, pf, &pfd)) return false;
    HGLRC legacy = wglCreateContext(g_hdc);
    wglMakeCurrent(g_hdc, legacy);
    using CreateCtxFn = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
    auto createCtx = reinterpret_cast<CreateCtxFn>(wglGetProcAddress("wglCreateContextAttribsARB"));
    if (!createCtx) return false;
    const int attrs[] = {0x2091 /*MAJOR*/, 3, 0x2092 /*MINOR*/, 3, 0x9126 /*PROFILE_MASK*/, 0x1 /*CORE*/, 0};
    g_rc = createCtx(g_hdc, nullptr, attrs);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(legacy);
    if (!g_rc) return false;
    wglMakeCurrent(g_hdc, g_rc);
    using SwapFn = BOOL(WINAPI*)(int);
    if (auto swap = reinterpret_cast<SwapFn>(wglGetProcAddress("wglSwapIntervalEXT"))) swap(1);
    return gfx::loadGLFunctions();
}
} // namespace

const char* name() { return "windows"; }
void setTitle(const std::string& t) { if (g_hwnd) SetWindowTextW(g_hwnd, widen(t).c_str()); }

std::string clipboardText() {
    std::string out;
    if (!OpenClipboard(g_hwnd)) return out;
    if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* w = static_cast<const wchar_t*>(GlobalLock(hd))) { out = narrow(w); GlobalUnlock(hd); }
    }
    CloseClipboard();
    return out;
}
void setClipboardText(const std::string& s) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    std::wstring w = widen(s);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (mem) {
        memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
}

int run(const AppConfig& cfg, AppCallbacks cb) {
    auto& S = state();
    S.cfg = cfg; S.cb = std::move(cb);

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::string exeDir = narrow(exe);
    exeDir = exeDir.substr(0, exeDir.find_last_of("\\/"));
    auto getenvW = [](const wchar_t* n) -> std::string {
        wchar_t buf[2048];
        DWORD len = GetEnvironmentVariableW(n, buf, 2048);
        return len > 0 && len < 2048 ? narrow(buf) : std::string();
    };
    if (std::string env = getenvW(L"RTS_CONTENT"); !env.empty()) S.contentRoot = env;
    else if (GetFileAttributesW(widen(exeDir + "\\content").c_str()) != INVALID_FILE_ATTRIBUTES) S.contentRoot = exeDir + "\\content";
    else S.contentRoot = "content";
    if (std::string ad = getenvW(L"APPDATA"); !ad.empty()) S.userDir = ad + "\\" + cfg.title;

    if (auto fn = reinterpret_cast<BOOL(WINAPI*)(HANDLE)>(GetProcAddress(GetModuleHandleW(L"user32"), "SetProcessDpiAwarenessContext")))
        fn(reinterpret_cast<HANDLE>(-4)); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc; wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc = wndProc;
    wc.hInstance = inst; wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /* IDC_ARROW */); wc.lpszClassName = L"RTSEngineWindow";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    RECT r{0, 0, cfg.width, cfg.height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, widen(cfg.title).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    g_hdc = GetDC(g_hwnd);
    if (!createGLContext()) { MessageBoxW(nullptr, L"OpenGL 3.3 is required", L"RTSEngine", MB_ICONERROR); return 1; }
    ShowWindow(g_hwnd, SW_SHOW);
    updateSize();

    auto device = gfx::createGLDevice(gfx::Backend::GL33);
    if (S.cb.init) S.cb.init(*device);
    auto last = std::chrono::steady_clock::now();
    // CI hooks (same as Linux): RTS_SCREENSHOT=out.ppm RTS_EXIT_AFTER_FRAMES=N
    std::string shot = getenvW(L"RTS_SCREENSHOT");
    long maxFrames = std::atol(getenvW(L"RTS_EXIT_AFTER_FRAMES").c_str());
    long frameNo = 0;
    while (!S.quit) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) S.quit = true;
            TranslateMessage(&m); DispatchMessageW(&m);
        }
        if (S.quit) break;
        if (IsIconic(g_hwnd) || S.fbW <= 0 || S.fbH <= 0) { Sleep(16); continue; }
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        device->beginFrame(S.fbW, S.fbH);
        if (S.cb.frame) S.cb.frame(*device, dt);
        device->endFrame();
        if (maxFrames > 0 && ++frameNo >= maxFrames) {
            if (!shot.empty()) {
                std::vector<unsigned char> px(size_t(S.fbW) * S.fbH * 4);
                glReadPixels(0, 0, S.fbW, S.fbH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                if (FILE* f = std::fopen(shot.c_str(), "wb")) {
                    std::fprintf(f, "P6\n%d %d\n255\n", S.fbW, S.fbH);
                    for (int y = S.fbH - 1; y >= 0; --y)
                        for (int x = 0; x < S.fbW; ++x) std::fwrite(&px[(size_t(y) * S.fbW + x) * 4], 1, 3, f);
                    std::fclose(f);
                }
            }
            S.quit = true;
        }
        SwapBuffers(g_hdc);
    }
    if (S.cb.shutdown) S.cb.shutdown();
    device.reset();
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(g_rc);
    ReleaseDC(g_hwnd, g_hdc);
    DestroyWindow(g_hwnd);
    return 0;
}

} // namespace rts::platform

int main(int argc, char** argv) { return rtsMain(argc, argv); }
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return rtsMain(__argc, __argv); }
