// Client <-> server messages. Every message is [u8 type][fields], little-endian, built on the same
// ByteWriter/ByteReader as replays. Bump kProtocolVersion on any incompatible change.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Conn.h"
#include "NetText.h"
#include "../sim/Command.h"

namespace rts::net {

constexpr uint32_t kProtocolVersion = 2; // 2: texts are localization keys (NetText)

enum class Msg : uint8_t {
    // client -> server
    Hello = 1, ListRooms, CreateRoom, JoinRoom, LeaveRoom, SetMap, SetSettings, TakeSlot, SetSlotKind,
    SetReady, StartMatch, Chat, QueueJoin, QueueLeave, InputFrame, Checksum, MatchResult, Ping,
    // server -> client
    Welcome = 64, Error, RoomList, RoomState, QueueStatus, MatchStart, TickBatch, SlotControl,
    PlayerStatus, Desync, MatchEnd, ChatMsg, Pong,
};

enum class ErrorCode : uint16_t {
    None = 0, Version, BadRequest, NoSuchRoom, RoomFull, DataMismatch, NotHost, NotReady, InGame, NoSuchQueue, ServerFull,
};

// A playable map as both sides see it. dataHash = hashFileContents({rules, map}): players can only
// meet on byte-identical game data, otherwise the lockstep simulation would desync.
struct MapInfo {
    std::string file;               // relative to the game folder, e.g. "maps/valley.json"
    std::string title;
    uint64_t dataHash = 0;
    std::vector<uint8_t> slotTeams; // team of each player slot (index 0 = slot 1)
    uint8_t players() const { return uint8_t(slotTeams.size()); }
};

struct RoomSettings {
    uint32_t tickRate = 20;
    uint32_t inputDelay = 2;          // ticks between issuing a command and its execution
    uint32_t rollbackWindow = 8;      // ticks a client may predict ahead of confirmation (0 = pure lockstep)
    uint32_t lagToleranceMs = 300;    // the server waits this long past a tick's time for late inputs
    uint32_t checksumInterval = 20;   // ticks between desync checks
    uint32_t spectatorDelayTicks = 60;
    bool allowSpectators = true;
};

enum class SlotKind : uint8_t { Open = 0, Human = 1, Bot = 2, Closed = 3 };
enum class RoomPhase : uint8_t { Lobby = 0, InMatch = 1, Finished = 2 };

struct SlotEntry { SlotKind kind = SlotKind::Open; uint32_t clientId = 0; std::string name; bool ready = false; bool connected = true; };
struct Spectator { uint32_t clientId = 0; std::string name; };

struct RoomSummary {
    uint32_t id = 0;
    std::string name, gameId, mapFile, mapTitle;
    uint64_t dataHash = 0;
    uint8_t occupied = 0, slots = 0;
    RoomPhase phase = RoomPhase::Lobby;
};

struct RoomStateMsg {
    uint32_t id = 0, hostId = 0;
    std::string name, gameId;
    MapInfo map;
    RoomSettings settings;
    RoomPhase phase = RoomPhase::Lobby;
    bool matchmade = false;
    std::vector<SlotEntry> slots;   // one per map slot
    std::vector<Spectator> spectators;
};

struct QueueInfo { std::string id, title; uint8_t teams = 2, teamSize = 1; bool botFill = false; };

struct HelloMsg { uint32_t protocol = kProtocolVersion; std::string name, gameId; uint64_t clientKey = 0, reconnectToken = 0; };
struct WelcomeMsg { uint32_t clientId = 0; uint64_t reconnectToken = 0; std::string serverName; bool resumed = false; std::vector<QueueInfo> queues; };
struct ErrorMsg { ErrorCode code = ErrorCode::None; NetText text; };

struct QueueStatusMsg { uint8_t state = 0; std::string queueId; uint32_t waitedSec = 0, inQueue = 0; }; // state: 0 idle, 1 searching, 2 found

struct MatchPlayer { uint8_t slot = 0, team = 0; SlotKind kind = SlotKind::Human; uint32_t clientId = 0; std::string name; };
struct MatchStartMsg {
    uint32_t roomId = 0;
    uint64_t seed = 0;
    RoomSettings settings;
    MapInfo map;
    std::string gameId;
    std::vector<MatchPlayer> players;
    uint8_t localSlot = 0;                // 0 = spectator
    std::vector<uint8_t> controlled;      // slots whose commands this client sends (own + hosted bots)
    bool resumed = false;                 // reconnect / late spectator: history follows in TickBatch
};

// Confirmed input for ticks [first, first + count). Commands carry their tick; empty ticks are implicit.
struct TickBatchMsg { uint32_t serverTick = 0, first = 0, count = 0; std::vector<Command> commands; };
struct InputFrameMsg { uint32_t tick = 0; std::vector<Command> commands; };
struct ChecksumMsg { uint32_t tick = 0; uint64_t hash = 0; };
struct MatchResultMsg { uint32_t tick = 0; int32_t result = 0; uint64_t hash = 0; };
struct PlayerStatusMsg { uint8_t slot = 0; uint8_t status = 0; }; // 0 connected, 1 disconnected, 2 left
struct DesyncMsg { uint32_t tick = 0; std::vector<std::pair<uint8_t, uint64_t>> hashes; };
struct MatchEndMsg { int32_t result = 0; NetText reason; /* empty = decided in the game */ std::vector<std::pair<uint8_t, int32_t>> ratingDelta; };
struct ChatMsg { uint32_t from = 0; std::string name, text; };

// ---- encoding
struct Writer : ByteWriter {
    explicit Writer(Msg t) { u8(uint8_t(t)); }
    void boolean(bool b) { u8(b ? 1 : 0); }
    void u64v(uint64_t v) { u64(v); }
};

void write(ByteWriter& w, const MapInfo& m);
bool read(ByteReader& r, MapInfo& m);
void write(ByteWriter& w, const RoomSettings& s);
bool read(ByteReader& r, RoomSettings& s);
void write(ByteWriter& w, const NetText& t);
bool read(ByteReader& r, NetText& t);
void writeCommands(ByteWriter& w, const std::vector<Command>& cmds);
bool readCommands(ByteReader& r, std::vector<Command>& cmds);

Bytes encode(const HelloMsg&);        bool decode(ByteReader&, HelloMsg&);
Bytes encode(const WelcomeMsg&);      bool decode(ByteReader&, WelcomeMsg&);
Bytes encode(const ErrorMsg&);        bool decode(ByteReader&, ErrorMsg&);
Bytes encode(const std::vector<RoomSummary>&); bool decode(ByteReader&, std::vector<RoomSummary>&);
Bytes encode(const RoomStateMsg&);    bool decode(ByteReader&, RoomStateMsg&);
Bytes encode(const QueueStatusMsg&);  bool decode(ByteReader&, QueueStatusMsg&);
Bytes encode(const MatchStartMsg&);   bool decode(ByteReader&, MatchStartMsg&);
Bytes encode(const TickBatchMsg&);    bool decode(ByteReader&, TickBatchMsg&);
Bytes encode(const InputFrameMsg&);   bool decode(ByteReader&, InputFrameMsg&);
Bytes encode(const ChecksumMsg&);     bool decode(ByteReader&, ChecksumMsg&);
Bytes encode(const MatchResultMsg&);  bool decode(ByteReader&, MatchResultMsg&);
Bytes encode(const PlayerStatusMsg&); bool decode(ByteReader&, PlayerStatusMsg&);
Bytes encode(const DesyncMsg&);       bool decode(ByteReader&, DesyncMsg&);
Bytes encode(const MatchEndMsg&);     bool decode(ByteReader&, MatchEndMsg&);
Bytes encode(Msg type, const ChatMsg&); bool decode(ByteReader&, ChatMsg&); // Msg::Chat (c->s) or Msg::ChatMsg (s->c)

// Small requests built inline by NetClient (type + a few fields) use Writer directly.
inline Msg peekType(const Bytes& b) { return b.empty() ? Msg(0) : Msg(b[0]); }

} // namespace rts::net
