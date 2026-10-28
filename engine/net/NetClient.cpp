#include "NetClient.h"
#include <algorithm>
#include "../data/Json.h"
#include "../sim/Replay.h"

namespace rts::net {

MapInfo describeMap(const std::string& gameDir, const std::string& mapFile, const std::string& rulesFile, const std::string& title) {
    MapInfo m;
    m.file = mapFile;
    m.title = title.empty() ? mapFile : title;
    const std::string mapPath = gameDir + "/" + mapFile, rulesPath = gameDir + "/" + rulesFile;
    m.dataHash = hashFileContents({rulesPath, mapPath});
    Json map = Json::parseFile(mapPath);
    const Json& ps = map["players"];
    for (size_t i = 0; i < ps.size(); ++i) m.slotTeams.push_back(uint8_t(ps[i].getInt("team", int64_t(i + 1))));
    return m;
}

std::vector<MapInfo> describeGameMaps(const std::string& gameDir) {
    Json manifest = Json::parseFile(gameDir + "/game.json");
    const std::string rules = manifest.getStr("rules", "rules.json");
    std::vector<MapInfo> out;
    const Json& maps = manifest["maps"];
    for (size_t i = 0; i < maps.size(); ++i) out.push_back(describeMap(gameDir, maps[i].getStr("file"), rules, maps[i].getStr("title")));
    return out;
}

NetClient::NetClient(std::unique_ptr<Conn> conn, ClientIdentity id, uint64_t reconnectToken)
    : conn_(std::move(conn)), id_(std::move(id)), reconnectToken_(reconnectToken) {
    if (!conn_) phase_ = Phase::Disconnected;
}

NetClient::NetClient(const std::string& address, ClientIdentity id, uint64_t reconnectToken)
    : NetClient(connectTo(address), std::move(id), reconnectToken) {
    if (conn_) setReconnect([address] { return connectTo(address); });
}

void NetClient::setReconnect(std::function<std::unique_ptr<Conn>()> connect, uint32_t reconnectWindowMs) {
    reconnect_ = std::move(connect);
    reconnectWindowMs_ = reconnectWindowMs;
}

NetClient::~NetClient() { session_.reset(); if (conn_) conn_->close(); }

std::vector<NetEvent> NetClient::takeEvents() {
    std::vector<NetEvent> out(events_.begin(), events_.end());
    events_.clear();
    return out;
}

void NetClient::update(uint64_t now) {
    now_ = now;
    if (phase_ == Phase::Disconnected) return;
    if (conn_) conn_->update(now);
    if (!conn_ || conn_->state() == ConnState::Closed) {
        // Lost (or a retry failed): keep the session and try again while the server keeps our slot.
        const bool canRetry = reconnect_ && reconnectToken_ && (phase_ != Phase::Connecting || droppedAt_);
        if (!canRetry || (droppedAt_ && now - droppedAt_ > reconnectWindowMs_)) {
            phase_ = Phase::Disconnected;
            event(NetEvent::Kind::Disconnected, NetText("net.disconnected"));
            return;
        }
        if (!droppedAt_) { droppedAt_ = now; nextRetry_ = now; event(NetEvent::Kind::Reconnecting, NetText("net.reconnecting")); }
        phase_ = Phase::Reconnecting;
        if (now < nextRetry_) return;
        nextRetry_ = now + 2000;
        conn_ = reconnect_();
        helloSent_ = false;
        if (!conn_) return;
        conn_->update(now);
    }
    if (conn_->state() == ConnState::Connected && !helloSent_) {
        HelloMsg h; h.name = id_.name; h.gameId = id_.gameId; h.clientKey = id_.clientKey; h.reconnectToken = reconnectToken_;
        if (match_ && session_) { h.resumeSeed = match_->seed; h.resumeTick = session_->confirmedNext(); }
        send(encode(h));
        helloSent_ = true;
    }
    Bytes msg;
    while (conn_->recv(msg)) handle(msg);
    if (phase_ == Phase::Online && now - lastPing_ > 2000) {
        Writer w(Msg::Ping); w.u64(now); send(w.buf);
        lastPing_ = now;
    }
    conn_->update(now); // flush replies right away
}

void NetClient::handle(const Bytes& b) {
    ByteReader r(b.data(), b.size());
    const Msg type = Msg(r.u8());
    switch (type) {
    case Msg::Welcome: {
        WelcomeMsg m;
        if (!decode(r, m)) return;
        clientId_ = m.clientId; reconnectToken_ = m.reconnectToken; serverName_ = m.serverName; queues_ = m.queues;
        phase_ = Phase::Online;
        droppedAt_ = 0;
        // Input sent while the link was dead may be lost: send every unconfirmed frame again (the
        // server drops the ones it already has). If the server cannot resume the match from here,
        // it answers with a full MatchStart instead and the game rebuilds the simulation.
        if (m.resumed && session_) session_->resendUnconfirmed();
        event(NetEvent::Kind::Connected, NetText("net.connected").str("server", m.serverName), m.resumed ? 1 : 0);
        break;
    }
    case Msg::Error: {
        ErrorMsg m;
        if (decode(r, m)) event(NetEvent::Kind::Error, m.text, int32_t(m.code));
        break;
    }
    case Msg::RoomList: decode(r, rooms_); break;
    case Msg::RoomState: {
        RoomStateMsg m;
        if (!decode(r, m)) return;
        if (m.id == 0) room_.reset(); else room_ = std::move(m);
        event(NetEvent::Kind::RoomChanged);
        break;
    }
    case Msg::QueueStatus: decode(r, queue_); event(NetEvent::Kind::QueueChanged); break;
    case Msg::MatchStart: {
        MatchStartMsg m;
        if (!decode(r, m)) return;
        session_.reset();
        sim_ = nullptr;
        early_.clear();
        resultSent_ = false;
        match_ = std::move(m);
        pendingMatch_ = true;
        event(NetEvent::Kind::MatchStarted);
        break;
    }
    case Msg::TickBatch: {
        TickBatchMsg m;
        if (!decode(r, m) || !match_) return;
        if (session_) {
            session_->confirm(m.first, m.count, m.commands);
            session_->serverTime(m.serverTick, conn_->rttMs(), now_);
        } else {
            early_.push_back(std::move(m));
        }
        break;
    }
    case Msg::SlotControl: {
        std::vector<uint8_t> slots;
        uint32_t n = r.u32();
        for (uint32_t i = 0; i < n && i < 64 && r.ok(); ++i) slots.push_back(r.u8());
        if (!r.ok() || !match_) return;
        match_->controlled = slots;
        if (session_) session_->setControlled(slots);
        event(NetEvent::Kind::ControlChanged);
        break;
    }
    case Msg::PlayerStatus: {
        PlayerStatusMsg m;
        if (!decode(r, m)) return;
        static const char* keys[] = {"net.player.back", "net.player.dropped", "net.player.left"};
        std::string name = "@net.player.unnamed";   // resolved by the client with the slot argument
        if (match_) for (auto& p : match_->players) if (p.slot == m.slot && !p.name.empty()) name = p.name;
        event(NetEvent::Kind::PlayerStatus, NetText(keys[std::min<uint8_t>(m.status, 2)]).str("name", name).num("slot", m.slot), m.slot);
        break;
    }
    case Msg::Desync: {
        DesyncMsg m;
        if (decode(r, m)) event(NetEvent::Kind::Desync, NetText("net.desync").num("tick", m.tick), int32_t(m.tick));
        break;
    }
    case Msg::MatchEnd: {
        MatchEndMsg m;
        if (!decode(r, m)) return;
        int32_t delta = 0;
        if (match_) for (auto& [s, d] : m.ratingDelta) if (s == match_->localSlot) delta = d;
        event(NetEvent::Kind::MatchEnded, m.reason, m.result);
        if (delta) event(NetEvent::Kind::RatingChanged, NetText(delta > 0 ? "net.rating_up" : "net.rating_down").num("delta", delta > 0 ? delta : -delta), delta);
        break;
    }
    case Msg::ChatMsg: {
        ChatMsg m;
        if (decode(r, m)) event(NetEvent::Kind::Chat, NetText("net.chat_line").str("name", m.name).str("text", m.text), int32_t(m.from));
        break;
    }
    case Msg::Pong: break;
    default: break;
    }
}

void NetClient::refreshRooms() { Writer w(Msg::ListRooms); w.str(id_.gameId); send(w.buf); }

void NetClient::createRoom(const std::string& name, const MapInfo& map, const RoomSettings& settings) {
    Writer w(Msg::CreateRoom); w.str(name); write(w, map); write(w, settings); send(w.buf);
}
void NetClient::joinRoom(uint32_t roomId, uint64_t myDataHash, bool spectator) {
    Writer w(Msg::JoinRoom); w.u32(roomId); w.u64(myDataHash); w.boolean(spectator); send(w.buf);
}
void NetClient::leaveRoom() { Writer w(Msg::LeaveRoom); send(w.buf); }
void NetClient::setMap(const MapInfo& map) { Writer w(Msg::SetMap); write(w, map); send(w.buf); }
void NetClient::setSettings(const RoomSettings& s) { Writer w(Msg::SetSettings); write(w, s); send(w.buf); }
void NetClient::takeSlot(uint8_t slot) { Writer w(Msg::TakeSlot); w.u8(slot); send(w.buf); }
void NetClient::setSlotKind(uint8_t slot, SlotKind kind) { Writer w(Msg::SetSlotKind); w.u8(slot); w.u8(uint8_t(kind)); send(w.buf); }
void NetClient::setReady(bool ready) { Writer w(Msg::SetReady); w.boolean(ready); send(w.buf); }
void NetClient::startMatch() { Writer w(Msg::StartMatch); send(w.buf); }
void NetClient::chat(const std::string& text) { ChatMsg m; m.text = text.substr(0, 500); send(encode(Msg::Chat, m)); }
void NetClient::joinQueue(const std::string& queueId, const std::vector<MapInfo>& maps) {
    Writer w(Msg::QueueJoin); w.str(queueId); w.u32(uint32_t(maps.size()));
    for (auto& m : maps) write(w, m);
    send(w.buf);
}
void NetClient::leaveQueue() { Writer w(Msg::QueueLeave); send(w.buf); }

SimConfig NetClient::simConfig() const {
    SimConfig c;
    if (match_) { c.seed = match_->seed; c.tickRate = match_->settings.tickRate; c.inputDelayTicks = match_->settings.inputDelay; }
    return c;
}

void NetClient::attach(Simulation& sim) {
    if (!match_) return;
    // Humans' own names replace the map's; bots ("@net.bot") and unnamed players keep the map's names.
    for (auto& p : match_->players)
        if (auto* pi = sim.player(p.slot); pi && !p.name.empty() && p.name != "@net.bot") pi->name = p.name.rfind("@@", 0) == 0 ? p.name.substr(1) : p.name;
    SessionConfig cfg;
    cfg.inputDelay = match_->settings.inputDelay;
    cfg.rollbackWindow = match_->settings.rollbackWindow;
    cfg.checksumInterval = match_->settings.checksumInterval;
    sim_ = &sim;
    session_ = std::make_unique<RollbackSession>(
        sim, cfg, match_->localSlot ? match_->controlled : std::vector<uint8_t>{},
        [this](uint32_t tick, const std::vector<Command>& cmds) { InputFrameMsg f; f.tick = tick; f.commands = cmds; send(encode(f)); },
        [this](uint32_t tick, uint64_t hash) { ChecksumMsg c; c.tick = tick; c.hash = hash; send(encode(c)); });
    for (auto& m : early_) {
        session_->confirm(m.first, m.count, m.commands);
        session_->serverTime(m.serverTick, conn_ ? conn_->rttMs() : 0, now_);
    }
    early_.clear();
    pendingMatch_ = false;
}

int NetClient::advance(double seconds) {
    if (!session_) return 0;
    int n = session_->update(seconds, now_);
    if (!resultSent_ && session_->confirmedResult() && match_ && match_->localSlot) {
        MatchResultMsg m; m.tick = session_->resultTick(); m.result = session_->confirmedResult(); m.hash = session_->resultHash();
        send(encode(m));
        resultSent_ = true;
    }
    if (conn_) conn_->update(now_); // inputs out without waiting for the next update()
    return n;
}

void NetClient::leaveMatch() {
    session_.reset();
    sim_ = nullptr;
    match_.reset();
    pendingMatch_ = false;
    early_.clear();
    leaveRoom();
}

} // namespace rts::net
