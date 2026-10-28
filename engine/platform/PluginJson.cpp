#include "PluginJson.h"

#include <cstdlib>

namespace rts::plugins::json {
namespace {

struct Parser {
    const std::string& t;
    size_t i = 0;
    std::string err;

    void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i; }
    bool fail(const char* what) { if (err.empty()) err = std::string(what) + " at offset " + std::to_string(i); return false; }

    static void utf8(std::string& o, uint32_t c) {
        if (c < 0x80) o += char(c);
        else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
        else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 63)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
    }
    bool hex4(uint32_t& v) {
        if (i + 4 > t.size()) return fail("truncated \\u escape");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = t[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool string(std::string& o) {
        ++i; // opening quote
        while (i < t.size()) {
            char c = t[i++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (i >= t.size()) break;
            char e = t[i++];
            switch (e) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                uint32_t c1 = 0;
                if (!hex4(c1)) return false;
                if (c1 >= 0xD800 && c1 < 0xDC00 && i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                    i += 2;
                    uint32_t c2 = 0;
                    if (!hex4(c2)) return false;
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                }
                utf8(o, c1);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool value(Value& v, int depth) {
        if (depth > 64) return fail("nesting too deep");
        ws();
        if (i >= t.size()) return fail("unexpected end");
        char c = t[i];
        if (c == '{') {
            v = Value::object(); ++i; ws();
            if (i < t.size() && t[i] == '}') { ++i; return true; }
            for (;;) {
                ws();
                if (i >= t.size() || t[i] != '"') return fail("expected key");
                std::string k;
                if (!string(k)) return false;
                ws();
                if (i >= t.size() || t[i] != ':') return fail("expected ':'");
                ++i;
                Value item;
                if (!value(item, depth + 1)) return false;
                v.obj.emplace_back(std::move(k), std::move(item));
                ws();
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == '}') { ++i; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v = Value::array(); ++i; ws();
            if (i < t.size() && t[i] == ']') { ++i; return true; }
            for (;;) {
                Value item;
                if (!value(item, depth + 1)) return false;
                v.arr.push_back(std::move(item));
                ws();
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == ']') { ++i; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.type = Value::Type::String; return string(v.s); }
        if (t.compare(i, 4, "true") == 0) { v = Value::boolean_(true); i += 4; return true; }
        if (t.compare(i, 5, "false") == 0) { v = Value::boolean_(false); i += 5; return true; }
        if (t.compare(i, 4, "null") == 0) { v = Value(); i += 4; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t s = i;
            if (t[i] == '-') ++i;
            while (i < t.size() && ((t[i] >= '0' && t[i] <= '9') || t[i] == '.' || t[i] == 'e' || t[i] == 'E' || t[i] == '+' || t[i] == '-')) ++i;
            v = Value::number(t.substr(s, i - s));
            return true;
        }
        return fail("unexpected character");
    }
};

void dumpTo(const Value& v, std::string& o) {
    switch (v.type) {
    case Value::Type::Null: o += "null"; break;
    case Value::Type::Bool: o += v.b ? "true" : "false"; break;
    case Value::Type::Number: o += v.s.empty() ? "0" : v.s; break;
    case Value::Type::String: o += quote(v.s); break;
    case Value::Type::Array:
        o += '[';
        for (size_t k = 0; k < v.arr.size(); ++k) { if (k) o += ','; dumpTo(v.arr[k], o); }
        o += ']';
        break;
    case Value::Type::Object:
        o += '{';
        for (size_t k = 0; k < v.obj.size(); ++k) {
            if (k) o += ',';
            o += quote(v.obj[k].first); o += ':'; dumpTo(v.obj[k].second, o);
        }
        o += '}';
        break;
    }
}

const Value& nullValue() { static const Value n; return n; }
} // namespace

const Value* Value::find(const std::string& key) const {
    for (auto& [k, v] : obj) if (k == key) return &v;
    return nullptr;
}
const Value& Value::operator[](const std::string& key) const { const Value* v = find(key); return v ? *v : nullValue(); }
std::string Value::str(const std::string& def) const {
    if (type == Type::String || type == Type::Number) return s;
    if (type == Type::Bool) return b ? "true" : "false";
    return def;
}
double Value::num(double def) const {
    if (type == Type::Number || type == Type::String) { char* e = nullptr; double d = std::strtod(s.c_str(), &e); return e != s.c_str() ? d : def; }
    return def;
}
int64_t Value::i64(int64_t def) const {
    if (type == Type::Number || type == Type::String) { char* e = nullptr; long long d = std::strtoll(s.c_str(), &e, 10); return e != s.c_str() ? int64_t(d) : def; }
    return def;
}
bool Value::boolean(bool def) const {
    if (type == Type::Bool) return b;
    if (type == Type::String) return s == "true" || s == "1";
    if (type == Type::Number) return num() != 0;
    return def;
}
Value& Value::set(const std::string& key, Value v) {
    type = Type::Object;
    for (auto& [k, old] : obj) if (k == key) { old = std::move(v); return *this; }
    obj.emplace_back(key, std::move(v));
    return *this;
}

bool parse(const std::string& text, Value& out, std::string* error) {
    Parser p{text, 0, {}};
    Value v;
    bool ok = p.value(v, 0);
    if (ok) { p.ws(); if (p.i != text.size()) ok = p.fail("trailing characters"); }
    if (!ok) { if (error) *error = p.err; out = Value(); return false; }
    out = std::move(v);
    return true;
}
Value parseOr(const std::string& text, Value fallback) {
    Value v;
    return parse(text, v) ? v : fallback;
}
std::string dump(const Value& v) { std::string o; dumpTo(v, o); return o; }

std::string stripComments(const std::string& t) {
    std::string o;
    o.reserve(t.size());
    bool inStr = false;
    for (size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (inStr) {
            o += c;
            if (c == '\\' && i + 1 < t.size()) o += t[++i];
            else if (c == '"') inStr = false;
        } else if (c == '"') {
            inStr = true; o += c;
        } else if (c == '/' && i + 1 < t.size() && t[i + 1] == '/') {
            while (i < t.size() && t[i] != '\n') ++i;
            o += '\n';
        } else if (c == '/' && i + 1 < t.size() && t[i + 1] == '*') {
            size_t e = t.find("*/", i + 2);
            i = e == std::string::npos ? t.size() : e + 1;
        } else {
            o += c;
        }
    }
    return o;
}

std::string quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { static const char* h = "0123456789abcdef"; o += "\\u00"; o += h[c >> 4]; o += h[c & 15]; }
            else o += char(c);
        }
    }
    return o + "\"";
}

} // namespace rts::plugins::json
