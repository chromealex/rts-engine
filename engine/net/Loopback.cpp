#include "Conn.h"
#include <chrono>
#include <deque>
#include "../core/Random.h"

namespace rts::net {

uint64_t nowMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

namespace {

struct Wire {
    struct Item { uint64_t at; Bytes data; };
    std::deque<Item> q;
    uint64_t lastAt = 0;
};

struct Shared {
    Wire ab, ba;              // a->b, b->a
    bool closed = false;
    LoopbackOptions opt;
    Random rng;
    uint64_t now = 0;         // newest time seen from either side
};

class LoopbackConn final : public Conn {
public:
    LoopbackConn(std::shared_ptr<Shared> s, bool isA) : s_(std::move(s)), a_(isA) {}
    ~LoopbackConn() override { s_->closed = true; }
    void send(const Bytes& msg) override {
        if (s_->closed) return;
        Wire& w = a_ ? s_->ab : s_->ba;
        uint64_t at = s_->now + s_->opt.latencyMs + (s_->opt.jitterMs ? s_->rng.below(s_->opt.jitterMs + 1) : 0);
        if (at < w.lastAt) at = w.lastAt;
        w.lastAt = at;
        w.q.push_back({at, msg});
    }
    bool recv(Bytes& msg) override {
        Wire& w = a_ ? s_->ba : s_->ab;
        if (w.q.empty() || w.q.front().at > s_->now) return false;
        msg = std::move(w.q.front().data);
        w.q.pop_front();
        return true;
    }
    void update(uint64_t now) override { if (now > s_->now) s_->now = now; }
    ConnState state() const override { return s_->closed ? ConnState::Closed : ConnState::Connected; }
    void close() override { s_->closed = true; }
    uint32_t rttMs() const override { return 2 * s_->opt.latencyMs; }
    std::string describe() const override { return a_ ? "loopback-a" : "loopback-b"; }

private:
    std::shared_ptr<Shared> s_;
    bool a_;
};

} // namespace

std::pair<std::unique_ptr<Conn>, std::unique_ptr<Conn>> makeLoopbackPair(LoopbackOptions opt) {
    auto s = std::make_shared<Shared>();
    s->opt = opt;
    s->rng.reseed(opt.seed);
    return {std::make_unique<LoopbackConn>(s, true), std::make_unique<LoopbackConn>(s, false)};
}

} // namespace rts::net
