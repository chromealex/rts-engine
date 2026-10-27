#include "WebSocket.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <random>
#include "../sim/Command.h"
#ifndef __EMSCRIPTEN__
#include "Socket.h"
#else
#include <emscripten/websocket.h>
#endif

namespace rts::net {

// ---------------------------------------------------------------- SHA-1 / base64 (handshake only)
std::array<uint8_t, 20> sha1(const void* data, size_t n) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<uint8_t> m(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n);
    const uint64_t bits = uint64_t(n) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back(uint8_t(bits >> (i * 8)));
    auto rol = [](uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(m[off + 4 * i]) << 24 | uint32_t(m[off + 4 * i + 1]) << 16 | uint32_t(m[off + 4 * i + 2]) << 8 | m[off + 4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<uint8_t, 20> out{};
    for (int i = 0; i < 20; ++i) out[size_t(i)] = uint8_t(h[i / 4] >> (24 - 8 * (i % 4)));
    return out;
}

std::string base64(const uint8_t* d, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(d[i]) << 16 | (i + 1 < n ? uint32_t(d[i + 1]) << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        s += t[(v >> 18) & 63]; s += t[(v >> 12) & 63];
        s += i + 1 < n ? t[(v >> 6) & 63] : '=';
        s += i + 2 < n ? t[v & 63] : '=';
    }
    return s;
}

std::string websocketAccept(const std::string& key) {
    std::string s = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    auto h = sha1(s.data(), s.size());
    return base64(h.data(), h.size());
}

#ifndef __EMSCRIPTEN__
// ---------------------------------------------------------------- native WebSocket connection
namespace {
constexpr uint64_t kHandshakeTimeoutMs = 10000;
constexpr uint64_t kPingMs = 5000, kSilenceTimeoutMs = 20000;

std::string headerValue(const std::string& head, const std::string& name) {
    // Case-insensitive header lookup in an HTTP head.
    std::string lower = head;
    for (auto& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    std::string key = "\r\n" + name + ":";
    for (auto& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    size_t p = lower.find(key);
    if (p == std::string::npos) return {};
    p += key.size();
    size_t e = head.find("\r\n", p);
    std::string v = head.substr(p, e - p);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    return v;
}

class WsConn final : public Conn {
public:
    // Server side: accepted socket, waiting for the HTTP upgrade request.
    WsConn(std::unique_ptr<TcpSocket> s, std::string peer) : sock_(std::move(s)), server_(true), peer_(std::move(peer)) {}
    // Client side.
    WsConn(const Address& to, std::string host, std::string path) : server_(false), peer_(to.str()), host_(std::move(host)), path_(std::move(path)) {
        sock_ = std::make_unique<TcpSocket>();
        if (!sock_->connect(to)) state_ = ConnState::Closed;
        std::random_device rd;
        uint8_t k[16];
        for (auto& b : k) b = uint8_t(rd());
        key_ = base64(k, 16);
        maskRng_ = rd();
    }
    ~WsConn() override { close(); }

    void startAsClient() { phase_ = Phase::Connecting; }
    void send(const Bytes& msg) override { if (state_ != ConnState::Closed) { if (phase_ == Phase::Open) frame(2, msg.data(), msg.size()); else queued_.push_back(msg); } }
    bool recv(Bytes& msg) override {
        if (in_.empty()) return false;
        msg = std::move(in_.front()); in_.pop_front();
        return true;
    }
    ConnState state() const override { return state_; }
    std::string describe() const override { return "ws " + peer_; }
    uint32_t rttMs() const override { return rtt_; }
    void close() override {
        if (state_ == ConnState::Closed) return;
        if (phase_ == Phase::Open) { uint8_t code[2] = {0x03, 0xE8}; frame(8, code, 2); writeOut(); }
        state_ = ConnState::Closed;
        sock_.reset();
    }

    void update(uint64_t now) override {
        if (state_ == ConnState::Closed) return;
        if (!started_) { started_ = now; lastHeard_ = now; }
        if (!server_ && phase_ == Phase::Connecting) {
            int c = sock_->pollConnected();
            if (c < 0) { fail(); return; }
            if (c == 0) { if (now - started_ > kHandshakeTimeoutMs) fail(); return; }
            std::string req = "GET " + path_ + " HTTP/1.1\r\nHost: " + host_ + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Key: " + key_ + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
            out_.insert(out_.end(), req.begin(), req.end());
            phase_ = Phase::Handshake;
        }
        uint8_t buf[16384];
        for (;;) {
            int n = sock_->recv(buf, sizeof(buf));
            if (n < 0) { fail(); return; }
            if (n == 0) break;
            inBuf_.insert(inBuf_.end(), buf, buf + n);
            lastHeard_ = now;
            if (inBuf_.size() > kMaxMessageSize + 64) { fail(); return; }
        }
        if (phase_ == Phase::Handshake && !handshake()) { if (state_ != ConnState::Closed && now - started_ > kHandshakeTimeoutMs) fail(); }
        if (phase_ == Phase::Open) {
            if (!parseFrames(now)) { fail(); return; }
            if (now - lastPing_ >= kPingMs) { lastPing_ = now; pingSent_ = now; uint8_t p[8]; for (int i = 0; i < 8; ++i) p[i] = uint8_t(now >> (8 * i)); frame(9, p, 8); }
            if (now - lastHeard_ > kSilenceTimeoutMs) { fail(); return; }
        }
        writeOut();
    }

private:
    enum class Phase { Connecting, Handshake, Open };

    void fail() { state_ = ConnState::Closed; sock_.reset(); }

    bool handshake() {
        std::string s(inBuf_.begin(), inBuf_.end());
        size_t end = s.find("\r\n\r\n");
        if (end == std::string::npos) { if (s.size() > 8192) fail(); return false; }
        std::string head = s.substr(0, end + 2);
        inBuf_.erase(inBuf_.begin(), inBuf_.begin() + long(end + 4));
        if (server_) {
            std::string key = headerValue(head, "Sec-WebSocket-Key");
            if (head.rfind("GET ", 0) != 0 || key.empty()) {
                const char* bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                out_.insert(out_.end(), bad, bad + std::strlen(bad));
                writeOut();
                fail();
                return false;
            }
            std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                               websocketAccept(key) + "\r\n\r\n";
            out_.insert(out_.end(), resp.begin(), resp.end());
        } else {
            if (head.find(" 101") == std::string::npos || headerValue(head, "Sec-WebSocket-Accept") != websocketAccept(key_)) { fail(); return false; }
        }
        phase_ = Phase::Open;
        state_ = ConnState::Connected;
        for (auto& m : queued_) frame(2, m.data(), m.size());
        queued_.clear();
        return true;
    }

    void frame(uint8_t opcode, const uint8_t* data, size_t n) {
        out_.push_back(uint8_t(0x80 | opcode));
        const uint8_t maskBit = server_ ? 0 : 0x80; // clients must mask
        if (n < 126) out_.push_back(uint8_t(maskBit | n));
        else if (n < 65536) { out_.push_back(maskBit | 126); out_.push_back(uint8_t(n >> 8)); out_.push_back(uint8_t(n)); }
        else { out_.push_back(maskBit | 127); for (int i = 7; i >= 0; --i) out_.push_back(uint8_t(uint64_t(n) >> (8 * i))); }
        if (server_) { out_.insert(out_.end(), data, data + n); return; }
        maskRng_ = maskRng_ * 1664525u + 1013904223u;
        uint8_t mask[4] = {uint8_t(maskRng_), uint8_t(maskRng_ >> 8), uint8_t(maskRng_ >> 16), uint8_t(maskRng_ >> 24)};
        out_.insert(out_.end(), mask, mask + 4);
        for (size_t i = 0; i < n; ++i) out_.push_back(data[i] ^ mask[i & 3]);
    }

    bool parseFrames(uint64_t now) {
        size_t p = 0;
        for (;;) {
            if (inBuf_.size() - p < 2) break;
            const uint8_t b0 = inBuf_[p], b1 = inBuf_[p + 1];
            const bool fin = b0 & 0x80, masked = b1 & 0x80;
            const uint8_t op = b0 & 0x0F;
            uint64_t len = b1 & 0x7F;
            size_t h = 2;
            if (len == 126) { if (inBuf_.size() - p < 4) break; len = uint64_t(inBuf_[p + 2]) << 8 | inBuf_[p + 3]; h = 4; }
            else if (len == 127) { if (inBuf_.size() - p < 10) break; len = 0; for (int i = 0; i < 8; ++i) len = len << 8 | inBuf_[p + 2 + size_t(i)]; h = 10; }
            if (len > kMaxMessageSize) return false;
            if (server_ && !masked) return false;   // RFC 6455: client frames are always masked
            const size_t maskAt = p + h;
            if (masked) h += 4;
            if (inBuf_.size() - p < h + len) break;
            uint8_t* payload = inBuf_.data() + p + h;
            if (masked) for (uint64_t i = 0; i < len; ++i) payload[i] ^= inBuf_[maskAt + (i & 3)];
            if (op == 0x8) { state_ = ConnState::Closed; sock_.reset(); return true; }
            if (op == 0x9) frame(0xA, payload, size_t(len));
            else if (op == 0xA) { if (len == 8 && pingSent_) rtt_ = uint32_t(now - pingSent_); }
            else if (op == 0x1 || op == 0x2 || op == 0x0) {
                if (op != 0x0) msg_.clear();
                msg_.insert(msg_.end(), payload, payload + len);
                if (msg_.size() > kMaxMessageSize) return false;
                if (fin) { in_.push_back(std::move(msg_)); msg_.clear(); }
            }
            p += h + len;
        }
        inBuf_.erase(inBuf_.begin(), inBuf_.begin() + long(p));
        return true;
    }

    void writeOut() {
        if (!sock_) return;
        while (!out_.empty()) {
            int n = sock_->send(out_.data(), out_.size());
            if (n < 0) { fail(); return; }
            if (n == 0) break;
            out_.erase(out_.begin(), out_.begin() + n);
        }
        if (out_.size() > kMaxMessageSize * 4) fail(); // peer stopped reading
    }

    std::unique_ptr<TcpSocket> sock_;
    bool server_;
    std::string peer_, host_, path_, key_;
    Phase phase_ = Phase::Handshake;
    ConnState state_ = ConnState::Connecting;
    Bytes inBuf_, out_, msg_;
    std::deque<Bytes> in_;
    std::vector<Bytes> queued_;
    uint64_t started_ = 0, lastHeard_ = 0, lastPing_ = 0, pingSent_ = 0;
    uint32_t rtt_ = 0, maskRng_ = 1;
};
} // namespace

WsListener::WsListener() = default;
WsListener::~WsListener() = default;
bool WsListener::open(uint16_t port) { listener_ = std::make_unique<TcpListener>(); return listener_->listen(port); }
uint16_t WsListener::port() const { return listener_ ? listener_->localPort() : 0; }

std::vector<std::unique_ptr<Conn>> WsListener::poll(uint64_t now) {
    std::vector<std::unique_ptr<Conn>> ready;
    if (!listener_) return ready;
    Address from;
    for (int i = 0; i < 64; ++i) {
        auto s = listener_->accept(&from);
        if (!s) break;
        if (handshaking_.size() < 1024) handshaking_.push_back(std::make_unique<WsConn>(std::move(s), from.str()));
    }
    for (auto& c : handshaking_) {
        c->update(now);
        if (c->state() == ConnState::Connected) ready.push_back(std::move(c));
    }
    handshaking_.erase(std::remove_if(handshaking_.begin(), handshaking_.end(),
                                      [](auto& c) { return !c || c->state() == ConnState::Closed; }), handshaking_.end());
    return ready;
}

std::unique_ptr<Conn> connectWebSocket(const std::string& url) {
    // ws://host[:port][/path]; wss needs TLS which the native client does not implement.
    std::string rest = url;
    if (rest.rfind("ws://", 0) == 0) rest = rest.substr(5);
    else if (rest.find("://") != std::string::npos) return nullptr;
    std::string path = "/";
    if (size_t slash = rest.find('/'); slash != std::string::npos) { path = rest.substr(slash); rest = rest.substr(0, slash); }
    std::string host = rest;
    uint16_t port = kDefaultWsPort;
    if (!rest.empty() && rest[0] == '[') {
        size_t close = rest.find(']');
        if (close == std::string::npos) return nullptr;
        host = rest.substr(1, close - 1);
        if (close + 1 < rest.size() && rest[close + 1] == ':') port = uint16_t(std::stoi(rest.substr(close + 2)));
    } else if (size_t colon = rest.rfind(':'); colon != std::string::npos) {
        host = rest.substr(0, colon);
        port = uint16_t(std::stoi(rest.substr(colon + 1)));
    }
    Address a;
    if (!Address::resolve(host, port, a)) return nullptr;
    auto c = std::make_unique<WsConn>(a, rest, path);
    c->startAsClient();
    return c;
}

#else
// ---------------------------------------------------------------- browser WebSocket (Emscripten)
namespace {
class BrowserWsConn final : public Conn {
public:
    explicit BrowserWsConn(const std::string& url) : url_(url) {
        EmscriptenWebSocketCreateAttributes attr;
        emscripten_websocket_init_create_attributes(&attr);
        attr.url = url_.c_str();
        attr.createOnMainThread = EM_TRUE;
        ws_ = emscripten_websocket_new(&attr);
        if (ws_ <= 0) { state_ = ConnState::Closed; return; }
        emscripten_websocket_set_onopen_callback(ws_, this, [](int, const EmscriptenWebSocketOpenEvent*, void* u) -> EM_BOOL {
            auto* self = static_cast<BrowserWsConn*>(u);
            self->state_ = ConnState::Connected;
            for (auto& m : self->queued_) emscripten_websocket_send_binary(self->ws_, m.data(), uint32_t(m.size()));
            self->queued_.clear();
            return EM_TRUE;
        });
        emscripten_websocket_set_onmessage_callback(ws_, this, [](int, const EmscriptenWebSocketMessageEvent* e, void* u) -> EM_BOOL {
            auto* self = static_cast<BrowserWsConn*>(u);
            if (!e->isText) self->in_.emplace_back(e->data, e->data + e->numBytes);
            return EM_TRUE;
        });
        emscripten_websocket_set_onclose_callback(ws_, this, [](int t, const EmscriptenWebSocketCloseEvent* e, void* u) -> EM_BOOL {
            static_cast<BrowserWsConn*>(u)->state_ = ConnState::Closed; (void)t; (void)e; return EM_TRUE; });
        emscripten_websocket_set_onerror_callback(ws_, this, [](int t, const EmscriptenWebSocketErrorEvent* e, void* u) -> EM_BOOL {
            static_cast<BrowserWsConn*>(u)->state_ = ConnState::Closed; (void)t; (void)e; return EM_TRUE; });
    }
    ~BrowserWsConn() override { close(); if (ws_ > 0) emscripten_websocket_delete(ws_); }
    void send(const Bytes& msg) override {
        if (state_ == ConnState::Connected) emscripten_websocket_send_binary(ws_, const_cast<uint8_t*>(msg.data()), uint32_t(msg.size()));
        else if (state_ == ConnState::Connecting) queued_.push_back(msg);
    }
    bool recv(Bytes& msg) override { if (in_.empty()) return false; msg = std::move(in_.front()); in_.pop_front(); return true; }
    void update(uint64_t) override {}
    ConnState state() const override { return state_; }
    void close() override { if (state_ != ConnState::Closed && ws_ > 0) emscripten_websocket_close(ws_, 1000, "bye"); state_ = ConnState::Closed; }
    std::string describe() const override { return "ws " + url_; }

private:
    std::string url_;
    EMSCRIPTEN_WEBSOCKET_T ws_ = 0;
    ConnState state_ = ConnState::Connecting;
    std::deque<Bytes> in_;
    std::vector<Bytes> queued_;
};
} // namespace

std::unique_ptr<Conn> connectWebSocket(const std::string& url) { return std::make_unique<BrowserWsConn>(url); }
std::unique_ptr<Conn> connectUdp(const std::string&, uint16_t) { return nullptr; }
#endif

std::unique_ptr<Conn> connectTo(const std::string& address) {
    try {
        if (address.rfind("ws://", 0) == 0 || address.rfind("wss://", 0) == 0) return connectWebSocket(address);
        std::string rest = address.rfind("udp://", 0) == 0 ? address.substr(6) : address;
#ifdef __EMSCRIPTEN__
        return connectWebSocket("ws://" + rest);
#else
        std::string host = rest;
        uint16_t port = kDefaultUdpPort;
        if (!rest.empty() && rest[0] == '[') {
            size_t close = rest.find(']');
            if (close == std::string::npos) return nullptr;
            host = rest.substr(1, close - 1);
            if (close + 1 < rest.size() && rest[close + 1] == ':') port = uint16_t(std::stoi(rest.substr(close + 2)));
        } else if (size_t colon = rest.rfind(':'); colon != std::string::npos && rest.find(':') == colon) {
            host = rest.substr(0, colon);
            port = uint16_t(std::stoi(rest.substr(colon + 1)));
        }
        return connectUdp(host, port);
#endif
    } catch (const std::exception&) {
        return nullptr; // bad port number
    }
}

} // namespace rts::net
