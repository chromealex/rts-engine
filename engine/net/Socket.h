// Thin non-blocking BSD/Winsock socket wrappers (native builds only; the web build has no sockets).
#pragma once
#ifndef __EMSCRIPTEN__
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rts::net {

#ifdef _WIN32
using SocketHandle = uintptr_t;
#else
using SocketHandle = int;
#endif

struct Address {
    alignas(8) uint8_t storage[128] = {};   // sockaddr_storage
    uint32_t len = 0;
    bool valid() const { return len != 0; }
    int family() const;
    bool operator==(const Address& o) const;
    std::string str() const;               // "1.2.3.4:5" or "[::1]:5"
    // Name or literal -> address. preferIpv4 picks an A record when both exist.
    static bool resolve(const std::string& host, uint16_t port, Address& out, bool preferIpv4 = false);
};

class UdpSocket {
public:
    ~UdpSocket() { close(); }
    // Bind to port (0 = ephemeral). family 0 = dual-stack IPv6 with IPv4 fallback.
    bool open(uint16_t port, int family = 0);
    void close();
    bool isOpen() const;
    bool sendTo(const Address& to, const uint8_t* data, size_t n);
    // Returns bytes received, or -1 when nothing is pending.
    int recvFrom(Address& from, uint8_t* buf, size_t cap);
    uint16_t localPort() const;

private:
    SocketHandle s_ = SocketHandle(-1);
    int family_ = 0;
};

class TcpSocket {
public:
    TcpSocket() = default;
    explicit TcpSocket(SocketHandle s) : s_(s) {}
    ~TcpSocket() { close(); }
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    bool connect(const Address& to);       // starts a non-blocking connect
    // 1 = connected, 0 = still connecting, -1 = failed.
    int pollConnected();
    // Bytes sent/received; 0 = would block; -1 = closed or error.
    int send(const uint8_t* data, size_t n);
    int recv(uint8_t* buf, size_t cap);
    void close();
    bool isOpen() const;

private:
    SocketHandle s_ = SocketHandle(-1);
};

class TcpListener {
public:
    ~TcpListener() { close(); }
    bool listen(uint16_t port);
    std::unique_ptr<TcpSocket> accept(Address* from = nullptr);
    void close();
    uint16_t localPort() const;

private:
    SocketHandle s_ = SocketHandle(-1);
};

} // namespace rts::net
#endif
