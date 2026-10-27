// Tests for networking: rollback snapshots, protocol, transports, lockstep/rollback sessions,
// server rooms and matchmaking. Run: ./rts_net_tests (from the repo root or with RTS_SOURCE_DIR).
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/net/Conn.h"
#include "engine/net/NetClient.h"
#include "engine/net/server/Server.h"
#include "engine/sim/Replay.h"
#include <filesystem>
#include <map>
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
};

static std::string gameIdOf(const std::string& dir) { return Json::parseFile(dir + "/game.json").getStr("id"); }

static void connectPlayer(net::Server& srv, NetPlayer& p, net::LoopbackOptions link, uint64_t key, uint64_t token = 0) {
    auto [c, s] = net::makeLoopbackPair(link);
    srv.addConnection(std::move(s));
    p.net = std::make_unique<net::NetClient>(std::move(c), net::ClientIdentity{p.name, gameIdOf(p.gameDir), key}, token);
}

static void pump(NetPlayer& p, uint64_t now, double dt) {
    if (!p.net || p.paused) return;
    p.net->update(now);
    for (auto& e : p.net->takeEvents()) {
        p.log.push_back(e.text);
        if (e.kind == net::NetEvent::Kind::MatchEnded && !p.ended) { p.ended = true; p.endResult = e.value; }
        if (e.kind == net::NetEvent::Kind::Desync) p.desync = true;
        if (e.kind == net::NetEvent::Kind::Error) std::printf("    [%s] error: %s\n", p.name.c_str(), e.text.c_str());
    }
    if (p.net->matchReady()) {
        auto& m = p.net->match();
        Json manifest = Json::parseFile(p.gameDir + "/game.json");
        p.sim = std::make_unique<Simulation>(p.net->simConfig(), std::make_unique<game::RtsModule>());
        p.sim->setup(Json::parseFile(p.gameDir + "/" + manifest.getStr("rules", "rules.json")), Json::parseFile(p.gameDir + "/" + m.map.file));
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

static const std::string kReplayDir = "/tmp/rts_net_test_replays";

static net::ServerConfig testServerConfig() {
    net::ServerConfig cfg;
    cfg.udpPort = 0; cfg.wsPort = 0; cfg.log = false;
    cfg.replayDir = kReplayDir;
    cfg.gameDirs[gameIdOf(kSkirmish)] = kSkirmish;
    cfg.gameDirs[gameIdOf(kMushrooms)] = kMushrooms;
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
static bool verifyLatestReplay(uint64_t expectHash, std::string* name = nullptr, const std::vector<Command>* truth = nullptr) {
    std::filesystem::path latest;
    for (auto& e : std::filesystem::directory_iterator(kReplayDir))
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
    return sim.checksum() == rep.finalChecksum && rep.finalChecksum == expectHash && ci == rep.checksums.size();
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
    std::filesystem::remove_all(kReplayDir);
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
    b.net->joinRoom(b.net->rooms()[0].id, maps[0].dataHash);
    step(300);
    CHECK(b.net->room() && b.net->room()->slots.size() == 2);
    a.net->startMatch();                                  // Bob not ready yet
    step(300);
    CHECK(!a.net->inMatch());
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
