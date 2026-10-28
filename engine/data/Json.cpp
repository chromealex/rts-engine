#include "Json.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

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

thread_local JsonAccessLog* Json::accessLog_ = nullptr;
void Json::noteKey(const std::string& key) const { if (type_ == Type::Object) accessLog_->keys.emplace_back(this, key); }
void Json::noteIterated() const { if (type_ == Type::Object) accessLog_->iterated.push_back(this); }

bool Json::has(const std::string& key) const {
    if (accessLog_) noteKey(key);
    for (auto& [k, v] : object_) if (k == key) return true;
    return false;
}
const Json& Json::operator[](const std::string& key) const {
    if (accessLog_) noteKey(key);
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


// ---------------------------------------------------------------- building / editing

Json Json::number(Fixed v) { return number(v.toDouble(), 5); }
Json Json::number(int64_t v) { Json j; j.type_ = Type::Number; j.text_ = std::to_string(v); return j; }
Json Json::number(double v, int decimals) {
    Json j;
    j.type_ = Type::Number;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    j.text_ = s;
    return j;
}
Json Json::numberText(std::string literal) { Json j; j.type_ = Type::Number; j.text_ = std::move(literal); return j; }

Json& Json::set(const std::string& key, Json v) {
    if (type_ != Type::Object) { *this = object(); }
    for (auto& [k, x] : object_) if (k == key) { x = std::move(v); return x; }
    object_.emplace_back(key, std::move(v));
    return object_.back().second;
}
bool Json::erase(const std::string& key) {
    for (size_t i = 0; i < object_.size(); ++i)
        if (object_[i].first == key) { object_.erase(object_.begin() + long(i)); return true; }
    return false;
}
Json& Json::push(Json v) {
    if (type_ != Type::Array) *this = array();
    array_.push_back(std::move(v));
    return array_.back();
}
void Json::eraseAt(size_t i) { if (i < array_.size()) array_.erase(array_.begin() + long(i)); }
Json* Json::find(const std::string& key) {
    for (auto& [k, v] : object_) if (k == key) return &v;
    return nullptr;
}
Json& Json::member(const std::string& key) {
    if (Json* j = find(key)) return *j;
    return set(key, object());
}
double Json::toDouble(double def) const { return type_ == Type::Number ? std::strtod(text_.c_str(), nullptr) : def; }

bool Json::operator==(const Json& o) const {
    if (type_ != o.type_) return false;
    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: return bool_ == o.bool_;
        case Type::Number: case Type::String: return text_ == o.text_;
        case Type::Array: return array_ == o.array_;
        case Type::Object: return object_ == o.object_;
    }
    return false;
}

namespace {
void escapeTo(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += char(c);
        }
    }
    out += '"';
}
bool isScalar(const Json& j) { return !j.isArray() && !j.isObject(); }
} // namespace

void Json::dumpTo(std::string& out, int indent, int depth) const {
    auto nl = [&](int d) { if (indent >= 0) { out += '\n'; out.append(size_t(d * indent), ' '); } };
    switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: out += text_; break;
    case Type::String: escapeTo(out, text_); break;
    case Type::Array: {
        if (array_.empty()) { out += "[]"; break; }
        // rows of terrain strings / short numeric tuples read best on one line each or inline
        bool inlineArr = indent < 0;
        if (!inlineArr) {
            bool allScalar = true;
            size_t len = 0;
            for (auto& v : array_) { if (!isScalar(v)) allScalar = false; len += v.text_.size() + 2; }
            inlineArr = allScalar && (len < 80 || !array_[0].isString());
        }
        out += '[';
        for (size_t i = 0; i < array_.size(); ++i) {
            if (i) out += indent >= 0 && inlineArr ? ", " : ",";
            if (!inlineArr) nl(depth + 1);
            array_[i].dumpTo(out, inlineArr ? -1 : indent, depth + 1);
        }
        if (!inlineArr) nl(depth);
        out += ']';
        break;
    }
    case Type::Object: {
        if (object_.empty()) { out += "{}"; break; }
        // small flat objects ({"x": 1, "y": 2}) stay on one line
        bool small = indent >= 0 && object_.size() <= 4;
        size_t len = 0;
        for (auto& [k, v] : object_) { if (!isScalar(v) && !(v.isArray() && v.array_.size() <= 4 && std::all_of(v.array_.begin(), v.array_.end(), isScalar))) small = false; len += k.size() + v.text_.size() + 6; }
        if (len > 90) small = false;
        out += '{';
        for (size_t i = 0; i < object_.size(); ++i) {
            if (i) out += ',';
            if (small) out += i ? " " : "";
            else nl(depth + 1);
            escapeTo(out, object_[i].first);
            out += indent >= 0 ? ": " : ":";
            object_[i].second.dumpTo(out, small ? -1 : indent, depth + 1);
        }
        if (!small) nl(depth);
        out += '}';
        break;
    }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

} // namespace rts
