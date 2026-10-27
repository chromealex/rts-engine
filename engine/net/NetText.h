// User-facing text produced by the network layer: a localization key plus named arguments.
// The server talks to players with different languages, so it never sends finished sentences;
// the client translates (see NetLocalize.h). engine/net does not depend on engine/i18n.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rts::net {

struct NetText {
    struct Arg { std::string name, text; int64_t n = 0; bool isNum = false; };
    std::string key;                 // "net.error.room_full"; empty = no text
    std::vector<Arg> args;

    NetText() = default;
    explicit NetText(std::string k) : key(std::move(k)) {}
    // value: plain text, or "@key" that the client translates too (NetLocalize.h).
    NetText& str(std::string name, std::string value) { args.push_back({std::move(name), std::move(value), 0, false}); return *this; }
    NetText& num(std::string name, int64_t value) { args.push_back({std::move(name), std::to_string(value), value, true}); return *this; }
    bool empty() const { return key.empty(); }
    bool operator==(const NetText&) const = default;
};
inline bool operator==(const NetText::Arg& a, const NetText::Arg& b) { return a.name == b.name && a.text == b.text && a.n == b.n && a.isNum == b.isNum; }

// Every key the network code can produce. Each must exist in engine/assets/lang/en.json and every
// other engine table (rts_net_tests checks it).
inline const std::vector<std::string>& netTextKeys() {
    static const std::vector<std::string> keys = {
        "net.connected", "net.disconnected", "net.chat_line", "net.desync", "net.rating_up", "net.rating_down",
        "net.player.back", "net.player.dropped", "net.player.left", "net.player.unnamed", "net.bot",
        "net.end.opponents_left", "net.end.everyone_left", "net.end.result_mismatch",
        "net.error.version", "net.error.bad_request", "net.error.no_such_room", "net.error.data_mismatch",
        "net.error.in_game", "net.error.no_spectators", "net.error.not_host", "net.error.slot_taken",
        "net.error.not_ready", "net.error.no_such_queue", "net.error.no_maps_for_queue",
        "net.queue.default",
    };
    return keys;
}

} // namespace rts::net
