// Tests for networking: rollback snapshots, protocol, transports, lockstep/rollback sessions,
// server rooms and matchmaking. Run: ./rts_net_tests (from the repo root or with RTS_SOURCE_DIR).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/net/Conn.h"
#include "apps/server/GameData.h"
#include "engine/i18n/GameLanguage.h"
#include "engine/net/NetClient.h"
#include "engine/net/NetLocalize.h"
#include "engine/net/server/Server.h"
#include "engine/sim/Replay.h"
#include <filesystem>
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include "engine/net/Udp.h"
#include "engine/net/WebSocket.h"
#include "engine/sim/Simulation.h"
#include <thread>
#include "modules/rts/RtsAI.h"
#include "modules/rts/RtsModule.h"

using namespace rts;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
static std::vector<std::pair<const char*, std::function<void()>>>& registry() { static std::vector<std::pair<const char*, std::function<void()>>> r; return r; }
struct Reg { Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
#define TEST(name) static void name(); static Reg reg_##name(#name, name); static void name()

#ifndef RTS_SOURCE_DIR
#define RTS_SOURCE_DIR "."
#endif
static const std::string kSkirmish = std::string(RTS_SOURCE_DIR) + "/games/skirmish";
static const std::string kMushrooms = std::string(RTS_SOURCE_DIR) + "/games/mushrooms";

static std::unique_ptr<Simulation> makeSim(const std::string& game, const std::string& map, uint64_t seed) {
    SimConfig cfg; cfg.seed = seed;
    auto sim = std::make_unique<Simulation>(cfg, std::make_unique<game::RtsModule>());
    sim->setup(Json::parseFile(game + "/rules.json"), Json::parseFile(game + "/" + map));
    return sim;
}

// ---------------- snapshots ----------------
static void snapshotRoundTrip(const std::string& game, const std::string& map) {
    auto sim = makeSim(game, map, 5);
    std::vector<game::RtsAI> bots;
    for (auto& p : sim->players()) bots.emplace_back(p.slot, 5);
    auto botsStep = [&](uint32_t n) { for (uint32_t i = 0; i < n && sim->result() == 0; ++i) { for (auto& b : bots) b.update(*sim); sim->step(); } };
    botsStep(600);
    SimSnapshot snap;
    sim->saveState(snap);
    const uint64_t atSnap = sim->checksum();
    const size_t execBefore = sim->executedCommands().size();
    botsStep(300);
    const uint64_t reference = sim->checksum();
    const uint32_t endTick = sim->tick();
    std::vector<Command> later(sim->executedCommands().begin() + long(execBefore), sim->executedCommands().end());

    for (int round = 0; round < 2; ++round) {   // twice: the second restore reuses pool memory
        sim->loadState(snap);
        CHECK(sim->checksum() == atSnap);
        CHECK(sim->executedCommands().size() == execBefore);
        for (auto& c : later) {
            bool queued = false;   // commands submitted before the snapshot are still pending in it
            auto it = snap.pending.find(c.tick);
            if (it != snap.pending.end()) for (auto& p : it->second) queued |= p.player == c.player && p.seq == c.seq;
            if (!queued) sim->inject(c);
        }
        while (sim->tick() < endTick) sim->step();
        CHECK(sim->checksum() == reference);
    }

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) sim->saveState(snap);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 50;
    std::printf("  snapshot %s: %.3f ms, %zu entities\n", map.c_str(), ms, sim->world().count<game::Unit>());
}

TEST(snapshot_restore_resimulates_identically) {
    snapshotRoundTrip(kSkirmish, "maps/valley.json");
    snapshotRoundTrip(kMushrooms, "maps/duel.json");
}

TEST(command_sink_intercepts_submit) {
    auto sim = makeSim(kSkirmish, "maps/valley.json", 1);
    std::vector<Command> got;
    sim->setCommandSink([&](Command&& c) { got.push_back(c); return sim->tick() + 7; });
    Command c; c.player = 1; c.type = 1;
    CHECK(sim->submit(c) == 7);
    CHECK(got.size() == 1 && got[0].player == 1);
    sim->step();
    CHECK(sim->executedCommands().empty());
}

// ---------------- transport ----------------
static net::Bytes pattern(size_t n, uint8_t salt) { net::Bytes b(n); for (size_t i = 0; i < n; ++i) b[i] = uint8_t(i * 31 + salt); return b; }

// Client sends `count` messages (one of them large); server echoes; client must get all back in order.
template <class Listener>
static bool echoRoundTrip(Listener& lis, std::unique_ptr<net::Conn> client, int count, size_t bigSize) {
    std::vector<std::unique_ptr<net::Conn>> serverSide;
    std::vector<net::Bytes> sent;
    for (int i = 0; i < count; ++i) sent.push_back(pattern(i == count / 2 ? bigSize : size_t(1 + i * 7 % 300), uint8_t(i)));
    for (auto& m : sent) client->send(m);
    size_t got = 0;
    bool inOrder = true;
    const uint64_t t0 = net::nowMs();
    while (got < sent.size() && net::nowMs() - t0 < 20000) {
        uint64_t now = net::nowMs();
        for (auto& c : lis.poll(now)) serverSide.push_back(std::move(c));
        for (auto& c : serverSide) {
            c->update(now);
            net::Bytes m;
            while (c->recv(m)) c->send(m);
            c->update(now);
        }
        client->update(now);
        net::Bytes m;
        while (client->recv(m)) { inOrder &= got < sent.size() && m == sent[got]; ++got; }
        if (client->state() == net::ConnState::Closed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("  %s: %zu/%zu echoed in %llu ms, rtt %u ms\n", client->describe().c_str(), got, sent.size(),
                (unsigned long long)(net::nowMs() - t0), client->rttMs());
    return got == sent.size() && inOrder;
}

TEST(transport_loopback_latency_preserves_order) {
    auto [a, b] = net::makeLoopbackPair({40, 30, 7});
    for (int i = 0; i < 100; ++i) a->send(pattern(10, uint8_t(i)));
    int got = 0; bool ok = true;
    for (uint64_t t = 0; t < 200; ++t) {
        a->update(t); b->update(t);
        net::Bytes m;
        while (b->recv(m)) { ok &= m == pattern(10, uint8_t(got)); ++got; CHECK(t >= 40); }
    }
    CHECK(got == 100 && ok);
}

TEST(transport_udp_survives_packet_loss) {
    net::UdpListener lis;
    CHECK(lis.open(0));
    net::setUdpTestFaults({25, 11});
    auto c = net::connectUdp("127.0.0.1", lis.port());
    CHECK(c != nullptr);
    if (c) CHECK(echoRoundTrip(lis, std::move(c), 300, 200000));
    net::setUdpTestFaults({});
    auto c6 = net::connectTo("udp://[::1]:" + std::to_string(lis.port()));
    if (c6) CHECK(echoRoundTrip(lis, std::move(c6), 50, 5000));   // IPv6 when the host has it
}

TEST(transport_websocket_echo) {
    CHECK(net::websocketAccept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="); // RFC 6455 example
    net::WsListener lis;
    CHECK(lis.open(0));
    auto c = net::connectTo("ws://127.0.0.1:" + std::to_string(lis.port()) + "/rts");
    CHECK(c != nullptr);
    if (c) CHECK(echoRoundTrip(lis, std::move(c), 200, 300000));
}

// ---------------- network matches (in-process server, loopback links, simulated time) ----------------
struct NetPlayer {
    NetPlayer(std::string n, std::string dir) : name(std::move(n)), gameDir(std::move(dir)) {}
    std::string name, gameDir;
    std::unique_ptr<Simulation> sim;            // declared first: must outlive the client's session
    std::unique_ptr<net::NetClient> net;
    std::vector<game::RtsAI> bots;
    bool ended = false, desync = false;
    int endResult = 0;
    std::vector<std::string> log;
    bool paused = false;
    int matchStarts = 0;
};

static std::string gameIdOf(const std::string& dir) { return Json::parseFile(dir + "/game.json").getStr("id"); }

static void connectPlayer(net::Server& srv, NetPlayer& p, net::LoopbackOptions link, uint64_t key, uint64_t token = 0) {
    auto [c, s] = net::makeLoopbackPair(link);
    srv.addConnection(std::move(s));
    p.net = std::make_unique<net::NetClient>(std::move(c), net::ClientIdentity{p.name, gameIdOf(p.gameDir), key}, token);
}

static std::set<std::string> g_seenKeys;   // every NetText key that reached a client in these tests

static void pump(NetPlayer& p, uint64_t now, double dt) {
    if (!p.net || p.paused) return;
    p.net->update(now);
    for (auto& e : p.net->takeEvents()) {
        p.log.push_back(e.text.key);
        if (!e.text.empty()) g_seenKeys.insert(e.text.key);
        if (e.kind == net::NetEvent::Kind::MatchEnded && !p.ended) { p.ended = true; p.endResult = e.value; }
        if (e.kind == net::NetEvent::Kind::Desync) p.desync = true;
        if (e.kind == net::NetEvent::Kind::MatchStarted) ++p.matchStarts;
        if (e.kind == net::NetEvent::Kind::Error) std::printf("    [%s] error: %s\n", p.name.c_str(), e.text.key.c_str());
    }
    if (p.net->matchReady()) {
        auto& m = p.net->match();
        auto data = netgame::loadMatchData(p.gameDir, m.map.file);
        p.sim = netgame::makeSimulation(data.module, p.net->simConfig());
        p.sim->setup(data.rules, data.map);
        p.net->attach(*p.sim);
        p.bots.clear();
        for (uint8_t s : m.controlled) p.bots.emplace_back(s, 1000 + s);
        p.ended = false;
    }
    if (p.sim && p.net->session()) {
        // Bots follow control changes (a host leaving hands its bots over).
        auto& ctl = p.net->session()->controlled();
        if (p.bots.size() != ctl.size()) { p.bots.clear(); for (uint8_t s : ctl) p.bots.emplace_back(s, 1000 + s); }
        if (!p.net->session()->confirmedResult()) for (auto& b : p.bots) b.update(*p.sim);
        p.net->advance(dt);
    }
}

// Per-run folder under the system temp dir: a fixed /tmp path collides with leftovers of other
// users/runs, and the app bundle's cwd is read-only on macOS.
static const std::string kReplayDir = [] {
    std::error_code ec;
    auto base = std::filesystem::temp_directory_path(ec);
    if (ec) base = ".";
    auto dir = base / ("rts_net_test_replays-" + std::to_string(net::nowMs()));
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}();

static net::ServerConfig testServerConfig() {
    net::ServerConfig cfg;
    cfg.udpPort = 0; cfg.wsPort = 0; cfg.log = false;
    cfg.replayDir = kReplayDir;
    cfg.gameDirs[gameIdOf(kSkirmish)] = kSkirmish;
    cfg.gameDirs[gameIdOf(kMushrooms)] = kMushrooms;
    cfg.surrenderCommand = game::kCmdSurrender;
    cfg.arbiter = netgame::makeArbiter(cfg.gameDirs);
    net::QueueConfig q1; q1.id = "1v1"; q1.title = "1 на 1"; q1.settings.rollbackWindow = 0; // classic lockstep here
    net::QueueConfig qb; qb.id = "1v1bots"; qb.botFillSec = 2;
    cfg.queues = {q1, qb};
    return cfg;
}

// Runs everyone in lock-step simulated time until all players got MatchEnd (or the time limit).
static uint64_t runUntilEnd(net::Server& srv, std::vector<NetPlayer*> ps, uint64_t& now, uint64_t limitMs,
                            const std::function<void(uint64_t)>& hook = nullptr) {
    const uint64_t start = now;
    for (; now - start < limitMs; now += 5) {
        srv.update(now);
        for (auto* p : ps) pump(*p, now, 0.005);
        if (hook) hook(now - start);
        bool all = true;
        for (auto* p : ps) all &= p->ended || !p->net;
        if (all) break;
    }
    return now - start;
}

// Re-simulate the server's replay offline: it must reproduce the players' final state exactly.
// expectHash 0: accept the replay's own final checksum (a server verdict). result: the winner it re-simulates to.
static bool verifyLatestReplay(uint64_t expectHash, std::string* name = nullptr, const std::vector<Command>* truth = nullptr,
                               int* result = nullptr, Replay* out = nullptr) {
    std::filesystem::path latest;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(kReplayDir, ec))
        if (latest.empty() || e.last_write_time() > std::filesystem::last_write_time(latest)) latest = e.path();
    Replay rep;
    if (latest.empty() || !rep.load(latest.string())) return false;
    if (name) *name = latest.filename().string();
    if (hashFileContents({rep.rulesPath, rep.mapPath}) != rep.dataHash) return false;
    SimConfig cfg; cfg.seed = rep.seed; cfg.tickRate = rep.tickRate; cfg.inputDelayTicks = rep.inputDelay;
    Simulation sim(cfg, std::make_unique<game::RtsModule>());
    sim.setup(Json::parseFile(rep.rulesPath), Json::parseFile(rep.mapPath));
    for (auto& c : rep.commands) sim.inject(c);
    if (truth) {
        size_t i = 0;
        for (; i < rep.commands.size() && i < truth->size(); ++i) if (!(rep.commands[i] == (*truth)[i])) break;
        if (i < rep.commands.size()) {
            auto& r = rep.commands[i];
            std::printf("    first difference at #%zu: replay tick %u p%u type %u seq %u", i, r.tick, r.player, r.type, r.seq);
            if (i < truth->size()) { auto& t = (*truth)[i]; std::printf(" | client tick %u p%u type %u seq %u", t.tick, t.player, t.type, t.seq); }
            std::printf(" (replay %zu cmds, client %zu)\n", rep.commands.size(), truth->size());
        }
    }
    size_t ci = 0;
    while (sim.tick() < rep.endTick) {
        sim.step();
        if (ci < rep.checksums.size() && rep.checksums[ci].first == sim.tick()) { if (sim.checksum() != rep.checksums[ci].second) return false; ++ci; }
    }
    std::printf("    replay %s: %u ticks, %zu commands, %zu/%zu checkpoints ok\n", latest.filename().string().c_str(), rep.endTick,
                rep.commands.size(), ci, rep.checksums.size());
    if (result) *result = sim.result();
    if (out) *out = rep;
    return sim.checksum() == rep.finalChecksum && (expectHash == 0 || rep.finalChecksum == expectHash) && ci == rep.checksums.size();
}

static void printSession(NetPlayer& p) {
    auto* s = p.net->session();
    if (!s) return;
    auto& st = s->stats();
    std::printf("    %-6s slot %u: result %d at tick %u, rollbacks %u (max %u, resim %u), predicted %u, stalls %u\n", p.name.c_str(),
                p.net->match().localSlot, s->confirmedResult(), s->resultTick(), st.rollbacks, st.maxRollback, st.resimulatedTicks,
                st.predictedTicks, st.stallUpdates);
}

TEST(net_room_match_with_rollback_and_spectator) {
    { std::error_code ec; std::filesystem::remove_all(kReplayDir, ec); }
    net::Server srv(testServerConfig());
    NetPlayer a{"Alice", kMushrooms}, b{"Bob", kMushrooms}, spec{"Watcher", kMushrooms};
    connectPlayer(srv, a, {35, 50, 1}, 101);   // high jitter: predictions miss, rollbacks happen
    connectPlayer(srv, b, {25, 50, 2}, 202);
    connectPlayer(srv, spec, {60, 10, 3}, 303);
    uint64_t now = 1000;
    auto step = [&](int ms) { for (int i = 0; i < ms; i += 5, now += 5) { srv.update(now); pump(a, now, .005); pump(b, now, .005); pump(spec, now, .005); } };
    step(300);
    CHECK(a.net->phase() == net::NetClient::Phase::Online && b.net->phase() == net::NetClient::Phase::Online);
    auto maps = net::describeGameMaps(kMushrooms);
    net::RoomSettings rs; rs.rollbackWindow = 10;
    a.net->createRoom("Test room", maps[0], rs);
    step(300);
    CHECK(a.net->room() && a.net->isHost());
    b.net->refreshRooms();
    step(300);
    CHECK(b.net->rooms().size() == 1);
    if (b.net->rooms().empty()) return;
    b.net->joinRoom(b.net->rooms()[0].id, 12345);         // wrong data hash: refused
    step(300);
    CHECK(!b.net->room());
    CHECK(std::count(b.log.begin(), b.log.end(), "net.error.data_mismatch") == 1);
    b.net->joinRoom(b.net->rooms()[0].id, maps[0].dataHash);
    step(300);
    CHECK(b.net->room() && b.net->room()->slots.size() == 2);
    a.net->startMatch();                                  // Bob not ready yet
    step(300);
    CHECK(!a.net->inMatch());
    CHECK(std::count(a.log.begin(), a.log.end(), "net.error.not_ready") == 1);
    b.net->setReady(true);
    step(300);
    a.net->startMatch();
    step(300);
    CHECK(a.net->inMatch() && b.net->inMatch());
    bool specJoined = false;
    runUntilEnd(srv, {&a, &b, &spec}, now, 400000, [&](uint64_t t) {
        if (!specJoined && t > 20000) { spec.net->joinRoom(a.net->room()->id, maps[0].dataHash, true); specJoined = true; }
    });
    printSession(a); printSession(b); printSession(spec);
    CHECK(a.ended && b.ended && a.endResult > 0 && a.endResult == b.endResult);
    CHECK(!a.desync && !b.desync && !spec.desync);
    auto* sa = a.net->session(); auto* sb = b.net->session(); auto* ss = spec.net->session();
    CHECK(sa && sb && ss);
    if (!sa || !sb || !ss) return;
    CHECK(sa->resultTick() == sb->resultTick() && sa->resultHash() == sb->resultHash());
    CHECK(sa->stats().rollbacks > 0);
    // The spectator joined late, caught up from history and saw the same ending.
    for (int i = 0; i < 2000 && !ss->confirmedResult(); ++i) { now += 5; srv.update(now); pump(spec, now, .005); }
    CHECK(ss->confirmedResult() == sa->confirmedResult() && ss->resultHash() == sa->resultHash());
    CHECK(verifyLatestReplay(sa->resultHash()));
}

TEST(net_matchmaking_lockstep_reconnect_skirmish) {
    net::Server srv(testServerConfig());
    NetPlayer a{"Ann", kSkirmish}, b{"Ben", kSkirmish};
    connectPlayer(srv, a, {40, 15, 4}, 111);
    connectPlayer(srv, b, {20, 5, 5}, 222);
    uint64_t now = 1000;
    for (int i = 0; i < 60; ++i, now += 5) { srv.update(now); pump(a, now, .005); pump(b, now, .005); }
    CHECK(a.net->queues().size() == 2);
    auto maps = net::describeGameMaps(kSkirmish);
    a.net->joinQueue("1v1", maps);
    b.net->joinQueue("1v1", maps);
    uint64_t droppedToken = 0;
    bool dropped = false, back = false;
    runUntilEnd(srv, {&a, &b}, now, 2000000, [&](uint64_t t) {
        if (!dropped && t > 60000) {            // Ben's game crashes one minute in...
            droppedToken = b.net->reconnectToken();
            b.net.reset(); b.sim.reset(); b.bots.clear();
            dropped = true;
        }
        if (dropped && !back && t > 75000) {    // ...and comes back 15 s later, catching up from history
            connectPlayer(srv, b, {20, 5, 6}, 222, droppedToken);
            back = true;
        }
        if (t == 100000) a.paused = true;       // Ann freezes for 2 s: Ben must not freeze with her
        if (t == 102000) a.paused = false;
    });
    printSession(a); printSession(b);
    CHECK(dropped && back);
    CHECK(a.ended && b.ended && a.endResult > 0 && a.endResult == b.endResult);
    CHECK(!a.desync && !b.desync);
    auto* sa = a.net->session(); auto* sb = b.net->session();
    CHECK(sa && sb && sa->resultHash() == sb->resultHash());
    if (sa) CHECK(sa->stats().rollbacks == 0);  // lockstep queue: rollbackWindow 0
    if (sa) CHECK(verifyLatestReplay(sa->resultHash(), nullptr, &a.sim->executedCommands()));
    CHECK(srv.rating("1v1", 111) != 1000 && srv.rating("1v1", 111) + srv.rating("1v1", 222) == 2000);
}

TEST(net_matchmaking_fills_with_bots) {
    net::Server srv(testServerConfig());
    NetPlayer a{"Solo", kMushrooms};
    connectPlayer(srv, a, {30, 10, 7}, 333);
    uint64_t now = 1000;
    for (int i = 0; i < 60; ++i, now += 5) { srv.update(now); pump(a, now, .005); }
    a.net->joinQueue("1v1bots", net::describeGameMaps(kMushrooms));
    runUntilEnd(srv, {&a}, now, 400000);
    printSession(a);
    CHECK(a.net->inMatch() && a.net->match().controlled.size() == 2);   // own slot + the bot
    CHECK(a.ended && a.endResult > 0 && !a.desync);
}

TEST(net_desync_is_detected) {
    net::Server srv(testServerConfig());
    NetPlayer a{"A", kMushrooms}, b{"B", kMushrooms};
    connectPlayer(srv, a, {20, 0, 8}, 1);
    connectPlayer(srv, b, {20, 0, 9}, 2);
    uint64_t now = 1000;
    for (int i = 0; i < 60; ++i, now += 5) { srv.update(now); pump(a, now, .005); pump(b, now, .005); }
    auto maps = net::describeGameMaps(kMushrooms);
    a.net->createRoom("r", maps[0]);
    for (int i = 0; i < 60; ++i, now += 5) { srv.update(now); pump(a, now, .005); pump(b, now, .005); }
    b.net->refreshRooms();
    for (int i = 0; i < 60; ++i, now += 5) { srv.update(now); pump(a, now, .005); pump(b, now, .005); }
    if (b.net->rooms().empty()) { CHECK(false); return; }
    b.net->joinRoom(b.net->rooms()[0].id, maps[0].dataHash);
    for (int i = 0; i < 60; ++i, now += 5) { srv.update(now); pump(a, now, .005); pump(b, now, .005); }
    b.net->setReady(true);
    for (int i = 0; i < 60; ++i, now += 5) { srv.update(now); pump(a, now, .005); pump(b, now, .005); }
    a.net->startMatch();
    bool tampered = false;
    for (int i = 0; i < 4000 && !a.desync; ++i, now += 5) {
        srv.update(now); pump(a, now, .005); pump(b, now, .005);
        if (!tampered && b.sim && b.sim->tick() > 100) { b.sim->rng().next(); tampered = true; } // a "bug" on one machine
    }
    CHECK(tampered && a.desync && b.desync);
}

TEST(net_real_sockets_udp_and_websocket) {
    net::ServerConfig cfg = testServerConfig();
    cfg.udpPort = 0; cfg.wsPort = 0;
    net::Server probe(cfg);
    // Ephemeral ports: open listeners on port 0 by asking the OS, then point clients at them.
    cfg.udpPort = 39000 + uint16_t(net::nowMs() % 2000); cfg.wsPort = cfg.udpPort + 1;
    net::Server srv(cfg);
    std::string err;
    CHECK(srv.listen(&err));
    if (!err.empty()) { std::printf("    %s\n", err.c_str()); return; }
    NetPlayer a{"Udp", kMushrooms}, b{"Ws", kMushrooms};
    a.net = std::make_unique<net::NetClient>(net::connectTo("127.0.0.1:" + std::to_string(srv.udpPort())), net::ClientIdentity{"Udp", gameIdOf(kMushrooms), 1});
    b.net = std::make_unique<net::NetClient>(net::connectTo("ws://127.0.0.1:" + std::to_string(srv.wsPort())), net::ClientIdentity{"Ws", gameIdOf(kMushrooms), 2});
    auto maps = net::describeGameMaps(kMushrooms);
    uint64_t last = net::nowMs();
    int stage = 0;
    const uint64_t t0 = net::nowMs();
    while (net::nowMs() - t0 < 8000) {
        const uint64_t now = net::nowMs();
        const double dt = double(now - last) / 1000.0; last = now;
        srv.update(now); pump(a, now, dt); pump(b, now, dt);
        if (stage == 0 && a.net->phase() == net::NetClient::Phase::Online && b.net->phase() == net::NetClient::Phase::Online) { a.net->createRoom("real", maps[0]); stage = 1; }
        if (stage == 1 && a.net->room()) { b.net->refreshRooms(); stage = 2; }
        if (stage == 2 && !b.net->rooms().empty()) { b.net->joinRoom(b.net->rooms()[0].id, maps[0].dataHash); stage = 3; }
        if (stage == 3 && b.net->room()) { b.net->setReady(true); stage = 4; }
        if (stage == 4 && a.net->room() && a.net->room()->slots[1].ready) { a.net->startMatch(); stage = 5; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(stage == 5 && a.sim && b.sim);
    if (!a.sim || !b.sim) return;
    auto* sa = a.net->session(); auto* sb = b.net->session();
    std::printf("    real sockets: udp at tick %u (confirmed %u, rtt %u), ws at tick %u (confirmed %u, rtt %u)\n", sa->tick(), sa->confirmedNext(),
                a.net->rttMs(), sb->tick(), sb->confirmedNext(), b.net->rttMs());
    CHECK(sa->confirmedNext() > 60 && sb->confirmedNext() > 60);      // a few seconds of play went through
    CHECK(!a.desync && !b.desync);
    CHECK(std::abs(int(sa->tick()) - int(sb->tick())) < 10);
    (void)probe;
}


// ---------------- leaving, server verdicts, delta resume ----------------
// Room with two mushroom players; returns once both are in the match.
static bool startRoomMatch(net::Server& srv, NetPlayer& a, NetPlayer& b, uint64_t& now, net::RoomSettings rs = {}) {
    auto step = [&](int ms) { for (int i = 0; i < ms; i += 5, now += 5) { srv.update(now); pump(a, now, .005); pump(b, now, .005); } };
    step(200);
    auto maps = net::describeGameMaps(a.gameDir);
    a.net->createRoom("r", maps[0], rs);
    step(200);
    b.net->refreshRooms();
    step(200);
    if (b.net->rooms().empty()) return false;
    b.net->joinRoom(b.net->rooms()[0].id, maps[0].dataHash);
    step(200);
    b.net->setReady(true);
    step(200);
    a.net->startMatch();
    step(300);
    return a.net->inMatch() && b.net->inMatch();
}

static bool hasSurrender(const Replay& rep, uint8_t slot) {
    for (auto& c : rep.commands) if (c.type == game::kCmdSurrender && c.player == slot) return true;
    return false;
}

TEST(net_leaving_is_a_surrender_in_the_simulation) {
    net::Server srv(testServerConfig());
    NetPlayer a{"Stayer", kMushrooms}, b{"Leaver", kMushrooms};
    connectPlayer(srv, a, {30, 10, 21}, 501);
    connectPlayer(srv, b, {30, 10, 22}, 502);
    uint64_t now = 1000;
    CHECK(startRoomMatch(srv, a, b, now));
    const uint8_t aTeam = a.sim ? a.sim->player(a.net->match().localSlot)->team : 0;
    const uint8_t bSlot = b.net->match().localSlot;
    runUntilEnd(srv, {&a}, now, 200000, [&](uint64_t t) { if (t == 15000) b.net->leaveMatch(); });
    CHECK(a.ended && a.endResult == aTeam);
    CHECK(std::count(a.log.begin(), a.log.end(), "net.player.left") == 1);
    auto* sa = a.net->session();
    Replay rep; int res = 0;
    CHECK(sa && verifyLatestReplay(sa->resultHash(), nullptr, nullptr, &res, &rep));
    CHECK(res == aTeam && hasSurrender(rep, bSlot));                  // the replay shows why the match ended
}

TEST(net_server_decides_when_nobody_reports) {
    net::ServerConfig cfg = testServerConfig();
    cfg.reconnectGraceSec = 2;
    net::Server srv(cfg);
    NetPlayer a{"Silent", kMushrooms}, b{"Quitter", kMushrooms};
    connectPlayer(srv, a, {30, 10, 23}, 601);
    connectPlayer(srv, b, {30, 10, 24}, 602);
    uint64_t now = 1000;
    for (int i = 0; i < 60; ++i, now += 5) { srv.update(now); pump(a, now, .005); pump(b, now, .005); }
    auto maps = net::describeGameMaps(kMushrooms);
    a.net->joinQueue("1v1", maps);
    b.net->joinQueue("1v1", maps);
    uint8_t aTeam = 0, bSlot = 0;
    for (int i = 0; i < 3000; ++i, now += 5) {           // 15 s of play
        srv.update(now); pump(a, now, .005); pump(b, now, .005);
        if (a.sim && !aTeam) aTeam = a.sim->player(a.net->match().localSlot)->team;
    }
    CHECK(a.net->inMatch() && b.net->inMatch());
    if (!b.net->inMatch()) return;
    bSlot = b.net->match().localSlot;
    b.net->leaveMatch();       // surrenders...
    a.paused = true;           // ...and the winner freezes before it can report, then its connection dies
    for (int i = 0; i < 40; ++i, now += 5) { srv.update(now); pump(b, now, .005); }
    a.net.reset();             // the client dies (net before sim: the session detaches from the sim)
    const uint64_t t0 = net::nowMs();
    for (int i = 0; i < 2000 || (srv.busy() && net::nowMs() - t0 < 60000); ++i, now += 5) {
        srv.update(now); pump(b, now, .005);
        if (srv.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(!srv.busy() && srv.stats().verdicts == 1);
    CHECK(srv.rating("1v1", 601) > 1000 && srv.rating("1v1", 602) < 1000);   // the server's verdict counts
    Replay rep; int res = 0;
    CHECK(verifyLatestReplay(0, nullptr, nullptr, &res, &rep));
    CHECK(res == aTeam && hasSurrender(rep, bSlot));
}

TEST(net_crashing_out_still_costs_rating) {
    net::ServerConfig cfg = testServerConfig();
    cfg.reconnectGraceSec = 2;
    net::Server srv(cfg);
    NetPlayer a{"Winner", kMushrooms}, b{"RageQuit", kMushrooms};
    connectPlayer(srv, a, {30, 10, 25}, 801);
    connectPlayer(srv, b, {30, 10, 26}, 802);
    uint64_t now = 1000;
    for (int i = 0; i < 60; ++i, now += 5) { srv.update(now); pump(a, now, .005); pump(b, now, .005); }
    auto maps = net::describeGameMaps(kMushrooms);
    a.net->joinQueue("1v1", maps);
    b.net->joinQueue("1v1", maps);
    runUntilEnd(srv, {&a}, now, 200000, [&](uint64_t t) {
        if (t < 15000) pump(b, now, .005);
        if (t == 15000) { b.net.reset(); b.sim.reset(); }      // the game is killed and never comes back
    });
    CHECK(a.ended && a.endResult > 0);
    CHECK(std::count(a.log.begin(), a.log.end(), "net.player.dropped") == 1 && std::count(a.log.begin(), a.log.end(), "net.player.left") == 1);
    CHECK(srv.rating("1v1", 801) > 1000 && srv.rating("1v1", 802) < 1000);
}

// Wraps a client connection so a test can cut it like a dead network link.
struct CuttableConn final : net::Conn {
    std::unique_ptr<net::Conn> inner;
    std::shared_ptr<bool> cut;
    CuttableConn(std::unique_ptr<net::Conn> c, std::shared_ptr<bool> flag) : inner(std::move(c)), cut(std::move(flag)) {}
    void send(const net::Bytes& m) override { if (!*cut) inner->send(m); }
    bool recv(net::Bytes& m) override { return !*cut && inner->recv(m); }
    void update(uint64_t now) override { if (!*cut) inner->update(now); }
    net::ConnState state() const override { return *cut ? net::ConnState::Closed : inner->state(); }
    void close() override { inner->close(); }
    uint32_t rttMs() const override { return inner->rttMs(); }
    std::string describe() const override { return "cuttable " + inner->describe(); }
};

TEST(net_network_drop_resumes_with_a_delta) {
    net::Server srv(testServerConfig());
    NetPlayer a{"Subway", kMushrooms}, b{"Home", kMushrooms};
    uint64_t now = 1000, outageEnd = 0;
    auto cut = std::make_shared<bool>(false);
    auto open = [&](net::LoopbackOptions link) -> std::unique_ptr<net::Conn> {
        auto [c, sv] = net::makeLoopbackPair(link);
        srv.addConnection(std::move(sv));
        *cut = false;
        return std::make_unique<CuttableConn>(std::move(c), cut);
    };
    a.net = std::make_unique<net::NetClient>(open({30, 20, 31}), net::ClientIdentity{"Subway", gameIdOf(kMushrooms), 701});
    a.net->setReconnect([&]() -> std::unique_ptr<net::Conn> { return now < outageEnd ? nullptr : open({30, 20, 32}); });
    connectPlayer(srv, b, {30, 20, 33}, 702);
    net::RoomSettings rs; rs.rollbackWindow = 8;
    CHECK(startRoomMatch(srv, a, b, now, rs));
    Simulation* simBefore = a.sim.get();
    net::RollbackSession* sessionBefore = a.net->session();
    uint32_t tickAtCut = 0;
    runUntilEnd(srv, {&a, &b}, now, 400000, [&](uint64_t t) {
        if (t == 20000) { *cut = true; outageEnd = now + 3000; tickAtCut = a.sim->tick(); }   // 3 s without network
    });
    printSession(a); printSession(b);
    CHECK(tickAtCut > 0 && std::count(a.log.begin(), a.log.end(), "net.reconnecting") == 1);
    CHECK(a.matchStarts == 1);                                          // no second MatchStart: nothing rebuilt
    CHECK(a.sim.get() == simBefore && a.net->session() == sessionBefore);
    CHECK(a.ended && b.ended && a.endResult == b.endResult && !a.desync && !b.desync);
    auto* sa = a.net->session(); auto* sb = b.net->session();
    CHECK(sa && sb && sa->resultHash() == sb->resultHash());
    if (sa) CHECK(sa->stats().resimulatedTicks < 200);                 // re-simulated the gap, not the match
    if (sa) CHECK(verifyLatestReplay(sa->resultHash()));
}

// ---------------- localization of network texts ----------------
TEST(net_texts_are_localization_keys) {
    auto readTable = [](const std::string& lang) {
        std::ifstream f(std::string(RTS_SOURCE_DIR) + "/engine/assets/lang/" + lang + ".json");
        std::stringstream ss; ss << f.rdbuf();
        return Json::parse(ss.str());
    };
    i18n::Localization en, ru;
    en.addTable("en", readTable("en")); en.setLanguage("en");
    ru.addTable("en", readTable("en")); ru.addTable("ru", readTable("ru")); ru.setLanguage("ru");
    const auto& keys = net::netTextKeys();
    for (auto& k : keys) {
        CHECK(en.catalog("en")->find(k) != nullptr);
        CHECK(ru.catalog("ru")->find(k) != nullptr);
        if (!ru.catalog("ru")->find(k)) std::printf("    missing in ru: %s\n", k.c_str());
    }
    for (auto& k : g_seenKeys) {   // the code emits nothing outside the list
        CHECK(std::find(keys.begin(), keys.end(), k) != keys.end());
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) std::printf("    unlisted key: %s\n", k.c_str());
    }
    CHECK(g_seenKeys.count("net.connected") && g_seenKeys.count("net.error.not_ready"));
    CHECK(net::translate(ru, net::NetText("net.player.left").str("name", "Bob").num("slot", 2)) == "Bob покинул игру");
    CHECK(net::translate(en, net::NetText("net.error.no_maps_for_queue").str("queue", "Custom")) == "No suitable maps for “Custom”");
    CHECK(net::translate(ru, net::NetText("net.error.no_maps_for_queue").str("queue", "@net.queue.2v2")) == "Нет подходящих карт для режима «2 на 2»");
    CHECK(net::translate(ru, net::NetText("net.player.dropped").str("name", "@net.player.unnamed").num("slot", 2)) == "Игрок 2 потерял соединение");
    CHECK(net::translate(en, net::NetText("net.player.left").str("name", "@@root").num("slot", 1)) == "@root left the game");
    CHECK(net::translate(ru, net::NetText("net.rating_down").num("delta", 18)) == "Рейтинг −18");
    CHECK(net::displayName(ru, "@net.bot", 2) == "Бот 2");
    CHECK(net::displayName(en, "", 3) == "Player 3");
    CHECK(net::displayName(ru, "@net.queue.2v2", 0) == "2 на 2");     // titles from server.json
    CHECK(net::displayName(en, "@@admin", 1) == "@admin");            // a player's own "@" is escaped by the server
    // NetText survives the wire.
    net::ErrorMsg e; e.code = net::ErrorCode::RoomFull; e.text = net::NetText("net.error.no_maps_for_queue").str("queue", "x").num("n", -5);
    net::Bytes b = net::encode(e);
    ByteReader r(b.data() + 1, b.size() - 1);
    net::ErrorMsg d;
    CHECK(net::decode(r, d) && d.code == e.code && d.text == e.text);
    // Nothing user-facing in the network code is a hard-coded sentence any more.
    for (const char* f : {"/engine/net/NetClient.cpp", "/engine/net/server/Server.cpp", "/engine/net/server/ServerMatch.cpp", "/engine/net/server/Matchmaker.cpp"}) {
        std::ifstream in(std::string(RTS_SOURCE_DIR) + f);
        std::string line; int n = 0, bad = 0;
        while (std::getline(in, line)) {
            ++n;
            const size_t comment = line.find("//");
            for (size_t i = 0; i + 1 < line.size() && i < comment; ++i)
                if ((uint8_t(line[i]) == 0xD0 || uint8_t(line[i]) == 0xD1) && uint8_t(line[i + 1]) >= 0x80) { ++bad; std::printf("    Cyrillic in code: %s:%d\n", f, n); break; }
        }
        CHECK(bad == 0);
    }
}

int main(int argc, char** argv) {
    for (auto& [name, fn] : registry()) {
        if (argc > 1 && std::string(name).find(argv[1]) == std::string::npos) continue; // ./rts_net_tests <name filter>
        int before = g_failed;
        fn();
        std::printf("%s %s\n", g_failed == before ? "[ OK ]" : "[FAIL]", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
