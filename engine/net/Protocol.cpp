#include "Protocol.h"

namespace rts::net {

namespace {
constexpr uint32_t kMaxList = 100000;
bool boolean(ByteReader& r) { return r.u8() != 0; }
template <class T, class F> bool readList(ByteReader& r, std::vector<T>& out, F&& one) {
    uint32_t n = r.u32();
    if (!r.ok() || n > kMaxList) return false;
    out.clear();
    out.reserve(n);
    for (uint32_t i = 0; i < n && r.ok(); ++i) { T t{}; if (!one(t)) return false; out.push_back(std::move(t)); }
    return r.ok();
}
} // namespace

void write(ByteWriter& w, const MapInfo& m) {
    w.str(m.file); w.str(m.title); w.u64(m.dataHash);
    w.u32(uint32_t(m.slotTeams.size()));
    for (auto t : m.slotTeams) w.u8(t);
}
bool read(ByteReader& r, MapInfo& m) {
    m.file = r.str(); m.title = r.str(); m.dataHash = r.u64();
    return readList(r, m.slotTeams, [&](uint8_t& t) { t = r.u8(); return r.ok(); }) && m.slotTeams.size() <= 64;
}

void write(ByteWriter& w, const RoomSettings& s) {
    w.u32(s.tickRate); w.u32(s.inputDelay); w.u32(s.rollbackWindow); w.u32(s.lagToleranceMs);
    w.u32(s.checksumInterval); w.u32(s.spectatorDelayTicks); w.u8(s.allowSpectators ? 1 : 0);
}
bool read(ByteReader& r, RoomSettings& s) {
    s.tickRate = r.u32(); s.inputDelay = r.u32(); s.rollbackWindow = r.u32(); s.lagToleranceMs = r.u32();
    s.checksumInterval = r.u32(); s.spectatorDelayTicks = r.u32(); s.allowSpectators = boolean(r);
    // Clamp to sane ranges: these come from other clients through the server.
    if (s.tickRate < 1 || s.tickRate > 120) s.tickRate = 20;
    if (s.inputDelay > 30) s.inputDelay = 30;
    if (s.rollbackWindow > 60) s.rollbackWindow = 60;
    if (s.lagToleranceMs > 10000) s.lagToleranceMs = 10000;
    if (s.checksumInterval < 1 || s.checksumInterval > 10000) s.checksumInterval = 20;
    if (s.spectatorDelayTicks > 100000) s.spectatorDelayTicks = 100000;
    return r.ok();
}

void writeCommands(ByteWriter& w, const std::vector<Command>& cmds) {
    w.u32(uint32_t(cmds.size()));
    for (auto& c : cmds) writeCommand(w, c);
}
bool readCommands(ByteReader& r, std::vector<Command>& cmds) {
    return readList(r, cmds, [&](Command& c) { c = readCommand(r); return r.ok(); });
}

Bytes encode(const HelloMsg& m) {
    Writer w(Msg::Hello); w.u32(m.protocol); w.str(m.name); w.str(m.gameId); w.u64(m.clientKey); w.u64(m.reconnectToken);
    return w.buf;
}
bool decode(ByteReader& r, HelloMsg& m) {
    m.protocol = r.u32(); m.name = r.str(); m.gameId = r.str(); m.clientKey = r.u64(); m.reconnectToken = r.u64();
    return r.ok();
}

Bytes encode(const WelcomeMsg& m) {
    Writer w(Msg::Welcome); w.u32(m.clientId); w.u64(m.reconnectToken); w.str(m.serverName); w.boolean(m.resumed);
    w.u32(uint32_t(m.queues.size()));
    for (auto& q : m.queues) { w.str(q.id); w.str(q.title); w.u8(q.teams); w.u8(q.teamSize); w.boolean(q.botFill); }
    return w.buf;
}
bool decode(ByteReader& r, WelcomeMsg& m) {
    m.clientId = r.u32(); m.reconnectToken = r.u64(); m.serverName = r.str(); m.resumed = boolean(r);
    return readList(r, m.queues, [&](QueueInfo& q) {
        q.id = r.str(); q.title = r.str(); q.teams = r.u8(); q.teamSize = r.u8(); q.botFill = boolean(r); return r.ok(); });
}

Bytes encode(const ErrorMsg& m) { Writer w(Msg::Error); w.u16(uint16_t(m.code)); w.str(m.text); return w.buf; }
bool decode(ByteReader& r, ErrorMsg& m) { m.code = ErrorCode(r.u16()); m.text = r.str(); return r.ok(); }

Bytes encode(const std::vector<RoomSummary>& rooms) {
    Writer w(Msg::RoomList);
    w.u32(uint32_t(rooms.size()));
    for (auto& s : rooms) {
        w.u32(s.id); w.str(s.name); w.str(s.gameId); w.str(s.mapFile); w.str(s.mapTitle); w.u64(s.dataHash);
        w.u8(s.occupied); w.u8(s.slots); w.u8(uint8_t(s.phase));
    }
    return w.buf;
}
bool decode(ByteReader& r, std::vector<RoomSummary>& rooms) {
    return readList(r, rooms, [&](RoomSummary& s) {
        s.id = r.u32(); s.name = r.str(); s.gameId = r.str(); s.mapFile = r.str(); s.mapTitle = r.str(); s.dataHash = r.u64();
        s.occupied = r.u8(); s.slots = r.u8(); s.phase = RoomPhase(r.u8() % 3); return r.ok(); });
}

Bytes encode(const RoomStateMsg& m) {
    Writer w(Msg::RoomState);
    w.u32(m.id); w.u32(m.hostId); w.str(m.name); w.str(m.gameId);
    write(w, m.map); write(w, m.settings);
    w.u8(uint8_t(m.phase)); w.boolean(m.matchmade);
    w.u32(uint32_t(m.slots.size()));
    for (auto& s : m.slots) { w.u8(uint8_t(s.kind)); w.u32(s.clientId); w.str(s.name); w.boolean(s.ready); w.boolean(s.connected); }
    w.u32(uint32_t(m.spectators.size()));
    for (auto& s : m.spectators) { w.u32(s.clientId); w.str(s.name); }
    return w.buf;
}
bool decode(ByteReader& r, RoomStateMsg& m) {
    m.id = r.u32(); m.hostId = r.u32(); m.name = r.str(); m.gameId = r.str();
    if (!read(r, m.map) || !read(r, m.settings)) return false;
    m.phase = RoomPhase(r.u8() % 3); m.matchmade = boolean(r);
    if (!readList(r, m.slots, [&](SlotEntry& s) {
            s.kind = SlotKind(r.u8() & 3); s.clientId = r.u32(); s.name = r.str(); s.ready = boolean(r); s.connected = boolean(r); return r.ok(); }))
        return false;
    return readList(r, m.spectators, [&](Spectator& s) { s.clientId = r.u32(); s.name = r.str(); return r.ok(); });
}

Bytes encode(const QueueStatusMsg& m) { Writer w(Msg::QueueStatus); w.u8(m.state); w.str(m.queueId); w.u32(m.waitedSec); w.u32(m.inQueue); return w.buf; }
bool decode(ByteReader& r, QueueStatusMsg& m) { m.state = r.u8(); m.queueId = r.str(); m.waitedSec = r.u32(); m.inQueue = r.u32(); return r.ok(); }

Bytes encode(const MatchStartMsg& m) {
    Writer w(Msg::MatchStart);
    w.u32(m.roomId); w.u64(m.seed); write(w, m.settings); write(w, m.map); w.str(m.gameId);
    w.u32(uint32_t(m.players.size()));
    for (auto& p : m.players) { w.u8(p.slot); w.u8(p.team); w.u8(uint8_t(p.kind)); w.u32(p.clientId); w.str(p.name); }
    w.u8(m.localSlot);
    w.u32(uint32_t(m.controlled.size()));
    for (auto s : m.controlled) w.u8(s);
    w.boolean(m.resumed);
    return w.buf;
}
bool decode(ByteReader& r, MatchStartMsg& m) {
    m.roomId = r.u32(); m.seed = r.u64();
    if (!read(r, m.settings) || !read(r, m.map)) return false;
    m.gameId = r.str();
    if (!readList(r, m.players, [&](MatchPlayer& p) {
            p.slot = r.u8(); p.team = r.u8(); p.kind = SlotKind(r.u8() & 3); p.clientId = r.u32(); p.name = r.str(); return r.ok(); }))
        return false;
    m.localSlot = r.u8();
    if (!readList(r, m.controlled, [&](uint8_t& s) { s = r.u8(); return r.ok(); })) return false;
    m.resumed = boolean(r);
    return r.ok();
}

Bytes encode(const TickBatchMsg& m) {
    Writer w(Msg::TickBatch); w.u32(m.serverTick); w.u32(m.first); w.u32(m.count); writeCommands(w, m.commands);
    return w.buf;
}
bool decode(ByteReader& r, TickBatchMsg& m) {
    m.serverTick = r.u32(); m.first = r.u32(); m.count = r.u32();
    return readCommands(r, m.commands);
}

Bytes encode(const InputFrameMsg& m) { Writer w(Msg::InputFrame); w.u32(m.tick); writeCommands(w, m.commands); return w.buf; }
bool decode(ByteReader& r, InputFrameMsg& m) { m.tick = r.u32(); return readCommands(r, m.commands); }

Bytes encode(const ChecksumMsg& m) { Writer w(Msg::Checksum); w.u32(m.tick); w.u64(m.hash); return w.buf; }
bool decode(ByteReader& r, ChecksumMsg& m) { m.tick = r.u32(); m.hash = r.u64(); return r.ok(); }

Bytes encode(const MatchResultMsg& m) { Writer w(Msg::MatchResult); w.u32(m.tick); w.u32(uint32_t(m.result)); w.u64(m.hash); return w.buf; }
bool decode(ByteReader& r, MatchResultMsg& m) { m.tick = r.u32(); m.result = int32_t(r.u32()); m.hash = r.u64(); return r.ok(); }

Bytes encode(const PlayerStatusMsg& m) { Writer w(Msg::PlayerStatus); w.u8(m.slot); w.u8(m.status); return w.buf; }
bool decode(ByteReader& r, PlayerStatusMsg& m) { m.slot = r.u8(); m.status = r.u8(); return r.ok(); }

Bytes encode(const DesyncMsg& m) {
    Writer w(Msg::Desync); w.u32(m.tick); w.u32(uint32_t(m.hashes.size()));
    for (auto& [s, h] : m.hashes) { w.u8(s); w.u64(h); }
    return w.buf;
}
bool decode(ByteReader& r, DesyncMsg& m) {
    m.tick = r.u32();
    return readList(r, m.hashes, [&](std::pair<uint8_t, uint64_t>& p) { p.first = r.u8(); p.second = r.u64(); return r.ok(); });
}

Bytes encode(const MatchEndMsg& m) {
    Writer w(Msg::MatchEnd); w.u32(uint32_t(m.result)); w.str(m.reason); w.u32(uint32_t(m.ratingDelta.size()));
    for (auto& [s, d] : m.ratingDelta) { w.u8(s); w.u32(uint32_t(d)); }
    return w.buf;
}
bool decode(ByteReader& r, MatchEndMsg& m) {
    m.result = int32_t(r.u32()); m.reason = r.str();
    return readList(r, m.ratingDelta, [&](std::pair<uint8_t, int32_t>& p) { p.first = r.u8(); p.second = int32_t(r.u32()); return r.ok(); });
}

Bytes encode(Msg type, const ChatMsg& m) { Writer w(type); w.u32(m.from); w.str(m.name); w.str(m.text); return w.buf; }
bool decode(ByteReader& r, ChatMsg& m) { m.from = r.u32(); m.name = r.str(); m.text = r.str(); return r.ok(); }

} // namespace rts::net
