// UDP transport: client connection and the server-side listener.
#pragma once
#ifndef __EMSCRIPTEN__
#include <memory>
#include <vector>
#include "Conn.h"
#include "Reliable.h"
#include "Socket.h"

namespace rts::net {

// Drop a share of outgoing datagrams (both directions when set on client and server). Tests only.
void setUdpTestFaults(UdpTestFaults f);

class UdpListener {
public:
    ~UdpListener();
    bool open(uint16_t port);
    uint16_t port() const { return sock_.localPort(); }
    // Pump the socket; returns connections that completed the handshake since the last call.
    // The listener must outlive the connections it returns.
    std::vector<std::unique_ptr<Conn>> poll(uint64_t now);

    struct Peer;
private:
    UdpSocket sock_;
    std::vector<std::weak_ptr<Peer>> peers_;
};

} // namespace rts::net
#endif
