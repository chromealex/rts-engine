// Android: NativeActivity (android_native_app_glue) + EGL + OpenGL ES 3.
// Content is packed into the APK under assets/content and read through AAssetManager.
#include "../PlatformCommon.h"
#include "../Plugins.h"
#include "../../gfx/Gfx.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/asset_manager.h>
#include <android/input.h>
#include <android/keycodes.h>
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
struct TextMsg { enum Kind { Text, Backspace, Enter, Keyboard, Ended, SafeArea } kind; std::string text; int px = 0; int inset[4] = {}; };
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
        case TextMsg::SafeArea: {
            const float s = state().scale;
            detail::setSafeArea({float(m.inset[0]) / s, float(m.inset[1]) / s, float(m.inset[2]) / s, float(m.inset[3]) / s});
            break;
        }
        }
    }
}

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

Key mapKeycode(int32_t k) {
    if (k >= AKEYCODE_A && k <= AKEYCODE_Z) return Key(int(Key::A) + (k - AKEYCODE_A));
    if (k >= AKEYCODE_0 && k <= AKEYCODE_9) return Key(int(Key::Num0) + (k - AKEYCODE_0));
    if (k >= AKEYCODE_F1 && k <= AKEYCODE_F12) return Key(int(Key::F1) + (k - AKEYCODE_F1));
    switch (k) {
    case AKEYCODE_BACK: case AKEYCODE_ESCAPE: return Key::Escape; // Back = Escape: pause / close a menu
    case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: return Key::Enter;
    case AKEYCODE_DEL: return Key::Backspace;
    case AKEYCODE_FORWARD_DEL: return Key::Delete;
    case AKEYCODE_TAB: return Key::Tab;
    case AKEYCODE_SPACE: return Key::Space;
    case AKEYCODE_INSERT: return Key::Insert;
    case AKEYCODE_DPAD_LEFT: return Key::Left;
    case AKEYCODE_DPAD_RIGHT: return Key::Right;
    case AKEYCODE_DPAD_UP: return Key::Up;
    case AKEYCODE_DPAD_DOWN: return Key::Down;
    case AKEYCODE_MOVE_HOME: return Key::Home;
    case AKEYCODE_MOVE_END: return Key::End;
    case AKEYCODE_PAGE_UP: return Key::PageUp;
    case AKEYCODE_PAGE_DOWN: return Key::PageDown;
    case AKEYCODE_SHIFT_LEFT: return Key::LeftShift;
    case AKEYCODE_SHIFT_RIGHT: return Key::RightShift;
    case AKEYCODE_CTRL_LEFT: return Key::LeftCtrl;
    case AKEYCODE_CTRL_RIGHT: return Key::RightCtrl;
    case AKEYCODE_ALT_LEFT: return Key::LeftAlt;
    case AKEYCODE_ALT_RIGHT: return Key::RightAlt;
    case AKEYCODE_META_LEFT: return Key::LeftSuper;
    case AKEYCODE_META_RIGHT: return Key::RightSuper;
    case AKEYCODE_MINUS: return Key::Minus;
    case AKEYCODE_EQUALS: return Key::Equal;
    case AKEYCODE_LEFT_BRACKET: return Key::LeftBracket;
    case AKEYCODE_RIGHT_BRACKET: return Key::RightBracket;
    case AKEYCODE_SEMICOLON: return Key::Semicolon;
    case AKEYCODE_APOSTROPHE: return Key::Apostrophe;
    case AKEYCODE_COMMA: return Key::Comma;
    case AKEYCODE_PERIOD: return Key::Period;
    case AKEYCODE_SLASH: return Key::Slash;
    case AKEYCODE_BACKSLASH: return Key::Backslash;
    case AKEYCODE_GRAVE: return Key::GraveAccent;
    default: return Key::Unknown;
    }
}

uint8_t modsOf(int32_t meta) {
    return uint8_t(((meta & AMETA_SHIFT_ON) ? kModShift : 0) | ((meta & AMETA_CTRL_ON) ? kModCtrl : 0) |
                   ((meta & AMETA_ALT_ON) ? kModAlt : 0) | ((meta & AMETA_META_ON) ? kModSuper : 0));
}

// The character a hardware key produces with the current layout and modifiers (KeyCharacterMap via
// android.view.KeyEvent.getUnicodeChar): the NDK key event carries only the key code.
uint32_t unicodeOf(int32_t action, int32_t code, int32_t meta) {
    JNIEnv* env = gameThreadEnv();
    if (!env) return 0;
    static jclass cls = nullptr;
    static jmethodID ctor = nullptr, getChar = nullptr;
    if (!cls) {
        jclass local = env->FindClass("android/view/KeyEvent");
        if (!local) { env->ExceptionClear(); return 0; }
        cls = jclass(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        ctor = env->GetMethodID(cls, "<init>", "(II)V");
        getChar = env->GetMethodID(cls, "getUnicodeChar", "(I)I");
    }
    jobject ke = env->NewObject(cls, ctor, action, code);
    jint ch = ke ? env->CallIntMethod(ke, getChar, meta) : 0;
    if (ke) env->DeleteLocalRef(ke);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
    return ch > 0 ? uint32_t(ch) : 0;
}

int32_t onKey(AInputEvent* ev) {
    const int32_t code = AKeyEvent_getKeyCode(ev), action = AKeyEvent_getAction(ev), meta = AKeyEvent_getMetaState(ev);
    const Key key = mapKeycode(code);
    if (key == Key::Unknown) {
        // volume, media, camera... stay with the system; unmapped printable keys still type
        if (action != AKEY_EVENT_ACTION_DOWN) return 0;
        uint32_t cp = unicodeOf(action, code, meta);
        if (cp < 32) return 0;
        Event c; c.type = EventType::Char; c.codepoint = cp; detail::dispatch(c);
        return 1;
    }
    if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) return 1;
    Event e;
    e.type = action == AKEY_EVENT_ACTION_DOWN ? EventType::KeyDown : EventType::KeyUp;
    e.key = key; e.mods = modsOf(meta); e.repeat = AKeyEvent_getRepeatCount(ev) > 0;
    detail::dispatch(e);
    if (action == AKEY_EVENT_ACTION_DOWN && code != AKEYCODE_BACK && !(meta & (AMETA_CTRL_ON | AMETA_META_ON))) {
        uint32_t cp = unicodeOf(action, code, meta);
        if (cp >= 32 && cp != 127) { Event c; c.type = EventType::Char; c.codepoint = cp; detail::dispatch(c); }
    }
    return 1; // consumed: Back must not finish the activity, the game decides
}

// Mouse / touchpad (Chromebooks, Samsung DeX, OTG mice): real buttons, hover and wheel instead of touches.
int32_t onMouse(AInputEvent* ev) {
    auto& S = state();
    const int32_t code = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;
    Event e;
    e.x = AMotionEvent_getX(ev, 0) / S.scale; e.y = AMotionEvent_getY(ev, 0) / S.scale;
    e.mods = modsOf(AMotionEvent_getMetaState(ev));
    auto buttonOf = [](int32_t b) { return b == AMOTION_EVENT_BUTTON_SECONDARY ? 1 : b == AMOTION_EVENT_BUTTON_TERTIARY ? 2 : 0; };
    switch (code) {
    case AMOTION_EVENT_ACTION_BUTTON_PRESS: e.type = EventType::MouseDown; e.button = buttonOf(AMotionEvent_getActionButton(ev)); break;
    case AMOTION_EVENT_ACTION_BUTTON_RELEASE: e.type = EventType::MouseUp; e.button = buttonOf(AMotionEvent_getActionButton(ev)); break;
    case AMOTION_EVENT_ACTION_MOVE: case AMOTION_EVENT_ACTION_HOVER_MOVE: case AMOTION_EVENT_ACTION_HOVER_ENTER: e.type = EventType::MouseMove; break;
    case AMOTION_EVENT_ACTION_SCROLL:
        e.type = EventType::MouseWheel;
        e.wheelX = -AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HSCROLL, 0);
        e.wheelY = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_VSCROLL, 0);
        break;
    case AMOTION_EVENT_ACTION_DOWN: case AMOTION_EVENT_ACTION_UP: return 1; // BUTTON_PRESS/RELEASE carry the button
    default: return 0;
    }
    detail::dispatch(e);
    return 1;
}

int32_t onInput(android_app*, AInputEvent* ev) {
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY) return onKey(ev);
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return 0;
    if (AMotionEvent_getToolType(ev, 0) == AMOTION_EVENT_TOOL_TYPE_MOUSE) return onMouse(ev);
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

namespace {
jstring toJString(JNIEnv* env, const std::string& s) {
    std::u16string u; // NewStringUTF expects modified UTF-8; go through UTF-16 for emoji etc.
    for (size_t i = 0; i < s.size();) {
        uint32_t c = static_cast<unsigned char>(s[i]);
        int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (n) c &= 0x3Fu >> n;
        ++i;
        for (; n > 0 && i < s.size(); --n, ++i) c = (c << 6) | (static_cast<unsigned char>(s[i]) & 0x3Fu);
        if (c >= 0x10000) { c -= 0x10000; u += char16_t(0xD800 + (c >> 10)); u += char16_t(0xDC00 + (c & 0x3FF)); }
        else u += char16_t(c);
    }
    return env->NewString(reinterpret_cast<const jchar*>(u.data()), jsize(u.size()));
}
} // namespace

// ---- native plugins: Java classes registered in the manifest (RtsActivity / RtsPlugin.java)
namespace {
struct AndroidPluginBackend : plugins::detail::Backend {
    bool available(const std::string& name) override {
        JNIEnv* env = gameThreadEnv();
        if (!env) return false;
        jobject act = g_app->activity->clazz;
        jclass cls = env->GetObjectClass(act);
        jmethodID m = env->GetMethodID(cls, "pluginAvailable", "(Ljava/lang/String;)Z");
        jstring js = toJString(env, name);
        bool r = m && env->CallBooleanMethod(act, m, js);
        if (env->ExceptionCheck()) { env->ExceptionClear(); r = false; }
        env->DeleteLocalRef(js);
        env->DeleteLocalRef(cls);
        return r;
    }
    void call(const std::string& name, const std::string& method, const std::string& args, uint64_t id) override {
        JNIEnv* env = gameThreadEnv();
        if (!env) { plugins::detail::completeCall(id, false, "no JNI environment"); return; }
        jobject act = g_app->activity->clazz;
        jclass cls = env->GetObjectClass(act);
        jmethodID m = env->GetMethodID(cls, "pluginCall", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;J)V");
        if (!m) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            plugins::detail::completeCall(id, false, "activity has no pluginCall: not RtsActivity");
        } else {
            jstring a = toJString(env, name), b = toJString(env, method), c = toJString(env, args);
            env->CallVoidMethod(act, m, a, b, c, jlong(id));
            if (env->ExceptionCheck()) { env->ExceptionClear(); plugins::detail::completeCall(id, false, "pluginCall threw"); }
            env->DeleteLocalRef(a); env->DeleteLocalRef(b); env->DeleteLocalRef(c);
        }
        env->DeleteLocalRef(cls);
    }
} g_pluginBackend;
} // namespace

std::string clipboardText() {
    JNIEnv* env = gameThreadEnv();
    if (!env) return state().clipboard;
    jobject act = g_app->activity->clazz;
    jclass cls = env->GetObjectClass(act);
    jmethodID m = env->GetMethodID(cls, "clipboardText", "()Ljava/lang/String;");
    std::string out = state().clipboard;
    if (m) {
        auto js = jstring(env->CallObjectMethod(act, m));
        if (!env->ExceptionCheck() && js) out = utf8(env, js);
        if (js) env->DeleteLocalRef(js);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return out;
}

void setClipboardText(const std::string& s) {
    state().clipboard = s;
    JNIEnv* env = gameThreadEnv();
    if (!env) return;
    jobject act = g_app->activity->clazz;
    jclass cls = env->GetObjectClass(act);
    if (jmethodID m = env->GetMethodID(cls, "setClipboardText", "(Ljava/lang/String;)V")) {
        jstring js = toJString(env, s);
        env->CallVoidMethod(act, m, js);
        env->DeleteLocalRef(js);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
}
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
    if (!state().packs.empty()) {
        if (state().packs.read(rel, out)) return true;
        detail::requestChunkFor(rel);
        if (state().packs.read(rel, out)) return true;
    }
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
    pack::setAndroidJavaVm(g_app->activity->vm);   // HTTP downloads of pack chunks
    AConfiguration* conf = AConfiguration_new();
    AConfiguration_fromAssetManager(conf, g_app->activity->assetManager);
    int32_t density = AConfiguration_getDensity(conf);
    AConfiguration_delete(conf);
    S.scale = density > 0 ? float(density) / 160.0f : 1.0f;

    g_app->onAppCmd = onCmd;
    g_app->onInputEvent = onInput;
    plugins::detail::setBackend(&g_pluginBackend);
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

extern "C" {
JNIEXPORT void JNICALL Java_com_rtsengine_platform_RtsActivity_nativeText(JNIEnv* env, jclass, jstring s) {
    rts::platform::pushText({rts::platform::TextMsg::Text, rts::platform::utf8(env, s)});
}
JNIEXPORT void JNICALL Java_com_rtsengine_platform_RtsActivity_nativeKey(JNIEnv*, jclass, jint key) {
    rts::platform::pushText({key == 0 ? rts::platform::TextMsg::Backspace : rts::platform::TextMsg::Enter, {}});
}
JNIEXPORT void JNICALL Java_com_rtsengine_platform_RtsActivity_nativeKeyboard(JNIEnv*, jclass, jint px) {
    rts::platform::TextMsg m{rts::platform::TextMsg::Keyboard, {}}; m.px = px;
    rts::platform::pushText(std::move(m));
}
JNIEXPORT void JNICALL Java_com_rtsengine_platform_RtsActivity_nativeSafeArea(JNIEnv*, jclass, jint l, jint t, jint r, jint b) {
    rts::platform::TextMsg m{rts::platform::TextMsg::SafeArea, {}};
    m.inset[0] = l; m.inset[1] = t; m.inset[2] = r; m.inset[3] = b;
    rts::platform::pushText(std::move(m));
}
JNIEXPORT void JNICALL Java_com_rtsengine_platform_RtsActivity_nativePluginReply(JNIEnv* env, jclass, jlong id, jboolean ok, jstring data) {
    rts::plugins::detail::completeCall(uint64_t(id), ok != JNI_FALSE, rts::platform::utf8(env, data));
}
JNIEXPORT void JNICALL Java_com_rtsengine_platform_RtsActivity_nativePluginEvent(JNIEnv* env, jclass, jstring plugin, jstring event, jstring data) {
    rts::plugins::emit(rts::platform::utf8(env, plugin), rts::platform::utf8(env, event), rts::platform::utf8(env, data));
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
