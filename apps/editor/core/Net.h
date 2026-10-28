// Minimal localhost TCP with newline-delimited JSON messages: the running editor listens, the MCP
// server connects. Non-blocking server side so the editor polls it once per frame on the UI thread.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rts::editor::net {

constexpr int kDefaultPort = 47321;

class LineServer {
public:
    ~LineServer();
    bool listen(int port);                 // 127.0.0.1 only
    // Accepts connections and returns complete lines received since the last poll, tagged by client.
    struct Msg { int client; std::string line; };
    std::vector<Msg> poll();
    void send(int client, const std::string& line);   // appends '\n'
    bool listening() const { return fd_ >= 0; }
    int port() const { return port_; }
    int clients() const { return int(conns_.size()); }   // connected MCP servers (status pill)

private:
    struct Conn { intptr_t fd; std::string in; };
    intptr_t fd_ = -1;
    int port_ = 0;
    std::vector<Conn> conns_;
};

class LineClient {
public:
    ~LineClient();
    bool connect(int port, int timeoutMs = 300);
    bool send(const std::string& line);
    bool recv(std::string& line, int timeoutMs);      // waits for one full line
    void close();
    bool connected() const { return fd_ >= 0; }

private:
    intptr_t fd_ = -1;
    std::string buf_;
};

} // namespace rts::editor::net
