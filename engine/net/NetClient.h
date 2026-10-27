// Game-side network client: connection, lobby, matchmaking queue, and the match itself.
//
// Typical use (the game owns the Simulation; this class never loads content):
//   NetClient net(connectTo("play.example.com"), {"Alex", "com.rtsengine.ironandgold", key});
//   each frame: net.update(nowMs());
//   lobby:      net.rooms(), net.createRoom(...), net.joinRoom(...), net.joinQueue("1v1", maps) ...
//   match:      if (net.matchReady()) { build sim from net.match().map.file, sim.setup(...);
//                                       net.attach(*sim); }
//               each frame: net.advance(dt)   (instead of sim.advance(dt))
//               the UI keeps calling sim.submit(cmd) with cmd.player = net.match().localSlot.
#pragma once
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "Conn.h"
#include "Protocol.h"
#include "Rollback.h"

namespace rts::net {

// Describe a map of a local game folder for the lobby/queue (reads the map's player slots and
// hashes rules + map exactly like replays do).
MapInfo describeMap(const std::string& gameDir, const std::string& mapFile, const std::string& rulesFile = "rules.json",
                    const std::string& title = "");
// Every map listed in the game's game.json.
std::vector<MapInfo> describeGameMaps(const std::string& gameDir);

struct ClientIdentity {
    std::string name;
    std::string gameId;        // game.json "id": rooms and queues only show matching games
    uint64_t clientKey = 0;    // stable per install; keys the matchmaking rating
};

struct NetEvent {
    enum class Kind { Connected, Disconnected, Error, Chat, RoomChanged, QueueChanged, MatchStarted, PlayerStatus, Desync, MatchEnded, ControlChanged };
    Kind kind;
    std::string text;
    int32_t value = 0;   // error code, result, player slot ...
};

class NetClient {
public:
    enum class Phase { Connecting, Online, Disconnected };

    NetClient(std::unique_ptr<Conn> conn, ClientIdentity id, uint64_t reconnectToken = 0);
    ~NetClient();

    void update(uint64_t now);
    std::vector<NetEvent> takeEvents();

    Phase phase() const { return phase_; }
    uint32_t clientId() const { return clientId_; }
    uint64_t reconnectToken() const { return reconnectToken_; }   // keep it to resume after a drop
    uint32_t rttMs() const { return conn_ ? conn_->rttMs() : 0; }
    const std::string& serverName() const { return serverName_; }

    // ---- lobby
    const std::vector<QueueInfo>& queues() const { return queues_; }
    const std::vector<RoomSummary>& rooms() const { return rooms_; }
    const std::optional<RoomStateMsg>& room() const { return room_; }
    bool isHost() const { return room_ && room_->hostId == clientId_; }
    const QueueStatusMsg& queueStatus() const { return queue_; }

    void refreshRooms();
    void createRoom(const std::string& name, const MapInfo& map, const RoomSettings& settings = {});
    void joinRoom(uint32_t roomId, uint64_t myDataHash, bool spectator = false);
    void leaveRoom();
    void setMap(const MapInfo& map);                 // host
    void setSettings(const RoomSettings& s);         // host
    void takeSlot(uint8_t slot);                     // 0 = become a spectator
    void setSlotKind(uint8_t slot, SlotKind kind);   // host: open / bot / closed
    void setReady(bool ready);
    void startMatch();                               // host
    void chat(const std::string& text);
    void joinQueue(const std::string& queueId, const std::vector<MapInfo>& maps);
    void leaveQueue();

    // ---- match
    bool matchReady() const { return pendingMatch_ && !session_; }   // MatchStart received, sim not attached yet
    bool inMatch() const { return bool(match_); }
    const MatchStartMsg& match() const { return *match_; }
    // Sets player names from the lobby and starts driving the sim. The sim must be freshly set up
    // from match().map with simConfig(), and must outlive the attachment: destroy the NetClient
    // or call leaveMatch() before destroying the Simulation.
    void attach(Simulation& sim);
    int advance(double seconds);
    void leaveMatch();                               // also detaches
    RollbackSession* session() { return session_.get(); }
    // SimConfig matching the match (seed, tick rate, input delay).
    SimConfig simConfig() const;

private:
    void handle(const Bytes& msg);
    void send(const Bytes& b) { if (conn_) conn_->send(b); }
    void event(NetEvent::Kind k, std::string text = {}, int32_t v = 0) { events_.push_back({k, std::move(text), v}); }

    std::unique_ptr<Conn> conn_;
    ClientIdentity id_;
    Phase phase_ = Phase::Connecting;
    bool helloSent_ = false;
    uint32_t clientId_ = 0;
    uint64_t reconnectToken_ = 0;
    std::string serverName_;
    std::vector<QueueInfo> queues_;
    std::vector<RoomSummary> rooms_;
    std::optional<RoomStateMsg> room_;
    QueueStatusMsg queue_;
    std::deque<NetEvent> events_;

    std::optional<MatchStartMsg> match_;
    bool pendingMatch_ = false;
    std::vector<TickBatchMsg> early_;               // confirmed input that arrived before attach()
    Simulation* sim_ = nullptr;
    std::unique_ptr<RollbackSession> session_;
    bool resultSent_ = false;
    uint64_t now_ = 0, lastPing_ = 0;
};

} // namespace rts::net
