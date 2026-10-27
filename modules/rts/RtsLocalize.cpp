#include "RtsLocalize.h"

#include <set>

namespace rts::game {

std::string mapStem(const std::string& mapFile) {
    size_t slash = mapFile.find_last_of("/\\");
    std::string s = slash == std::string::npos ? mapFile : mapFile.substr(slash + 1);
    size_t dot = s.rfind('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}

static void localize(std::string& name, const std::string& key, const i18n::Localization& loc) {
    if (name.size() > 1 && name[0] == '@') name = loc.text(name);
    else name = loc.trOr(key, name);
}

void localizeRules(Rules& rules, const i18n::Localization& loc) {
    for (auto& r : rules.resources) localize(r.name, "resource." + r.id, loc);
    for (auto& t : rules.types) {
        localize(t.name, "type." + t.id, loc);
        for (auto& a : t.abilities) localize(a.name, "ability." + a.id, loc);
    }
    for (auto& t : rules.techs) localize(t.name, "tech." + t.id, loc);
    for (auto& f : rules.factions) localize(f.name, "faction." + f.id, loc);
}

std::string localizedPlayerName(const i18n::Localization& loc, const std::string& mapFile, size_t index,
                                const std::string& dataName) {
    std::string key = "map." + mapStem(mapFile) + ".player." + std::to_string(index + 1);
    if (dataName.size() > 1 && dataName[0] == '@') return loc.text(dataName);
    return loc.trOr(key, dataName);
}

std::vector<SourceString> collectSourceStrings(const Json& manifest, const Json& rules,
                                               const std::vector<std::pair<std::string, Json>>& maps) {
    std::vector<SourceString> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& key, const std::string& text, const std::string& where) {
        if (text.empty() || text[0] == '@') return;   // explicit keys are translated where they are defined
        if (seen.insert(key).second) out.push_back({key, text, where});
    };
    add("game.name", manifest.getStr("name"), "game.json");
    for (auto& m : manifest["maps"].items()) {
        std::string file = m.getStr("file");
        add("map." + mapStem(file) + ".title", m.getStr("title"), "game.json");
    }
    for (auto& [file, map] : maps) {
        std::string stem = mapStem(file);
        add("map." + stem + ".name", map.getStr("name"), file);
        size_t i = 0;
        for (auto& p : map["players"].items()) {
            ++i;
            add("map." + stem + ".player." + std::to_string(i), p.getStr("name"), file);
        }
    }
    // From JSON (no need for valid rules); a missing "name" shows the id, so the id is the source text.
    auto section = [&](const char* block, const char* prefix) {
        for (auto& [id, j] : rules[block].members()) add(std::string(prefix) + id, j.getStr("name", id), std::string("rules.json ") + block);
    };
    section("resources", "resource.");
    section("types", "type.");
    for (auto& [tid, t] : rules["types"].members()) {
        const Json& ab = t["abilities"];
        for (auto& a : ab.items()) {
            std::string id = a.getStr("id", "ability");
            add("ability." + id, a.getStr("name", id), "rules.json types." + tid);
        }
    }
    section("techs", "tech.");
    section("factions", "faction.");
    return out;
}

} // namespace rts::game
