// Translating NetText with the game's Localization. Header-only so engine/net itself stays free of
// engine/i18n: only code that includes this (the client UI, tools) must link rts_i18n.
#pragma once
#include "NetText.h"
#include "../i18n/Localization.h"

namespace rts::net {

// String arguments are data from the server: a queue title from its config ("@net.queue.2v2"),
// a bot/unnamed player ("@net.bot", "@net.player.unnamed", filled from the same arguments) or a
// player's own text (escaped by the server, so "@@x" shows "@x"). loc.text() handles all three.
inline i18n::Args toArgs(const i18n::Localization& loc, const NetText& t) {
    i18n::Args raw, a;
    for (auto& x : t.args) { if (x.isNum) raw.num(x.name, x.n); else raw.str(x.name, x.text); }
    for (auto& x : t.args) { if (x.isNum) a.num(x.name, x.n); else a.str(x.name, loc.text(x.text, raw)); }
    return a;
}

inline std::string translate(const i18n::Localization& loc, const NetText& t) { return t.empty() ? std::string() : loc.tr(t.key, toArgs(loc, t)); }

// Names that come from the server: a bot ("@net.bot" with its slot), an unnamed player, or a
// player's own name (shown as is). Room and queue titles from the server config may be "@keys".
inline std::string displayName(const i18n::Localization& loc, const std::string& name, uint8_t slot) {
    if (name.empty()) return loc.tr("net.player.unnamed", i18n::Args().num("slot", slot));
    if (name == "@net.bot") return loc.tr("net.bot", i18n::Args().num("slot", slot));
    return loc.text(name);
}

} // namespace rts::net
