// Which languages a game ships, which one to use, and loading the tables for it.
//
// game.json:
//   "sourceLanguage": "en",          // language of names written directly in rules/maps (default "en")
//   "languages": ["en", "ru"],       // languages offered to the player (default: just the source)
// Tables: <content>/engine/lang/<tag>.json (engine/UI strings), then <content>/lang/<tag>.json
// (the game's own strings; may override engine ones).
#pragma once
#include "Localization.h"

namespace rts::i18n {

struct GameLanguages {
    std::string source = "en";
    std::vector<std::string> available;
};

GameLanguages readGameLanguages(const Json& manifest);

// Saved choice if it is still offered, else the best match for the OS languages, else the source.
std::string chooseLanguage(const GameLanguages& g, const std::string& saved,
                           const std::vector<std::string>& system = systemLanguages());

extern const std::vector<std::string> kDefaultLangDirs;   // {"engine/lang", "lang"}

// "qps" loads the source language with pseudo-localization on.
void loadGameLocalization(Localization& loc, const FileReader& read, const GameLanguages& g, const std::string& language);

// Name of a language in itself ("Русский") from the "@language" metadata of its tables; the tag
// when no table names it.
std::string languageSelfName(const FileReader& read, const std::string& tag);

// The player's choice persists in <userDataDir>/language.txt.
std::string readSavedLanguage(const std::string& userDir);
bool saveLanguage(const std::string& userDir, const std::string& tag);

} // namespace rts::i18n
