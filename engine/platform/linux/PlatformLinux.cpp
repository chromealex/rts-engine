// Linux: X11 window + EGL + OpenGL ES 3. Used for development and CI (works under Xvfb with Mesa).
// Debug hooks (env):  RTS_SCREENSHOT=out.ppm  RTS_EXIT_AFTER_FRAMES=N  -> capture frame N and exit.
#include "../PlatformCommon.h"
#include "../../gfx/Gfx.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace rts::platform {
using detail::state;

namespace {
Display* g_dpy = nullptr;
Window g_win = 0;
Atom g_wmDelete = 0;

uint8_t mods(unsigned s) {
    uint8_t m = 0;
    if (s & ShiftMask) m |= kModShift;
    if (s & ControlMask) m |= kModCtrl;
    if (s & Mod1Mask) m |= kModAlt;
    if (s & Mod4Mask) m |= kModSuper;
    return m;
}

Key mapKey(KeySym k) {
    if (k >= XK_a && k <= XK_z) return Key(int(Key::A) + int(k - XK_a));
    if (k >= XK_A && k <= XK_Z) return Key(int(Key::A) + int(k - XK_A));
    if (k >= XK_0 && k <= XK_9) return Key(int(Key::Num0) + int(k - XK_0));
    if (k >= XK_F1 && k <= XK_F12) return Key(int(Key::F1) + int(k - XK_F1));
    switch (k) {
        case XK_Escape: return Key::Escape; case XK_Return: return Key::Enter; case XK_Tab: return Key::Tab;
        case XK_BackSpace: return Key::Backspace; case XK_Delete: return Key::Delete; case XK_Insert: return Key::Insert;
        case XK_space: return Key::Space; case XK_Left: return Key::Left; case XK_Right: return Key::Right;
        case XK_Up: return Key::Up; case XK_Down: return Key::Down; case XK_Home: return Key::Home; case XK_End: return Key::End;
        case XK_Page_Up: return Key::PageUp; case XK_Page_Down: return Key::PageDown;
        case XK_Shift_L: return Key::LeftShift; case XK_Shift_R: return Key::RightShift;
        case XK_Control_L: return Key::LeftCtrl; case XK_Control_R: return Key::RightCtrl;
        case XK_Alt_L: return Key::LeftAlt; case XK_Alt_R: return Key::RightAlt;
        case XK_Super_L: return Key::LeftSuper; case XK_Super_R: return Key::RightSuper;
        case XK_minus: return Key::Minus; case XK_equal: return Key::Equal; case XK_comma: return Key::Comma;
        case XK_period: return Key::Period; case XK_slash: return Key::Slash; case XK_semicolon: return Key::Semicolon;
        default: return Key::Unknown;
    }
}

void saveScreenshot(const std::string& path, int w, int h) {
    std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    if (!detail::writePPM(path, w, h, px.data())) std::fprintf(stderr, "screenshot: cannot write %s\n", path.c_str());
}
} // namespace

const char* name() { return "linux"; }
void setTitle(const std::string& t) { if (g_dpy && g_win) XStoreName(g_dpy, g_win, t.c_str()); }

namespace detail {
// EWMH: ask the window manager to toggle _NET_WM_STATE_FULLSCREEN (works on every modern WM).
void applyFullscreen(bool on) {
    if (!g_dpy || !g_win) return;
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = g_win;
    ev.xclient.message_type = XInternAtom(g_dpy, "_NET_WM_STATE", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = on ? 1 : 0; // _NET_WM_STATE_ADD / _REMOVE
    ev.xclient.data.l[1] = long(XInternAtom(g_dpy, "_NET_WM_STATE_FULLSCREEN", False));
    ev.xclient.data.l[2] = 1;          // source: application
    XSendEvent(g_dpy, DefaultRootWindow(g_dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(g_dpy);
    state().fullscreen = on;
}
} // namespace detail

int run(const AppConfig& cfg, AppCallbacks cb) {
    auto& S = state();
    S.cfg = cfg; S.cb = std::move(cb);
    if (const char* c = std::getenv("RTS_CONTENT")) S.contentRoot = c;
    else {
        char exe[4096] = {};
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        std::string dir = n > 0 ? std::string(exe, size_t(n)) : std::string();
        dir = dir.substr(0, dir.find_last_of('/'));
        S.contentRoot = access((dir + "/content").c_str(), F_OK) == 0 ? dir + "/content" : "content";
    }
    if (const char* h = std::getenv("HOME")) S.userDir = std::string(h) + "/.local/share/rtsengine";

    g_dpy = XOpenDisplay(nullptr);
    if (!g_dpy) { std::fprintf(stderr, "cannot open X display\n"); return 1; }
    int scr = DefaultScreen(g_dpy);
    XSetWindowAttributes swa{};
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask | FocusChangeMask;
    g_win = XCreateWindow(g_dpy, RootWindow(g_dpy, scr), 0, 0, unsigned(cfg.width), unsigned(cfg.height), 0,
                          CopyFromParent, InputOutput, CopyFromParent, CWEventMask, &swa);
    XStoreName(g_dpy, g_win, cfg.title.c_str());
    g_wmDelete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_dpy, g_win, &g_wmDelete, 1);
    XMapWindow(g_dpy, g_win);
    XIM xim = XOpenIM(g_dpy, nullptr, nullptr, nullptr);
    XIC xic = xim ? XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, g_win, nullptr) : nullptr;

    EGLDisplay ed = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(g_dpy));
    eglInitialize(ed, nullptr, nullptr);
    // RTS_GL=desktop: desktop GL 3.3 core through EGL (the GL33 backend used on Windows) - for Mesa
    // builds without GLES (dev VMs, CI images); the default stays GLES3 like Android/web.
    const char* glEnv = std::getenv("RTS_GL");
    const bool desktopGL = glEnv && std::strcmp(glEnv, "desktop") == 0;
    eglBindAPI(desktopGL ? EGL_OPENGL_API : EGL_OPENGL_ES_API);
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE, desktopGL ? EGL_OPENGL_BIT : EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
    EGLConfig ec; EGLint n = 0;
    if (!eglChooseConfig(ed, attrs, &ec, 1, &n) || n == 0) { std::fprintf(stderr, "no EGL config\n"); return 1; }
    const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLint glAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                              EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext ctx = eglCreateContext(ed, ec, EGL_NO_CONTEXT, desktopGL ? glAttrs : ctxAttrs);
    EGLSurface surf = eglCreateWindowSurface(ed, ec, static_cast<EGLNativeWindowType>(g_win), nullptr);
    eglMakeCurrent(ed, surf, surf, ctx);
    eglSwapInterval(ed, 1);

    S.winW = float(cfg.width); S.winH = float(cfg.height);
    S.fbW = cfg.width; S.fbH = cfg.height; S.scale = 1.0f;

    auto device = gfx::createGLDevice(desktopGL ? gfx::Backend::GL33 : gfx::Backend::GLES3);
    if (S.cb.init) S.cb.init(*device);

    const detail::CaptureRequest capture = detail::captureRequestFromEnv();
    detail::FrameClock clock(capture.active());
    long frameNo = 0;

    while (!S.quit) {
        while (XPending(g_dpy)) {
            XEvent xe; XNextEvent(g_dpy, &xe);
            if (xic && XFilterEvent(&xe, None)) continue;
            Event e;
            switch (xe.type) {
            case ClientMessage:
                if (Atom(xe.xclient.data.l[0]) == g_wmDelete) { e.type = EventType::QuitRequested; detail::dispatch(e); S.quit = true; }
                break;
            case ConfigureNotify:
                if (xe.xconfigure.width != S.fbW || xe.xconfigure.height != S.fbH) {
                    S.fbW = xe.xconfigure.width; S.fbH = xe.xconfigure.height;
                    S.winW = float(S.fbW); S.winH = float(S.fbH);
                    e.type = EventType::Resized; detail::dispatch(e);
                }
                break;
            case MotionNotify:
                e.type = EventType::MouseMove; e.x = float(xe.xmotion.x); e.y = float(xe.xmotion.y); e.mods = mods(xe.xmotion.state);
                detail::dispatch(e); break;
            case ButtonPress: case ButtonRelease: {
                e.x = float(xe.xbutton.x); e.y = float(xe.xbutton.y); e.mods = mods(xe.xbutton.state);
                unsigned b = xe.xbutton.button;
                if (b >= 4 && b <= 7) {
                    if (xe.type == ButtonPress) {
                        e.type = EventType::MouseWheel;
                        e.wheelY = b == 4 ? 1.f : b == 5 ? -1.f : 0.f;
                        e.wheelX = b == 6 ? 1.f : b == 7 ? -1.f : 0.f;
                        detail::dispatch(e);
                    }
                    break;
                }
                e.type = xe.type == ButtonPress ? EventType::MouseDown : EventType::MouseUp;
                e.button = b == 1 ? 0 : b == 3 ? 1 : 2;
                detail::dispatch(e); break;
            }
            case KeyPress: case KeyRelease: {
                KeySym ks = XLookupKeysym(&xe.xkey, 0);
                e.type = xe.type == KeyPress ? EventType::KeyDown : EventType::KeyUp;
                e.key = mapKey(ks); e.mods = mods(xe.xkey.state);
                detail::dispatch(e);
                if (xe.type == KeyPress && e.key == Key::F11) detail::applyFullscreen(!state().fullscreen);
                if (xe.type == KeyPress) {
                    char buf[16] = {}; KeySym sym; Status st;
                    int len = xic ? Xutf8LookupString(xic, &xe.xkey, buf, sizeof buf - 1, &sym, &st)
                                  : XLookupString(&xe.xkey, buf, sizeof buf - 1, &sym, nullptr);
                    if (len > 0 && static_cast<unsigned char>(buf[0]) >= 32 && buf[0] != 127) {
                        // decode UTF-8 (1..4 bytes)
                        const unsigned char* p = reinterpret_cast<unsigned char*>(buf);
                        uint32_t cp = p[0];
                        if (cp >= 0xF0 && len >= 4) cp = ((cp & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63);
                        else if (cp >= 0xE0 && len >= 3) cp = ((cp & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63);
                        else if (cp >= 0xC0 && len >= 2) cp = ((cp & 31) << 6) | (p[1] & 63);
                        Event c; c.type = EventType::Char; c.codepoint = cp; detail::dispatch(c);
                    }
                }
                break;
            }
            case FocusIn: e.type = EventType::FocusGained; detail::dispatch(e); break;
            case FocusOut: e.type = EventType::FocusLost; detail::dispatch(e); break;
            default: break;
            }
        }
        double dt = clock.tick(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());

        device->beginFrame(S.fbW, S.fbH);
        if (S.cb.frame) S.cb.frame(*device, dt);
        device->endFrame();
        ++frameNo;
        if (capture.active() && frameNo >= capture.frames) {
            if (!capture.path.empty()) saveScreenshot(capture.path, S.fbW, S.fbH);
            S.quit = true;
        }
        eglSwapBuffers(ed, surf);
    }
    if (S.cb.shutdown) S.cb.shutdown();
    device.reset();
    eglMakeCurrent(ed, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(ed, surf); eglDestroyContext(ed, ctx); eglTerminate(ed);
    XDestroyWindow(g_dpy, g_win); XCloseDisplay(g_dpy);
    return 0;
}

} // namespace rts::platform

int main(int argc, char** argv) { return rtsMain(argc, argv); }
