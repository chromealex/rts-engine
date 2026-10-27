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
    const std::vector<std::pair<std::string, Json>>& members() const { return object_; }

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

private:
    friend class JsonParser;
    Type type_ = Type::Null;
    bool bool_ = false;
    std::string text_; // string value or number literal
    std::vector<Json> array_;
    std::vector<std::pair<std::string, Json>> object_;
};

struct JsonError : std::runtime_error { using std::runtime_error::runtime_error; };

} // namespace rts
