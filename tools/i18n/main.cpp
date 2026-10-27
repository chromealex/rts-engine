// rts_i18n — string tables for the engine and games.
//
//   rts_i18n check   [--project games/skirmish] [--engine engine/assets]
//       Validates engine tables against English and the game's tables against its data
//       (names in rules/maps/game.json). Exit code 1 if there are errors.
//   rts_i18n extract --project games/skirmish --lang ru [--write]
//       Prints lang/ru.json with every translatable key: existing translations are kept,
//       missing ones get the source text and a // TODO comment. --write saves it.
#include "engine/data/Json.h"
#include "engine/i18n/GameLanguage.h"
#include "engine/i18n/Localization.h"
#include "modules/rts/RtsLocalize.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <algorithm>

using namespace rts;
namespace fs = std::filesystem;

namespace {

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

Json parseFile(const fs::path& p) {
    std::string text;
    if (!readFile(p, text)) throw std::runtime_error(p.string() + ": cannot read");
    try { return Json::parse(text); }
    catch (const std::exception& e) { throw std::runtime_error(p.string() + ": " + e.what()); }
}

std::string quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += char(c);
        }
    }
    return o + "\"";
}

struct Game {
    fs::path dir;
    Json manifest, rules;
    std::vector<std::pair<std::string, Json>> maps;
    i18n::GameLanguages langs;
    std::vector<game::SourceString> strings;
};

Game loadGame(const fs::path& dir) {
    Game g;
    g.dir = dir;
    g.manifest = parseFile(dir / "game.json");
    g.rules = parseFile(dir / g.manifest.getStr("rules", "rules.json"));
    for (auto& m : g.manifest["maps"].items()) {
        std::string f = m.getStr("file");
        g.maps.push_back({f, parseFile(dir / f)});
    }
    g.langs = i18n::readGameLanguages(g.manifest);
    g.strings = game::collectSourceStrings(g.manifest, g.rules, g.maps);
    return g;
}

int report(const std::vector<i18n::Issue>& issues, const std::string& file) {
    int errors = 0;
    for (auto& is : issues) {
        if (is.severity == i18n::Issue::Error) ++errors;
        std::printf("%s: %s: %s: %s\n", file.c_str(), is.severity == i18n::Issue::Error ? "error" : "warning",
                    is.key.c_str(), is.message.c_str());
    }
    return errors;
}

// Engine tables: every language checked against English.
int checkEngine(const fs::path& assets, int& warnings) {
    fs::path dir = assets / "lang";
    if (!fs::exists(dir)) return 0;
    i18n::Catalog en;
    en.merge(parseFile(dir / "en.json"));
    int errors = report(i18n::check(en, "en", en, "en"), (dir / "en.json").string());
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(dir)) if (e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        std::string tag = f.stem().string();
        if (tag == "en") continue;
        i18n::Catalog c;
        c.merge(parseFile(f));
        if (c.meta().find("@language") == c.meta().end())
            std::printf("%s: warning: no \"@language\" (name of the language in itself)\n", f.string().c_str());
        auto issues = i18n::check(en, "en", c, tag);
        for (auto& i : issues) if (i.severity == i18n::Issue::Warning) ++warnings;
        errors += report(issues, f.string());
    }
    return errors;
}

// UI documents: every {{ 'key' | tr(lang) }} must exist in `table`; visible text written directly
// in markup (outside tags and {{ }}) is reported as not localizable.
int checkMarkup(const fs::path& dir, const i18n::Catalog& table, const std::string& tableName, int& warnings) {
    if (!fs::exists(dir)) return 0;
    int errors = 0;
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(dir)) if (e.path().extension() == ".rml") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        std::string text;
        readFile(f, text);
        for (auto& k : i18n::markupKeys(text))
            if (!table.find(k)) { std::printf("%s: error: key '%s' is not in %s\n", f.string().c_str(), k.c_str(), tableName.c_str()); ++errors; }
        // text nodes: between '>' and '<', minus {{ }} expressions and comments
        // (<title> is the document's internal name, not shown)
        std::string plain;
        bool inTag = false, inComment = false, inTitle = false;
        char quote = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            if (inComment) { if (text.compare(i, 3, "-->") == 0) { inComment = false; i += 2; } continue; }
            if (inTag) {   // attribute values may contain '>' (data-if="a >= 0")
                if (quote) { if (text[i] == quote) quote = 0; }
                else if (text[i] == '"' || text[i] == '\'') quote = text[i];
                else if (text[i] == '>') inTag = false;
                continue;
            }
            if (text.compare(i, 4, "<!--") == 0) { inComment = true; continue; }
            if (text.compare(i, 7, "<title>") == 0) inTitle = true;
            if (text.compare(i, 8, "</title>") == 0) inTitle = false;
            if (text[i] == '<') { inTag = true; plain += ' '; continue; }
            if (inTitle) continue;
            if (text.compare(i, 2, "{{") == 0) { size_t e = text.find("}}", i); i = e == std::string::npos ? text.size() : e + 1; continue; }
            plain += text[i];
        }
        std::istringstream words(plain);
        for (std::string w; words >> w;) {
            bool letters = false;
            for (unsigned char c : w) if (std::isalpha(c) || c >= 0x80) letters = true;
            if (letters && w.find("&amp;") == std::string::npos) {
                std::printf("%s: warning: hard-coded text '%s' (use {{ 'key' | tr(lang) }})\n", f.string().c_str(), w.c_str());
                ++warnings;
            }
        }
    }
    return errors;
}

int checkGame(const Game& g, const fs::path& assets, int& warnings) {
    // Base = source text from data, overridden by the game's source-language table if any.
    i18n::Catalog base;
    for (auto& s : g.strings) { i18n::Entry e; e.text = s.text; base.set(s.key, e); }
    fs::path srcFile = g.dir / "lang" / (g.langs.source + ".json");
    if (fs::exists(srcFile)) base.merge(parseFile(srcFile));
    int errors = 0;
    for (auto& tag : g.langs.available) {
        if (!fs::exists(assets / "lang" / (tag + ".json")) && tag != "en") {
            std::printf("%s: warning: engine has no '%s' table, menus will fall back to English\n",
                        (g.dir / "game.json").string().c_str(), tag.c_str());
            ++warnings;
        }
        if (tag == g.langs.source) continue;
        fs::path f = g.dir / "lang" / (tag + ".json");
        if (!fs::exists(f)) {
            std::printf("%s: warning: missing (run: rts_i18n extract --project %s --lang %s --write)\n",
                        f.string().c_str(), g.dir.string().c_str(), tag.c_str());
            ++warnings;
            continue;
        }
        i18n::Catalog c;
        c.merge(parseFile(f));
        auto issues = i18n::check(base, g.langs.source, c, tag);
        // Keys that override engine strings are legitimate in game tables.
        std::vector<i18n::Issue> kept;
        i18n::Catalog engineEn;
        if (fs::exists(assets / "lang" / "en.json")) engineEn.merge(parseFile(assets / "lang" / "en.json"));
        for (auto& i : issues)
            if (!(i.message.find("stale key") != std::string::npos && engineEn.find(i.key))) kept.push_back(i);
        for (auto& i : kept) if (i.severity == i18n::Issue::Warning) ++warnings;
        errors += report(kept, f.string());
    }
    return errors;
}

int extract(const Game& g, const std::string& tagIn, bool write) {
    std::string tag = i18n::normalizeTag(tagIn);
    fs::path f = g.dir / "lang" / (tag + ".json");
    Json existing;
    if (fs::exists(f)) existing = parseFile(f);
    i18n::Catalog have;
    if (existing.isObject()) have.merge(existing);

    std::ostringstream o;
    o << "// " << g.manifest.getStr("name") << " — " << tag << ". Keys: see modules/rts/RtsLocalize.h.\n{\n";
    std::string self = have.meta().count("@language") ? have.meta().at("@language") : std::string();
    std::vector<std::string> lines;
    if (!self.empty()) lines.push_back("  \"@language\": " + quote(self));
    for (auto& [k, v] : have.meta()) if (k != "@language") lines.push_back("  " + quote(k) + ": " + quote(v));
    std::set<std::string> done;
    int todo = 0;
    std::string lastWhere;
    for (auto& s : g.strings) {
        std::string line;
        if (s.where != lastWhere) { line = std::string(lines.empty() ? "" : "\n") + "  // " + s.where + "\n"; lastWhere = s.where; }
        if (const i18n::Entry* e = have.find(s.key); e && !e->plural) line += "  " + quote(s.key) + ": " + quote(e->text);
        else if (e) continue;  // plural entries are written below as they are
        else { line += "  " + quote(s.key) + ": " + quote(s.text) + "  // TODO"; ++todo; }
        lines.push_back(line);
        done.insert(s.key);
    }
    bool extraHeader = false;
    for (auto& [k, e] : have.entries()) {
        if (done.count(k)) continue;
        std::string line;
        if (!extraHeader) { line = "\n  // other strings\n"; extraHeader = true; }
        line += "  " + quote(k) + ": ";
        if (e.plural) {
            line += "{ ";
            bool first = true;
            for (int p = 0; p < i18n::kPluralCount; ++p) {
                if (e.forms[p].empty()) continue;
                line += (first ? "" : ", ") + quote(i18n::pluralName(i18n::Plural(p))) + ": " + quote(e.forms[p]);
                first = false;
            }
            line += " }";
        } else {
            line += quote(e.text);
        }
        lines.push_back(line);
    }
    // Commas go before a trailing "// TODO" so the file stays valid.
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string& l = lines[i];
        if (i + 1 < lines.size()) {
            size_t c = l.rfind("  // TODO");
            if (c != std::string::npos && c + 9 == l.size()) l.insert(c, ",");
            else l += ",";
        }
        o << l << "\n";
    }
    o << "}\n";

    if (write) {
        fs::create_directories(f.parent_path());
        std::ofstream out(f, std::ios::binary | std::ios::trunc);
        out << o.str();
        std::fprintf(stderr, "%s: %zu keys, %d to translate\n", f.string().c_str(), done.size(), todo);
    } else {
        std::fputs(o.str().c_str(), stdout);
        std::fprintf(stderr, "%d to translate\n", todo);
    }
    return 0;
}

int usage() {
    std::fprintf(stderr,
                 "usage: rts_i18n check [--project DIR] [--engine DIR]\n"
                 "       rts_i18n extract --project DIR --lang TAG [--write]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1], project, lang;
#ifdef RTS_SOURCE_DIR
    fs::path assets = fs::path(RTS_SOURCE_DIR) / "engine" / "assets";
#else
    fs::path assets = "engine/assets";
#endif
    bool write = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--project" && i + 1 < argc) project = argv[++i];
        else if (a == "--engine" && i + 1 < argc) assets = argv[++i];
        else if (a == "--lang" && i + 1 < argc) lang = argv[++i];
        else if (a == "--write") write = true;
        else return usage();
    }
    try {
        if (cmd == "check") {
            int warnings = 0, errors = checkEngine(assets, warnings);
            i18n::Catalog engineEn;
            if (fs::exists(assets / "lang" / "en.json")) engineEn.merge(parseFile(assets / "lang" / "en.json"));
            errors += checkMarkup(assets / "ui", engineEn, "engine/assets/lang/en.json", warnings);
            if (!project.empty()) {   // a game's ui/ overrides may use engine keys and its own
                i18n::Catalog t = engineEn;
                Game g = loadGame(project);
                fs::path own = g.dir / "lang" / (g.langs.source + ".json");
                if (fs::exists(own)) t.merge(parseFile(own));
                errors += checkMarkup(g.dir / "ui", t, "engine en + " + own.string(), warnings);
            }
            if (!project.empty()) errors += checkGame(loadGame(project), assets, warnings);
            std::printf("%d error(s), %d warning(s)\n", errors, warnings);
            return errors ? 1 : 0;
        }
        if (cmd == "extract") {
            if (project.empty() || lang.empty()) return usage();
            return extract(loadGame(project), lang, write);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rts_i18n: %s\n", e.what());
        return 1;
    }
    return usage();
}
