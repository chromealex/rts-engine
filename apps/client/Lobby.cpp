// Online play: connection, lobby (quick match queues, rooms), room screen and network matches.
// The match itself is the same client as offline: NetClient drives the Simulation (attach / advance),
// the UI keeps submitting Commands as the local slot. Layout: engine/assets/ui/lobby.rml.
#include "engine/net/NetLocalize.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <random>

#include "GameClient.h"
#include "engine/core/Hash.h"
#include <RmlUi/Core/Input.h>

namespace rts::client {

using i18n::Args;

namespace {

std::string clockText(int64_t seconds) {
    char b[32];
    std::snprintf(b, sizeof b, "%lld:%02lld", (long long)(seconds / 60), (long long)(seconds % 60));
    return b;
}

std::string netFile() { return platform::userDataDir() + "/net.txt"; }

} // namespace

// ------------------------------------------------------------------ settings

void GameClient::loadNetSettings() {
    std::ifstream f(netFile());
    std::string key, name, address;
    if (f && std::getline(f, key) && std::getline(f, name) && std::getline(f, address)) {
        netKey_ = std::strtoull(key.c_str(), nullptr, 16);
        netName_ = name;
        if (!address.empty()) netAddress_ = address;
    }
    if (!netKey_) {
        std::random_device rd; // only identifies this install for matchmaking ratings
        netKey_ = (uint64_t(rd()) << 32) ^ rd();
    }
    if (netName_.empty()) netName_ = tr("lobby.default_name");
}

void GameClient::saveNetSettings() const {
    std::ofstream f(netFile());
    char key[32];
    std::snprintf(key, sizeof key, "%llx", (unsigned long long)netKey_);
    f << key << "\n" << netName_ << "\n" << netAddress_ << "\n";
}

// Same identity as net::describeMap(), but through the content reader so packed and mobile builds
// (APK assets, web preload, .rpak) work: players only meet on byte-identical rules + map.
void GameClient::describeNetMaps() {
    netMaps_.clear();
    std::string rules;
    if (!platform::readContentFile(manifest_.getStr("rules", "rules.json"), rules)) return;
    for (auto& m : maps_) {
        std::string text;
        if (!platform::readContentFile(m.file, text)) continue;
        net::MapInfo info;
        info.file = m.file;
        info.title = m.title;
        Hasher h;
        h.str(rules);
        h.str(text);
        info.dataHash = h.value();
        try {
            const Json map = Json::parse(text);
            const Json& ps = map["players"];
            for (size_t i = 0; i < ps.size(); ++i) info.slotTeams.push_back(uint8_t(ps[i].getInt("team", int64_t(i + 1))));
        } catch (const std::exception&) { continue; }
        netMaps_.push_back(std::move(info));
    }
}

// ------------------------------------------------------------------ actions from the documents

void GameClient::netAction(const std::string& what, int arg) {
    if (what == "open") { setScreen(Screen::Lobby); netError_.clear(); return; }
    if (what == "back") { net_.reset(); setScreen(Screen::Menu); return; }
    if (what == "connect") {
        if (netName_.empty()) netName_ = tr("lobby.default_name");
        saveNetSettings();
        describeNetMaps();
        net_ = std::make_unique<net::NetClient>(net::connectTo(netAddress_),
                                               net::ClientIdentity{netName_, manifest_.getStr("id"), netKey_});
        netError_.clear();
        chat_.clear();
        return;
    }
    if (!net_) return;
    if (what == "disconnect") { net_.reset(); return; }
    if (what == "refresh") net_->refreshRooms();
    else if (what == "create" && arg >= 0 && size_t(arg) < netMaps_.size()) {
        const std::string name = roomName_.empty() ? tr("lobby.room_default", Args().str("name", netName_)) : roomName_;
        net_->createRoom(name, netMaps_[size_t(arg)]);
    }
    else if (what == "leave") net_->leaveRoom();
    else if (what == "ready") {
        bool ready = false;
        if (auto& r = net_->room())
            for (auto& sl : r->slots) if (sl.clientId == net_->clientId()) ready = sl.ready;
        net_->setReady(!ready);
    }
    else if (what == "start") net_->startMatch();
    else if (what == "spectate") net_->takeSlot(0);
    else if (what == "chatkey" && arg != Rml::Input::KI_RETURN) return;
    else if ((what == "chat" || what == "chatkey") && !chatInput_.empty()) { net_->chat(chatInput_); chatInput_.clear(); model_.DirtyVariable("chatInput"); }
    else if (what == "unqueue") net_->leaveQueue();
}

// ------------------------------------------------------------------ per frame

void GameClient::updateNet(double dt, bool& advanced) {
    advanced = false;
    if (!net_) return;
    net_->update(net::nowMs());
    for (auto& e : net_->takeEvents()) {
        const std::string text = net::translate(loc_, e.text); // the server sends keys (protocol v2)
        using K = net::NetEvent::Kind;
        switch (e.kind) {
        case K::Connected: net_->refreshRooms(); break;
        case K::Disconnected:
            netError_ = text;
            if (netMatch_) leaveNetMatch();
            break;
        case K::Error: netError_ = text; break;
        case K::Chat:
            chat_.push_back(text);
            if (chat_.size() > 50) chat_.erase(chat_.begin());
            if (netMatch_) notice(text);
            break;
        case K::PlayerStatus: case K::Desync:
            if (netMatch_) notice(text);
            break;
        case K::MatchEnded:
            if (!text.empty()) { chat_.push_back(text); if (netMatch_) notice(text); }
            if (netMatch_ && screen_ != Screen::Over) setScreen(Screen::Over);
            break;
        case K::ControlChanged:
            // slots of players who dropped may be handed to us: their bots run here now
            if (netMatch_ && sim_ && net_->session()) {
                bots_.clear();
                for (uint8_t sl : net_->session()->controlled())
                    if (sl != human_) bots_.emplace_back(sl, sim_->config().seed, AIConfig{});
            }
            break;
        default: break;
        }
    }
    if (net_ && net_->matchReady()) startNetMatch();
    if (!netMatch_ || !sim_ || !net_ || !net_->session()) return;
    if (!net_->session()->confirmedResult())
        for (auto& b : bots_) b.update(*sim_);
    net_->advance(dt);   // instead of sim_->advance(): predicted ticks, rollbacks, confirmed input
    advanced = true;
}

void GameClient::startNetMatch() {
    const net::MatchStartMsg& m = net_->match();
    std::string rulesText, mapText;
    if (!platform::readContentFile(manifest_.getStr("rules", "rules.json"), rulesText) || !platform::readContentFile(m.map.file, mapText)) {
        netError_ = tr("lobby.map_missing", Args().str("map", m.map.file));
        net_->leaveMatch();
        return;
    }
    try {
        selection_.clear();
        for (auto& g : groups_) g.clear();
        sim_.reset(); // MatchStart already dropped the previous session
        sim_ = std::make_unique<Simulation>(net_->simConfig(), std::make_unique<RtsModule>());
        sim_->setup(Json::parse(rulesText), Json::parse(mapText));
    } catch (const std::exception& e) {
        netError_ = e.what();
        sim_.reset();
        net_->leaveMatch();
        return;
    }
    net_->attach(*sim_);
    if (!netMatch_) prevSpectator_ = spectator_;
    netMatch_ = true;
    human_ = m.localSlot;            // 0 = spectator: no commands, full vision
    spectator_ = m.localSlot == 0;
    bots_.clear();                   // bots hosted by this client (slots it controls besides its own)
    for (uint8_t sl : m.controlled)
        if (sl != human_) bots_.emplace_back(sl, m.seed, AIConfig{});
    mapIndex_ = 0;
    for (size_t i = 0; i < maps_.size(); ++i) if (maps_[i].file == m.map.file) mapIndex_ = i;
    view_.buildTerrain(*dev_, mod().state(*sim_).grid);
    sceneFromSim(*sim_, scene_, 0, spectator_ ? 0 : human_);
    fitCamera();
    minimap_.build(*dev_, mod().state(*sim_), rules_);
    ui_.setDynamicTexture("minimap", minimap_.texture(), minimap_.width(), minimap_.height());
    mode_ = Mode::Normal;
    notices_.clear();
    setScreen(Screen::Playing);
}

void GameClient::leaveNetMatch() {
    if (net_) { net_->leaveMatch(); net_->refreshRooms(); }   // detach before the simulation goes away
    sim_.reset();
    bots_.clear();
    selection_.clear();
    netMatch_ = false;
    spectator_ = prevSpectator_;
    human_ = 1;
    setScreen(net_ ? Screen::Lobby : Screen::Menu);
}

// ------------------------------------------------------------------ lobby model

// Map titles in rooms come from whoever created the room, in their language: show ours when the
// map is in this game, else the server's text (which may be a "@key").
std::string GameClient::mapTitle(const std::string& file, const std::string& serverTitle) const {
    for (auto& m : maps_) if (m.file == file) return m.title;
    return loc_.text(serverTitle);
}

void GameClient::buildLobby(UiState& s) {
    s.netError = netError_;
    s.netMatch = netMatch_;
    if (!net_) { s.lobby = "connect"; return; }
    if (net_->phase() == net::NetClient::Phase::Disconnected) { s.lobby = "connect"; return; }
    if (net_->phase() == net::NetClient::Phase::Connecting) {
        s.lobby = "connecting";
        s.netStatus = tr("lobby.connecting", Args().str("address", netAddress_));
        return;
    }
    s.netStatus = tr("lobby.status", Args().str("server", net_->serverName()).num("ms", net_->rttMs()));
    s.chat = chat_;

    const auto& room = net_->room();
    if (!room) {
        s.lobby = "browse";
        net::QueueStatusMsg qs = net_->queueStatus();
        if (qs.state == 2 && !net_->inMatch()) qs.state = 0; // "found" is stale once that match is over
        s.searching = qs.state != 0;
        for (auto& q : net_->queues()) {
            UiAction a;
            a.label = loc_.text(q.title);   // server config titles may be "@net.queue.x" keys
            const bool mine = qs.state != 0 && qs.queueId == q.id;
            a.active = mine;
            if (mine) a.cost = qs.state == 2 ? tr("lobby.found")
                                             : tr("lobby.searching", Args().str("time", clockText(qs.waitedSec)).num("count", qs.inQueue));
            const std::string id = q.id;
            a.id = addAction([this, id] { net_->joinQueue(id, netMaps_); }, qs.state == 0);
            a.enabled = qs.state == 0;
            s.queues.push_back(a);
        }
        for (auto& r : net_->rooms()) {
            uint64_t hash = 0;
            for (auto& m : netMaps_) if (m.file == r.mapFile) hash = m.dataHash;
            UiAction a;
            a.label = loc_.text(r.name);
            a.cost = tr("lobby.room_line", Args().str("map", mapTitle(r.mapFile, r.mapTitle)).num("occupied", r.occupied).num("slots", r.slots));
            if (r.phase != net::RoomPhase::Lobby) a.tip = tr("lobby.in_game");
            const uint32_t id = r.id;
            a.enabled = hash != 0 && r.phase == net::RoomPhase::Lobby && r.occupied < r.slots;
            a.id = addAction([this, id, hash] { net_->joinRoom(id, hash, false); }, a.enabled);
            a.altLabel = tr("lobby.watch");
            a.alt = addAction([this, id, hash] { net_->joinRoom(id, hash, true); }, hash != 0);
            s.rooms.push_back(a);
        }
        for (size_t i = 0; i < netMaps_.size(); ++i) {
            UiAction a;
            a.label = mapTitle(netMaps_[i].file, netMaps_[i].title);
            a.cost = std::to_string(netMaps_[i].players());
            a.id = addAction([this, i] { netAction("create", int(i)); });
            s.createMaps.push_back(a);
        }
        return;
    }

    s.lobby = "room";
    s.isHost = net_->isHost();
    s.roomTitle = loc_.text(room->name);
    s.roomInfo = mapTitle(room->map.file, room->map.title);
    bool allReady = true, anyHuman = false;
    for (size_t i = 0; i < room->slots.size(); ++i) {
        const net::SlotEntry& sl = room->slots[i];
        const uint8_t slot = uint8_t(i + 1);
        UiAction a;
        const bool mine = sl.clientId != 0 && sl.clientId == net_->clientId();
        a.active = mine;
        a.label = sl.kind == net::SlotKind::Human ? net::displayName(loc_, sl.name, slot)
                : sl.kind == net::SlotKind::Bot ? (sl.name.empty() ? tr("lobby.slot_bot") : net::displayName(loc_, sl.name, slot))
                : sl.kind == net::SlotKind::Closed ? tr("lobby.slot_closed") : tr("lobby.slot_open");
        if (sl.kind == net::SlotKind::Human && sl.clientId == room->hostId) a.hotkey = tr("lobby.host");
        a.cost = tr("lobby.team", Args().num("team", i < room->map.slotTeams.size() ? room->map.slotTeams[i] : int64_t(i + 1)));
        if (sl.kind == net::SlotKind::Human && sl.ready) a.tip = tr("lobby.is_ready");
        if (sl.kind == net::SlotKind::Human) {
            anyHuman = true;
            if (!sl.ready && sl.clientId != room->hostId) allReady = false;
        }
        a.enabled = sl.kind == net::SlotKind::Open;
        a.id = addAction([this, slot] { net_->takeSlot(slot); }, a.enabled);
        if (s.isHost && sl.kind != net::SlotKind::Human) { // host cycles open -> bot -> closed -> open
            const net::SlotKind next = sl.kind == net::SlotKind::Open ? net::SlotKind::Bot
                                     : sl.kind == net::SlotKind::Bot ? net::SlotKind::Closed : net::SlotKind::Open;
            a.altLabel = tr(next == net::SlotKind::Bot ? "lobby.make_bot" : next == net::SlotKind::Closed ? "lobby.make_closed" : "lobby.make_open");
            a.alt = addAction([this, slot, next] { net_->setSlotKind(slot, next); });
        }
        if (mine) s.myReady = sl.ready;
        s.slots.push_back(a);
    }
    std::string specs;
    for (auto& sp : room->spectators) specs += (specs.empty() ? "" : ", ") + net::displayName(loc_, sp.name, 0);
    if (!specs.empty()) s.spectators = tr("lobby.spectators", Args().str("list", specs));
    s.canStart = s.isHost && anyHuman && allReady;
    if (!s.canStart) s.waitHint = tr("lobby.waiting");
}

} // namespace rts::client
