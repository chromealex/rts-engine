#ifndef __EMSCRIPTEN__
#include "Udp.h"
#include <algorithm>
#include <chrono>
#include <random>
#include "../core/Random.h"

namespace rts::net {

namespace {
constexpr uint64_t kConnectRetryMs = 250;
constexpr uint64_t kConnectTimeoutMs = 10000;
constexpr uint64_t kSilenceTimeoutMs = 10000;

UdpTestFaults g_faults;
Random g_faultRng;

bool dropForTest() { return g_faults.dropPercent && g_faultRng.below(100) < g_faults.dropPercent; }

void sendPacket(UdpSocket& s, const Address& to, const Bytes& b) {
    if (!dropForTest()) s.sendTo(to, b.data(), b.size());
}

uint32_t randomToken() {
    std::random_device rd;
    uint64_t t = uint64_t(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    uint32_t v = rd() ^ uint32_t(t) ^ uint32_t(t >> 32);
    return v ? v : 1;
}

Bytes controlPacket(PacketKind k, uint32_t token) {
    ByteWriter w;
    writePacketHeader(w, k, token);
    if (k == PacketKind::Connect) w.u32(kUdpProtocolVersion);
    return w.buf;
}
} // namespace

void setUdpTestFaults(UdpTestFaults f) { g_faults = f; g_faultRng.reseed(f.seed); }

// ---------------------------------------------------------------- client
class UdpClientConn final : public Conn {
public:
    UdpClientConn(const Address& server) : server_(server), token_(randomToken()) {
        if (!sock_.open(0, server.family())) state_ = ConnState::Closed;
    }
    ~UdpClientConn() override { close(); }

    void send(const Bytes& msg) override { if (state_ != ConnState::Closed) chan_.queue(msg); }
    bool recv(Bytes& msg) override { return chan_.recv(msg); }
    ConnState state() const override { return state_; }
    uint32_t rttMs() const override { return chan_.rttMs(); }
    std::string describe() const override { return "udp " + server_.str(); }

    void close() override {
        if (state_ == ConnState::Connected) for (int i = 0; i < 2; ++i) sendPacket(sock_, server_, controlPacket(PacketKind::Disconnect, token_));
        state_ = ConnState::Closed;
    }

    void update(uint64_t now) override {
        if (state_ == ConnState::Closed) return;
        if (!started_) started_ = now;
        uint8_t buf[2048];
        Address from;
        int n;
        while ((n = sock_.recvFrom(from, buf, sizeof(buf))) >= 0) {
            if (!(from == server_)) continue;
            ByteReader r(buf, size_t(n));
            PacketKind kind; uint32_t token;
            if (!readPacketHeader(r, kind, token) || token != token_) continue;
            if (kind == PacketKind::Accept && state_ == ConnState::Connecting) { state_ = ConnState::Connected; chan_.heard(now); }
            else if (kind == PacketKind::Data && state_ == ConnState::Connected) chan_.onData(r, now);
            else if (kind == PacketKind::Disconnect) { state_ = ConnState::Closed; return; }
        }
        if (state_ == ConnState::Connecting) {
            if (now - started_ > kConnectTimeoutMs) { state_ = ConnState::Closed; return; }
            if (now - lastConnect_ >= kConnectRetryMs) { sendPacket(sock_, server_, controlPacket(PacketKind::Connect, token_)); lastConnect_ = now; }
            return;
        }
        if (chan_.failed() || now - chan_.lastHeard() > kSilenceTimeoutMs) { state_ = ConnState::Closed; return; }
        chan_.flush(now, token_, [&](const Bytes& b) { sendPacket(sock_, server_, b); });
    }

private:
    UdpSocket sock_;
    Address server_;
    uint32_t token_;
    ReliableChannel chan_;
    ConnState state_ = ConnState::Connecting;
    uint64_t started_ = 0, lastConnect_ = 0;
};

std::unique_ptr<Conn> connectUdp(const std::string& host, uint16_t port) {
    Address a;
    if (!Address::resolve(host, port, a)) return nullptr;
    return std::make_unique<UdpClientConn>(a);
}

// ---------------------------------------------------------------- server
struct UdpListener::Peer {
    UdpSocket* sock = nullptr;
    Address addr;
    uint32_t token = 0;
    ReliableChannel chan;
    bool closed = false;
};

namespace {
class UdpPeerConn final : public Conn {
public:
    explicit UdpPeerConn(std::shared_ptr<UdpListener::Peer> p) : p_(std::move(p)) {}
    ~UdpPeerConn() override { close(); }
    void send(const Bytes& msg) override { if (!p_->closed) p_->chan.queue(msg); }
    bool recv(Bytes& msg) override { return p_->chan.recv(msg); }
    ConnState state() const override { return p_->closed ? ConnState::Closed : ConnState::Connected; }
    uint32_t rttMs() const override { return p_->chan.rttMs(); }
    std::string describe() const override { return "udp " + p_->addr.str(); }
    void close() override {
        if (p_->closed) return;
        p_->closed = true;
        if (p_->sock) for (int i = 0; i < 2; ++i) sendPacket(*p_->sock, p_->addr, controlPacket(PacketKind::Disconnect, p_->token));
    }
    void update(uint64_t now) override {
        if (p_->closed || !p_->sock) return;
        if (p_->chan.failed() || now - p_->chan.lastHeard() > kSilenceTimeoutMs) { p_->closed = true; return; }
        p_->chan.flush(now, p_->token, [&](const Bytes& b) { sendPacket(*p_->sock, p_->addr, b); });
    }

private:
    std::shared_ptr<UdpListener::Peer> p_;
};
} // namespace

UdpListener::~UdpListener() {
    for (auto& w : peers_) if (auto p = w.lock()) p->sock = nullptr;
}

bool UdpListener::open(uint16_t port) { return sock_.open(port); }

std::vector<std::unique_ptr<Conn>> UdpListener::poll(uint64_t now) {
    std::vector<std::unique_ptr<Conn>> fresh;
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [](auto& w) { return w.expired(); }), peers_.end());
    uint8_t buf[2048];
    Address from;
    int n;
    for (int budget = 0; budget < 4096 && (n = sock_.recvFrom(from, buf, sizeof(buf))) >= 0; ++budget) {
        ByteReader r(buf, size_t(n));
        PacketKind kind; uint32_t token;
        if (!readPacketHeader(r, kind, token)) continue;
        std::shared_ptr<Peer> peer;
        for (auto& w : peers_) if (auto p = w.lock(); p && p->addr == from) { peer = p; break; }
        if (kind == PacketKind::Connect) {
            if (r.u32() != kUdpProtocolVersion) continue;
            if (peer && peer->token == token) { sendPacket(sock_, from, controlPacket(PacketKind::Accept, token)); continue; }
            if (peer) peer->closed = true; // same address, new session: the old one is gone
            auto p = std::make_shared<Peer>();
            p->sock = &sock_; p->addr = from; p->token = token;
            p->chan.heard(now);
            peers_.push_back(p);
            sendPacket(sock_, from, controlPacket(PacketKind::Accept, token));
            fresh.push_back(std::make_unique<UdpPeerConn>(p));
            continue;
        }
        if (!peer || peer->token != token || peer->closed) continue;
        if (kind == PacketKind::Data) peer->chan.onData(r, now);
        else if (kind == PacketKind::Disconnect) peer->closed = true;
    }
    return fresh;
}

} // namespace rts::net
#endif
