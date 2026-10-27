#include "Localization.h"
#include "../data/Json.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace rts::i18n {

// ------------------------------------------------------------------ plurals

static const char* const kPluralNames[kPluralCount] = {"zero", "one", "two", "few", "many", "other"};

const char* pluralName(Plural p) { return kPluralNames[int(p)]; }

bool parsePlural(std::string_view s, Plural& out) {
    for (int i = 0; i < kPluralCount; ++i)
        if (s == kPluralNames[i]) { out = Plural(i); return true; }
    return false;
}

static bool among(std::string_view lang, std::initializer_list<const char*> list) {
    for (const char* l : list) if (lang == l) return true;
    return false;
}

Plural pluralOf(std::string_view tag, int64_t n) {
    // Integer rules from CLDR (cardinal). Unlisted languages use the English rule.
    const std::string lang = baseLanguage(tag);
    const int64_t a = n < 0 ? -n : n;
    const int64_t m10 = a % 10, m100 = a % 100;
    if (among(lang, {"ja", "zh", "ko", "vi", "th", "id", "ms", "lo", "my", "km", "yue"})) return Plural::Other;
    if (among(lang, {"ru", "uk", "be"})) {
        if (m10 == 1 && m100 != 11) return Plural::One;
        if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return Plural::Few;
        return Plural::Many;
    }
    if (lang == "pl") {
        if (a == 1) return Plural::One;
        if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return Plural::Few;
        return Plural::Many;
    }
    if (among(lang, {"cs", "sk"})) return a == 1 ? Plural::One : (a >= 2 && a <= 4) ? Plural::Few : Plural::Other;
    if (among(lang, {"hr", "sr", "bs", "sh"})) {
        if (m10 == 1 && m100 != 11) return Plural::One;
        if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return Plural::Few;
        return Plural::Other;
    }
    if (lang == "lt") {
        if (m10 == 1 && (m100 < 11 || m100 > 19)) return Plural::One;
        if (m10 >= 2 && (m100 < 11 || m100 > 19)) return Plural::Few;
        return Plural::Other;
    }
    if (lang == "lv") {
        if (m10 == 0 || (m100 >= 11 && m100 <= 19)) return Plural::Zero;
        if (m10 == 1 && m100 != 11) return Plural::One;
        return Plural::Other;
    }
    if (lang == "ro") {
        if (a == 1) return Plural::One;
        if (a == 0 || (m100 >= 2 && m100 <= 19)) return Plural::Few;
        return Plural::Other;
    }
    if (lang == "sl") {
        if (m100 == 1) return Plural::One;
        if (m100 == 2) return Plural::Two;
        if (m100 == 3 || m100 == 4) return Plural::Few;
        return Plural::Other;
    }
    if (lang == "ar") {
        if (a == 0) return Plural::Zero;
        if (a == 1) return Plural::One;
        if (a == 2) return Plural::Two;
        if (m100 >= 3 && m100 <= 10) return Plural::Few;
        if (m100 >= 11) return Plural::Many;
        return Plural::Other;
    }
    if (among(lang, {"he", "iw"})) return a == 1 ? Plural::One : a == 2 ? Plural::Two : Plural::Other;
    if (lang == "ga") {
        if (a == 1) return Plural::One;
        if (a == 2) return Plural::Two;
        if (a >= 3 && a <= 6) return Plural::Few;
        if (a >= 7 && a <= 10) return Plural::Many;
        return Plural::Other;
    }
    // "one" covers 0 too (French, Brazilian Portuguese, Hindi...). European Portuguese is like English.
    if (among(lang, {"fr", "hi", "bn", "fa", "am", "zu", "hy", "kab"}) || (lang == "pt" && tag != "pt-PT"))
        return a <= 1 ? Plural::One : Plural::Other;
    return a == 1 ? Plural::One : Plural::Other;
}

std::vector<Plural> requiredPlurals(std::string_view lang) {
    // Integers cover every reachable category within 0..199; "other" is always required as the
    // last-resort form.
    bool used[kPluralCount] = {};
    for (int64_t n = 0; n < 200; ++n) used[int(pluralOf(lang, n))] = true;
    used[int(Plural::Other)] = true;
    std::vector<Plural> out;
    for (int i = 0; i < kPluralCount; ++i) if (used[i]) out.push_back(Plural(i));
    return out;
}

// ------------------------------------------------------------------ language tags

std::string normalizeTag(std::string_view tag) {
    // cut ".UTF-8" and "@euro"
    size_t cut = tag.find_first_of(".@");
    if (cut != std::string_view::npos) tag = tag.substr(0, cut);
    if (tag.empty() || tag == "C" || tag == "POSIX") return {};
    std::string out;
    int part = 0;
    size_t i = 0;
    while (i <= tag.size()) {
        size_t j = tag.find_first_of("-_", i);
        if (j == std::string_view::npos) j = tag.size();
        std::string p(tag.substr(i, j - i));
        if (!p.empty()) {
            for (auto& c : p) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (part > 0) {
                if (p.size() == 2) for (auto& c : p) c = char(std::toupper(static_cast<unsigned char>(c)));   // region
                else if (p.size() == 4) p[0] = char(std::toupper(static_cast<unsigned char>(p[0])));          // script
                out += '-';
            }
            out += p;
            ++part;
        }
        i = j + 1;
    }
    return out;
}

std::string baseLanguage(std::string_view tag) {
    std::string t = normalizeTag(tag);
    size_t d = t.find('-');
    return d == std::string::npos ? t : t.substr(0, d);
}

std::string negotiate(const std::vector<std::string>& preferred, const std::vector<std::string>& available,
                      const std::string& fallback) {
    std::vector<std::string> avail;
    for (auto& a : available) avail.push_back(normalizeTag(a));
    for (auto& p : preferred) {
        std::string want = normalizeTag(p);
        if (want.empty()) continue;
        for (size_t i = 0; i < avail.size(); ++i) if (avail[i] == want) return available[i];
        std::string base = baseLanguage(want);
        for (size_t i = 0; i < avail.size(); ++i) if (avail[i] == base) return available[i];
        for (size_t i = 0; i < avail.size(); ++i) if (baseLanguage(avail[i]) == base) return available[i];
    }
    if (!fallback.empty()) return fallback;
    return available.empty() ? std::string() : available.front();
}

// ------------------------------------------------------------------ args / formatting

Args& Args::str(std::string name, std::string value) {
    args_.push_back({std::move(name), std::move(value), 0, false});
    return *this;
}

Args& Args::num(std::string name, int64_t value) {
    args_.push_back({std::move(name), std::to_string(value), value, true});
    return *this;
}

const std::string* Args::find(std::string_view name) const {
    for (auto& a : args_) if (a.name == name) return &a.text;
    return nullptr;
}

bool Args::number(std::string_view name, int64_t& out) const {
    for (auto& a : args_) if (a.name == name && a.isNum) { out = a.n; return true; }
    return false;
}

bool Args::firstNumber(int64_t& out) const {
    for (auto& a : args_) if (a.isNum) { out = a.n; return true; }
    return false;
}

std::string format(std::string_view p, const Args& args) {
    std::string out;
    out.reserve(p.size() + 16);
    for (size_t i = 0; i < p.size(); ++i) {
        char c = p[i];
        if (c == '{' && i + 1 < p.size() && p[i + 1] == '{') { out += '{'; ++i; continue; }
        if (c == '}' && i + 1 < p.size() && p[i + 1] == '}') { out += '}'; ++i; continue; }
        if (c == '{') {
            size_t end = p.find('}', i + 1);
            if (end != std::string_view::npos) {
                std::string_view name = p.substr(i + 1, end - i - 1);
                if (const std::string* v = args.find(name)) { out += *v; i = end; continue; }
            }
        }
        out += c;
    }
    return out;
}

std::vector<std::string> placeholders(std::string_view p) {
    std::vector<std::string> out;
    for (size_t i = 0; i < p.size(); ++i) {
        if ((p[i] == '{' || p[i] == '}') && i + 1 < p.size() && p[i + 1] == p[i]) { ++i; continue; }
        if (p[i] != '{') continue;
        size_t end = p.find('}', i + 1);
        if (end == std::string_view::npos) break;
        out.emplace_back(p.substr(i + 1, end - i - 1));
        i = end;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static bool bracesBalanced(std::string_view p) {
    bool open = false;
    for (size_t i = 0; i < p.size(); ++i) {
        char c = p[i];
        if (!open && (c == '{' || c == '}') && i + 1 < p.size() && p[i + 1] == c) { ++i; continue; }
        if (c == '{') { if (open) return false; open = true; }
        else if (c == '}') { if (!open) return false; open = false; }
    }
    return !open;
}

static void appendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
    else { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
}

std::string pseudoLocalize(std::string_view s) {
    // Latin letters get accents (still readable), placeholders are kept intact, then padding.
    static const char* const lower[26] = {"á","ƀ","ç","ð","é","ƒ","ĝ","ĥ","í","ĵ","ķ","ĺ","ɱ","ñ","ó","þ","ǫ","ŕ","š","ţ","ú","ṽ","ŵ","ẋ","ý","ž"};
    static const char* const upper[26] = {"Å","Ɓ","Ç","Ð","É","Ƒ","Ĝ","Ĥ","Í","Ĵ","Ķ","Ĺ","Ṁ","Ñ","Ó","Þ","Ǫ","Ŕ","Š","Ţ","Û","Ṽ","Ŵ","Ẋ","Ý","Ž"};
    std::string out = "[";
    size_t letters = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '{') {
            size_t end = s.find('}', i);
            if (end != std::string_view::npos) { out += s.substr(i, end - i + 1); i = end; continue; }
        }
        if (c >= 'a' && c <= 'z') { out += lower[c - 'a']; ++letters; }
        else if (c >= 'A' && c <= 'Z') { out += upper[c - 'A']; ++letters; }
        else { out += c; if (static_cast<unsigned char>(c) >= 0xC0) ++letters; }
    }
    for (size_t i = 0, pad = (letters * 35 + 99) / 100; i < pad; ++i) appendUtf8(out, i % 2 ? 0x00B7 : 0x2013);
    out += "]";
    return out;
}

// ------------------------------------------------------------------ catalog

static bool isPluralObject(const Json& j) {
    if (!j.isObject() || j.size() == 0) return false;
    bool other = false;
    for (auto& [k, v] : j.members()) {
        Plural p;
        if (!parsePlural(k, p) || !v.isString()) return false;
        if (p == Plural::Other) other = true;
    }
    return other;
}

void Catalog::merge(const Json& table) {
    if (!table.isObject()) throw std::runtime_error("string table must be a JSON object");
    mergeObject(table, "");
}

void Catalog::mergeObject(const Json& obj, const std::string& prefix) {
    for (auto& [k, v] : obj.members()) {
        if (!k.empty() && k[0] == '@') {
            if (prefix.empty()) meta_[k] = v.str();
            continue;
        }
        std::string key = prefix.empty() ? k : prefix + "." + k;
        if (v.isString()) {
            Entry e; e.text = v.str();
            entries_[key] = std::move(e);
        } else if (isPluralObject(v)) {
            Entry e; e.plural = true;
            for (auto& [pk, pv] : v.members()) { Plural p; parsePlural(pk, p); e.forms[int(p)] = pv.str(); }
            e.text = e.forms[int(Plural::Other)];
            entries_[key] = std::move(e);
        } else if (v.isObject()) {
            mergeObject(v, key);
        } else {
            throw std::runtime_error("key '" + key + "': expected a string, plural forms or a nested object");
        }
    }
}

const Entry* Catalog::find(std::string_view key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second;
}

// ------------------------------------------------------------------ localization

void Localization::clear() {
    catalogs_.clear();
    chain_.clear();
    language_.clear();
}

void Localization::addTable(const std::string& language, const Json& table) {
    catalogs_[normalizeTag(language)].merge(table);
}

void Localization::setLanguage(const std::string& language, const std::vector<std::string>& fallbacks) {
    language_ = normalizeTag(language);
    chain_.clear();
    auto push = [&](const std::string& l) {
        if (!l.empty() && std::find(chain_.begin(), chain_.end(), l) == chain_.end()) chain_.push_back(l);
    };
    // "zh-Hant-TW" -> "zh-Hant" -> "zh"
    for (std::string l = language_; !l.empty();) {
        push(l);
        size_t d = l.rfind('-');
        l = d == std::string::npos ? std::string() : l.substr(0, d);
    }
    for (auto& f : fallbacks) {
        std::string n = normalizeTag(f);
        push(n);
        push(baseLanguage(n));
    }
}

void Localization::load(const FileReader& read, const std::string& language, const std::vector<std::string>& dirs,
                        const std::vector<std::string>& fallbacks) {
    clear();
    setLanguage(language, fallbacks);
    for (auto& dir : dirs) {
        for (auto& lang : chain_) {
            std::string path = dir.empty() ? lang + ".json" : dir + "/" + lang + ".json";
            std::string text;
            if (!read(path, text)) continue;
            try {
                catalogs_[lang].merge(Json::parse(text));
            } catch (const std::exception& e) {
                throw std::runtime_error(path + ": " + e.what());
            }
        }
    }
}

const Catalog* Localization::catalog(const std::string& language) const {
    auto it = catalogs_.find(normalizeTag(language));
    return it == catalogs_.end() ? nullptr : &it->second;
}

std::pair<const Entry*, const std::string*> Localization::lookup(std::string_view key) const {
    for (auto& lang : chain_) {
        auto it = catalogs_.find(lang);
        if (it == catalogs_.end()) continue;
        if (const Entry* e = it->second.find(key)) return {e, &lang};
    }
    return {nullptr, nullptr};
}

std::string Localization::render(const Entry* e, const std::string& lang, std::string_view fallback, const Args& args) const {
    std::string_view pattern = fallback;
    if (e) {
        pattern = e->text;
        if (e->plural) {
            int64_t n = 0;
            if (args.number("count", n) || args.firstNumber(n)) {
                const std::string& f = e->forms[int(pluralOf(lang, n))];
                if (!f.empty()) pattern = f;
            }
        }
    }
    std::string out = format(pattern, args);
    return pseudo_ ? pseudoLocalize(out) : out;
}

std::string Localization::tr(std::string_view key, const Args& args) const {
    auto [e, lang] = lookup(key);
    return render(e, lang ? *lang : language_, key, args);
}

std::string Localization::trOr(std::string_view key, std::string_view fallback, const Args& args) const {
    auto [e, lang] = lookup(key);
    return render(e, lang ? *lang : language_, fallback, args);
}

std::string Localization::text(std::string_view s, const Args& args) const {
    if (s.size() >= 2 && s[0] == '@' && s[1] == '@') return std::string(s.substr(1));
    if (s.size() >= 2 && s[0] == '@') return tr(s.substr(1), args);
    return format(s, args);
}

std::string Localization::meta(std::string_view key, std::string_view def) const {
    for (auto& lang : chain_) {
        auto it = catalogs_.find(lang);
        if (it == catalogs_.end()) continue;
        auto m = it->second.meta().find(key);
        if (m != it->second.meta().end()) return m->second;
    }
    return std::string(def);
}

// ------------------------------------------------------------------ validation

std::vector<Issue> check(const Catalog& base, const std::string& baseLang, const Catalog& lang, const std::string& language) {
    std::vector<Issue> out;
    auto add = [&](Issue::Severity s, const std::string& key, std::string msg) {
        out.push_back({s, language, key, std::move(msg)});
    };
    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (auto& x : v) s += (s.empty() ? "{" : ", {") + x + "}";
        return s.empty() ? std::string("none") : s;
    };
    auto patterns = [](const Entry& e) {
        std::vector<std::string> p;
        if (e.plural) { for (auto& f : e.forms) if (!f.empty()) p.push_back(f); }
        else p.push_back(e.text);
        return p;
    };
    for (auto& [key, be] : base.entries()) {
        const Entry* le = lang.find(key);
        if (!le) { add(Issue::Warning, key, "not translated"); continue; }
        std::vector<std::string> want;
        for (auto& p : patterns(be)) for (auto& n : placeholders(p)) want.push_back(n);
        std::sort(want.begin(), want.end());
        want.erase(std::unique(want.begin(), want.end()), want.end());
        for (auto& p : patterns(*le)) {
            auto got = placeholders(p);
            for (auto& n : got)
                if (!std::binary_search(want.begin(), want.end(), n))
                    add(Issue::Error, key, "unknown placeholder {" + n + "} (" + baseLang + " has " + join(want) + ")");
        }
        if (!le->plural && !be.plural && le->text == be.text && baseLanguage(language) != baseLanguage(baseLang) &&
            le->text.find_first_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos)
            add(Issue::Warning, key, "same text as in " + baseLang + " (not translated yet?)");
        if (be.plural && !le->plural)
            add(Issue::Warning, key, "is plural in " + baseLang + " but a plain string here");
        if (le->plural) {
            for (Plural p : requiredPlurals(language))
                if (le->forms[int(p)].empty()) add(Issue::Error, key, std::string("missing plural form '") + pluralName(p) + "'");
        }
    }
    for (auto& [key, le] : lang.entries()) {
        if (!base.find(key)) add(Issue::Warning, key, "not present in " + baseLang + " (stale key?)");
        for (auto& p : patterns(le))
            if (!bracesBalanced(p)) add(Issue::Error, key, "unbalanced braces (use {{ and }} for literal braces)");
    }
    return out;
}

} // namespace rts::i18n
