// Reliable ordered messages over an unreliable datagram path (our UDP transport).
//
// Each datagram carries a cumulative ack and as many unacknowledged messages as fit, so in the
// steady state of a match (a few small messages per tick) every packet repeats whatever the
// peer has not confirmed yet: one lost packet costs no extra round trip. Messages larger than a
// datagram are split into fragments that are reassembled in order.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include "Conn.h"
#include "../sim/Command.h"

namespace rts::net {

// Datagram header: magic, kind, session token. Shared by the client and the server listener.
enum class PacketKind : uint8_t { Connect = 0, Accept = 1, Data = 2, Disconnect = 3 };
constexpr uint16_t kPacketMagic = 0x4E52; // "RN"
constexpr size_t kMaxDatagram = 1200;     // stays under typical path MTU (incl. mobile/IPv6 tunnels)
constexpr uint32_t kUdpProtocolVersion = 1;

void writePacketHeader(ByteWriter& w, PacketKind kind, uint32_t token);
bool readPacketHeader(ByteReader& r, PacketKind& kind, uint32_t& token);

struct UdpTestFaults { uint32_t dropPercent = 0; uint64_t seed = 1; }; // for tests only

class ReliableChannel {
public:
    void queue(const Bytes& msg);
    // Payload of a Data packet (after the header). Returns false on malformed input.
    bool onData(ByteReader& r, uint64_t now);
    bool recv(Bytes& msg);
    // Emit Data packets that are due. Always sends at least an ack/keepalive when needed.
    void flush(uint64_t now, uint32_t token, const std::function<void(const Bytes&)>& out);
    uint32_t rttMs() const { return srtt_; }
    uint64_t lastHeard() const { return lastHeard_; }
    void heard(uint64_t now) { lastHeard_ = now; }
    size_t unacked() const { return out_.size(); }
    bool failed() const { return failed_; }

private:
    struct Out { uint32_t seq; Bytes data; bool more; uint64_t firstSent = 0, lastSent = 0; uint32_t sends = 0; };
    std::deque<Out> out_;
    uint32_t nextSeq_ = 0;                 // next seq to assign
    std::map<uint32_t, std::pair<Bytes, bool>> early_; // received out of order
    uint32_t expect_ = 0;                  // next seq we need (everything below delivered)
    Bytes partial_;                        // fragments being reassembled
    std::deque<Bytes> in_;
    bool ackDirty_ = false;
    uint64_t lastSend_ = 0, lastHeard_ = 0;
    uint32_t srtt_ = 100, rttvar_ = 50;
    bool failed_ = false;
};

} // namespace rts::net
