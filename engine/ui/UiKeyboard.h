// On-screen keyboard drawn with RmlUi, for devices without a physical keyboard (and no OS keyboard
// API in Platform.h): one implementation for iOS, Android, web and touch PCs, styled like the rest
// of the game. Layouts: engine/assets/ui/keyboard.json, looks: keyboard.rml / keyboard.rcss — a game
// overrides any of them in its ui/ folder.
//
// RmlUi tells the system interface when a text field gains / loses focus (Activate/DeactivateKeyboard);
// the keyboard document never takes focus itself, so keys type into the field that has the caret.
#pragma once
#include <RmlUi/Core/DataModelHandle.h>

#include <string>
#include <vector>

namespace Rml { class Context; class ElementDocument; }

namespace rts::ui {

class UiKeyboard {
public:
    bool init(Rml::Context& ctx);
    void shutdown();
    void setEnabled(bool on) { enabled_ = on; if (!on) wantVisible_ = false; }
    bool enabled() const { return enabled_; }
    void setLanguage(const std::string& tag);   // picks the matching layout when the keyboard opens

    // From RmlUi's system interface (may run inside event dispatch: only records the request).
    void activate(float caretY) { wantVisible_ = enabled_; caretY_ = caretY; }
    void deactivate() { wantVisible_ = false; }
    void update();                              // applies requests, once per frame before Context::Update
    bool visible() const { return visible_; }

    struct Key {
        std::string label, cls;
        int id = -1;
        bool operator==(const Key&) const = default;
    };

private:
    struct Layout { std::string id, name; std::vector<std::vector<std::string>> rows, shiftRows; };
    void loadLayouts();
    void rebuild();
    void press(int id);
    void typeText(const std::string& s);
    void sendKey(int rmlKey);

    Rml::Context* ctx_ = nullptr;
    Rml::ElementDocument* doc_ = nullptr;
    Rml::DataModelHandle model_;
    std::vector<Layout> layouts_;
    std::vector<std::string> order_;
    std::string symbols_;
    std::vector<std::pair<std::string, std::string>> labels_;
    int layout_ = 0, returnLayout_ = 0;
    bool shift_ = false, caps_ = false;
    double lastShift_ = -1;
    std::vector<std::string> outputs_;   // per key id: text, or "{special}"
    // bound
    bool visible_ = false, top_ = false;
    std::vector<Key> keys_;
    // requests
    bool enabled_ = false, wantVisible_ = false;
    float caretY_ = 0;
};

} // namespace rts::ui
