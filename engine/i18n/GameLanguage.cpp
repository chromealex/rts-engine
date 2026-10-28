#include "GameLanguage.h"
#include "../data/Json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace rts::i18n {

const std::vector<std::string> kDefaultLangDirs = {"engine/lang", "lang"};

GameLanguages readGameLanguages(const Json& manifest) {
    GameLanguages g;
    std::string src = normalizeTag(manifest.getStr("sourceLanguage", "en"));
    if (!src.empty()) g.source = src;
    for (auto& l : manifest["languages"].items()) {
        std::string t = normalizeTag(l.str());
        if (!t.empty() && std::find(g.available.begin(), g.available.end(), t) == g.available.end()) g.available.push_back(t);
    }
    if (g.available.empty()) g.available.push_back(g.source);
    return g;
}

std::string chooseLanguage(const GameLanguages& g, const std::string& saved, const std::vector<std::string>& system) {
    std::string s = normalizeTag(saved);
    for (auto& a : g.available) if (a == s) return a;
    std::string fallback = std::find(g.available.begin(), g.available.end(), g.source) != g.available.end()
                               ? g.source : g.available.front();
    return negotiate(system, g.available, fallback);
}

void loadGameLocalization(Localization& loc, const FileReader& read, const GameLanguages& g, const std::string& language) {
    // "qps" (pseudo-locale): source texts, visibly transformed — finds strings that bypass tr().
    bool pseudo = normalizeTag(language) == "qps";
    loc.load(read, pseudo ? g.source : language, kDefaultLangDirs, {g.source, "en"});
    loc.setPseudo(pseudo);
}

std::string languageSelfName(const FileReader& read, const std::string& tag) {
    for (auto& dir : kDefaultLangDirs) {
        std::string text;
        if (!read(dir + "/" + tag + ".json", text)) continue;
        try {
            std::string n = Json::parse(text).getStr("@language");
            if (!n.empty()) return n;
        } catch (const std::exception&) {}
    }
    return tag;
}

std::string readSavedLanguage(const std::string& userDir) {
    std::ifstream f(userDir + "/language.txt");
    std::string tag;
    if (f) std::getline(f, tag);
    return normalizeTag(tag);
}

bool saveLanguage(const std::string& userDir, const std::string& tag) {
    std::error_code ec;   // first run: the per-user directory may not exist yet
    std::filesystem::create_directories(userDir, ec);
    std::ofstream f(userDir + "/language.txt", std::ios::trunc);
    if (!f) return false;
    f << normalizeTag(tag) << "\n";
    return bool(f);
}

} // namespace rts::i18n
