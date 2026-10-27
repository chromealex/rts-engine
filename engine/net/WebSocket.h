// WebSocket transport (RFC 6455, binary messages). The server accepts plain ws:// — put a TLS
// reverse proxy (nginx, Caddy) in front for wss:// on HTTPS pages. Natively a client can use
// ws:// too (useful where UDP is blocked); the web build uses the browser's WebSocket.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include "Conn.h"

namespace rts::net {

std::array<uint8_t, 20> sha1(const void* data, size_t n);
std::string base64(const uint8_t* data, size_t n);
std::string websocketAccept(const std::string& key);   // Sec-WebSocket-Accept for a client key

#ifndef __EMSCRIPTEN__
class TcpListener;
class WsListener {
public:
    WsListener();
    ~WsListener();
    bool open(uint16_t port);
    uint16_t port() const;
    // Accepted sockets finishing the HTTP upgrade are returned once the handshake completes.
    std::vector<std::unique_ptr<Conn>> poll(uint64_t now);

private:
    std::unique_ptr<TcpListener> listener_;
    std::vector<std::unique_ptr<Conn>> handshaking_;
};
#endif

} // namespace rts::net
