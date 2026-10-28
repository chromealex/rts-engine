// Running matches: the input relay. A tick is confirmed when every connected player sent input
// for it, or when its deadline (its nominal time + lagToleranceMs) passes; input that arrives
// after its tick was confirmed is moved to the next unconfirmed tick, so nobody waits on a
// lagging player for longer than the tolerance.
#include "ServerInternal.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <set>
#include "../../data/Json.h"
#include "../../sim/Replay.h"

namespace rts::net {

Member* Server::Impl::member(Room& r, uint32_t clientId) {
    if (!r.match) return nullptr;
    for (auto& m : r.match->members) if (m.clientId == clientId) return &m;
    return nullptr;
}

void Server::Impl::startMatch(Room& r) {
    auto m = std::make_unique<Match>();
    m->seed = rng() >> 1;
    m->startMs = now;
    Member* host = nullptr;
    for (size_t i = 0; i < r.slots.size(); ++i) {
        const SlotEntry& s = r.slots[i];
        if (s.kind != SlotKind::Human && s.kind != SlotKind::Bot) continue;
        MatchPlayer p;
        p.slot = uint8_t(i + 1); p.team = r.map.slotTeams[i]; p.kind = s.kind; p.clientId = s.kind == SlotKind::Human ? s.clientId : 0;
        p.name = s.name;
        m->players.push_back(p);
        if (s.kind == SlotKind::Human) {
            Member mb;
            mb.clientId = s.clientId; mb.slot = p.slot; mb.slots = {p.slot}; mb.nextFrame = r.settings.inputDelay;
            m->members.push_back(mb);
        }
    }
    for (uint32_t id : r.spectators) { Member mb; mb.clientId = id; m->members.push_back(mb); }
    r.match = std::move(m);
    r.phase = RoomPhase::InMatch;
    for (auto& mb : r.match->members) if (mb.clientId == r.hostId && mb.slot) host = &mb;
    if (!host) for (auto& mb : r.match->members) if (mb.slot) { host = &mb; break; }
    for (auto& p : r.match->players) if (p.kind == SlotKind::Bot && host) host->slots.push_back(p.slot);
    ++counters.matchesStarted;
    log("match in room #%u: %zu players, seed %llu", r.id, r.match->players.size(), (unsigned long long)r.match->seed);
    broadcastRoom(r);
    for (auto& mb : r.match->members) sendMatchStart(r, mb, false);
}

void Server::Impl::sendMatchStart(Room& r, Member& m, bool resumed) {
    Client* c = client(m.clientId);
    if (!c) return;
    MatchStartMsg s;
    s.roomId = r.id; s.seed = r.match->seed; s.settings = r.settings; s.map = r.map; s.gameId = r.gameId;
    s.players = r.match->players; s.localSlot = m.slot; s.controlled = m.slots; s.resumed = resumed;
    send(*c, encode(s));
    m.sent = 0;
}

static uint32_t nominalTick(uint64_t now, uint64_t start, uint32_t tickRate) {
    return now > start ? uint32_t((now - start) * tickRate / 1000) : 0;
}

void Server::Impl::matchUpdate(Room& r) {
    Match& M = *r.match;
    const RoomSettings& s = r.settings;
    const uint32_t nominal = nominalTick(now, M.startMs, s.tickRate);
    const uint32_t horizon = nominal + s.inputDelay + s.rollbackWindow + 2; // nobody can legitimately be further ahead

    // ---- confirm ticks
    while (M.confirmedNext <= horizon) {
        const uint32_t T = M.confirmedNext;
        bool all = true;
        for (auto& m : M.members)
            if (m.slot && m.connected && !m.left && m.synced && !m.slots.empty() && m.nextFrame <= T) all = false;
        const uint64_t deadline = M.startMs + uint64_t(T + 1) * 1000 / s.tickRate + s.lagToleranceMs;
        if (!all && now < deadline) break;
        std::vector<Command> cmds;
        for (auto& m : M.members) {
            auto it = m.frames.find(T);
            if (it == m.frames.end()) continue;
            for (auto& c : it->second) cmds.push_back(std::move(c));
            m.frames.erase(it);
        }
        for (auto& c : M.carry) cmds.push_back(std::move(c));
        M.carry.clear();
        // Canonical order: by player, keeping each player's own order; seq becomes 1..n per player.
        std::stable_sort(cmds.begin(), cmds.end(), [](const Command& a, const Command& b) { return a.player < b.player; });
        std::map<uint8_t, uint32_t> seq;
        for (auto& c : cmds) { c.tick = T; c.seq = ++seq[c.player]; M.history.push_back(c); }
        ++M.confirmedNext;
    }

    // ---- send confirmed input
    auto batch = [&](uint32_t from, uint32_t to, uint32_t serverTick) {
        TickBatchMsg b;
        b.serverTick = serverTick; b.first = from; b.count = to - from;
        auto lo = std::lower_bound(M.history.begin(), M.history.end(), from, [](const Command& c, uint32_t t) { return c.tick < t; });
        for (auto it = lo; it != M.history.end() && it->tick < to; ++it) b.commands.push_back(*it);
        return encode(b);
    };
    const bool clockTick = now - M.lastBroadcast >= 250;
    for (auto& m : M.members) {
        Client* c = client(m.clientId);
        if (!c || !c->conn || m.left || c->roomId != r.id) continue;
        if (m.slot) {
            if (m.sent < M.confirmedNext || clockTick) { send(*c, batch(m.sent, M.confirmedNext, nominal)); m.sent = M.confirmedNext; }
        } else {
            const uint32_t delayed = nominal > s.spectatorDelayTicks ? nominal - s.spectatorDelayTicks : 0;
            const uint32_t limit = std::min(delayed, M.confirmedNext);
            if (m.sent < limit || clockTick) { send(*c, batch(m.sent, std::max(m.sent, limit), delayed)); m.sent = std::max(m.sent, limit); }
        }
    }
    if (clockTick) M.lastBroadcast = now;

    // ---- desync detection
    for (auto it = M.checks.begin(); it != M.checks.end();) {
        size_t expected = 0;
        bool missing = false;
        for (auto& m : M.members)
            if (m.slot && m.connected && !m.left && m.synced) { ++expected; missing |= !it->second.count(m.slot); }
        const bool timedOut = it->first + 10 * s.tickRate < M.confirmedNext;
        if (missing && !timedOut) { ++it; continue; }
        std::set<uint64_t> distinct;
        for (auto& [slot, h] : it->second) distinct.insert(h);
        if (distinct.size() == 1) M.agreed[it->first] = *distinct.begin();
        else if (distinct.size() > 1) reportDesync(r, it->first, it->second);
        (void)expected;
        it = M.checks.erase(it);
    }

    // ---- result: every player still in the game must report the same outcome
    bool anyReport = false, everyone = true, agree = true;
    int32_t result = 0;
    for (auto& m : M.members) {
        if (!m.slot || m.left || !m.connected) continue;
        if (!m.reported) { everyone = false; continue; }
        if (anyReport && m.result != result) agree = false;
        result = m.result; anyReport = true;
    }
    if (anyReport && everyone) {
        if (agree) endMatch(r, result, NetText());
        else { M.desync = true; endMatch(r, 0, NetText("net.end.result_mismatch")); }
    }
}

void Server::Impl::onInputFrame(Client& c, Room& r, ByteReader& rd) {
    Member* m = member(r, c.id);
    InputFrameMsg f;
    if (!m || !m->slot || m->left || !decode(rd, f)) return;
    Match& M = *r.match;
    if (f.tick > M.confirmedNext + 2000) return;
    std::vector<Command> ok;
    for (auto& cmd : f.commands) {
        if (ok.size() >= 64) break;   // flood guard: nobody issues more per tick
        if (std::find(m->slots.begin(), m->slots.end(), cmd.player) == m->slots.end()) continue; // not theirs
        if (cmd.units.size() > 2000) cmd.units.resize(2000);
        cmd.tick = f.tick;
        ok.push_back(std::move(cmd));
    }
    if (f.tick < M.confirmedNext) { for (auto& cmd : ok) M.carry.push_back(std::move(cmd)); }
    else {
        auto& slot = m->frames[f.tick];
        for (auto& cmd : ok) slot.push_back(std::move(cmd));
        m->synced = true;
    }
    m->nextFrame = std::max(m->nextFrame, f.tick + 1);
}

void Server::Impl::onChecksum(Client& c, Room& r, ByteReader& rd) {
    Member* m = member(r, c.id);
    ChecksumMsg k;
    if (!m || !m->slot || !decode(rd, k) || k.tick > r.match->confirmedNext) return;
    // A resuming player re-simulates from the start and re-sends old checksums: compare them with
    // what everyone agreed on back then (this is how a resumed game is proven to be in sync).
    if (auto it = r.match->agreed.find(k.tick); it != r.match->agreed.end()) {
        if (it->second != k.hash) reportDesync(r, k.tick, {{0, it->second}, {m->slot, k.hash}});
        return;
    }
    if (r.match->checks.size() < 10000) r.match->checks[k.tick][m->slot] = k.hash;
}

void Server::Impl::reportDesync(Room& r, uint32_t tick, const std::map<uint8_t, uint64_t>& hashes) {
    Match& M = *r.match;
    if (!M.desync) ++counters.desyncs;
    M.desync = true;
    DesyncMsg d; d.tick = tick;
    for (auto& [slot, h] : hashes) d.hashes.emplace_back(slot, h);
    Bytes b = encode(d);
    for (auto& m : M.members) if (Client* c = client(m.clientId)) send(*c, b);
    log("room #%u: DESYNC at tick %u", r.id, tick);
}

void Server::Impl::onMatchResult(Client& c, Room& r, ByteReader& rd) {
    Member* m = member(r, c.id);
    MatchResultMsg res;
    if (!m || !m->slot || !decode(rd, res)) return;
    m->reported = true; m->result = res.result; m->resultTick = res.tick; m->resultHash = res.hash;
}

void Server::Impl::memberResumed(Room& r, Client& c) {
    Member* m = member(r, c.id);
    if (!m || m->left) return;
    m->connected = true; m->synced = false; m->frames.clear();
    sendMatchStart(r, *m, true);
    PlayerStatusMsg ps; ps.slot = m->slot; ps.status = 0;
    Bytes b = encode(ps);
    for (auto& o : r.match->members) if (o.clientId != c.id) if (Client* oc = client(o.clientId)) send(*oc, b);
    broadcastRoom(r);
}

void Server::Impl::memberDropped(Room& r, uint32_t clientId, bool left) {
    Member* m = member(r, clientId);
    if (!m || m->left) return;
    if (left) m->left = true; else m->connected = false;
    if (!m->slot) return; // spectator
    PlayerStatusMsg ps; ps.slot = m->slot; ps.status = left ? 2 : 1;
    Bytes b = encode(ps);
    for (auto& o : r.match->members) if (o.clientId != clientId) if (Client* oc = client(o.clientId)) send(*oc, b);
    if (m->slots.size() > 1) reassignBots(r);
    if (!left) return;
    // Leaving counts as giving up: end the match when only one team has anyone left in it.
    std::set<uint8_t> activeTeams, startTeams;
    bool anyHuman = false;
    for (auto& p : r.match->players) startTeams.insert(p.team);
    for (auto& o : r.match->members)
        if (o.slot && !o.left) { anyHuman = true; for (auto& p : r.match->players) if (p.slot == o.slot) activeTeams.insert(p.team); }
    if (anyHuman) for (auto& p : r.match->players) if (p.kind == SlotKind::Bot) activeTeams.insert(p.team);
    if (!anyHuman) endMatch(r, 0, NetText("net.end.everyone_left"));
    else if (activeTeams.size() == 1 && startTeams.size() > 1) endMatch(r, *activeTeams.begin(), NetText("net.end.opponents_left"));
}

void Server::Impl::reassignBots(Room& r) {
    Match& M = *r.match;
    std::vector<uint8_t> bots;
    for (auto& p : M.players) if (p.kind == SlotKind::Bot) bots.push_back(p.slot);
    if (bots.empty()) return;
    Member* target = nullptr;
    for (auto& m : M.members) if (m.slot && m.connected && !m.left && m.clientId == r.hostId) target = &m;
    if (!target) for (auto& m : M.members) if (m.slot && m.connected && !m.left) { target = &m; break; }
    for (auto& m : M.members) {
        if (!m.slot) continue;
        std::vector<uint8_t> want = {m.slot};
        if (&m == target) want.insert(want.end(), bots.begin(), bots.end());
        if (want == m.slots) continue;
        m.slots = want;
        if (Client* c = client(m.clientId); c && m.connected && !m.left) {
            Writer w(Msg::SlotControl);
            w.u32(uint32_t(want.size()));
            for (auto s : want) w.u8(s);
            send(*c, w.buf);
        }
    }
}

void Server::Impl::endMatch(Room& r, int32_t result, const NetText& reason) {
    Match& M = *r.match;
    uint32_t endTick = M.confirmedNext;
    uint64_t endHash = 0;
    for (auto& m : M.members) if (m.reported && m.result == result) { endTick = m.resultTick; endHash = m.resultHash; break; }
    saveReplay(r, endTick, endHash);

    MatchEndMsg end;
    end.result = result;
    end.reason = reason;
    if (r.matchmade && !M.desync && result > 0) {
        // Elo against the average rating of the other teams' humans (bots count as 1000).
        std::map<uint8_t, std::vector<int>> teamRatings;
        for (auto& p : M.players) {
            Client* c = client(p.clientId);
            teamRatings[p.team].push_back(p.kind == SlotKind::Human && c ? rating(r.queueId, c->key).value : 1000);
        }
        for (auto& p : M.players) {
            Client* c = client(p.clientId);
            if (p.kind != SlotKind::Human || !c) continue;
            Rating& mine = rating(r.queueId, c->key);
            double opp = 0; int n = 0;
            for (auto& [team, list] : teamRatings) if (team != p.team) for (int v : list) { opp += v; ++n; }
            if (!n) continue;
            const double expected = 1.0 / (1.0 + std::pow(10.0, (opp / n - mine.value) / 400.0));
            const double k = mine.games < 20 ? 40 : 24;
            const int delta = int(std::lround(k * ((p.team == result ? 1.0 : 0.0) - expected)));
            mine.value += delta;
            ++mine.games;
            end.ratingDelta.emplace_back(p.slot, delta);
        }
        saveRatings();
    }
    // Spectators lag behind: give them the rest so they can watch the ending.
    for (auto& m : M.members) {
        Client* c = client(m.clientId);
        if (m.slot || m.left || !c || c->roomId != r.id || m.sent >= M.confirmedNext) continue;
        TickBatchMsg tb;
        tb.serverTick = M.confirmedNext; tb.first = m.sent; tb.count = M.confirmedNext - m.sent;
        for (auto& cmd : M.history) if (cmd.tick >= m.sent) tb.commands.push_back(cmd);
        send(*c, encode(tb));
        m.sent = M.confirmedNext;
    }
    Bytes b = encode(end);
    for (auto& m : M.members) if (Client* c = client(m.clientId); c && c->roomId == r.id) send(*c, b);
    ++counters.matchesFinished;
    log("room #%u: match over, result %d %s (ticks %u, desync %d)", r.id, result, reason.key.c_str(), endTick, int(M.desync));

    r.match.reset();
    r.phase = RoomPhase::Lobby;
    for (auto& s : r.slots) if (s.kind == SlotKind::Human) s.ready = false;
    std::vector<uint32_t> inRoom;
    for (auto& [id, c] : clients) if (c->roomId == r.id) inRoom.push_back(id);
    const uint32_t roomId = r.id;
    for (uint32_t id : inRoom) {
        Client* c = client(id);
        if (!c) continue;
        if (!c->conn) dropClient(id);              // dropped players do not come back to a finished match
        else if (room(roomId) && room(roomId)->matchmade) leaveRoom(*c);
    }
    if (Room* still = room(roomId)) broadcastRoom(*still);
}

void Server::Impl::saveReplay(Room& r, uint32_t endTick, uint64_t endHash) {
    if (cfg.replayDir.empty()) return;
    Match& M = *r.match;
    Replay rep;
    std::string dir;
    if (auto it = cfg.gameDirs.find(r.gameId); it != cfg.gameDirs.end()) dir = it->second + "/";
    rep.module = "rts";
    std::string rules = "rules.json";
    try {
        if (!dir.empty()) {
            Json manifest = Json::parseFile(dir + "game.json");
            rep.module = manifest.getStr("module", "rts");
            rules = manifest.getStr("rules", rules);
        }
    } catch (const std::exception&) {}
    rep.rulesPath = dir + rules;
    rep.mapPath = dir + r.map.file;
    rep.dataHash = r.map.dataHash;
    rep.seed = M.seed;
    rep.tickRate = r.settings.tickRate;
    rep.inputDelay = r.settings.inputDelay;
    rep.endTick = endTick;
    rep.finalChecksum = endHash;
    rep.checksumInterval = r.settings.checksumInterval;
    for (auto& c : M.history) if (c.tick < endTick) rep.commands.push_back(c);
    for (auto& a : M.agreed) if (a.first <= endTick) rep.checksums.push_back(a);
    std::error_code ec;
    std::filesystem::create_directories(cfg.replayDir, ec);
    const std::time_t t = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::gmtime(&t));
    const std::string path = cfg.replayDir + "/" + stamp + "-room" + std::to_string(r.id) + (M.desync ? "-desync" : "") + ".rpl";
    if (rep.save(path)) log("replay %s", path.c_str());
}

} // namespace rts::net
