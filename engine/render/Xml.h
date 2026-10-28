// Minimal XML DOM reader for asset formats (Collada). Elements, attributes, character data,
// comments, CDATA, processing instructions / DOCTYPE (skipped), the five named entities and
// numeric character references. Namespace prefixes are stripped from element names
// ("collada:node" -> "node"). Throws std::runtime_error("<what> at line N") on malformed input;
// nesting depth and entity expansion are bounded, so hostile files cannot blow the stack.
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rts::render::xml {

struct Element {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string text;                 // character data directly inside this element (concatenated)
    std::vector<Element> children;

    const std::string* findAttr(std::string_view n) const {
        for (auto& a : attrs) if (a.first == n) return &a.second;
        return nullptr;
    }
    std::string attr(std::string_view n, const std::string& def = std::string()) const {
        const std::string* a = findAttr(n);
        return a ? *a : def;
    }
    const Element* child(std::string_view n) const {
        for (auto& c : children) if (c.name == n) return &c;
        return nullptr;
    }
    // first descendant (depth-first) with that name
    const Element* find(std::string_view n) const {
        for (auto& c : children) {
            if (c.name == n) return &c;
            if (const Element* d = c.find(n)) return d;
        }
        return nullptr;
    }
    template <class F> void each(std::string_view n, F&& f) const {
        for (auto& c : children) if (c.name == n) f(c);
    }
};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Element parse() {
        Element root;
        bool haveRoot = false;
        while (true) {
            skipMisc();
            if (p_ >= s_.size()) break;
            if (s_[p_] != '<') fail("text outside the root element");
            if (haveRoot) fail("more than one root element");
            element(root, 0);
            haveRoot = true;
        }
        if (!haveRoot) fail("no root element");
        return root;
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        int line = 1;
        for (size_t i = 0; i < p_ && i < s_.size(); ++i) line += s_[i] == '\n';
        throw std::runtime_error("XML: " + what + " at line " + std::to_string(line));
    }
    bool startsWith(std::string_view t) const { return s_.compare(p_, t.size(), t) == 0; }
    void skipTo(std::string_view end) {
        size_t e = s_.find(end, p_);
        if (e == std::string_view::npos) fail("unterminated '" + std::string(end) + "'");
        p_ = e + end.size();
    }
    static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    void skipSpace() { while (p_ < s_.size() && space(s_[p_])) ++p_; }
    // whitespace, comments, <?...?>, <!DOCTYPE ...> between elements
    void skipMisc() {
        while (true) {
            skipSpace();
            if (startsWith("<!--")) { p_ += 4; skipTo("-->"); }
            else if (startsWith("<?")) { p_ += 2; skipTo("?>"); }
            else if (startsWith("<!DOCTYPE") || startsWith("<!doctype")) skipDoctype();
            else if (p_ == 0 && startsWith("\xEF\xBB\xBF")) p_ += 3;   // UTF-8 BOM
            else return;
        }
    }
    void skipDoctype() {
        int depth = 0;
        for (; p_ < s_.size(); ++p_) {
            if (s_[p_] == '[') ++depth;
            else if (s_[p_] == ']') --depth;
            else if (s_[p_] == '>' && depth <= 0) { ++p_; return; }
        }
        fail("unterminated DOCTYPE");
    }
    std::string name() {
        size_t b = p_;
        while (p_ < s_.size() && !space(s_[p_]) && s_[p_] != '>' && s_[p_] != '/' && s_[p_] != '=' && s_[p_] != '<') ++p_;
        if (p_ == b) fail("expected a name");
        return std::string(s_.substr(b, p_ - b));
    }
    static std::string local(std::string n) {
        size_t c = n.find(':');
        return c == std::string::npos ? n : n.substr(c + 1);
    }
    void decode(std::string_view raw, std::string& out) {
        out.reserve(out.size() + raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '&') { out += raw[i]; continue; }
            size_t e = raw.find(';', i);
            if (e == std::string_view::npos || e - i > 12) { out += '&'; continue; }   // lenient: stray '&'
            std::string_view ent = raw.substr(i + 1, e - i - 1);
            if (ent == "lt") out += '<';
            else if (ent == "gt") out += '>';
            else if (ent == "amp") out += '&';
            else if (ent == "quot") out += '"';
            else if (ent == "apos") out += '\'';
            else if (!ent.empty() && ent[0] == '#') {
                uint32_t cp = 0;
                const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
                for (size_t k = hex ? 2 : 1; k < ent.size(); ++k) {
                    const char c = ent[k];
                    const int d = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                    if (d < 0) { cp = 0xFFFD; break; }
                    cp = cp * (hex ? 16 : 10) + uint32_t(d);
                    if (cp > 0x10FFFF) { cp = 0xFFFD; break; }
                }
                utf8(cp, out);
            } else { out += '&'; continue; }
            i = e;
        }
    }
    static void utf8(uint32_t cp, std::string& out) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
    }

    void element(Element& el, int depth) {
        if (depth > 256) fail("elements nested too deep");
        ++p_;   // '<'
        const std::string raw = name();
        el.name = local(raw);
        // attributes
        while (true) {
            skipSpace();
            if (p_ >= s_.size()) fail("unterminated tag <" + raw + ">");
            if (s_[p_] == '/') {
                if (p_ + 1 >= s_.size() || s_[p_ + 1] != '>') fail("expected '/>'");
                p_ += 2;
                return;
            }
            if (s_[p_] == '>') { ++p_; break; }
            std::string an = name();
            skipSpace();
            if (p_ >= s_.size() || s_[p_] != '=') fail("attribute '" + an + "' without a value");
            ++p_;
            skipSpace();
            if (p_ >= s_.size() || (s_[p_] != '"' && s_[p_] != '\'')) fail("attribute '" + an + "' value is not quoted");
            const char q = s_[p_++];
            size_t e = s_.find(q, p_);
            if (e == std::string_view::npos) fail("unterminated attribute value");
            std::string v;
            decode(s_.substr(p_, e - p_), v);
            p_ = e + 1;
            el.attrs.emplace_back(local(an), std::move(v));
        }
        // content
        while (true) {
            if (p_ >= s_.size()) fail("element <" + raw + "> is not closed");
            if (s_[p_] != '<') {
                size_t e = s_.find('<', p_);
                if (e == std::string_view::npos) e = s_.size();
                decode(s_.substr(p_, e - p_), el.text);
                p_ = e;
                continue;
            }
            if (startsWith("</")) {
                p_ += 2;
                const std::string close = name();
                skipSpace();
                if (p_ >= s_.size() || s_[p_] != '>') fail("bad closing tag </" + close + ">");
                ++p_;
                if (close != raw) fail("closing tag </" + close + "> does not match <" + raw + ">");
                return;
            }
            if (startsWith("<!--")) { p_ += 4; skipTo("-->"); continue; }
            if (startsWith("<![CDATA[")) {
                p_ += 9;
                size_t e = s_.find("]]>", p_);
                if (e == std::string_view::npos) fail("unterminated CDATA");
                el.text.append(s_.substr(p_, e - p_));
                p_ = e + 3;
                continue;
            }
            if (startsWith("<?")) { p_ += 2; skipTo("?>"); continue; }
            el.children.emplace_back();
            element(el.children.back(), depth + 1);
        }
    }

    std::string_view s_;
    size_t p_ = 0;
};

inline Element parse(std::string_view s) { return Parser(s).parse(); }

} // namespace rts::render::xml
