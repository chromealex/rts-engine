#ifndef __EMSCRIPTEN__
#include "Socket.h"
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define RTS_INVALID INVALID_SOCKET
static bool wouldBlock() { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
static void closeSock(SOCKET s) { closesocket(s); }
static void setNonBlocking(SOCKET s) { u_long one = 1; ioctlsocket(s, FIONBIO, &one); }
static void initSockets() {
    static bool done = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    (void)done;
}
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#define RTS_INVALID (-1)
static bool wouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
static void closeSock(int s) { ::close(s); }
static void setNonBlocking(int s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static void initSockets() {}
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace rts::net {

static const sockaddr* sa(const Address& a) { return reinterpret_cast<const sockaddr*>(a.storage); }

int Address::family() const { return len ? sa(*this)->sa_family : 0; }

bool Address::operator==(const Address& o) const {
    if (len != o.len || family() != o.family()) return false;
    if (family() == AF_INET) {
        auto* a = reinterpret_cast<const sockaddr_in*>(storage); auto* b = reinterpret_cast<const sockaddr_in*>(o.storage);
        return a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr;
    }
    if (family() == AF_INET6) {
        auto* a = reinterpret_cast<const sockaddr_in6*>(storage); auto* b = reinterpret_cast<const sockaddr_in6*>(o.storage);
        return a->sin6_port == b->sin6_port && std::memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(a->sin6_addr)) == 0;
    }
    return std::memcmp(storage, o.storage, len) == 0;
}

std::string Address::str() const {
    char host[64] = "?";
    uint16_t port = 0;
    if (family() == AF_INET) {
        auto* a = reinterpret_cast<const sockaddr_in*>(storage);
        inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
        port = ntohs(a->sin_port);
        return std::string(host) + ":" + std::to_string(port);
    }
    if (family() == AF_INET6) {
        auto* a = reinterpret_cast<const sockaddr_in6*>(storage);
        inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof(host));
        port = ntohs(a->sin6_port);
        return "[" + std::string(host) + "]:" + std::to_string(port);
    }
    return "?";
}

bool Address::resolve(const std::string& host, uint16_t port, Address& out, bool preferIpv4) {
    initSockets();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_ADDRCONFIG;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || !res) {
        hints.ai_flags = 0; // AI_ADDRCONFIG hides loopback on hosts without a configured address
        if (getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || !res) return false;
    }
    const addrinfo* pick = res;
    if (preferIpv4) for (auto* p = res; p; p = p->ai_next) if (p->ai_family == AF_INET) { pick = p; break; }
    out = Address{};
    std::memcpy(out.storage, pick->ai_addr, pick->ai_addrlen);
    out.len = uint32_t(pick->ai_addrlen);
    freeaddrinfo(res);
    return true;
}

// ---------------------------------------------------------------- UDP
bool UdpSocket::open(uint16_t port, int family) {
    initSockets();
    close();
    auto tryOpen = [&](int fam) -> bool {
        auto s = ::socket(fam, SOCK_DGRAM, IPPROTO_UDP);
        if (s == RTS_INVALID) return false;
        if (fam == AF_INET6) { int off = 0; setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off)); }
        sockaddr_storage ss{};
        socklen_t len;
        if (fam == AF_INET6) {
            auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
            a->sin6_family = AF_INET6; a->sin6_addr = in6addr_any; a->sin6_port = htons(port);
            len = sizeof(sockaddr_in6);
        } else {
            auto* a = reinterpret_cast<sockaddr_in*>(&ss);
            a->sin_family = AF_INET; a->sin_addr.s_addr = htonl(INADDR_ANY); a->sin_port = htons(port);
            len = sizeof(sockaddr_in);
        }
        if (::bind(s, reinterpret_cast<sockaddr*>(&ss), len) != 0) { closeSock(s); return false; }
        int buf = 1 << 20;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
        setNonBlocking(s);
        s_ = SocketHandle(s);
        family_ = fam;
        return true;
    };
    if (family == AF_INET) return tryOpen(AF_INET);
    if (family == AF_INET6) return tryOpen(AF_INET6);
    return tryOpen(AF_INET6) || tryOpen(AF_INET);
}

void UdpSocket::close() { if (s_ != SocketHandle(RTS_INVALID)) { closeSock(s_); s_ = SocketHandle(RTS_INVALID); } }
bool UdpSocket::isOpen() const { return s_ != SocketHandle(RTS_INVALID); }

bool UdpSocket::sendTo(const Address& to, const uint8_t* data, size_t n) {
    if (!isOpen()) return false;
    Address dst = to;
    if (family_ == AF_INET6 && to.family() == AF_INET) { // dual-stack socket: send to the v4-mapped address
        auto* v4 = reinterpret_cast<const sockaddr_in*>(to.storage);
        sockaddr_in6 v6{};
        v6.sin6_family = AF_INET6; v6.sin6_port = v4->sin_port;
        uint8_t* b = reinterpret_cast<uint8_t*>(&v6.sin6_addr);
        b[10] = 0xff; b[11] = 0xff;
        std::memcpy(b + 12, &v4->sin_addr, 4);
        dst = Address{};
        std::memcpy(dst.storage, &v6, sizeof(v6));
        dst.len = sizeof(v6);
    }
    auto r = ::sendto(s_, reinterpret_cast<const char*>(data), int(n), 0, sa(dst), socklen_t(dst.len));
    return r == decltype(r)(n);
}

int UdpSocket::recvFrom(Address& from, uint8_t* buf, size_t cap) {
    if (!isOpen()) return -1;
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    auto r = ::recvfrom(s_, reinterpret_cast<char*>(buf), int(cap), 0, reinterpret_cast<sockaddr*>(&ss), &len);
    if (r < 0) return -1; // would block, or an ICMP error surfaced on Windows: ignore either way
    from = Address{};
    // Normalize v4-mapped IPv6 to plain IPv4 so the same peer always compares equal.
    if (ss.ss_family == AF_INET6) {
        auto* v6 = reinterpret_cast<sockaddr_in6*>(&ss);
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&v6->sin6_addr);
        static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (std::memcmp(b, prefix, 12) == 0) {
            sockaddr_in v4{};
            v4.sin_family = AF_INET; v4.sin_port = v6->sin6_port;
            std::memcpy(&v4.sin_addr, b + 12, 4);
            std::memcpy(from.storage, &v4, sizeof(v4));
            from.len = sizeof(v4);
            return int(r);
        }
    }
    std::memcpy(from.storage, &ss, len);
    from.len = uint32_t(len);
    return int(r);
}

uint16_t UdpSocket::localPort() const {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (getsockname(s_, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return 0;
    if (ss.ss_family == AF_INET6) return ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
    return ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
}

// ---------------------------------------------------------------- TCP
bool TcpSocket::connect(const Address& to) {
    initSockets();
    close();
    auto s = ::socket(to.family(), SOCK_STREAM, IPPROTO_TCP);
    if (s == RTS_INVALID) return false;
    setNonBlocking(s);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (::connect(s, sa(to), socklen_t(to.len)) != 0 && !wouldBlock()) { closeSock(s); return false; }
    s_ = SocketHandle(s);
    return true;
}

int TcpSocket::pollConnected() {
    if (!isOpen()) return -1;
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(s_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0 || err != 0) return -1;
    sockaddr_storage ss{};
    socklen_t sl = sizeof(ss);
    return getpeername(s_, reinterpret_cast<sockaddr*>(&ss), &sl) == 0 ? 1 : 0;
}

int TcpSocket::send(const uint8_t* data, size_t n) {
    if (!isOpen()) return -1;
    auto r = ::send(s_, reinterpret_cast<const char*>(data), int(n), MSG_NOSIGNAL);
    if (r < 0) return wouldBlock() ? 0 : -1;
    return int(r);
}

int TcpSocket::recv(uint8_t* buf, size_t cap) {
    if (!isOpen()) return -1;
    auto r = ::recv(s_, reinterpret_cast<char*>(buf), int(cap), 0);
    if (r == 0) return -1;              // orderly shutdown
    if (r < 0) return wouldBlock() ? 0 : -1;
    return int(r);
}

void TcpSocket::close() { if (s_ != SocketHandle(RTS_INVALID)) { closeSock(s_); s_ = SocketHandle(RTS_INVALID); } }
bool TcpSocket::isOpen() const { return s_ != SocketHandle(RTS_INVALID); }

bool TcpListener::listen(uint16_t port) {
    initSockets();
    close();
    for (int fam : {AF_INET6, AF_INET}) {
        auto s = ::socket(fam, SOCK_STREAM, IPPROTO_TCP);
        if (s == RTS_INVALID) continue;
        int one = 1, off = 0;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
        sockaddr_storage ss{};
        socklen_t len;
        if (fam == AF_INET6) {
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
            auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
            a->sin6_family = AF_INET6; a->sin6_addr = in6addr_any; a->sin6_port = htons(port);
            len = sizeof(sockaddr_in6);
        } else {
            auto* a = reinterpret_cast<sockaddr_in*>(&ss);
            a->sin_family = AF_INET; a->sin_addr.s_addr = htonl(INADDR_ANY); a->sin_port = htons(port);
            len = sizeof(sockaddr_in);
        }
        if (::bind(s, reinterpret_cast<sockaddr*>(&ss), len) != 0 || ::listen(s, 64) != 0) { closeSock(s); continue; }
        setNonBlocking(s);
        s_ = SocketHandle(s);
        return true;
    }
    return false;
}

std::unique_ptr<TcpSocket> TcpListener::accept(Address* from) {
    if (s_ == SocketHandle(RTS_INVALID)) return nullptr;
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    auto c = ::accept(s_, reinterpret_cast<sockaddr*>(&ss), &len);
    if (c == RTS_INVALID) return nullptr;
    setNonBlocking(c);
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (from) { *from = Address{}; std::memcpy(from->storage, &ss, len); from->len = uint32_t(len); }
    return std::make_unique<TcpSocket>(SocketHandle(c));
}

void TcpListener::close() { if (s_ != SocketHandle(RTS_INVALID)) { closeSock(s_); s_ = SocketHandle(RTS_INVALID); } }

uint16_t TcpListener::localPort() const {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (getsockname(s_, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return 0;
    if (ss.ss_family == AF_INET6) return ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
    return ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
}

} // namespace rts::net
#endif
