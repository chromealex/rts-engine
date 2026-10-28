// Network bot client: plays matches on a real server with the built-in AI. Doubles as a load test
// and as a way to watch bots play through the whole network stack.
//   rts_netbot --server HOST[:PORT] | ws://HOST:PORT  --project games/skirmish
//              [--queue 1v1 | --create NAME [--map maps/x.json] [--bots N] | --join ROOM | --spectate ROOM]
//              [--clients N] [--name Bot] [--matches N] [--quiet] [--engine-assets engine/assets]
// Messages are printed in the system language (RTS_LANG=ru to force one), like the game does.
#include <chrono>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "apps/server/GameData.h"
#include "engine/i18n/GameLanguage.h"
#include "engine/net/NetClient.h"
#include "engine/net/NetLocalize.h"
#include "modules/rts/RtsAI.h"
#include "modules/rts/RtsModule.h"

using namespace rts;

struct Args {
    std::string server = "127.0.0.1", project = "games/skirmish", name = "Bot", queue, create, map, engineAssets = "engine/assets";
    uint32_t join = 0, spectate = 0, clients = 1, matches = 1, bots = 0;
    bool quiet = false;
};

struct BotClient {
    std::string name;
    std::unique_ptr<Simulation> sim;            // must outlive the session inside `net`
    std::unique_ptr<net::NetClient> net;
    std::vector<game::RtsAI> ai;
    bool requested = false, roomSetUp = false, started = false;
    uint32_t matchesDone = 0;
    uint64_t lastStatus = 0;
};

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) std::exit(2); return argv[++i]; };
        if (k == "--server") a.server = next();
        else if (k == "--project") a.project = next();
        else if (k == "--name") a.name = next();
        else if (k == "--queue") a.queue = next();
        else if (k == "--create") a.create = next();
        else if (k == "--map") a.map = next();
        else if (k == "--bots") a.bots = uint32_t(std::stoul(next()));
        else if (k == "--join") a.join = uint32_t(std::stoul(next()));
        else if (k == "--spectate") a.spectate = uint32_t(std::stoul(next()));
        else if (k == "--clients") a.clients = uint32_t(std::stoul(next()));
        else if (k == "--matches") a.matches = uint32_t(std::stoul(next()));
        else if (k == "--quiet") a.quiet = true;
        else if (k == "--engine-assets") a.engineAssets = next();
        else { std::printf("see the header of apps/netbot/main.cpp for usage\n"); return 2; }
    }
    if (a.queue.empty() && a.create.empty() && !a.join && !a.spectate) a.queue = "1v1";
    Json manifest;
    std::vector<net::MapInfo> maps;
    try {
        manifest = Json::parseFile(a.project + "/game.json");
        maps = net::describeGameMaps(a.project);
    } catch (const std::exception& e) { std::fprintf(stderr, "error: %s\n", e.what()); return 2; }
    const std::string gameId = manifest.getStr("id");
    // Packaged layout: "engine/..." from the engine assets, everything else from the game folder.
    i18n::FileReader reader = [&](const std::string& rel, std::string& out) {
        const std::string path = rel.rfind("engine/", 0) == 0 ? a.engineAssets + "/" + rel.substr(7) : a.project + "/" + rel;
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::stringstream ss; ss << f.rdbuf(); out = ss.str();
        return true;
    };
    i18n::Localization loc;
    try {
        const auto langs = i18n::readGameLanguages(manifest);
        i18n::loadGameLocalization(loc, reader, langs, i18n::chooseLanguage(langs, ""));
    } catch (const std::exception& e) { std::fprintf(stderr, "localization: %s\n", e.what()); }
    net::MapInfo chosen = maps.empty() ? net::MapInfo{} : maps[0];
    if (!a.map.empty()) {
        try { chosen = net::describeMap(a.project, a.map, manifest.getStr("rules", "rules.json")); }
        catch (const std::exception& e) { std::fprintf(stderr, "error: %s\n", e.what()); return 2; }
    }

    std::mt19937_64 rng{std::random_device{}()};
    std::vector<BotClient> bots(a.clients);
    for (size_t i = 0; i < bots.size(); ++i) {
        bots[i].name = a.clients > 1 ? a.name + std::to_string(i + 1) : a.name;
        bots[i].net = std::make_unique<net::NetClient>(net::connectTo(a.server), net::ClientIdentity{bots[i].name, gameId, rng()});
        if (!bots[i].net || bots[i].net->phase() == net::NetClient::Phase::Disconnected) { std::fprintf(stderr, "bad server address %s\n", a.server.c_str()); return 2; }
    }
    auto say = [&](BotClient& b, const std::string& s) { if (!a.quiet) std::printf("[%s] %s\n", b.name.c_str(), s.c_str()); std::fflush(stdout); };

    uint64_t last = net::nowMs();
    int exitCode = 0;
    for (;;) {
        const uint64_t now = net::nowMs();
        const double dt = double(now - last) / 1000.0;
        last = now;
        bool allDone = true;
        for (auto& b : bots) {
            if (b.matchesDone >= a.matches) continue;
            allDone = false;
            b.net->update(now);
            for (auto& e : b.net->takeEvents()) {
                using K = net::NetEvent::Kind;
                const std::string msg = net::translate(loc, e.text);
                if (e.kind == K::Connected || e.kind == K::Chat || e.kind == K::PlayerStatus || e.kind == K::RatingChanged) say(b, msg);
                else if (e.kind == K::Disconnected) { say(b, msg); b.matchesDone = a.matches; exitCode = 1; }
                else if (e.kind == K::Error) say(b, "error: " + msg);
                else if (e.kind == K::Desync) { say(b, "DESYNC: " + msg); exitCode = 3; }
                else if (e.kind == K::MatchEnded) {
                    if (!msg.empty()) say(b, msg);
                    say(b, "match over, winner team " + std::to_string(e.value));
                    ++b.matchesDone;
                    b.requested = false; b.started = false;
                }
            }
            if (b.net->phase() != net::NetClient::Phase::Online) continue;
            if (!b.requested && !b.net->inMatch()) {
                b.requested = true;
                if (!a.queue.empty()) b.net->joinQueue(a.queue, maps);
                else if (!a.create.empty()) b.net->createRoom(a.create, chosen);
                else if (a.join) b.net->joinRoom(a.join, chosen.dataHash, false);
                else b.net->joinRoom(a.spectate, chosen.dataHash, true);
            }
            // Room host: add bots, wait until everyone is ready, start.
            if (!a.create.empty() && b.net->isHost() && b.net->room() && b.net->room()->phase == net::RoomPhase::Lobby && !b.started) {
                auto& room = *b.net->room();
                if (!b.roomSetUp) {
                    for (uint32_t k = 0, slot = 2; k < a.bots && slot <= room.slots.size(); ++slot)
                        if (room.slots[slot - 1].kind == net::SlotKind::Open) { b.net->setSlotKind(uint8_t(slot), net::SlotKind::Bot); ++k; }
                    b.roomSetUp = true;
                    say(b, "room #" + std::to_string(room.id) + " created, waiting for players");
                }
                bool full = true, ready = true;
                for (auto& s : room.slots) {
                    full &= s.kind != net::SlotKind::Open;
                    if (s.kind == net::SlotKind::Human && s.clientId != b.net->clientId()) ready &= s.ready;
                }
                if (full && ready) { b.net->startMatch(); b.started = true; }
            }
            if (!a.create.empty() || a.join) {
                // Joined players declare themselves ready once seated.
                if (b.net->room() && b.net->room()->phase == net::RoomPhase::Lobby && !b.net->isHost())
                    for (auto& s : b.net->room()->slots) if (s.clientId == b.net->clientId() && !s.ready) b.net->setReady(true);
            }
            if (b.net->matchReady()) {
                auto& m = b.net->match();
                b.sim.reset(); // the old session was already dropped by MatchStart
                auto data = netgame::loadMatchData(a.project, m.map.file);   // tile maps: same as the game
                b.sim = netgame::makeSimulation(data.module, b.net->simConfig());
                b.sim->setup(data.rules, data.map);
                b.net->attach(*b.sim);
                b.ai.clear();
                for (uint8_t s : m.controlled) b.ai.emplace_back(s, rng());
                say(b, std::string(m.resumed ? "resumed" : "match started") + " on " + loc.text(m.map.title) + ", slot " + std::to_string(m.localSlot) +
                           (m.controlled.size() > 1 ? " (+" + std::to_string(m.controlled.size() - 1) + " bot)" : ""));
            }
            if (auto* s = b.net->session(); s && b.sim) {
                if (s->controlled().size() != b.ai.size()) { b.ai.clear(); for (uint8_t sl : s->controlled()) b.ai.emplace_back(sl, rng()); }
                if (!s->confirmedResult()) for (auto& ai : b.ai) ai.update(*b.sim);
                b.net->advance(dt);
                if (now - b.lastStatus > 30000 && !a.quiet) {
                    b.lastStatus = now;
                    auto& st = s->stats();
                    std::printf("[%s] t=%us confirmed=%u rtt=%ums rollbacks=%u (max %u) stalls=%u\n", b.name.c_str(), s->tick() / b.sim->tickRate(),
                                s->confirmedNext(), b.net->rttMs(), st.rollbacks, st.maxRollback, st.stallUpdates);
                }
            }
        }
        if (allDone) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return exitCode;
}
