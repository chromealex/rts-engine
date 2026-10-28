#include "UiKeyboard.h"

#include <RmlUi/Core.h>

#include <algorithm>

#include "engine/data/Json.h"

namespace rts::ui {

namespace {

// Upper case of Latin, Latin-1 and Cyrillic letters (enough for the shipped layouts; a game with
// other scripts gives explicit "shift" rows).
std::string upper(const std::string& s) {
    std::string out;
    for (auto it = Rml::StringIteratorU8(s.data(), s.data(), s.data() + s.size()); it; ++it) {
        char32_t c = char32_t(*it);
        if ((c >= U'a' && c <= U'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x430 && c <= 0x44F)) c -= 0x20;
        else if (c >= 0x450 && c <= 0x45F) c -= 0x50;
        out += Rml::StringUtilities::ToUTF8(Rml::Character(c));
    }
    return out;
}

std::vector<std::vector<std::string>> parseRows(const Json& rows) {
    std::vector<std::vector<std::string>> out;
    for (const Json& r : rows.items()) {
        std::vector<std::string> keys;
        if (r.isArray()) {
            for (const Json& k : r.items()) keys.push_back(k.str());
        } else {
            const std::string line = r.str();
            size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() && line[i] == ' ') ++i;
                size_t j = i;
                while (j < line.size() && line[j] != ' ') ++j;
                if (j > i) keys.push_back(line.substr(i, j - i));
                i = j;
            }
        }
        out.push_back(std::move(keys));
    }
    return out;
}

bool isTextField(Rml::Element* e) {
    if (!e) return false;
    const std::string& tag = e->GetTagName();
    if (tag == "textarea") return true;
    if (tag != "input") return false;
    const std::string type = e->GetAttribute<Rml::String>("type", "text");
    return type == "text" || type == "password";
}

} // namespace

bool UiKeyboard::init(Rml::Context& ctx) {
    ctx_ = &ctx;
    loadLayouts();
    Rml::DataModelConstructor c = ctx.CreateDataModel("keyboard");
    if (!c) return false;
    if (auto h = c.RegisterStruct<Key>()) {
        h.RegisterMember("label", &Key::label);
        h.RegisterMember("cls", &Key::cls);
        h.RegisterMember("id", &Key::id);
    }
    c.RegisterArray<std::vector<Key>>();
    c.Bind("visible", &visible_);
    c.Bind("top", &top_);
    c.Bind("keys", &keys_);
    c.BindEventCallback("press", [this](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& a) {
        if (!a.empty()) press(a[0].Get<int>(-1));
        ev.StopPropagation();
    });
    model_ = c.GetModelHandle();
    rebuild();
    doc_ = ctx.LoadDocument("engine/ui/keyboard.rml");
    if (doc_) doc_->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    return doc_ != nullptr;
}

void UiKeyboard::shutdown() {
    doc_ = nullptr; // owned by the context
    ctx_ = nullptr;
}

void UiKeyboard::loadLayouts() {
    layouts_.clear();
    order_.clear();
    labels_.clear();
    std::string text;
    Json j;
    try {
        if (Rml::GetFileInterface()->LoadFile("engine/ui/keyboard.json", text)) j = Json::parse(text);
    } catch (const std::exception& e) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "keyboard.json: %s", e.what());
    }
    for (auto& [id, rows] : j["layouts"].members()) {
        Layout l;
        l.id = id;
        l.name = j["names"].getStr(id, id);
        if (rows.isObject()) {
            l.rows = parseRows(rows["keys"]);
            l.shiftRows = parseRows(rows["shift"]);
        } else {
            l.rows = parseRows(rows);
        }
        layouts_.push_back(std::move(l));
    }
    if (layouts_.empty()) { // no file: a minimal Latin layout keeps text fields usable
        Layout l;
        l.id = l.name = "en";
        Json rows = Json::array();
        for (const char* r : {"1 2 3 4 5 6 7 8 9 0", "q w e r t y u i o p", "a s d f g h j k l", "{shift} z x c v b n m {back}", "{space} {enter} {hide}"})
            rows.push(Json::string(r));
        l.rows = parseRows(rows);
        layouts_.push_back(std::move(l));
    }
    for (const Json& o : j["order"].items()) order_.push_back(o.str());
    if (order_.empty()) order_.push_back(layouts_[0].id);
    symbols_ = j.getStr("symbols", "sym");
    for (auto& [k, v] : j["labels"].members()) labels_.push_back({k, v.str()});
    layout_ = 0;
    for (size_t i = 0; i < layouts_.size(); ++i) if (layouts_[i].id == order_[0]) layout_ = int(i);
}

void UiKeyboard::setLanguage(const std::string& tag) {
    const std::string base = tag.substr(0, tag.find_first_of("-_"));
    for (size_t i = 0; i < layouts_.size(); ++i)
        if (layouts_[i].id == tag || layouts_[i].id == base) {
            if (std::find(order_.begin(), order_.end(), layouts_[i].id) == order_.end()) continue;
            layout_ = int(i);
            rebuild();
            return;
        }
}

void UiKeyboard::rebuild() {
    auto label = [&](const std::string& k) -> std::string {
        for (auto& [n, v] : labels_) if (n == k) return v;
        return k;
    };
    auto indexOf = [&](const std::string& id) {
        for (size_t i = 0; i < layouts_.size(); ++i) if (layouts_[i].id == id) return int(i);
        return -1;
    };
    const Layout& l = layouts_[size_t(std::clamp(layout_, 0, int(layouts_.size()) - 1))];
    const bool up = shift_ || caps_;
    const auto& rows = up && !l.shiftRows.empty() ? l.shiftRows : l.rows;
    std::vector<Key> keys;
    outputs_.clear();
    for (size_t r = 0; r < rows.size(); ++r) {
        if (r) keys.push_back({"", "br", -1});
        for (const std::string& k : rows[r]) {
            Key key;
            key.id = int(outputs_.size());
            if (k.size() > 2 && k.front() == '{' && k.back() == '}') {
                const std::string name = k.substr(1, k.size() - 2);
                outputs_.push_back(k);
                key.cls = name == "space" ? "key space" : (name == "shift" || name == "back" || name == "enter") ? "key fn wide" : "key fn";
                if (name == "shift" && up) key.cls += " on";
                if (name == "lang") {
                    // shows where the key leads: the next layout of the cycle
                    auto it = std::find(order_.begin(), order_.end(), l.id);
                    const std::string next = it == order_.end() || std::next(it) == order_.end() ? order_.front() : *std::next(it);
                    const int ni = indexOf(next);
                    key.label = label(name) == "{next}" && ni >= 0 ? layouts_[size_t(ni)].name : label(name);
                    if (order_.size() < 2) continue;
                } else if (name == "sym") {
                    key.label = l.id == symbols_ ? label("abc") : label("sym");
                    if (indexOf(symbols_) < 0) continue;
                } else {
                    key.label = label(name);
                }
            } else {
                const std::string out = up && l.shiftRows.empty() ? upper(k) : k;
                outputs_.push_back(out);
                key.label = out;
                key.cls = "key";
            }
            keys.push_back(std::move(key));
        }
    }
    if (!(keys == keys_)) {
        keys_ = std::move(keys);
        if (model_) model_.DirtyVariable("keys");
    }
}

void UiKeyboard::typeText(const std::string& s) { if (ctx_) ctx_->ProcessTextInput(s); }

void UiKeyboard::sendKey(int k) {
    if (!ctx_) return;
    ctx_->ProcessKeyDown(Rml::Input::KeyIdentifier(k), 0);
    ctx_->ProcessKeyUp(Rml::Input::KeyIdentifier(k), 0);
}

void UiKeyboard::press(int id) {
    if (id < 0 || size_t(id) >= outputs_.size()) return;
    const std::string out = outputs_[size_t(id)];
    using namespace Rml::Input;
    if (out == "{shift}") {
        const double now = Rml::GetSystemInterface()->GetElapsedTime();
        if (caps_) caps_ = shift_ = false;
        else if (shift_ && now - lastShift_ < 0.4) caps_ = true;   // double tap: caps lock
        else shift_ = !shift_;
        lastShift_ = now;
    } else if (out == "{back}") sendKey(KI_BACK);
    else if (out == "{enter}") sendKey(KI_RETURN);
    else if (out == "{left}") sendKey(KI_LEFT);
    else if (out == "{right}") sendKey(KI_RIGHT);
    else if (out == "{space}") typeText(" ");
    else if (out == "{hide}") { if (Rml::Element* f = ctx_->GetFocusElement()) f->Blur(); wantVisible_ = false; }
    else if (out == "{lang}") {
        auto it = std::find(order_.begin(), order_.end(), layouts_[size_t(layout_)].id);
        const std::string next = it == order_.end() || std::next(it) == order_.end() ? order_.front() : *std::next(it);
        for (size_t i = 0; i < layouts_.size(); ++i) if (layouts_[i].id == next) layout_ = int(i);
    } else if (out == "{sym}") {
        if (layouts_[size_t(layout_)].id == symbols_) layout_ = returnLayout_;
        else for (size_t i = 0; i < layouts_.size(); ++i) if (layouts_[i].id == symbols_) { returnLayout_ = layout_; layout_ = int(i); }
    } else if (!out.empty() && out.front() != '{') {
        typeText(out);
        if (shift_ && !caps_) shift_ = false; // one-shot shift
    }
    rebuild();
}

void UiKeyboard::update() {
    if (!ctx_) return;
    // a field that disappears (screen change) does not always report losing focus
    if (wantVisible_ && !isTextField(ctx_->GetFocusElement())) wantVisible_ = false;
    const bool top = caretY_ > float(ctx_->GetDimensions().y) * 0.55f; // keep the caret visible
    if (top != top_) { top_ = top; model_.DirtyVariable("top"); }
    if (wantVisible_ != visible_) {
        visible_ = wantVisible_;
        if (visible_) { shift_ = caps_ = false; rebuild(); }
        model_.DirtyVariable("visible");
        if (visible_ && doc_) doc_->PullToFront();
    }
}

} // namespace rts::ui
