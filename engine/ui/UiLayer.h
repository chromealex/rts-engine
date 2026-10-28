// In-game UI: RmlUi documents (HTML/CSS) on top of the 3D scene, rendered through the RHI.
//
// Documents are content files. Engine defaults live in engine/assets/ui (content path "engine/ui/...");
// a game overrides any file by shipping "ui/<same name>" in its folder. Relative links inside a
// document (styles, images, templates) resolve the same way, so a game can override a single
// stylesheet and keep the engine's markup, or the other way round.
//
// Units: the context works in framebuffer pixels; stylesheets should use `dp` so the layout scales
// with the display density (dp ratio = platform::dpiScale()).
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/gfx/Gfx.h"
#include "engine/platform/Platform.h"

namespace Rml { class Context; class ElementDocument; }

namespace rts::ui {

class UiLayer {
public:
    UiLayer();
    ~UiLayer();
    UiLayer(const UiLayer&) = delete;
    UiLayer& operator=(const UiLayer&) = delete;

    // gameFont: TTF in the game's content (optional); the engine font (Latin + Cyrillic) is the fallback.
    bool init(gfx::Device& dev, const char* gameFont = "fonts/ui.ttf");
    void shutdown();

    // Returns true when the UI consumed the event (the pointer is over an interactive element, or a
    // text field has focus); the game should then ignore it.
    bool handleEvent(const platform::Event& e);
    void update();   // layout + data bindings; call once per frame before render()
    void render();   // inside an active render pass, after the scene

    Rml::Context* context() const;
    // Loads "engine/ui/<name>" (or the game's "ui/<name>" override). Returns null on error (logged).
    Rml::ElementDocument* loadDocument(const std::string& name);

    // Exposes a texture owned by the caller to documents as <img src="#name"/> or
    // `decorator: image(#name)`. The caller may update its pixels every frame.
    void setDynamicTexture(const std::string& name, gfx::TextureHandle tex, int width, int height);

    // Localized documents: text nodes and attribute values that are exactly "@some.key" are passed
    // to `fn` while a document loads. After a language change, reload the documents.
    void setTranslator(std::function<std::string(const std::string& key)> fn);
    // Fallback for images that are not plain content files (e.g. packed into a texture atlas at
    // build time): returns straight-alpha RGBA8 pixels of the original image at `path`.
    using ImageSource = std::function<bool(const std::string& path, int& w, int& h, std::vector<unsigned char>& rgba)>;
    void setImageSource(ImageSource source);

    // On-screen keyboard (UiKeyboard): opens when a text field gets focus. Auto = on touch devices
    // only (or with RTS_OSK=1 in the environment, to try it on a desktop).
    enum class OnScreenKeyboard { Auto, Always, Never };
    void setOnScreenKeyboard(OnScreenKeyboard mode);
    void setKeyboardLanguage(const std::string& tag);  // layout that opens first (UI language)
    bool keyboardVisible() const;

    float dpRatio() const;              // framebuffer pixels per dp
    bool textInputFocused() const;
    void toggleDebugger();              // RmlUi visual debugger (F8 in the client)

    struct Impl;
private:
    bool processEvent(const platform::Event& e);
    // Opens/closes the on-screen keyboard to follow the focused text field (platform::startTextInput).
    void syncTextInput();
    std::unique_ptr<Impl> impl_;
};

} // namespace rts::ui
