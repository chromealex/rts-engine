// Server state shared by the lobby, match and matchmaking parts.
#pragma once
#include <cstdarg>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>
#include "Server.h"
#ifndef __EMSCRIPTEN__
#include "../Udp.h"
#include "../WebSocket.h"
#endif

namespace rts::net {

struct Client {
    uint32_t id = 0;
    std::unique_ptr<Conn> conn;           // null while a dropped player may still resume
    bool hello = false;
    std::string name, gameId;
    uint64_t key = 0, token = 0;
    uint32_t roomId = 0;
    uint64_t connectedAt = 0, droppedAt = 0;
    // matchmaking
    std::string queueId;
    uint64_t queuedAt = 0;
    std::vector<MapInfo> queueMaps;
};

struct Member {                            // a client taking part in a running match
    uint32_t clientId = 0;
    uint8_t slot = 0;                     // 0 = spectator
    std::vector<uint8_t> slots;           // slots whose commands it may send
    uint32_t nextFrame = 0;               // first tick we have no input frame for yet
    std::map<uint32_t, std::vector<Command>> frames;
    bool connected = true, left = false, synced = true;
    uint32_t sent = 0;                    // next confirmed tick to send to this member
    int32_t result = 0;
    uint32_t resultTick = 0;
    uint64_t resultHash = 0;
    bool reported = false;
};

struct Match {
    uint64_t seed = 0;
    uint64_t startMs = 0;
    std::vector<MatchPlayer> players;
    std::vector<Member> members;
    uint32_t confirmedNext = 0;
    std::vector<Command> history;         // every confirmed command, in order
    std::vector<Command> carry;           // late commands for the next confirmed tick
    std::map<uint32_t, std::map<uint8_t, uint64_t>> checks;
    std::map<uint32_t, uint64_t> agreed;  // checksums every player agreed on (replay checkpoints)
    bool desync = false;
    uint64_t lastBroadcast = 0;
};

struct Room {
    uint32_t id = 0;
    std::string name, gameId;
    uint32_t hostId = 0;
    MapInfo map;
    RoomSettings settings;
    RoomPhase phase = RoomPhase::Lobby;
    bool matchmade = false;
    std::string queueId;
    std::vector<SlotEntry> slots;
    std::vector<uint32_t> spectators;
    std::unique_ptr<Match> match;
};

struct Rating { int value = 1000; int games = 0; };

struct Server::Impl {
    ServerConfig cfg;
#ifndef __EMSCRIPTEN__
    UdpListener udp;
    WsListener ws;
#endif
    bool listening = false;
    std::map<uint32_t, std::unique_ptr<Client>> clients;
    std::map<uint32_t, std::unique_ptr<Room>> rooms;
    std::map<std::string, Rating> ratings;
    uint32_t nextClientId = 1, nextRoomId = 1;
    uint64_t now = 0, lastMatchmake = 0, lastQueueStatus = 0;
    std::mt19937_64 rng{std::random_device{}()};
    ServerStats counters;

    // log
    void log(const char* fmt, ...) {
        if (!cfg.log) return;
        va_list a; va_start(a, fmt);
        std::fprintf(stdout, "[%8.1f] ", double(now) / 1000.0);
        std::vfprintf(stdout, fmt, a);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        va_end(a);
    }

    // connections (Server.cpp)
    Client* client(uint32_t id) { auto it = clients.find(id); return it == clients.end() ? nullptr : it->second.get(); }
    Room* room(uint32_t id) { auto it = rooms.find(id); return it == rooms.end() ? nullptr : it->second.get(); }
    void send(Client& c, const Bytes& b) { if (c.conn) c.conn->send(b); }
    void sendError(Client& c, ErrorCode code, const std::string& text) { ErrorMsg e; e.code = code; e.text = text; send(c, encode(e)); }
    void handle(Client& c, const Bytes& msg);
    void onHello(Client& c, ByteReader& r);
    void onDisconnect(Client& c);
    void dropClient(uint32_t id);

    // lobby (Server.cpp)
    RoomStateMsg roomState(const Room& r) const;
    void broadcastRoom(Room& r);
    void sendRoomList(Client& c, const std::string& gameId);
    Room& createRoom(const std::string& name, const std::string& gameId, const MapInfo& map, const RoomSettings& s);
    void joinRoom(Client& c, Room& r, bool spectator);
    void leaveRoom(Client& c);
    void sweepRooms();
    bool roomHasClient(const Room& r, uint32_t id) const;

    // match (ServerMatch.cpp)
    void startMatch(Room& r);
    void sendMatchStart(Room& r, Member& m, bool resumed);
    void matchUpdate(Room& r);
    void onInputFrame(Client& c, Room& r, ByteReader& rd);
    void onChecksum(Client& c, Room& r, ByteReader& rd);
    void onMatchResult(Client& c, Room& r, ByteReader& rd);
    void reportDesync(Room& r, uint32_t tick, const std::map<uint8_t, uint64_t>& hashes);
    void memberDropped(Room& r, uint32_t clientId, bool left);
    void memberResumed(Room& r, Client& c);
    void reassignBots(Room& r);
    void endMatch(Room& r, int32_t result, const std::string& reason);
    void saveReplay(Room& r, uint32_t endTick, uint64_t endHash);
    Member* member(Room& r, uint32_t clientId);

    // matchmaking (Matchmaker.cpp)
    const QueueConfig* queue(const std::string& id) const;
    void onQueueJoin(Client& c, ByteReader& rd);
    void leaveQueue(Client& c);
    void matchmake();
    void sendQueueStatus(Client& c, uint8_t state);
    std::string ratingKey(const std::string& queueId, uint64_t key) const;
    Rating& rating(const std::string& queueId, uint64_t key);
    void loadRatings();
    void saveRatings();
};

} // namespace rts::net
