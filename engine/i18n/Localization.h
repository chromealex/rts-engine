// Localization: string tables per language, fallback chains, {placeholders}, CLDR plural forms.
//
// Tables are JSON (same reader as game data, comments allowed). Nested objects are flattened
// with dots, so  { "menu": { "play": "Играть" } }  defines "menu.play". An object whose keys are
// all plural categories (zero/one/two/few/many/other, "other" required) is a plural entry:
//   "units": { "one": "{count} юнит", "few": "{count} юнита", "many": "{count} юнитов", "other": "{count} юнита" }
// Keys starting with "@" are metadata ("@language": "Русский", "@font": "fonts/x.ttf").
//
// Lookup walks the chain  requested ("pt-BR") -> its base ("pt") -> fallback languages -> the
// fallback text given by the caller -> the key itself, so a missing string is visible, never empty.
// Presentation-only: never use translated text in simulation logic or hashes.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace rts { class Json; }

namespace rts::i18n {

enum class Plural : uint8_t { Zero, One, Two, Few, Many, Other };
constexpr int kPluralCount = 6;
const char* pluralName(Plural p);
bool parsePlural(std::string_view s, Plural& out);

// CLDR cardinal plural category of an integer for a language ("ru", "pt-BR", ...).
Plural pluralOf(std::string_view lang, int64_t n);
// Categories a complete translation into `lang` must provide.
std::vector<Plural> requiredPlurals(std::string_view lang);

// "ru_RU.UTF-8" / "RU-ru" / "zh_Hant_TW" -> "ru-RU" / "ru-RU" / "zh-Hant-TW". Empty for "C"/"POSIX".
std::string normalizeTag(std::string_view tag);
std::string baseLanguage(std::string_view tag);   // "pt-BR" -> "pt"
// Best of `available` for the user's ordered `preferred` list: exact tag, then same base
// language; `fallback` (or the first available) when nothing matches.
std::string negotiate(const std::vector<std::string>& preferred, const std::vector<std::string>& available,
                      const std::string& fallback = "");

// Languages the OS/browser reports for the user, most preferred first, normalized. May be empty.
std::vector<std::string> systemLanguages();

// Named arguments for placeholders:  tr("hud.left", Args().num("min", 3).str("who", name))
class Args {
public:
    Args& str(std::string name, std::string value);
    Args& num(std::string name, int64_t value);
    const std::string* find(std::string_view name) const;
    bool number(std::string_view name, int64_t& out) const;
    bool firstNumber(int64_t& out) const;  // plural selector when no "count" is given
private:
    struct A { std::string name, text; int64_t n = 0; bool isNum = false; };
    std::vector<A> args_;
};

struct Entry {
    std::string text;                      // simple string
    std::string forms[kPluralCount];       // plural forms (empty = not given)
    bool plural = false;
};

// One language's table.
class Catalog {
public:
    // Merges a JSON table; later merges override earlier ones key by key.
    void merge(const Json& table);
    const Entry* find(std::string_view key) const;
    const std::map<std::string, Entry, std::less<>>& entries() const { return entries_; }
    const std::map<std::string, std::string, std::less<>>& meta() const { return meta_; }
    void set(const std::string& key, Entry e) { entries_[key] = std::move(e); }
private:
    void mergeObject(const Json& obj, const std::string& prefix);
    std::map<std::string, Entry, std::less<>> entries_;
    std::map<std::string, std::string, std::less<>> meta_;
};

// Reads a content file (relative path); returns false if it does not exist.
using FileReader = std::function<bool(const std::string& path, std::string& out)>;

class Localization {
public:
    // Loads `language` with its chain from every dir in order (later dirs override earlier ones,
    // e.g. {"engine/lang", "lang"}: the game can override engine strings). Missing files are fine;
    // malformed ones throw std::runtime_error naming the file.
    void load(const FileReader& read, const std::string& language, const std::vector<std::string>& dirs,
              const std::vector<std::string>& fallbacks = {"en"});

    // Manual setup (tests, tools).
    void clear();
    void addTable(const std::string& language, const Json& table);
    void setLanguage(const std::string& language, const std::vector<std::string>& fallbacks = {"en"});

    const std::string& language() const { return language_; }
    const std::vector<std::string>& chain() const { return chain_; }
    const Catalog* catalog(const std::string& language) const;

    // Pseudo-localization: every string comes out accented and ~35% longer, wrapped in [ ],
    // to spot hard-coded text and layouts that break with longer languages.
    void setPseudo(bool on) { pseudo_ = on; }
    bool pseudo() const { return pseudo_; }

    bool has(std::string_view key) const { return lookup(key).first != nullptr; }
    std::string tr(std::string_view key, const Args& args = {}) const;
    // Same, with the text to use when no table has the key (source-language text from data).
    std::string trOr(std::string_view key, std::string_view fallback, const Args& args = {}) const;
    // Data strings: "@key" is looked up, "@@x" is the literal "@x", anything else is returned as is.
    std::string text(std::string_view s, const Args& args = {}) const;
    // Metadata of the active language ("@language", "@font", ...), through the chain.
    std::string meta(std::string_view key, std::string_view def = "") const;

private:
    std::pair<const Entry*, const std::string*> lookup(std::string_view key) const;
    std::string render(const Entry* e, const std::string& lang, std::string_view fallback, const Args& args) const;

    std::map<std::string, Catalog, std::less<>> catalogs_;
    std::string language_;
    std::vector<std::string> chain_;
    bool pseudo_ = false;
};

// Substitutes {name} placeholders ({{ and }} are literal braces). Unknown names stay as {name}.
std::string format(std::string_view pattern, const Args& args);
std::string pseudoLocalize(std::string_view s);
// Placeholder names used in a pattern, sorted and unique.
std::vector<std::string> placeholders(std::string_view pattern);

// ---- validation (tools, tests, editor) ----
struct Issue {
    enum Severity : uint8_t { Error, Warning } severity = Warning;
    std::string language, key, message;
};
// Compares `language` against `base`: missing keys, keys unknown to base, placeholder
// mismatches, missing plural forms, unbalanced braces.
std::vector<Issue> check(const Catalog& base, const std::string& baseLanguage,
                         const Catalog& lang, const std::string& language);

} // namespace rts::i18n
