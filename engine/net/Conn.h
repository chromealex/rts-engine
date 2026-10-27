// Message transport shared by the client and the server.
//
// A Conn is a reliable, ordered pipe of whole messages (byte vectors) between a client and the
// server, whatever carries it: UDP with our own reliability layer (native), WebSocket (web and
// native fallback), or an in-process loopback with simulated latency (tests).
// Everything is poll-based: call update(now) every frame/tick; no threads.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rts::net {

using Bytes = std::vector<uint8_t>;

// Wall-clock milliseconds (steady). Networking timing only; never feeds the simulation.
uint64_t nowMs();

constexpr uint16_t kDefaultUdpPort = 27015;
constexpr uint16_t kDefaultWsPort = 27016;
constexpr size_t kMaxMessageSize = 4u << 20; // larger messages are dropped and close the conn

enum class ConnState { Connecting, Connected, Closed };

class Conn {
public:
    virtual ~Conn() = default;
    virtual void send(const Bytes& msg) = 0;   // queued; delivered reliably and in order
    virtual bool recv(Bytes& msg) = 0;         // next received message, if any
    virtual void update(uint64_t now) = 0;     // pump I/O, resends, keepalives, timeouts
    virtual ConnState state() const = 0;
    virtual void close() = 0;
    virtual uint32_t rttMs() const { return 0; }
    virtual std::string describe() const = 0;  // "udp 1.2.3.4:5", for logs
};

// Two connected in-process endpoints. latencyMs/jitterMs delay delivery (order is preserved,
// like TCP); time is whatever the caller passes to update(), so tests can run faster than real time.
struct LoopbackOptions { uint32_t latencyMs = 0, jitterMs = 0; uint64_t seed = 1; };
std::pair<std::unique_ptr<Conn>, std::unique_ptr<Conn>> makeLoopbackPair(LoopbackOptions opt = {});

// Client connections. Host may be a name or an IPv4/IPv6 literal. On the web build only
// connectWebSocket exists (browsers have no UDP); url is "ws://host:port" or "wss://...".
std::unique_ptr<Conn> connectUdp(const std::string& host, uint16_t port);
std::unique_ptr<Conn> connectWebSocket(const std::string& url);

// "udp://host:port", "ws://host:port", "wss://host/path", or "host[:port]" (UDP natively,
// WebSocket on the web). Returns nullptr for a malformed address.
std::unique_ptr<Conn> connectTo(const std::string& address);

} // namespace rts::net
