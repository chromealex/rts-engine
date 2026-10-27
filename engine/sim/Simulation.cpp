#include "Simulation.h"
#include <algorithm>

namespace rts {

void writeCommand(ByteWriter& w, const Command& c) {
    w.u32(c.tick); w.u8(c.player); w.u8(c.flags); w.u16(c.type); w.u32(c.seq);
    w.u32(uint32_t(c.units.size()));
    for (auto& e : c.units) w.u64(e.packed());
    w.u64(c.target.packed());
    w.i64(c.point.x.raw); w.i64(c.point.y.raw); w.i64(c.point.z.raw);
    w.i64(c.param);
}

Command readCommand(ByteReader& r) {
    Command c;
    c.tick = r.u32(); c.player = r.u8(); c.flags = r.u8(); c.type = r.u16(); c.seq = r.u32();
    uint32_t n = r.u32();
    if (n > 100000) n = 0; // corrupted input guard
    c.units.reserve(n);
    for (uint32_t i = 0; i < n && r.ok(); ++i) c.units.push_back(Entity::fromPacked(r.u64()));
    c.target = Entity::fromPacked(r.u64());
    c.point.x = Fixed::fromRaw(r.i64()); c.point.y = Fixed::fromRaw(r.i64()); c.point.z = Fixed::fromRaw(r.i64());
    c.param = r.i64();
    return c;
}

Simulation::Simulation(SimConfig cfg, std::unique_ptr<GameModule> module)
    : cfg_(cfg), module_(std::move(module)), rng_(cfg.seed), tickRate_(cfg.tickRate ? cfg.tickRate : 20),
      dt_(Fixed::fromRatio(1, tickRate_)) {}

void Simulation::setup(const Json& rules, const Json& map) {
    players_.clear();
    const Json& ps = map["players"];
    for (size_t i = 0; i < ps.size(); ++i) {
        PlayerInfo p;
        p.slot = uint8_t(i + 1);
        p.name = ps[i].getStr("name", "Player " + std::to_string(i + 1));
        p.team = uint8_t(ps[i].getInt("team", int64_t(i + 1)));
        players_.push_back(p);
    }
    module_->setup(*this, rules, map);
}

PlayerInfo* Simulation::player(uint8_t slot) {
    return slot >= 1 && slot <= players_.size() ? &players_[slot - 1] : nullptr;
}

bool Simulation::allied(uint8_t a, uint8_t b) {
    if (a == b) return true;
    auto* pa = player(a); auto* pb = player(b);
    return pa && pb && pa->team == pb->team;
}

uint32_t Simulation::submit(Command cmd) {
    if (sink_) return sink_(std::move(cmd));
    if (cmd.tick <= tick_) cmd.tick = tick_ + cfg_.inputDelayTicks;
    if (cmd.seq == 0) cmd.seq = ++seqByPlayer_[cmd.player];
    const uint32_t t = cmd.tick;
    pending_[t].push_back(std::move(cmd));
    return t;
}

void Simulation::inject(Command cmd) {
    const uint32_t t = cmd.tick;
    pending_[t].push_back(std::move(cmd));
}

void Simulation::step() {
    events_.clear();

    auto it = pending_.find(tick_);
    if (it != pending_.end()) {
        auto cmds = std::move(it->second);
        pending_.erase(it);
        // Network arrival order differs per client: impose a canonical order.
        std::sort(cmds.begin(), cmds.end(), [](const Command& a, const Command& b) {
            return a.player != b.player ? a.player < b.player : a.seq < b.seq;
        });
        for (auto& c : cmds) {
            module_->applyCommand(*this, c);
            executed_.push_back(std::move(c));
        }
        world_.flush();
    }

    for (auto& s : systems_) {
        s.fn(*this);
        world_.flush();
    }
    ++tick_;
}

int Simulation::advance(double realSeconds, int maxTicks) {
    const double tickLen = 1.0 / tickRate_;
    accumulator_ += realSeconds;
    int n = 0;
    while (accumulator_ >= tickLen && n < maxTicks) { step(); accumulator_ -= tickLen; ++n; }
    if (n == maxTicks && accumulator_ > tickLen) accumulator_ = tickLen; // spiral-of-death guard
    return n;
}

void Simulation::saveState(SimSnapshot& out) const {
    out.world = world_;
    out.rng = rng_;
    out.tick = tick_;
    out.pending = pending_;
    out.executedCount = executed_.size();
    out.players = players_;
    out.seqByPlayer = seqByPlayer_;
}

void Simulation::loadState(const SimSnapshot& in) {
    world_ = in.world;
    rng_ = in.rng;
    tick_ = in.tick;
    pending_ = in.pending;
    if (executed_.size() > in.executedCount) executed_.resize(in.executedCount);
    players_ = in.players;
    seqByPlayer_ = in.seqByPlayer;
    events_.clear();
    accumulator_ = 0.0;
}

uint64_t Simulation::checksum() {
    Hasher h;
    h.u32(tick_);
    h.u64(rng_.state());
    for (auto& p : players_) { h.u32(p.slot); h.u32(p.team); h.u32(p.alive ? 1 : 0); }
    module_->hashState(*this, h);
    return h.value();
}

} // namespace rts
