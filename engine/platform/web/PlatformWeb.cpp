// Web (Emscripten): canvas + WebGL2 (GLES3 backend). Content is preloaded into the virtual FS at /content.
#include "../PlatformCommon.h"
#include "../Plugins.h"
#include "../../gfx/Gfx.h"

#include <emscripten.h>
#include <emscripten/html5.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sys/stat.h>

// ------------------------------------------------------------------ on-screen keyboard
// Mobile browsers open their keyboard only for a focused editable element, so a transparent
// <input> is placed over the UI field and focused. Its `input` events carry the final text after
// IME composition (Cyrillic, CJK, autocorrect, swipe typing). The input always holds one sentinel
// space: if it disappears the user pressed Backspace, anything after it is typed text.
// Desktop browsers keep using the canvas key events (no hidden input, no double characters).
EM_JS(void, rts_web_text_input, (int show, float x, float y, float w, float h, int touch), {
    var R = Module.rtsText || (Module.rtsText = {});
    if (!R.el) {
        var el = document.createElement('input');
        el.type = 'text';
        el.setAttribute('autocomplete', 'off'); el.setAttribute('autocorrect', 'off');
        el.setAttribute('autocapitalize', 'off'); el.setAttribute('spellcheck', 'false');
        el.setAttribute('enterkeyhint', 'done'); el.setAttribute('aria-hidden', 'true');
        // 16px: iOS Safari zooms the page into inputs with a smaller font
        el.style.cssText = 'position:fixed;opacity:0;font-size:16px;border:0;padding:0;margin:0;' +
                           'background:transparent;color:transparent;caret-color:transparent;outline:none;z-index:-1';
        var sentinel = function () { el.value = ' '; el.setSelectionRange(1, 1); };
        var flush = function () {
            var v = el.value;
            if (v.length === 0) _rts_web_key(8);
            else for (var t of v.substring(1)) _rts_web_char(t.codePointAt(0));
            sentinel();
        };
        el.addEventListener('input', function (e) { if (!e.isComposing) flush(); });
        el.addEventListener('compositionend', flush);
        el.addEventListener('blur', function () { if (R.active) { R.active = false; _rts_web_ended(); } });
        var vv = window.visualViewport;
        var report = function () {
            var covered = vv ? Math.max(0, window.innerHeight - vv.height - vv.offsetTop) : 0;
            _rts_web_keyboard(R.active ? covered : 0);
        };
        if (vv) { vv.addEventListener('resize', report); vv.addEventListener('scroll', report); }
        document.body.appendChild(el);
        R.el = el; R.sentinel = sentinel; R.report = report;
    }
    var coarse = window.matchMedia && window.matchMedia('(pointer: coarse)').matches;
    if (show && (touch || coarse)) {
        var r = Module.canvas.getBoundingClientRect();
        var s = R.el.style;
        s.left = (r.left + x) + 'px'; s.top = (r.top + y) + 'px';
        s.width = Math.max(1, w) + 'px'; s.height = Math.max(1, h) + 'px';
        R.active = true;
        if (document.activeElement !== R.el) { R.el.focus({preventScroll: true}); R.sentinel(); }
    } else if (!show && R.active) {
        R.active = false;
        R.el.blur();
        Module.canvas.focus();
        R.report();
    }
});
// Safe area from CSS env(safe-area-inset-*) (needs viewport-fit=cover in the shell's meta tag).
EM_JS(void, rts_web_safe_area, (float* out), {
    var p = Module.rtsSafeProbe;
    if (!p) {
        p = Module.rtsSafeProbe = document.createElement('div');
        p.style.cssText = 'position:fixed;left:0;top:0;width:0;height:0;visibility:hidden;pointer-events:none;' +
            'padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)';
        document.body.appendChild(p);
    }
    var s = getComputedStyle(p);
    HEAPF32[(out >> 2) + 0] = parseFloat(s.paddingLeft) || 0;
    HEAPF32[(out >> 2) + 1] = parseFloat(s.paddingTop) || 0;
    HEAPF32[(out >> 2) + 2] = parseFloat(s.paddingRight) || 0;
    HEAPF32[(out >> 2) + 3] = parseFloat(s.paddingBottom) || 0;
});

// Clipboard. Writing works from a key/click handler; reading silently is forbidden, so Ctrl/Cmd+V
// is held back until the browser's `paste` event hands over the text, then delivered to the game.
EM_JS(void, rts_web_clipboard_write, (const char* text), {
    var s = UTF8ToString(text);
    Module.rtsClip = s;
    if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(s).catch(function () {});
});
EM_JS(void, rts_web_clipboard_init, (), {
    document.addEventListener('paste', function (e) {
        if (Module.rtsText && document.activeElement === Module.rtsText.el) return; // hidden input types it
        var t = (e.clipboardData || window.clipboardData).getData('text') || "";
        e.preventDefault();
        _rts_web_clip_begin();
        for (var ch of t) _rts_web_clip_char(ch.codePointAt(0));
        _rts_web_clip_end();
    });
});

// Fullscreen with orientation lock; `fullscreenchange` keeps isFullscreen() honest (Esc, swipe).
EM_JS(void, rts_web_fullscreen, (int on, const char* orientation), {
    var o = UTF8ToString(orientation);
    var el = document.documentElement;
    if (!Module.rtsFsInit) {
        Module.rtsFsInit = true;
        var onChange = function () { _rts_web_fullscreen_changed((document.fullscreenElement || document.webkitFullscreenElement) ? 1 : 0); };
        document.addEventListener('fullscreenchange', onChange);
        document.addEventListener('webkitfullscreenchange', onChange);
    }
    if (on) {
        var req = el.requestFullscreen ? el.requestFullscreen({navigationUI: 'hide'}) : el.webkitRequestFullscreen ? el.webkitRequestFullscreen() : null;
        Promise.resolve(req).then(function () {
            if (o !== 'any' && screen.orientation && screen.orientation.lock) screen.orientation.lock(o).catch(function () {});
        }).catch(function (err) { console.warn('fullscreen refused: ' + err); });
    } else if (document.fullscreenElement || document.webkitFullscreenElement) {
        if (screen.orientation && screen.orientation.unlock) screen.orientation.unlock();
        (document.exitFullscreen || document.webkitExitFullscreen).call(document);
    }
});

// userDataDir() on IndexedDB: load, then start the game; flush periodically and on page hide.
EM_JS(void, rts_web_storage_init, (const char* path), {
    var dir = UTF8ToString(path);
    try { FS.mkdir(dir); } catch (e) {}
    try { FS.mount(IDBFS, {}, dir); } catch (e) { console.warn('IDBFS unavailable, saves are per-session: ' + e); }
    FS.syncfs(true, function (err) {
        if (err) console.warn('loading saved data failed: ' + err);
        Module.rtsSync = function () { FS.syncfs(false, function (e) { if (e) console.warn('saving data failed: ' + e); }); };
        setInterval(Module.rtsSync, 5000);
        document.addEventListener('visibilitychange', function () { if (document.visibilityState === 'hidden') Module.rtsSync(); });
        window.addEventListener('pagehide', Module.rtsSync);
        _rts_web_start();
    });
});
EM_JS(void, rts_web_storage_sync, (), { if (Module.rtsSync) Module.rtsSync(); });

// JavaScript plugins: window.RTSPlugins.register(name, { init(config), products(args), ... }).
// Methods may return a value or a Promise; a rejected Promise / thrown error is a failed call.
// The shell defines a small RTSPlugins stub so plugin scripts can load before the engine.
EM_JS_DEPS(rts_web_plugins, "$stringToNewUTF8");
EM_JS(void, rts_web_plugins_init, (), {
    var R = window.RTSPlugins = window.RTSPlugins || {};
    R.plugins = R.plugins || {};
    R.register = R.register || function (name, impl) { R.plugins[name] = impl; };
    var queued = R.queue || [];
    R.emit = function (plugin, event, data) {
        var s = function (v) { return stringToNewUTF8(String(v)); };
        _rts_web_plugin_event(s(plugin), s(event), s(JSON.stringify(data === undefined ? {} : data)));
    };
    R.queue = [];
    queued.forEach(function (e) { R.emit(e[0], e[1], e[2]); });
});
EM_JS(int, rts_web_plugin_available, (const char* name), {
    var R = window.RTSPlugins;
    return R && R.plugins && R.plugins[UTF8ToString(name)] ? 1 : 0;
});
EM_JS(void, rts_web_plugin_call, (const char* name, const char* method, const char* args, double id), {
    var n = UTF8ToString(name), m = UTF8ToString(method), a = UTF8ToString(args);
    var reply = function (ok, text) { _rts_web_plugin_reply(id, ok ? 1 : 0, stringToNewUTF8(text)); };
    Promise.resolve().then(function () {
        var p = window.RTSPlugins.plugins[n];
        if (!p) throw new Error("plugin '" + n + "' is not in this build");
        if (typeof p[m] !== "function") {
            if (m === "init" || m === "lifecycle") return null; // optional
            throw new Error(n + ": unknown method " + m);
        }
        return p[m](JSON.parse(a || "{}"));
    }).then(function (res) {
        reply(true, JSON.stringify(res === undefined ? null : res));
    }, function (err) {
        reply(false, err && err.message ? err.message : String(err));
    });
});

EM_JS(int, rts_web_text_focused, (), {
    return Module.rtsText && Module.rtsText.el && document.activeElement === Module.rtsText.el ? 1 : 0;
});

namespace rts::platform {
using detail::state;

namespace {
std::unique_ptr<gfx::Device> g_device;
EMSCRIPTEN_WEBGL_CONTEXT_HANDLE g_ctx = 0;
uint8_t g_pasteMods = kModCtrl;
std::string g_pasteText;
detail::FrameClock g_clock; // rAF stops in hidden tabs; the clamp keeps the first frame back sane
const char* kCanvas = "#canvas";

uint8_t modsOf(bool shift, bool ctrl, bool alt, bool meta) {
    return uint8_t((shift ? kModShift : 0) | (ctrl ? kModCtrl : 0) | (alt ? kModAlt : 0) | (meta ? kModSuper : 0));
}

void updateSize() {
    auto& S = state();
    double cssW = 0, cssH = 0;
    emscripten_get_element_css_size(kCanvas, &cssW, &cssH);
    double dpr = S.cfg.highDpi ? emscripten_get_device_pixel_ratio() : 1.0;
    S.winW = float(cssW); S.winH = float(cssH); S.scale = float(dpr);
    S.fbW = int(cssW * dpr); S.fbH = int(cssH * dpr);
    emscripten_set_canvas_element_size(kCanvas, S.fbW, S.fbH);
    float in[4] = {};
    rts_web_safe_area(in);
    detail::setSafeArea({in[0], in[1], in[2], in[3]});
}

Key mapCode(const char* code) {
    if (std::strncmp(code, "Key", 3) == 0 && code[3] && !code[4]) return detail::keyFromAscii(code[3]);
    if (std::strncmp(code, "Digit", 5) == 0 && code[5] && !code[6]) return detail::keyFromAscii(code[5]);
    if (code[0] == 'F' && code[1] >= '1' && code[1] <= '9') { int n = std::atoi(code + 1); if (n >= 1 && n <= 12) return Key(int(Key::F1) + n - 1); }
    static const struct { const char* c; Key k; } t[] = {
        {"Escape", Key::Escape}, {"Enter", Key::Enter}, {"Tab", Key::Tab}, {"Backspace", Key::Backspace}, {"Delete", Key::Delete},
        {"Insert", Key::Insert}, {"Space", Key::Space}, {"ArrowLeft", Key::Left}, {"ArrowRight", Key::Right}, {"ArrowUp", Key::Up},
        {"ArrowDown", Key::Down}, {"Home", Key::Home}, {"End", Key::End}, {"PageUp", Key::PageUp}, {"PageDown", Key::PageDown},
        {"ShiftLeft", Key::LeftShift}, {"ShiftRight", Key::RightShift}, {"ControlLeft", Key::LeftCtrl}, {"ControlRight", Key::RightCtrl},
        {"AltLeft", Key::LeftAlt}, {"AltRight", Key::RightAlt}, {"MetaLeft", Key::LeftSuper}, {"MetaRight", Key::RightSuper},
        {"Minus", Key::Minus}, {"Equal", Key::Equal}, {"Comma", Key::Comma}, {"Period", Key::Period}, {"Slash", Key::Slash},
    };
    for (auto& e : t) if (std::strcmp(e.c, code) == 0) return e.k;
    return Key::Unknown;
}

EM_BOOL onMouse(int type, const EmscriptenMouseEvent* m, void*) {
    Event e;
    e.x = float(m->targetX); e.y = float(m->targetY);
    e.button = m->button == 0 ? 0 : m->button == 2 ? 1 : 2;
    e.mods = modsOf(m->shiftKey, m->ctrlKey, m->altKey, m->metaKey);
    if (type == EMSCRIPTEN_EVENT_MOUSEDOWN) e.type = EventType::MouseDown;
    else if (type == EMSCRIPTEN_EVENT_MOUSEUP) e.type = EventType::MouseUp;
    else e.type = EventType::MouseMove;
    detail::dispatch(e);
    return EM_TRUE;
}
EM_BOOL onWheel(int, const EmscriptenWheelEvent* w, void*) {
    Event e; e.type = EventType::MouseWheel;
    e.x = float(w->mouse.targetX); e.y = float(w->mouse.targetY);
    float k = w->deltaMode == DOM_DELTA_PIXEL ? 0.01f : 1.0f;
    e.wheelX = float(-w->deltaX) * k; e.wheelY = float(-w->deltaY) * k;
    detail::dispatch(e);
    return EM_TRUE;
}
EM_BOOL onKey(int type, const EmscriptenKeyboardEvent* k, void*) {
    if (rts_web_text_focused()) {
        // The hidden input produces the text and Backspace itself; only pass navigation/Enter/Escape
        // and let the browser deliver the key to the input (no preventDefault).
        Key key = mapCode(k->code);
        bool passthrough = key == Key::Enter || key == Key::Escape || key == Key::Tab || key == Key::Left ||
                           key == Key::Right || key == Key::Up || key == Key::Down || key == Key::Home || key == Key::End;
        if (passthrough) {
            Event e; e.type = type == EMSCRIPTEN_EVENT_KEYDOWN ? EventType::KeyDown : EventType::KeyUp;
            e.key = key; e.mods = modsOf(k->shiftKey, k->ctrlKey, k->altKey, k->metaKey); e.repeat = k->repeat;
            detail::dispatch(e);
        }
        return passthrough ? EM_TRUE : EM_FALSE;
    }
    if (type == EMSCRIPTEN_EVENT_KEYDOWN && !k->repeat && std::strcmp(k->code, "F11") == 0) {
        detail::applyFullscreen(!state().fullscreen); // a key press is a user gesture: allowed
    }
    if ((k->ctrlKey || k->metaKey) && std::strcmp(k->code, "KeyV") == 0) {
        g_pasteMods = modsOf(k->shiftKey, k->ctrlKey, k->altKey, k->metaKey);
        return EM_FALSE; // the browser fires `paste` next; the game gets Ctrl+V from there
    }
    Event e;
    e.type = type == EMSCRIPTEN_EVENT_KEYDOWN ? EventType::KeyDown : EventType::KeyUp;
    e.key = mapCode(k->code);
    e.mods = modsOf(k->shiftKey, k->ctrlKey, k->altKey, k->metaKey);
    e.repeat = k->repeat;
    detail::dispatch(e);
    if (type == EMSCRIPTEN_EVENT_KEYDOWN && !k->ctrlKey && !k->metaKey) {
        // k->key holds the produced character for printable keys (UTF-8, single code point)
        const unsigned char* p = reinterpret_cast<const unsigned char*>(k->key);
        size_t len = std::strlen(k->key);
        uint32_t cp = 0;
        if (len == 1) cp = p[0];
        else if (len == 2 && p[0] >= 0xC0) cp = ((p[0] & 31u) << 6) | (p[1] & 63u);
        else if (len == 3 && p[0] >= 0xE0) cp = ((p[0] & 15u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u);
        if (cp >= 32 && cp != 127) { Event c; c.type = EventType::Char; c.codepoint = cp; detail::dispatch(c); }
    }
    // Let the browser keep shortcuts like Cmd+R, but stop Tab/Backspace/arrows from scrolling the page.
    return (k->ctrlKey || k->metaKey) ? EM_FALSE : EM_TRUE;
}
EM_BOOL onTouch(int type, const EmscriptenTouchEvent* t, void*) {
    state().touch = true;
    for (int i = 0; i < t->numTouches; ++i) {
        const auto& p = t->touches[i];
        if (!p.isChanged) continue;
        Event e;
        e.type = type == EMSCRIPTEN_EVENT_TOUCHSTART ? EventType::TouchBegin
               : type == EMSCRIPTEN_EVENT_TOUCHMOVE ? EventType::TouchMove
               : type == EMSCRIPTEN_EVENT_TOUCHEND ? EventType::TouchEnd : EventType::TouchCancel;
        e.x = float(p.targetX); e.y = float(p.targetY); e.touchId = p.identifier;
        detail::dispatch(e);
    }
    return EM_TRUE;
}
EM_BOOL onResize(int, const EmscriptenUiEvent*, void*) {
    updateSize();
    Event e; e.type = EventType::Resized; detail::dispatch(e);
    return EM_FALSE;
}

void frame(void*) {
    auto& S = state();
    double dt = g_clock.tick(emscripten_get_now() / 1000.0);
    double cssW = 0, cssH = 0;
    emscripten_get_element_css_size(kCanvas, &cssW, &cssH);
    if (int(cssW) != int(S.winW) || int(cssH) != int(S.winH)) onResize(0, nullptr, nullptr);
    g_device->beginFrame(S.fbW, S.fbH);
    if (S.cb.frame) S.cb.frame(*g_device, dt);
    g_device->endFrame();
}
} // namespace

namespace detail {
void applyFullscreen(bool on) { rts_web_fullscreen(on ? 1 : 0, state().cfg.orientation.c_str()); }
void showSoftKeyboard(bool show, const TextInputArea& a) {
    rts_web_text_input(show ? 1 : 0, a.x, a.y, a.w, a.h, state().touch ? 1 : 0);
}
} // namespace detail

const char* name() { return "web"; }
void setTitle(const std::string& t) { emscripten_set_window_title(t.c_str()); }

struct WebPluginBackend : plugins::detail::Backend {
    bool available(const std::string& n) override { return rts_web_plugin_available(n.c_str()) != 0; }
    void call(const std::string& n, const std::string& m, const std::string& a, uint64_t id) override {
        rts_web_plugin_call(n.c_str(), m.c_str(), a.c_str(), double(id)); // ids stay far below 2^53
    }
} g_pluginBackend;

int run(const AppConfig& cfg, AppCallbacks cb) {
    auto& S = state();
    S.cfg = cfg; S.cb = std::move(cb);
    S.contentRoot = "/content";
    S.userDir = "/user";
    updateSize();
    rts_web_clipboard_init();
    rts_web_plugins_init();
    plugins::detail::setBackend(&g_pluginBackend);

    EmscriptenWebGLContextAttributes a;
    emscripten_webgl_init_context_attributes(&a);
    a.majorVersion = 2; a.minorVersion = 0;
    a.depth = EM_TRUE; a.antialias = EM_TRUE; a.alpha = EM_FALSE;
    g_ctx = emscripten_webgl_create_context(kCanvas, &a);
    if (g_ctx <= 0) { emscripten_log(EM_LOG_ERROR, "WebGL2 is not available"); return 1; }
    emscripten_webgl_make_context_current(g_ctx);

    emscripten_set_mousedown_callback(kCanvas, nullptr, EM_TRUE, onMouse);
    emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, EM_TRUE, onMouse);
    emscripten_set_mousemove_callback(kCanvas, nullptr, EM_TRUE, onMouse);
    emscripten_set_wheel_callback(kCanvas, nullptr, EM_TRUE, onWheel);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, onKey);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, onKey);
    emscripten_set_touchstart_callback(kCanvas, nullptr, EM_TRUE, onTouch);
    emscripten_set_touchmove_callback(kCanvas, nullptr, EM_TRUE, onTouch);
    emscripten_set_touchend_callback(kCanvas, nullptr, EM_TRUE, onTouch);
    emscripten_set_touchcancel_callback(kCanvas, nullptr, EM_TRUE, onTouch);
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE, onResize);

    // userDataDir lives in IndexedDB (IDBFS). The game starts after the saved files are loaded, so
    // init() sees settings from the last visit; writes are flushed every few seconds and when the
    // page is hidden or closed (syncUserData() flushes right away).
    rts_web_storage_init(S.userDir.c_str());
    return 0; // main returns; the runtime stays alive (EXIT_RUNTIME=0) and rts_web_start runs the loop
}

void startAfterStorage() {
    auto& S = state();
    g_device = gfx::createGLDevice(gfx::Backend::GLES3);
    if (S.cb.init) S.cb.init(*g_device);
    emscripten_set_main_loop_arg(frame, nullptr, 0, EM_FALSE);
}

void syncUserData() { rts_web_storage_sync(); }

std::string clipboardText() { return state().clipboard; }
void setClipboardText(const std::string& s) {
    state().clipboard = s;
    rts_web_clipboard_write(s.c_str());
}

} // namespace rts::platform

// Called from the hidden input's JS handlers.
extern "C" {
EMSCRIPTEN_KEEPALIVE void rts_web_char(int cp) {
    rts::platform::Event e; e.type = rts::platform::EventType::Char; e.codepoint = uint32_t(cp);
    rts::platform::detail::dispatch(e);
}
EMSCRIPTEN_KEEPALIVE void rts_web_key(int code) {
    if (code == 8) rts::platform::detail::dispatchKeyPress(rts::platform::Key::Backspace);
}
EMSCRIPTEN_KEEPALIVE void rts_web_ended() { rts::platform::detail::textInputEndedByUser(); }
EMSCRIPTEN_KEEPALIVE void rts_web_keyboard(float h) { rts::platform::detail::setKeyboardHeight(h); }
EMSCRIPTEN_KEEPALIVE void rts_web_start() { rts::platform::startAfterStorage(); }
// Called from JS with strings allocated by stringToNewUTF8: copy, then free.
EMSCRIPTEN_KEEPALIVE void rts_web_plugin_reply(double id, int ok, char* text) {
    std::string s = text ? text : "";
    std::free(text);
    rts::plugins::detail::completeCall(uint64_t(id), ok != 0, s);
}
EMSCRIPTEN_KEEPALIVE void rts_web_plugin_event(char* plugin, char* event, char* data) {
    std::string p = plugin ? plugin : "", e = event ? event : "", d = data ? data : "{}";
    std::free(plugin); std::free(event); std::free(data);
    rts::plugins::emit(p, e, d);
}
EMSCRIPTEN_KEEPALIVE void rts_web_fullscreen_changed(int on) {
    rts::platform::detail::state().fullscreen = on != 0;
    rts::platform::Event e; e.type = rts::platform::EventType::Resized; rts::platform::detail::dispatch(e);
}
EMSCRIPTEN_KEEPALIVE void rts_web_clip_begin() { rts::platform::g_pasteText.clear(); }
EMSCRIPTEN_KEEPALIVE void rts_web_clip_char(int cp) {
    uint32_t c = uint32_t(cp);
    std::string& s = rts::platform::g_pasteText;
    if (c < 0x80) s += char(c);
    else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 63)); }
    else if (c < 0x10000) { s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 63)); s += char(0x80 | (c & 63)); }
    else { s += char(0xF0 | (c >> 18)); s += char(0x80 | ((c >> 12) & 63)); s += char(0x80 | ((c >> 6) & 63)); s += char(0x80 | (c & 63)); }
}
EMSCRIPTEN_KEEPALIVE void rts_web_clip_end() {
    using namespace rts::platform;
    detail::state().clipboard = g_pasteText;
    Event e; e.type = EventType::KeyDown; e.key = Key::V; e.mods = g_pasteMods; detail::dispatch(e);
    e.type = EventType::KeyUp; detail::dispatch(e);
}
}

int main(int argc, char** argv) { return rtsMain(argc, argv); }
