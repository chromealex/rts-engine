// Small dependency-free JSON reader for game data.
// Numbers are kept as their source text and converted with Fixed::parse, so data loading
// never goes through float and is bit-identical on every platform.
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <stdexcept>
#include <cstdint>
#include "../core/Fixed.h"

namespace rts {

class Json;
// Tools only (rules_lint, rules_schema): while installed on a thread, every key lookup (has, [],
// get*) on an object and every walk over an object's members is recorded, so a tool can tell
// which keys a loader actually reads. Off by default; the simulation never installs one.
struct JsonAccessLog {
    std::vector<std::pair<const Json*, std::string>> keys;   // (object, key) lookups, present or not
    std::vector<const Json*> iterated;                        // objects whose members were walked
};

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    static Json parse(const std::string& text);       // throws JsonError
    static Json parseFile(const std::string& path);   // throws JsonError

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }

    // Object access. Member order is preserved from the file (deterministic iteration).
    bool has(const std::string& key) const;
    const Json& operator[](const std::string& key) const; // returns null Json if missing
    const std::vector<std::pair<std::string, Json>>& members() const { if (accessLog_) noteIterated(); return object_; }

    // Array access
    size_t size() const { return type_ == Type::Array ? array_.size() : type_ == Type::Object ? object_.size() : 0; }
    const Json& operator[](size_t i) const;
    const std::vector<Json>& items() const { return array_; }

    // Values (with defaults when missing / wrong type)
    std::string str(const std::string& def = "") const { return type_ == Type::String ? text_ : def; }
    bool boolean(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    Fixed fx(Fixed def = {}) const;
    int64_t integer(int64_t def = 0) const;

    // Convenience: obj.get("speed", 1_fx)
    Fixed getFx(const std::string& key, Fixed def = {}) const { return (*this)[key].fx(def); }
    int64_t getInt(const std::string& key, int64_t def = 0) const { return (*this)[key].integer(def); }
    std::string getStr(const std::string& key, const std::string& def = "") const { return (*this)[key].str(def); }
    bool getBool(const std::string& key, bool def = false) const { return (*this)[key].boolean(def); }

    // ---- building / editing (editor, MCP, tools; the simulation only reads)
    static Json object() { Json j; j.type_ = Type::Object; return j; }
    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json string(std::string s) { Json j; j.type_ = Type::String; j.text_ = std::move(s); return j; }
    static Json fromBool(bool b) { Json j; j.type_ = Type::Bool; j.bool_ = b; return j; }
    static Json number(Fixed v);
    static Json number(int64_t v);
    static Json number(double v, int decimals = 3);       // tools only: rounded, no exponent
    static Json numberText(std::string literal);           // keeps the literal exactly
    Json& set(const std::string& key, Json v);             // replaces in place or appends (keeps order)
    bool erase(const std::string& key);
    Json& push(Json v);
    void eraseAt(size_t i);
    Json* find(const std::string& key);                    // mutable member, nullptr if missing
    Json& at(size_t i) { return array_[i]; }
    Json& member(const std::string& key);                  // creates an empty object member if missing
    std::vector<Json>& itemsMut() { return array_; }
    std::vector<std::pair<std::string, Json>>& membersMut() { return object_; }
    const std::string& numberLiteral() const { return text_; }
    double toDouble(double def = 0) const;                 // tools only
    // Serialize. indent < 0: compact. Short arrays of scalars stay on one line.
    std::string dump(int indent = 2) const;
    bool operator==(const Json& o) const;

    static void setAccessLog(JsonAccessLog* log) { accessLog_ = log; }   // nullptr: off
    static JsonAccessLog* accessLog() { return accessLog_; }

private:
    static thread_local JsonAccessLog* accessLog_;
    void noteKey(const std::string& key) const;
    void noteIterated() const;
    void dumpTo(std::string& out, int indent, int depth) const;
    friend class JsonParser;
    Type type_ = Type::Null;
    bool bool_ = false;
    std::string text_; // string value or number literal
    std::vector<Json> array_;
    std::vector<std::pair<std::string, Json>> object_;
};

struct JsonError : std::runtime_error { using std::runtime_error::runtime_error; };

} // namespace rts
