#include "ServerInternal.h"
#include <algorithm>
#include <set>
#include "../../data/Json.h"

namespace rts::net {

// Text typed by players is shown as is: clients treat a leading "@" as a localization key
// (titles from the server config may use that), so escape it ("@@x" displays "@x").
static std::string userText(const std::string& s) { return !s.empty() && s[0] == '@' ? "@" + s : s; }

static RoomSettings settingsFrom(const Json& j, RoomSettings s = {}) {
    s.tickRate = uint32_t(j.getInt("tickRate", s.tickRate));
    s.inputDelay = uint32_t(j.getInt("inputDelay", s.inputDelay));
    s.rollbackWindow = uint32_t(j.getInt("rollbackWindow", s.rollbackWindow));
    s.lagToleranceMs = uint32_t(j.getInt("lagToleranceMs", s.lagToleranceMs));
    s.checksumInterval = uint32_t(j.getInt("checksumInterval", s.checksumInterval));
    s.spectatorDelayTicks = uint32_t(j.getInt("spectatorDelayTicks", s.spectatorDelayTicks));
    s.allowSpectators = j.getBool("allowSpectators", s.allowSpectators);
    return s;
}

ServerConfig loadServerConfig(const Json& j) {
    ServerConfig c;
    c.name = j.getStr("name", c.name);
    c.udpPort = uint16_t(j.getInt("udpPort", c.udpPort));
    c.wsPort = uint16_t(j.getInt("wsPort", c.wsPort));
    c.replayDir = j.getStr("replays", "");
    c.ratingsFile = j.getStr("ratings", "");
    c.maxClients = uint32_t(j.getInt("maxClients", c.maxClients));
    c.reconnectGraceSec = uint32_t(j.getInt("reconnectGraceSec", c.reconnectGraceSec));
    c.log = j.getBool("log", true);
    c.surrenderCommand = uint16_t(j.getInt("surrenderCommand", 0));
    for (auto& [id, dir] : j["games"].members()) c.gameDirs[id] = dir.str();
    const RoomSettings defaults = settingsFrom(j["defaults"]);
    const Json& qs = j["queues"];
    for (size_t i = 0; i < qs.size(); ++i) {
        const Json& q = qs[i];
        QueueConfig qc;
        qc.id = q.getStr("id", "queue" + std::to_string(i + 1));
        qc.title = q.getStr("title", qc.id);
        qc.teams = uint8_t(std::clamp<int64_t>(q.getInt("teams", 2), 2, 16));
        qc.teamSize = uint8_t(std::clamp<int64_t>(q.getInt("teamSize", 1), 1, 16));
        qc.settings = settingsFrom(q, defaults);
        qc.botFillSec = uint32_t(q.getInt("botFillSec", 0));
        qc.ratingRange = uint32_t(q.getInt("ratingRange", qc.ratingRange));
        qc.ratingWidenPerSec = uint32_t(q.getInt("ratingWidenPerSec", qc.ratingWidenPerSec));
        c.queues.push_back(qc);
    }
    return c;
}

Server::Server(ServerConfig cfg) : impl_(std::make_unique<Impl>()) {
    impl_->cfg = std::move(cfg);
    impl_->loadRatings();
}

Server::~Server() {
    for (auto& v : impl_->verdicts) if (v.result.valid()) v.result.wait();   // the arbiter threads use the config
    impl_->saveRatings();
    impl_->clients.clear(); // connections reference the listeners: destroy them first
}

bool Server::listen(std::string* error) {
#ifndef __EMSCRIPTEN__
    auto& i = *impl_;
    if (i.cfg.udpPort && !i.udp.open(i.cfg.udpPort)) { if (error) *error = "cannot bind UDP port " + std::to_string(i.cfg.udpPort); return false; }
    if (i.cfg.wsPort && !i.ws.open(i.cfg.wsPort)) { if (error) *error = "cannot bind TCP port " + std::to_string(i.cfg.wsPort); return false; }
    i.listening = true;
    return true;
#else
    if (error) *error = "no server on the web build";
    return false;
#endif
}

uint16_t Server::udpPort() const {
#ifndef __EMSCRIPTEN__
    return impl_->cfg.udpPort ? impl_->udp.port() : 0;
#else
    return 0;
#endif
}
uint16_t Server::wsPort() const {
#ifndef __EMSCRIPTEN__
    return impl_->cfg.wsPort ? impl_->ws.port() : 0;
#else
    return 0;
#endif
}

void Server::addConnection(std::unique_ptr<Conn> conn) {
    auto& i = *impl_;
    if (i.clients.size() >= i.cfg.maxClients) { conn->close(); return; }
    auto c = std::make_unique<Client>();
    c->id = i.nextClientId++;
    c->conn = std::move(conn);
    c->connectedAt = i.now;
    i.log("conn #%u %s", c->id, c->conn->describe().c_str());
    i.clients.emplace(c->id, std::move(c));
}

bool Server::busy() const { return !impl_->verdicts.empty(); }

ServerStats Server::stats() const {
    ServerStats s = impl_->counters;
    s.clients = uint32_t(impl_->clients.size());
    s.rooms = uint32_t(impl_->rooms.size());
    for (auto& [id, r] : impl_->rooms) if (r->match) ++s.activeMatches;
    for (auto& [id, c] : impl_->clients) if (!c->queueId.empty()) ++s.queued;
    return s;
}

int Server::rating(const std::string& queueId, uint64_t clientKey) const {
    auto it = impl_->ratings.find(impl_->ratingKey(queueId, clientKey));
    return it == impl_->ratings.end() ? 1000 : it->second.value;
}

void Server::update(uint64_t now) {
    auto& i = *impl_;
    i.now = now;
#ifndef __EMSCRIPTEN__
    if (i.listening) {
        if (i.cfg.udpPort) for (auto& c : i.udp.poll(now)) addConnection(std::move(c));
        if (i.cfg.wsPort) for (auto& c : i.ws.poll(now)) addConnection(std::move(c));
    }
#endif
    std::vector<uint32_t> ids;
    for (auto& [id, c] : i.clients) ids.push_back(id);
    for (uint32_t id : ids) {
        Client* c = i.client(id);
        if (!c || !c->conn) continue;
        c->conn->update(now);
        Bytes msg;
        // handle() may delete the client (a resume moves the connection to the old record): look it up each time.
        for (int n = 0; n < 512 && (c = i.client(id)) && c->conn && c->conn->recv(msg); ++n) i.handle(*c, msg);
        if (!(c = i.client(id)) || !c->conn) continue;
        if (c->conn->state() == ConnState::Closed) i.onDisconnect(*c);
        else if (!c->hello && now - c->connectedAt > 10000) { c->conn->close(); i.onDisconnect(*c); }
    }
    // Dropped players whose grace period ran out are gone for good.
    for (uint32_t id : ids) {
        Client* c = i.client(id);
        if (c && !c->conn && now - c->droppedAt > uint64_t(i.cfg.reconnectGraceSec) * 1000) {
            if (Room* r = i.room(c->roomId); r && r->match) i.memberDropped(*r, c->id, true);
            i.dropClient(id);
        }
    }
    if (now - i.lastMatchmake >= 250) { i.lastMatchmake = now; i.matchmake(); }
    i.pollVerdicts();
    std::vector<uint32_t> roomIds;
    for (auto& [id, r] : i.rooms) roomIds.push_back(id);
    for (uint32_t id : roomIds) if (Room* r = i.room(id); r && r->match) i.matchUpdate(*r);
    i.sweepRooms();
    for (auto& [id, c] : i.clients) if (c->conn) c->conn->update(now); // flush
}

// ---------------------------------------------------------------- connections
void Server::Impl::handle(Client& c, const Bytes& msg) {
    ByteReader r(msg.data(), msg.size());
    const Msg type = Msg(r.u8());
    if (!c.hello) { if (type == Msg::Hello) onHello(c, r); return; }
    Room* room = this->room(c.roomId);
    switch (type) {
    case Msg::ListRooms: sendRoomList(c, r.str()); break;
    case Msg::CreateRoom: {
        std::string name = r.str();
        MapInfo map; RoomSettings s;
        if (!read(r, map) || !read(r, s) || map.players() == 0) { sendError(c, ErrorCode::BadRequest, NetText("net.error.bad_request")); break; }
        leaveQueue(c);
        leaveRoom(c);
        name = name.empty() ? c.name : userText(name);
        Room& nr = createRoom(name.substr(0, 64), c.gameId, map, s);
        nr.hostId = c.id;
        joinRoom(c, nr, false);
        break;
    }
    case Msg::JoinRoom: {
        uint32_t id = r.u32(); uint64_t hash = r.u64(); bool spectator = r.u8() != 0;
        Room* target = this->room(id);
        if (!r.ok() || !target || target->matchmade) { sendError(c, ErrorCode::NoSuchRoom, NetText("net.error.no_such_room")); break; }
        if (target->gameId != c.gameId || target->map.dataHash != hash) { sendError(c, ErrorCode::DataMismatch, NetText("net.error.data_mismatch")); break; }
        if (target->match && !spectator) { sendError(c, ErrorCode::InGame, NetText("net.error.in_game")); break; }
        if (spectator && !target->settings.allowSpectators) { sendError(c, ErrorCode::BadRequest, NetText("net.error.no_spectators")); break; }
        leaveQueue(c);
        if (c.roomId != id) leaveRoom(c);
        joinRoom(c, *target, spectator);
        break;
    }
    case Msg::LeaveRoom: leaveRoom(c); break;
    case Msg::SetMap: case Msg::SetSettings: {
        if (!room || room->hostId != c.id || room->phase != RoomPhase::Lobby || room->matchmade) { sendError(c, ErrorCode::NotHost, NetText("net.error.not_host")); break; }
        if (type == Msg::SetMap) {
            MapInfo map;
            if (!read(r, map) || map.players() == 0) break;
            room->map = map;
            std::vector<SlotEntry> slots(map.players());
            size_t k = 0; // keep humans and bots in order; extra ones become spectators
            for (auto& s : room->slots) {
                if (s.kind == SlotKind::Human || s.kind == SlotKind::Bot) {
                    if (k < slots.size()) { slots[k] = s; slots[k].ready = false; ++k; }
                    else if (s.kind == SlotKind::Human) room->spectators.push_back(s.clientId);
                }
            }
            room->slots = slots;
        } else {
            RoomSettings s;
            if (!read(r, s)) break;
            room->settings = s;
            for (auto& sl : room->slots) sl.ready = false;
        }
        broadcastRoom(*room);
        break;
    }
    case Msg::TakeSlot: {
        uint8_t slot = r.u8();
        if (!room || room->phase != RoomPhase::Lobby || room->matchmade) break;
        if (slot > room->slots.size() || (slot && room->slots[slot - 1].kind != SlotKind::Open)) { sendError(c, ErrorCode::RoomFull, NetText("net.error.slot_taken")); break; }
        for (auto& s : room->slots) if (s.kind == SlotKind::Human && s.clientId == c.id) s = SlotEntry{};
        room->spectators.erase(std::remove(room->spectators.begin(), room->spectators.end(), c.id), room->spectators.end());
        if (slot) { auto& s = room->slots[slot - 1]; s.kind = SlotKind::Human; s.clientId = c.id; s.name = c.name; s.ready = false; }
        else room->spectators.push_back(c.id);
        broadcastRoom(*room);
        break;
    }
    case Msg::SetSlotKind: {
        uint8_t slot = r.u8(); SlotKind kind = SlotKind(r.u8() & 3);
        if (!room || room->hostId != c.id || room->phase != RoomPhase::Lobby || room->matchmade) { sendError(c, ErrorCode::NotHost, NetText("net.error.not_host")); break; }
        if (slot == 0 || slot > room->slots.size() || kind == SlotKind::Human) break;
        auto& s = room->slots[slot - 1];
        if (s.kind == SlotKind::Human) room->spectators.push_back(s.clientId); // kicked to spectators
        s = SlotEntry{};
        s.kind = kind;
        if (kind == SlotKind::Bot) { s.name = "@net.bot"; s.ready = true; }
        broadcastRoom(*room);
        break;
    }
    case Msg::SetReady: {
        bool ready = r.u8() != 0;
        if (!room || room->phase != RoomPhase::Lobby) break;
        for (auto& s : room->slots) if (s.kind == SlotKind::Human && s.clientId == c.id) s.ready = ready;
        broadcastRoom(*room);
        break;
    }
    case Msg::StartMatch: {
        if (!room || room->hostId != c.id || room->phase != RoomPhase::Lobby) { sendError(c, ErrorCode::NotHost, NetText("net.error.not_host")); break; }
        int humans = 0; bool ready = true;
        for (auto& s : room->slots) if (s.kind == SlotKind::Human) { ++humans; ready &= s.ready || s.clientId == c.id; }
        if (!humans || !ready) { sendError(c, ErrorCode::NotReady, NetText("net.error.not_ready")); break; }
        startMatch(*room);
        break;
    }
    case Msg::Chat: {
        ChatMsg m;
        if (!decode(r, m) || !room) break;
        m.from = c.id; m.name = c.name; m.text = m.text.substr(0, 500);
        Bytes out = encode(Msg::ChatMsg, m);
        for (auto& [id, other] : clients) if (other->roomId == room->id) send(*other, out);
        break;
    }
    case Msg::QueueJoin: onQueueJoin(c, r); break;
    case Msg::QueueLeave: leaveQueue(c); sendQueueStatus(c, 0); break;
    case Msg::InputFrame: if (room && room->match) onInputFrame(c, *room, r); break;
    case Msg::Checksum: if (room && room->match) onChecksum(c, *room, r); break;
    case Msg::MatchResult: if (room && room->match) onMatchResult(c, *room, r); break;
    case Msg::Ping: {
        Writer w(Msg::Pong); w.u64(r.u64());
        w.u32(room && room->match ? uint32_t((now - room->match->startMs) * room->settings.tickRate / 1000) : 0);
        send(c, w.buf);
        break;
    }
    default: break;
    }
}

void Server::Impl::onHello(Client& c, ByteReader& r) {
    HelloMsg h;
    if (!decode(r, h) || h.protocol != kProtocolVersion) {
        sendError(c, ErrorCode::Version, NetText("net.error.version"));
        c.conn->update(now);
        c.conn->close();
        return;
    }
    // Resume: a dropped client with this token takes over the new connection.
    if (h.reconnectToken) {
        for (auto& [id, old] : clients) {
            if (old.get() == &c || old->token != h.reconnectToken || !old->hello) continue;
            if (old->conn) { old->conn->close(); old->conn.reset(); }
            old->conn = std::move(c.conn);
            old->droppedAt = 0;
            log("client #%u '%s' resumed", old->id, old->name.c_str());
            WelcomeMsg w;
            w.clientId = old->id; w.reconnectToken = old->token; w.serverName = cfg.name; w.resumed = true;
            for (auto& q : cfg.queues) w.queues.push_back({q.id, q.title, q.teams, q.teamSize, q.botFillSec > 0});
            send(*old, encode(w));
            if (Room* rm = room(old->roomId)) {
                send(*old, encode(roomState(*rm)));
                if (rm->match) memberResumed(*rm, *old, h.resumeSeed, h.resumeTick);
            }
            const uint32_t dead = c.id;
            clients.erase(dead); // `c` is gone after this line
            return;
        }
    }
    c.hello = true;
    c.name = userText(h.name.substr(0, 32));   // empty: clients show "Player <slot>"
    c.gameId = h.gameId.substr(0, 128);
    c.key = h.clientKey ? h.clientKey : rng();
    do c.token = rng(); while (!c.token);
    WelcomeMsg w;
    w.clientId = c.id; w.reconnectToken = c.token; w.serverName = cfg.name;
    for (auto& q : cfg.queues) w.queues.push_back({q.id, q.title, q.teams, q.teamSize, q.botFillSec > 0});
    send(c, encode(w));
    log("client #%u '%s' game=%s", c.id, c.name.c_str(), c.gameId.c_str());
}

void Server::Impl::onDisconnect(Client& c) {
    log("client #%u '%s' disconnected", c.id, c.name.c_str());
    c.conn.reset();
    leaveQueue(c);
    Room* r = room(c.roomId);
    if (r && r->match) {
        Member* m = member(*r, c.id);
        if (m && m->slot && !m->left) { c.droppedAt = now ? now : 1; memberDropped(*r, c.id, false); return; } // may resume
    }
    dropClient(c.id);
}

void Server::Impl::dropClient(uint32_t id) {
    Client* c = client(id);
    if (!c) return;
    leaveQueue(*c);
    leaveRoom(*c);
    clients.erase(id);
}

// ---------------------------------------------------------------- lobby
RoomStateMsg Server::Impl::roomState(const Room& r) const {
    RoomStateMsg m;
    m.id = r.id; m.hostId = r.hostId; m.name = r.name; m.gameId = r.gameId; m.map = r.map; m.settings = r.settings;
    m.phase = r.phase; m.matchmade = r.matchmade; m.slots = r.slots;
    for (auto& s : m.slots) {
        if (s.kind != SlotKind::Human) continue;
        auto it = clients.find(s.clientId);
        s.connected = it != clients.end() && it->second->conn;
    }
    for (uint32_t id : r.spectators) {
        auto it = clients.find(id);
        if (it != clients.end()) m.spectators.push_back({id, it->second->name});
    }
    return m;
}

bool Server::Impl::roomHasClient(const Room& r, uint32_t id) const {
    for (auto& s : r.slots) if (s.kind == SlotKind::Human && s.clientId == id) return true;
    return std::find(r.spectators.begin(), r.spectators.end(), id) != r.spectators.end();
}

void Server::Impl::broadcastRoom(Room& r) {
    Bytes b = encode(roomState(r));
    for (auto& [id, c] : clients) if (c->roomId == r.id) send(*c, b);
}

void Server::Impl::sendRoomList(Client& c, const std::string& gameId) {
    std::vector<RoomSummary> list;
    for (auto& [id, r] : rooms) {
        if (r->matchmade || (!gameId.empty() && r->gameId != gameId)) continue;
        RoomSummary s;
        s.id = r->id; s.name = r->name; s.gameId = r->gameId; s.mapFile = r->map.file; s.mapTitle = r->map.title;
        s.dataHash = r->map.dataHash; s.slots = r->map.players(); s.phase = r->phase;
        for (auto& sl : r->slots) if (sl.kind == SlotKind::Human || sl.kind == SlotKind::Bot) ++s.occupied;
        list.push_back(s);
        if (list.size() >= 500) break;
    }
    send(c, encode(list));
}

Room& Server::Impl::createRoom(const std::string& name, const std::string& gameId, const MapInfo& map, const RoomSettings& s) {
    auto r = std::make_unique<Room>();
    r->id = nextRoomId++;
    r->name = name; r->gameId = gameId; r->map = map; r->settings = s;
    r->slots.resize(map.players());
    Room& ref = *r;
    rooms.emplace(r->id, std::move(r));
    log("room #%u '%s' map=%s", ref.id, ref.name.c_str(), map.file.c_str());
    return ref;
}

void Server::Impl::joinRoom(Client& c, Room& r, bool spectator) {
    c.roomId = r.id;
    if (!roomHasClient(r, c.id)) {
        bool seated = false;
        if (!spectator && !r.match)
            for (auto& s : r.slots) if (s.kind == SlotKind::Open) { s.kind = SlotKind::Human; s.clientId = c.id; s.name = c.name; seated = true; break; }
        if (!seated) r.spectators.push_back(c.id);
    }
    broadcastRoom(r);
    if (r.match) { // late spectator: gets the whole history, then follows with a delay
        Member m; m.clientId = c.id; m.slot = 0; m.sent = 0;
        r.match->members.push_back(m);
        sendMatchStart(r, r.match->members.back(), true);
    }
}

void Server::Impl::leaveRoom(Client& c) {
    Room* r = room(c.roomId);
    c.roomId = 0;
    if (!r) return;
    if (r->match) memberDropped(*r, c.id, true);
    for (auto& s : r->slots) if (s.kind == SlotKind::Human && s.clientId == c.id) s = SlotEntry{};
    r->spectators.erase(std::remove(r->spectators.begin(), r->spectators.end(), c.id), r->spectators.end());
    if (c.conn) { RoomStateMsg none; send(c, encode(none)); } // id 0 = "you are in no room"
    if (r->hostId == c.id) { // hand the room over to someone still there
        r->hostId = 0;
        for (auto& s : r->slots) if (s.kind == SlotKind::Human) { r->hostId = s.clientId; break; }
        if (!r->hostId && !r->spectators.empty()) r->hostId = r->spectators.front();
        if (r->match) reassignBots(*r);
    }
    broadcastRoom(*r);
}

void Server::Impl::sweepRooms() {
    // Rooms are only deleted here, never in the middle of handling: callers keep Room references.
    std::set<uint32_t> used;
    for (auto& [id, c] : clients) if (c->roomId) used.insert(c->roomId);
    for (auto it = rooms.begin(); it != rooms.end();) {
        if (used.count(it->first)) { ++it; continue; }
        log("room #%u closed", it->first);
        it = rooms.erase(it);
    }
}

} // namespace rts::net
