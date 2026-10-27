#include "Json.h"
#include <fstream>
#include <sstream>

namespace rts {

namespace { const Json kNullJson{}; }

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    Json parseRoot() {
        Json v = value();
        ws();
        if (p_ != s_.size()) fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& msg) {
        size_t line = 1, col = 1;
        for (size_t i = 0; i < p_ && i < s_.size(); ++i) { if (s_[i] == '\n') { ++line; col = 1; } else ++col; }
        throw JsonError("JSON error at " + std::to_string(line) + ":" + std::to_string(col) + ": " + msg);
    }
    void ws() {
        for (;;) {
            while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) ++p_;
            // Extension: // line comments are allowed in data files.
            if (p_ + 1 < s_.size() && s_[p_] == '/' && s_[p_ + 1] == '/') {
                while (p_ < s_.size() && s_[p_] != '\n') ++p_;
                continue;
            }
            break;
        }
    }
    bool match(const char* lit) {
        size_t n = std::char_traits<char>::length(lit);
        if (s_.compare(p_, n, lit) == 0) { p_ += n; return true; }
        return false;
    }
    Json value() {
        ws();
        if (p_ >= s_.size()) fail("unexpected end");
        char c = s_[p_];
        Json j;
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') { j.type_ = Json::Type::String; j.text_ = string(); return j; }
        if (match("true")) { j.type_ = Json::Type::Bool; j.bool_ = true; return j; }
        if (match("false")) { j.type_ = Json::Type::Bool; j.bool_ = false; return j; }
        if (match("null")) return j;
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = p_;
            ++p_;
            while (p_ < s_.size() && ((s_[p_] >= '0' && s_[p_] <= '9') || s_[p_] == '.' || s_[p_] == 'e' || s_[p_] == 'E' || s_[p_] == '+' || s_[p_] == '-')) ++p_;
            j.type_ = Json::Type::Number;
            j.text_ = s_.substr(start, p_ - start);
            return j;
        }
        fail(std::string("unexpected character '") + c + "'");
    }
    std::string string() {
        ++p_; // opening quote
        std::string out;
        while (p_ < s_.size() && s_[p_] != '"') {
            char c = s_[p_++];
            if (c == '\\') {
                if (p_ >= s_.size()) fail("bad escape");
                char e = s_[p_++];
                switch (e) {
                    case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                    case 'u': {
                        if (p_ + 4 > s_.size()) fail("bad \\u escape");
                        unsigned cp = std::stoul(s_.substr(p_, 4), nullptr, 16); p_ += 4;
                        if (cp < 0x80) out += char(cp);
                        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                        else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: out += e;
                }
            } else out += c;
        }
        if (p_ >= s_.size()) fail("unterminated string");
        ++p_;
        return out;
    }
    Json array() {
        ++p_;
        Json j; j.type_ = Json::Type::Array;
        ws();
        if (p_ < s_.size() && s_[p_] == ']') { ++p_; return j; }
        for (;;) {
            j.array_.push_back(value());
            ws();
            if (p_ < s_.size() && s_[p_] == ',') { ++p_; ws(); if (p_ < s_.size() && s_[p_] == ']') { ++p_; return j; } continue; }
            if (p_ < s_.size() && s_[p_] == ']') { ++p_; return j; }
            fail("expected ',' or ']'");
        }
    }
    Json object() {
        ++p_;
        Json j; j.type_ = Json::Type::Object;
        ws();
        if (p_ < s_.size() && s_[p_] == '}') { ++p_; return j; }
        for (;;) {
            ws();
            if (p_ >= s_.size() || s_[p_] != '"') fail("expected key");
            std::string key = string();
            ws();
            if (p_ >= s_.size() || s_[p_] != ':') fail("expected ':'");
            ++p_;
            j.object_.emplace_back(std::move(key), value());
            ws();
            if (p_ < s_.size() && s_[p_] == ',') { ++p_; ws(); if (p_ < s_.size() && s_[p_] == '}') { ++p_; return j; } continue; }
            if (p_ < s_.size() && s_[p_] == '}') { ++p_; return j; }
            fail("expected ',' or '}'");
        }
    }

    const std::string& s_;
    size_t p_ = 0;
};

Json Json::parse(const std::string& text) { return JsonParser(text).parseRoot(); }

Json Json::parseFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw JsonError("cannot open " + path);
    std::stringstream ss; ss << f.rdbuf();
    try { return parse(ss.str()); }
    catch (const JsonError& e) { throw JsonError(path + ": " + e.what()); }
}

bool Json::has(const std::string& key) const {
    for (auto& [k, v] : object_) if (k == key) return true;
    return false;
}
const Json& Json::operator[](const std::string& key) const {
    for (auto& [k, v] : object_) if (k == key) return v;
    return kNullJson;
}
const Json& Json::operator[](size_t i) const { return i < array_.size() ? array_[i] : kNullJson; }

Fixed Json::fx(Fixed def) const {
    if (type_ != Type::Number) return def;
    bool ok = false;
    Fixed f = Fixed::parse(text_, &ok);
    if (!ok) throw JsonError("number '" + text_ + "' is not a plain decimal (exponents are not allowed in game data)");
    return f;
}
int64_t Json::integer(int64_t def) const {
    if (type_ != Type::Number) return def;
    return fx().toInt();
}

} // namespace rts
