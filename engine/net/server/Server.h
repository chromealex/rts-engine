// Game server: lobby rooms, automatic matchmaking, and the input relay for lockstep/rollback
// matches. It never runs the simulation and needs no game data: it confirms each tick's input,
// compares players' checksums, records replays and keeps players' ratings.
//
//   Server srv(loadServerConfig(Json::parseFile("server.json")));
//   srv.listen();                         // UDP + WebSocket ports from the config
//   for (;;) { srv.update(nowMs()); sleep ~1 ms; }
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../Conn.h"
#include "../Protocol.h"

namespace rts { class Json; }

namespace rts::net {

struct QueueConfig {
    std::string id = "1v1", title = "1 на 1";
    uint8_t teams = 2, teamSize = 1;
    RoomSettings settings;
    uint32_t botFillSec = 0;           // fill missing players with bots after this wait (0 = never)
    uint32_t ratingRange = 150;        // initial accepted rating difference
    uint32_t ratingWidenPerSec = 10;   // the range grows while players wait
};

struct ServerConfig {
    std::string name = "RTSEngine server";
    uint16_t udpPort = kDefaultUdpPort;   // 0 = disabled
    uint16_t wsPort = kDefaultWsPort;     // 0 = disabled
    std::string replayDir;                // empty = do not save replays
    std::string ratingsFile;              // empty = ratings live in memory only
    std::map<std::string, std::string> gameDirs; // game id -> folder, so saved replays point at real files
    std::vector<QueueConfig> queues;
    uint32_t maxClients = 5000;
    uint32_t reconnectGraceSec = 120;     // a dropped player keeps the slot this long
    bool log = true;
};

// Parse a server config (all keys optional):
// {"name": "...", "udpPort": 27015, "wsPort": 27016, "replays": "replays", "ratings": "ratings.txt",
//  "games": {"com.rtsengine.ironandgold": "games/skirmish"}, "reconnectGraceSec": 120,
//  "queues": [{"id": "1v1", "title": "1 на 1", "teams": 2, "teamSize": 1, "botFillSec": 60,
//              "ratingRange": 150, "ratingWidenPerSec": 10,
//              "tickRate": 20, "inputDelay": 2, "rollbackWindow": 8, "lagToleranceMs": 300}]}
ServerConfig loadServerConfig(const Json& j);

struct ServerStats {
    uint32_t clients = 0, rooms = 0, activeMatches = 0, queued = 0;
    uint32_t matchesStarted = 0, matchesFinished = 0, desyncs = 0;
};

class Server {
public:
    explicit Server(ServerConfig cfg);
    ~Server();
    bool listen(std::string* error = nullptr);
    uint16_t udpPort() const;
    uint16_t wsPort() const;
    void addConnection(std::unique_ptr<Conn> c);   // in-process clients (tests, local play)
    void update(uint64_t now);
    ServerStats stats() const;
    int rating(const std::string& queueId, uint64_t clientKey) const;

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

} // namespace rts::net
