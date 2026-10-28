// Minimal JSON DOM for plugin messages. Unlike engine/data/Json (fixed-point numbers for the sim),
// numbers keep their exact text, so prices, ids and tokens pass through plugins unchanged.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rts::plugins::json {

struct Member;

struct Value {
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    std::string s;                // String value, or Number text
    std::vector<Value> arr;
    std::vector<Member> obj;      // insertion order kept

    bool isNull() const { return type == Type::Null; }
    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    const Value* find(const std::string& key) const;
    const Value& operator[](const std::string& key) const; // null value if missing
    std::string str(const std::string& def = "") const;    // strings; numbers/bools as text
    double num(double def = 0) const;
    int64_t i64(int64_t def = 0) const;
    bool boolean(bool def = false) const;

    static Value string(std::string v);
    static Value number(const std::string& text);
    static Value number(int64_t v);
    static Value boolean_(bool v);
    static Value object();
    static Value array();
    Value& set(const std::string& key, Value v);       // object: add or replace; returns *this (chainable)
    Value& push(Value v);
};

// One object member. A named struct (not std::pair) so Value can hold a vector of it while Value
// itself is still incomplete — libstdc++ rejects pair<string, Value> there.
struct Member {
    std::string first;
    Value second;
    Member() = default;
    Member(std::string k, Value v) : first(std::move(k)), second(std::move(v)) {}
};

inline Value Value::string(std::string v) { Value r; r.type = Type::String; r.s = std::move(v); return r; }
inline Value Value::number(const std::string& text) { Value r; r.type = Type::Number; r.s = text; return r; }
inline Value Value::number(int64_t v) { return number(std::to_string(v)); }
inline Value Value::boolean_(bool v) { Value r; r.type = Type::Bool; r.b = v; return r; }
inline Value Value::object() { Value r; r.type = Type::Object; return r; }
inline Value Value::array() { Value r; r.type = Type::Array; return r; }
inline Value& Value::push(Value v) { arr.push_back(std::move(v)); return *this; }

// Returns false (and a readable message) on malformed input; `out` is then Null.
bool parse(const std::string& text, Value& out, std::string* error = nullptr);
Value parseOr(const std::string& text, Value fallback = Value::object());
std::string dump(const Value& v);
std::string quote(const std::string& s); // JSON string literal with escapes
// Removes // and /* */ comments outside strings (game.json allows them, like engine/data/Json).
std::string stripComments(const std::string& text);

} // namespace rts::plugins::json
