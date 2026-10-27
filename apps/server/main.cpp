// Dedicated game server: lobby, matchmaking and the input relay for network matches.
//   rts_server [--config server.json] [--udp-port N] [--ws-port N] [--replays DIR] [--ratings FILE] [--quiet]
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "engine/data/Json.h"
#include "engine/net/server/Server.h"

using namespace rts;

static std::atomic<bool> g_stop{false};

int main(int argc, char** argv) {
    std::string configPath;
    net::ServerConfig cfg;
    bool haveConfig = false;
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--config" && i + 1 < argc) { configPath = argv[++i]; haveConfig = true; }
    try {
        if (haveConfig) cfg = net::loadServerConfig(Json::parseFile(configPath));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "config: %s\n", e.what());
        return 2;
    }
    if (cfg.queues.empty()) cfg.queues.push_back({});   // a default 1v1 queue
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", k.c_str()); std::exit(2); } return argv[++i]; };
        if (k == "--config") ++i;
        else if (k == "--udp-port") cfg.udpPort = uint16_t(std::stoi(next()));
        else if (k == "--ws-port") cfg.wsPort = uint16_t(std::stoi(next()));
        else if (k == "--replays") cfg.replayDir = next();
        else if (k == "--ratings") cfg.ratingsFile = next();
        else if (k == "--quiet") cfg.log = false;
        else {
            std::printf("usage: rts_server [--config server.json] [--udp-port N] [--ws-port N] [--replays DIR] [--ratings FILE] [--quiet]\n");
            return 2;
        }
    }
    std::signal(SIGINT, [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });

    net::Server server(cfg);
    std::string err;
    if (!server.listen(&err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    std::printf("%s: udp %u, websocket %u, %zu queue(s)%s%s\n", cfg.name.c_str(), server.udpPort(), server.wsPort(), cfg.queues.size(),
                cfg.replayDir.empty() ? "" : ", replays in ", cfg.replayDir.c_str());
    std::fflush(stdout);

    uint64_t lastStats = net::nowMs();
    while (!g_stop) {
        const uint64_t now = net::nowMs();
        server.update(now);
        if (now - lastStats >= 60000) {
            lastStats = now;
            auto s = server.stats();
            std::printf("stats: %u clients, %u rooms, %u matches running, %u queued, %u started, %u finished, %u desyncs\n", s.clients, s.rooms,
                        s.activeMatches, s.queued, s.matchesStarted, s.matchesFinished, s.desyncs);
            std::fflush(stdout);
        }
        // 1 ms keeps relay latency negligible next to the 50 ms tick; an idle server costs ~nothing.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("shutting down\n");
    return 0;
}
