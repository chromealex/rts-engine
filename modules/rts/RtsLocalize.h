// Localized names for RTS game data. Presentation only: apply to the client's display copy of
// Rules, never to the Rules the simulation runs on (names are not hashed, but keep the sim clean).
//
// Key scheme (no changes to rules.json needed; the "name" in data is the source-language text):
//   game.name                    game.json "name"
//   map.<stem>.title             game.json maps[].title   (stem: "maps/valley.json" -> "valley")
//   map.<stem>.name              map "name"
//   map.<stem>.player.<n>        map players[n-1].name
//   resource.<id> | type.<id> | tech.<id> | faction.<id> | ability.<id>
// A data name written as "@some.key" is looked up under that key instead.
#pragma once
#include "RtsData.h"
#include "engine/i18n/Localization.h"

#include <utility>

namespace rts::game {

std::string mapStem(const std::string& mapFile);

// Replaces every name in `rules` with its translation (or resolves "@key" names).
void localizeRules(Rules& rules, const i18n::Localization& loc);

std::string localizedPlayerName(const i18n::Localization& loc, const std::string& mapFile, size_t index,
                                const std::string& dataName);

// Every translatable data string of a game as (key, source text), in a stable order: game.json,
// maps, then rules. Used by the rts_i18n tool to create/update tables.
struct SourceString { std::string key, text, where; };
std::vector<SourceString> collectSourceStrings(const Json& manifest, const Json& rules,
                                               const std::vector<std::pair<std::string, Json>>& maps);

} // namespace rts::game
