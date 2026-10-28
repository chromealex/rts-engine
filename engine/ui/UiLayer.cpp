#include "UiLayer.h"

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>

#include "UiInternal.h"
#include "UiKeyboard.h"

namespace rts::ui {

namespace {

class UiSystem final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        static const char* names[] = {"always", "error", "assert", "warning", "info", "debug"};
        if (type <= Rml::Log::LT_INFO) std::fprintf(stderr, "[ui %s] %s\n", type < 6 ? names[type] : "?", message.c_str());
        return true;
    }
    void JoinPath(Rml::String& out, const Rml::String& documentPath, const Rml::String& path) override {
        if (!path.empty() && path[0] == '#') { out = path; return; } // dynamic texture name
        Rml::SystemInterface::JoinPath(out, documentPath, path);
    }
    int TranslateString(Rml::String& out, const Rml::String& input) override {
        // only whole "@key" strings (surrounding whitespace allowed): plain text stays untouched
        const size_t b = input.find_first_not_of(" \t\r\n"), e = input.find_last_not_of(" \t\r\n");
        if (!translate || b == Rml::String::npos || input[b] != '@' || e == b) { out = input; return 0; }
        for (size_t i = b + 1; i <= e; ++i) {
            const char c = input[i];
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) { out = input; return 0; }
        }
        out = translate(input.substr(b + 1, e - b));
        return 1;
    }
    void SetClipboardText(const Rml::String& text) override { platform::setClipboardText(text); }

    void ActivateKeyboard(Rml::Vector2f caret, float) override { if (keyboard) keyboard->activate(caret.y); }
    void DeactivateKeyboard() override { if (keyboard) keyboard->deactivate(); }

    std::function<std::string(const std::string&)> translate;
    UiKeyboard* keyboard = nullptr;
    void GetClipboardText(Rml::String& text) override { text = platform::clipboardText(); }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// Content files through the platform layer; "engine/ui/x" prefers the game's "ui/x" and vice versa
// falls back, so overrides work for documents and everything they link to.
class UiFiles final : public Rml::FileInterface {
public:
    Rml::FileHandle Open(const Rml::String& path) override {
        auto f = std::make_unique<File>();
        if (!read(path, f->data)) return 0;
        return reinterpret_cast<Rml::FileHandle>(f.release());
    }
    void Close(Rml::FileHandle h) override { delete reinterpret_cast<File*>(h); }
    size_t Read(void* buffer, size_t size, Rml::FileHandle h) override {
        File& f = *reinterpret_cast<File*>(h);
        size = std::min(size, f.data.size() - f.pos);
        std::memcpy(buffer, f.data.data() + f.pos, size);
        f.pos += size;
        return size;
    }
    bool Seek(Rml::FileHandle h, long offset, int origin) override {
        File& f = *reinterpret_cast<File*>(h);
        long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? long(f.pos) : long(f.data.size());
        long p = base + offset;
        if (p < 0 || size_t(p) > f.data.size()) return false;
        f.pos = size_t(p);
        return true;
    }
    size_t Tell(Rml::FileHandle h) override { return reinterpret_cast<File*>(h)->pos; }
    size_t Length(Rml::FileHandle h) override { return reinterpret_cast<File*>(h)->data.size(); }
    bool LoadFile(const Rml::String& path, Rml::String& out) override { return read(path, out); }

private:
    struct File { std::string data; size_t pos = 0; };
    static bool read(const std::string& path, std::string& out) {
        static const std::string kEngine = "engine/ui/", kGame = "ui/";
        if (path.rfind(kEngine, 0) == 0 && platform::readContentFile(kGame + path.substr(kEngine.size()), out)) return true;
        if (platform::readContentFile(path, out)) return true;
        if (path.rfind(kGame, 0) == 0) return platform::readContentFile(kEngine + path.substr(kGame.size()), out);
        return false;
    }
};

Rml::Input::KeyIdentifier toRmlKey(platform::Key k) {
    using platform::Key;
    using namespace Rml::Input;
    if (k >= Key::A && k <= Key::Z) return KeyIdentifier(KI_A + (int(k) - int(Key::A)));
    if (k >= Key::Num0 && k <= Key::Num9) return KeyIdentifier(KI_0 + (int(k) - int(Key::Num0)));
    if (k >= Key::F1 && k <= Key::F12) return KeyIdentifier(KI_F1 + (int(k) - int(Key::F1)));
    switch (k) {
    case Key::Escape: return KI_ESCAPE; case Key::Enter: return KI_RETURN; case Key::Tab: return KI_TAB;
    case Key::Backspace: return KI_BACK; case Key::Delete: return KI_DELETE; case Key::Insert: return KI_INSERT;
    case Key::Space: return KI_SPACE; case Key::Left: return KI_LEFT; case Key::Right: return KI_RIGHT;
    case Key::Up: return KI_UP; case Key::Down: return KI_DOWN; case Key::Home: return KI_HOME; case Key::End: return KI_END;
    case Key::PageUp: return KI_PRIOR; case Key::PageDown: return KI_NEXT;
    case Key::LeftShift: return KI_LSHIFT; case Key::RightShift: return KI_RSHIFT;
    case Key::LeftCtrl: return KI_LCONTROL; case Key::RightCtrl: return KI_RCONTROL;
    case Key::LeftAlt: return KI_LMENU; case Key::RightAlt: return KI_RMENU;
    case Key::LeftSuper: return KI_LMETA; case Key::RightSuper: return KI_RMETA;
    case Key::Minus: return KI_OEM_MINUS; case Key::Equal: return KI_OEM_PLUS; case Key::Comma: return KI_OEM_COMMA;
    case Key::Period: return KI_OEM_PERIOD; case Key::Slash: return KI_OEM_2; case Key::Semicolon: return KI_OEM_1;
    case Key::LeftBracket: return KI_OEM_4; case Key::RightBracket: return KI_OEM_6; case Key::Apostrophe: return KI_OEM_7;
    case Key::Backslash: return KI_OEM_5; case Key::GraveAccent: return KI_OEM_3;
    default: return KI_UNKNOWN;
    }
}

int toRmlMods(uint8_t m) {
    using namespace Rml::Input;
    return (m & platform::kModCtrl ? KM_CTRL : 0) | (m & platform::kModShift ? KM_SHIFT : 0) |
           (m & platform::kModAlt ? KM_ALT : 0) | (m & platform::kModSuper ? KM_META : 0);
}

} // namespace

struct UiLayer::Impl {
    UiSystem system;
    UiFiles files;
    UiRenderer renderer;
    UiFontEngine fonts;
    UiKeyboard keyboard;
    Rml::Context* context = nullptr;
    gfx::Device* dev = nullptr;
    int64_t primaryTouch = -1;
    bool debugger = false;
};

UiLayer::UiLayer() = default;
UiLayer::~UiLayer() { shutdown(); }

bool UiLayer::init(gfx::Device& dev, const char* gameFont) {
    impl_ = std::make_unique<Impl>();
    Impl& m = *impl_;
    m.dev = &dev;
    if (!m.renderer.init(dev)) { impl_.reset(); return false; }
    Rml::SetSystemInterface(&m.system);
    Rml::SetFileInterface(&m.files);
    Rml::SetRenderInterface(&m.renderer);
    Rml::SetFontEngineInterface(&m.fonts);
    if (!Rml::Initialise()) { impl_.reset(); return false; }
    // Every family resolves to the first loaded font when unknown, so the game font goes first.
    std::string ttf;
    if (gameFont && platform::readContentFile(gameFont, ttf) && !ttf.empty())
        m.fonts.addFont(std::move(ttf), "ui", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Auto);
    if (platform::readContentFile("engine/fonts/ui.ttf", ttf) && !ttf.empty())
        m.fonts.addFont(std::move(ttf), "ui", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Auto);

    m.context = Rml::CreateContext("main", {platform::framebufferWidth(), platform::framebufferHeight()});
    if (!m.context) { shutdown(); return false; }
    m.context->SetDensityIndependentPixelRatio(platform::dpiScale());
    Rml::Debugger::Initialise(m.context);
    m.system.keyboard = &m.keyboard;
    setOnScreenKeyboard(OnScreenKeyboard::Auto);
    m.keyboard.init(*m.context); // its document is created last: drawn above the game's documents
    return true;
}

void UiLayer::shutdown() {
    if (!impl_) return;
    if (impl_->context) {
        impl_->system.keyboard = nullptr;
        impl_->keyboard.shutdown();
        Rml::Debugger::Shutdown();
        Rml::Shutdown(); // releases contexts, documents and all render resources
        impl_->context = nullptr;
    }
    impl_->renderer.shutdown();
    impl_.reset();
}

Rml::Context* UiLayer::context() const { return impl_ ? impl_->context : nullptr; }

Rml::ElementDocument* UiLayer::loadDocument(const std::string& name) {
    if (!context()) return nullptr;
    Rml::ElementDocument* doc = impl_->context->LoadDocument("engine/ui/" + name);
    if (doc) doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    return doc;
}

void UiLayer::setDynamicTexture(const std::string& name, gfx::TextureHandle tex, int w, int h) {
    if (impl_) impl_->renderer.setDynamicTexture(name, tex, w, h);
}

void UiLayer::setTranslator(std::function<std::string(const std::string&)> fn) {
    if (impl_) impl_->system.translate = std::move(fn);
}

void UiLayer::setOnScreenKeyboard(OnScreenKeyboard mode) {
    if (!impl_) return;
    const char* env = std::getenv("RTS_OSK");
    const bool on = mode == OnScreenKeyboard::Always ||
                    (mode == OnScreenKeyboard::Auto && (platform::isTouchDevice() || (env && env[0] == '1')));
    impl_->keyboard.setEnabled(on);
}

void UiLayer::setKeyboardLanguage(const std::string& tag) { if (impl_) impl_->keyboard.setLanguage(tag); }
bool UiLayer::keyboardVisible() const { return impl_ && impl_->keyboard.visible(); }

float UiLayer::dpRatio() const { return context() ? impl_->context->GetDensityIndependentPixelRatio() : 1.0f; }

bool UiLayer::textInputFocused() const {
    if (!context()) return false;
    Rml::Element* f = impl_->context->GetFocusElement();
    return f && (f->GetTagName() == "input" || f->GetTagName() == "textarea" || f->GetTagName() == "select");
}

void UiLayer::toggleDebugger() {
    if (!impl_) return;
    impl_->debugger = !impl_->debugger;
    Rml::Debugger::SetVisible(impl_->debugger);
}

bool UiLayer::handleEvent(const platform::Event& e) {
    using platform::EventType;
    if (!context()) return false;
    Impl& m = *impl_;
    Rml::Context& c = *m.context;
    const float s = platform::dpiScale();
    const int x = int(e.x * s), y = int(e.y * s), mods = toRmlMods(e.mods);
    switch (e.type) {
    case EventType::MouseMove: return !c.ProcessMouseMove(x, y, mods);
    case EventType::MouseDown:
        c.ProcessMouseMove(x, y, mods);
        return !c.ProcessMouseButtonDown(e.button, mods);
    case EventType::MouseUp:
        c.ProcessMouseMove(x, y, mods);
        return !c.ProcessMouseButtonUp(e.button, mods);
    case EventType::MouseWheel: return !c.ProcessMouseWheel(Rml::Vector2f(-e.wheelX, -e.wheelY), mods);
    case EventType::TouchBegin:
        if (m.primaryTouch != -1) return false;
        c.ProcessMouseMove(x, y, 0);
        if (c.ProcessMouseButtonDown(0, 0)) { c.ProcessMouseLeave(); return false; } // not on the UI: the game owns it
        m.primaryTouch = e.touchId;
        return true;
    case EventType::TouchMove:
        if (e.touchId != m.primaryTouch) return false;
        c.ProcessMouseMove(x, y, 0);
        return true;
    case EventType::TouchEnd: case EventType::TouchCancel:
        if (e.touchId != m.primaryTouch) return false;
        m.primaryTouch = -1;
        c.ProcessMouseMove(x, y, 0);
        c.ProcessMouseButtonUp(0, 0);
        c.ProcessMouseLeave(); // no hover state on touch screens
        return true;
    case EventType::KeyDown: {
        if (e.key == platform::Key::Unknown) return false;
        bool notConsumed = c.ProcessKeyDown(toRmlKey(e.key), mods);
        return !notConsumed || textInputFocused();
    }
    case EventType::KeyUp:
        if (e.key == platform::Key::Unknown) return false;
        return !c.ProcessKeyUp(toRmlKey(e.key), mods) || textInputFocused();
    case EventType::Char:
        if (e.codepoint < 32 || e.codepoint == 127) return textInputFocused();
        return !c.ProcessTextInput(Rml::Character(e.codepoint)) || textInputFocused();
    case EventType::FocusLost: c.ProcessMouseLeave(); return false;
    default: return false;
    }
}

void UiLayer::update() {
    if (!context()) return;
    Rml::Context& c = *impl_->context;
    const Rml::Vector2i dims{platform::framebufferWidth(), platform::framebufferHeight()};
    if (c.GetDimensions() != dims) c.SetDimensions(dims);
    if (c.GetDensityIndependentPixelRatio() != platform::dpiScale()) c.SetDensityIndependentPixelRatio(platform::dpiScale());
    impl_->keyboard.update();
    c.Update();
}

void UiLayer::render() {
    if (!context()) return;
    impl_->renderer.beginFrame(impl_->dev->framebufferWidth(), impl_->dev->framebufferHeight());
    impl_->context->Render();
    impl_->dev->setScissor({});
}

} // namespace rts::ui
