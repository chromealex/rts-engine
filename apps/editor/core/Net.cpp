#include "Net.h"
#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socklen_t = int;
#define RTS_CLOSE closesocket
static void netInit() { static bool done = false; if (!done) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); done = true; } }
static void setNonBlocking(intptr_t fd) { u_long m = 1; ioctlsocket(SOCKET(fd), FIONBIO, &m); }
static bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define RTS_CLOSE ::close
static void netInit() {}
static void setNonBlocking(intptr_t fd) { fcntl(int(fd), F_SETFL, fcntl(int(fd), F_GETFL, 0) | O_NONBLOCK); }
static bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS; }
#endif

#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

namespace rts::editor::net {

namespace {
bool sendAll(intptr_t fd, const std::string& data) {
    size_t off = 0;
    auto start = std::chrono::steady_clock::now();
    while (off < data.size()) {
        auto n = ::send(fd, data.data() + off, int(data.size() - off), MSG_NOSIGNAL);
        if (n > 0) { off += size_t(n); continue; }
        if (n < 0 && wouldBlock()) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) return false;
            continue;
        }
        return false;
    }
    return true;
}
sockaddr_in loopback(int port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}
} // namespace

LineServer::~LineServer() {
    for (auto& c : conns_) RTS_CLOSE(c.fd);
    if (fd_ >= 0) RTS_CLOSE(fd_);
}

bool LineServer::listen(int port) {
    netInit();
    intptr_t fd = intptr_t(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) return false;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
    sockaddr_in a = loopback(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 4) != 0) { RTS_CLOSE(fd); return false; }
    setNonBlocking(fd);
    fd_ = fd;
    port_ = port;
    return true;
}

std::vector<LineServer::Msg> LineServer::poll() {
    std::vector<Msg> out;
    if (fd_ < 0) return out;
    for (;;) {
        intptr_t c = intptr_t(::accept(fd_, nullptr, nullptr));
        if (c < 0) break;
        setNonBlocking(c);
        conns_.push_back({c, {}});
    }
    char buf[65536];
    for (size_t i = 0; i < conns_.size();) {
        bool closed = false;
        for (;;) {
            auto n = ::recv(conns_[i].fd, buf, sizeof buf, 0);
            if (n > 0) { conns_[i].in.append(buf, size_t(n)); continue; }
            if (n == 0 || !wouldBlock()) closed = true;
            break;
        }
        size_t nl;
        while ((nl = conns_[i].in.find('\n')) != std::string::npos) {
            out.push_back({int(conns_[i].fd), conns_[i].in.substr(0, nl)});
            conns_[i].in.erase(0, nl + 1);
        }
        if (closed) { RTS_CLOSE(conns_[i].fd); conns_.erase(conns_.begin() + long(i)); }
        else ++i;
    }
    return out;
}

void LineServer::send(int client, const std::string& line) {
    for (auto& c : conns_) if (int(c.fd) == client) { sendAll(c.fd, line + "\n"); return; }
}

LineClient::~LineClient() { close(); }

void LineClient::close() {
    if (fd_ >= 0) RTS_CLOSE(fd_);
    fd_ = -1;
    buf_.clear();
}

bool LineClient::connect(int port, int timeoutMs) {
    netInit();
    close();
    intptr_t fd = intptr_t(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) return false;
    sockaddr_in a = loopback(port);
    setNonBlocking(fd);
    int r = ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    if (r != 0 && !wouldBlock()) { RTS_CLOSE(fd); return false; }
    if (r != 0) {
        fd_set w; FD_ZERO(&w); FD_SET(fd, &w);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (::select(int(fd + 1), nullptr, &w, nullptr, &tv) <= 0) { RTS_CLOSE(fd); return false; }
        int err = 0;
        socklen_t len = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err != 0) { RTS_CLOSE(fd); return false; }
    }
    fd_ = fd;
    return true;
}

bool LineClient::send(const std::string& line) {
    if (fd_ < 0) return false;
    if (!sendAll(fd_, line + "\n")) { close(); return false; }
    return true;
}

bool LineClient::recv(std::string& line, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char buf[65536];
    for (;;) {
        size_t nl = buf_.find('\n');
        if (nl != std::string::npos) { line = buf_.substr(0, nl); buf_.erase(0, nl + 1); return true; }
        if (fd_ < 0) return false;
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
        fd_set rset; FD_ZERO(&rset); FD_SET(fd_, &rset);
        timeval tv{long(left / 1000), long((left % 1000) * 1000)};
        if (::select(int(fd_ + 1), &rset, nullptr, nullptr, &tv) <= 0) continue;
        auto n = ::recv(fd_, buf, sizeof buf, 0);
        if (n > 0) buf_.append(buf, size_t(n));
        else if (n == 0 || !wouldBlock()) { close(); return false; }
    }
}

} // namespace rts::editor::net
