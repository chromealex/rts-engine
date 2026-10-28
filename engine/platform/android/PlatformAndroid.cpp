// Android: NativeActivity (android_native_app_glue) + EGL + OpenGL ES 3.
// Content is packed into the APK under assets/content and read through AAssetManager.
#include "../PlatformCommon.h"
#include "../../gfx/Gfx.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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

// ---- on-screen keyboard (RtsActivity.java). Java calls arrive on the UI thread; they are queued
// and dispatched on the game thread, which owns the UI and the sim.
struct TextMsg { enum Kind { Text, Backspace, Enter, Keyboard, Ended } kind; std::string text; int px = 0; };
std::mutex g_textMutex;
std::vector<TextMsg> g_textQueue;
void pushText(TextMsg m) { std::lock_guard<std::mutex> l(g_textMutex); g_textQueue.push_back(std::move(m)); }

void drainTextQueue() {
    std::vector<TextMsg> q;
    { std::lock_guard<std::mutex> l(g_textMutex); q.swap(g_textQueue); }
    for (auto& m : q) {
        switch (m.kind) {
        case TextMsg::Text: detail::dispatchText(m.text); break;
        case TextMsg::Backspace: detail::dispatchKeyPress(Key::Backspace); break;
        case TextMsg::Enter: detail::dispatchKeyPress(Key::Enter); break;
        case TextMsg::Keyboard: detail::setKeyboardHeight(float(m.px) / state().scale); break;
        case TextMsg::Ended: detail::textInputEndedByUser(); break;
        }
    }
}

// JNIEnv of the game thread (attached on first use, detached when android_main ends).
JNIEnv* gameThreadEnv() {
    static thread_local JNIEnv* env = nullptr;
    if (!env && g_app) g_app->activity->vm->AttachCurrentThread(&env, nullptr);
    return env;
}
detail::FrameClock g_clock;

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
    case APP_CMD_TERM_WINDOW: termSurface(); g_inited = false; g_clock.reset(); break;
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

namespace detail {
void showSoftKeyboard(bool show, const TextInputArea&) {
    JNIEnv* env = gameThreadEnv();
    if (!env) return;
    jobject activity = g_app->activity->clazz;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, "showTextInput", "(Z)V");
    if (m) env->CallVoidMethod(activity, m, jboolean(show));
    else __android_log_print(ANDROID_LOG_WARN, "rts", "activity has no showTextInput: not RtsActivity?");
    if (env->ExceptionCheck()) env->ExceptionClear(); // never let a Java error kill the game thread
    env->DeleteLocalRef(cls);
}
} // namespace detail

const char* name() { return "android"; }
void setTitle(const std::string&) {}

namespace {
// Packs inside the APK. `noCompress 'rpak'` in Gradle keeps them uncompressed in the APK, so
// AASSET_MODE_BUFFER maps the file instead of inflating it into memory.
class AssetSource final : public pack::Source {
public:
    explicit AssetSource(AAsset* a) : a_(a), size_(uint64_t(AAsset_getLength64(a))), map_(static_cast<const uint8_t*>(AAsset_getBuffer(a))) {}
    ~AssetSource() override { AAsset_close(a_); }
    uint64_t size() const override { return size_; }
    bool read(uint64_t off, void* dst, size_t n) override {
        if (off > size_ || n > size_ - off) return false;
        if (map_) { std::memcpy(dst, map_ + off, n); return true; }
        if (AAsset_seek64(a_, off64_t(off), SEEK_SET) < 0) return false;
        return AAsset_read(a_, dst, n) == int(n);
    }
    const uint8_t* mapped() const override { return map_; }

private:
    AAsset* a_;
    uint64_t size_;
    const uint8_t* map_;
};

std::unique_ptr<pack::Source> openAssetPack(const std::string& path) {
    if (!g_app) return nullptr;
    AAsset* a = AAssetManager_open(g_app->activity->assetManager, path.c_str(), AASSET_MODE_BUFFER);
    return a ? std::make_unique<AssetSource>(a) : nullptr;
}
} // namespace

bool mountPack(const std::string& path) {
    detail::ensurePacks(openAssetPack);
    return detail::mountPackWith(openAssetPack, path.rfind("content/", 0) == 0 ? path : "content/" + path);
}

bool readContentFile(const std::string& rel, std::string& out) {
    if (!g_app) return false;
    detail::ensurePacks(openAssetPack);
    if (!state().packs.empty() && state().packs.read(rel, out)) return true;
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
    while (!S.quit) {
        int events = 0;
        android_poll_source* src = nullptr;
        while (ALooper_pollOnce(g_inited ? 0 : -1, nullptr, &events, reinterpret_cast<void**>(&src)) >= 0) {
            if (src) src->process(g_app, src);
            if (g_app->destroyRequested) { S.quit = true; break; }
        }
        if (g_device) drainTextQueue();
        if (!g_inited || S.quit) continue;
        EGLint w = 0, h = 0;
        eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &w);
        eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &h);
        if (w != S.fbW || h != S.fbH) {
            S.fbW = w; S.fbH = h; S.winW = float(w) / S.scale; S.winH = float(h) / S.scale;
            Event e; e.type = EventType::Resized; detail::dispatch(e);
        }
        double dt = g_clock.tick(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
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

// ---- JNI entry points of com.rtsengine.platform.RtsActivity (UI thread)
namespace {
std::string utf8(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr); // modified UTF-8; fine for BMP text,
    std::string r = c ? c : "";                          // supplementary chars are rebuilt below
    if (c) env->ReleaseStringUTFChars(s, c);
    // modified UTF-8 encodes U+10000.. as two 3-byte surrogates: merge them into 4-byte UTF-8
    std::string out;
    for (size_t i = 0; i < r.size();) {
        auto b = [&](size_t k) { return uint32_t(static_cast<unsigned char>(r[k])); };
        if (i + 5 < r.size() && b(i) == 0xED && (b(i + 1) & 0xF0) == 0xA0 && b(i + 3) == 0xED && (b(i + 4) & 0xF0) == 0xB0) {
            uint32_t hi = 0xD000 | ((b(i + 1) & 0x3F) << 6) | (b(i + 2) & 0x3F);
            uint32_t lo = 0xD000 | ((b(i + 4) & 0x3F) << 6) | (b(i + 5) & 0x3F);
            uint32_t cp = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
            out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63));
            out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63));
            i += 6;
        } else {
            out += r[i++];
        }
    }
    return out;
}
} // namespace

extern "C" {
JNIEXPORT void JNICALL Java_com_rtsengine_platform_RtsActivity_nativeText(JNIEnv* env, jclass, jstring s) {
    rts::platform::pushText({rts::platform::TextMsg::Text, utf8(env, s)});
}
JNIEXPORT void JNICALL Java_com_rtsengine_platform_RtsActivity_nativeKey(JNIEnv*, jclass, jint key) {
    rts::platform::pushText({key == 0 ? rts::platform::TextMsg::Backspace : rts::platform::TextMsg::Enter, {}});
}
JNIEXPORT void JNICALL Java_com_rtsengine_platform_RtsActivity_nativeKeyboard(JNIEnv*, jclass, jint px) {
    rts::platform::TextMsg m{rts::platform::TextMsg::Keyboard, {}}; m.px = px;
    rts::platform::pushText(std::move(m));
}
JNIEXPORT void JNICALL Java_com_rtsengine_platform_RtsActivity_nativeTextInputEnded(JNIEnv*, jclass) {
    rts::platform::pushText({rts::platform::TextMsg::Ended, {}});
}
}

void android_main(android_app* app) {
    rts::platform::g_app = app;
    char arg0[] = "rts";
    char* argv[] = {arg0, nullptr};
    rtsMain(1, argv);
    app->activity->vm->DetachCurrentThread();
    // NativeActivity may call android_main again in the same process when the activity is
    // recreated, but the engine and the app keep function-level statics (platform state with
    // quit=true, the game client) that are not designed to be re-entered. Ending the process
    // gives the next launch a clean start, which is what players expect after "back".
    std::exit(0);
}
