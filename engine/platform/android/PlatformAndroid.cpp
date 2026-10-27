// Android: NativeActivity (android_native_app_glue) + EGL + OpenGL ES 3.
// Content is packed into the APK under assets/content and read through AAssetManager.
#include "../PlatformCommon.h"
#include "../../gfx/Gfx.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <chrono>
#include <memory>

namespace rts::platform {
using detail::state;

namespace {
android_app* g_app = nullptr;
EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLContext g_ctx = EGL_NO_CONTEXT;
EGLSurface g_surf = EGL_NO_SURFACE;
EGLConfig g_cfg = nullptr;
std::unique_ptr<gfx::Device> g_device;
bool g_inited = false;

bool initEGL() {
    if (g_dpy == EGL_NO_DISPLAY) {
        g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglInitialize(g_dpy, nullptr, nullptr);
        const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
        EGLint n = 0;
        if (!eglChooseConfig(g_dpy, attrs, &g_cfg, 1, &n) || n == 0) return false;
        const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctxAttrs);
    }
    g_surf = eglCreateWindowSurface(g_dpy, g_cfg, g_app->window, nullptr);
    if (!eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) return false;
    EGLint w = 0, h = 0;
    eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &w);
    eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &h);
    auto& S = state();
    S.fbW = w; S.fbH = h;
    S.winW = float(w) / S.scale; S.winH = float(h) / S.scale;
    return true;
}

void termSurface() {
    if (g_surf != EGL_NO_SURFACE) {
        eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(g_dpy, g_surf);
        g_surf = EGL_NO_SURFACE;
    }
}

void onCmd(android_app* app, int32_t cmd) {
    auto& S = state();
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (app->window && initEGL()) {
            if (!g_device) {
                g_device = gfx::createGLDevice(gfx::Backend::GLES3);
                if (S.cb.init) S.cb.init(*g_device);
            }
            g_inited = true;
            Event e; e.type = EventType::Resized; detail::dispatch(e);
        }
        break;
    case APP_CMD_TERM_WINDOW: termSurface(); g_inited = false; break;
    case APP_CMD_GAINED_FOCUS: { Event e; e.type = EventType::FocusGained; detail::dispatch(e); break; }
    case APP_CMD_LOST_FOCUS: { Event e; e.type = EventType::FocusLost; detail::dispatch(e); break; }
    case APP_CMD_DESTROY: S.quit = true; break;
    default: break;
    }
}

int32_t onInput(android_app*, AInputEvent* ev) {
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return 0;
    auto& S = state();
    int32_t action = AMotionEvent_getAction(ev);
    int32_t code = action & AMOTION_EVENT_ACTION_MASK;
    size_t idx = size_t((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    auto emit = [&](EventType t, size_t i) {
        Event e; e.type = t;
        e.x = AMotionEvent_getX(ev, i) / S.scale; e.y = AMotionEvent_getY(ev, i) / S.scale;
        e.touchId = AMotionEvent_getPointerId(ev, i);
        detail::dispatch(e);
    };
    switch (code) {
    case AMOTION_EVENT_ACTION_DOWN: case AMOTION_EVENT_ACTION_POINTER_DOWN: emit(EventType::TouchBegin, idx); break;
    case AMOTION_EVENT_ACTION_UP: case AMOTION_EVENT_ACTION_POINTER_UP: emit(EventType::TouchEnd, idx); break;
    case AMOTION_EVENT_ACTION_CANCEL:
        for (size_t i = 0; i < AMotionEvent_getPointerCount(ev); ++i) emit(EventType::TouchCancel, i);
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        for (size_t i = 0; i < AMotionEvent_getPointerCount(ev); ++i) emit(EventType::TouchMove, i);
        break;
    default: return 0;
    }
    return 1;
}
} // namespace

const char* name() { return "android"; }
void setTitle(const std::string&) {}

bool readContentFile(const std::string& rel, std::string& out) {
    if (!g_app) return false;
    AAsset* a = AAssetManager_open(g_app->activity->assetManager, ("content/" + rel).c_str(), AASSET_MODE_BUFFER);
    if (!a) return false;
    out.assign(static_cast<const char*>(AAsset_getBuffer(a)), size_t(AAsset_getLength(a)));
    AAsset_close(a);
    return true;
}

int run(const AppConfig& cfg, AppCallbacks cb) {
    auto& S = state();
    S.cfg = cfg; S.cb = std::move(cb);
    S.touch = true;
    S.contentRoot = "content";
    S.userDir = g_app->activity->internalDataPath ? g_app->activity->internalDataPath : ".";
    AConfiguration* conf = AConfiguration_new();
    AConfiguration_fromAssetManager(conf, g_app->activity->assetManager);
    int32_t density = AConfiguration_getDensity(conf);
    AConfiguration_delete(conf);
    S.scale = density > 0 ? float(density) / 160.0f : 1.0f;

    g_app->onAppCmd = onCmd;
    g_app->onInputEvent = onInput;
    auto last = std::chrono::steady_clock::now();
    while (!S.quit) {
        int events = 0;
        android_poll_source* src = nullptr;
        while (ALooper_pollOnce(g_inited ? 0 : -1, nullptr, &events, reinterpret_cast<void**>(&src)) >= 0) {
            if (src) src->process(g_app, src);
            if (g_app->destroyRequested) { S.quit = true; break; }
        }
        if (!g_inited || S.quit) continue;
        EGLint w = 0, h = 0;
        eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &w);
        eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &h);
        if (w != S.fbW || h != S.fbH) {
            S.fbW = w; S.fbH = h; S.winW = float(w) / S.scale; S.winH = float(h) / S.scale;
            Event e; e.type = EventType::Resized; detail::dispatch(e);
        }
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        g_device->beginFrame(S.fbW, S.fbH);
        if (S.cb.frame) S.cb.frame(*g_device, dt);
        g_device->endFrame();
        eglSwapBuffers(g_dpy, g_surf);
    }
    if (S.cb.shutdown) S.cb.shutdown();
    g_device.reset();
    termSurface();
    if (g_ctx != EGL_NO_CONTEXT) eglDestroyContext(g_dpy, g_ctx);
    if (g_dpy != EGL_NO_DISPLAY) eglTerminate(g_dpy);
    return 0;
}

} // namespace rts::platform

void android_main(android_app* app) {
    rts::platform::g_app = app;
    char arg0[] = "rts";
    char* argv[] = {arg0, nullptr};
    rtsMain(1, argv);
}
