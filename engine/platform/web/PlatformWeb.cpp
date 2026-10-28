// Web (Emscripten): canvas + WebGL2 (GLES3 backend). Content is preloaded into the virtual FS at /content.
#include "../PlatformCommon.h"
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
EM_JS(int, rts_web_text_focused, (), {
    return Module.rtsText && Module.rtsText.el && document.activeElement === Module.rtsText.el ? 1 : 0;
});

namespace rts::platform {
using detail::state;

namespace {
std::unique_ptr<gfx::Device> g_device;
EMSCRIPTEN_WEBGL_CONTEXT_HANDLE g_ctx = 0;
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
void showSoftKeyboard(bool show, const TextInputArea& a) {
    rts_web_text_input(show ? 1 : 0, a.x, a.y, a.w, a.h, state().touch ? 1 : 0);
}
} // namespace detail

const char* name() { return "web"; }
void setTitle(const std::string& t) { emscripten_set_window_title(t.c_str()); }

int run(const AppConfig& cfg, AppCallbacks cb) {
    auto& S = state();
    S.cfg = cfg; S.cb = std::move(cb);
    S.contentRoot = "/content";
    S.userDir = "/user";
    mkdir(S.userDir.c_str(), 0777); // in-memory MEMFS: writes work, but don't survive a reload yet
    updateSize();

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

    g_device = gfx::createGLDevice(gfx::Backend::GLES3);
    if (S.cb.init) S.cb.init(*g_device);
    emscripten_set_main_loop_arg(frame, nullptr, 0, EM_TRUE); // does not return
    return 0;
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
}

int main(int argc, char** argv) { return rtsMain(argc, argv); }
