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

// ---------------------------------------------------------------- save states
namespace {
constexpr uint32_t kSaveMagic = 0x56535452;   // "RTSV"
constexpr uint32_t kSaveVersion = 1;
} // namespace
void io(Archive& a, Command& c) { a(c.tick, c.player, c.flags, c.type, c.seq, c.units, c.target, c.point, c.param); }   // found by ADL
void io(Archive& a, PlayerInfo& p) { a(p.slot, p.name, p.team, p.alive); }

std::string Simulation::serialize() {
    std::string out;
    Archive a(&out);
    uint32_t magic = kSaveMagic, version = kSaveVersion, rate = tickRate_;
    uint64_t seed = cfg_.seed, st = rng_.state(), inc = rng_.increment();
    a(magic, version, seed, rate, tick_, st, inc, players_, pending_, executed_, seqByPlayer_);
    World::EntityState es = world_.entityState();
    a(es.gens, es.alive, es.free, es.pendingDestroy);
    if (!module_->serializeState(*this, a)) return {};
    uint64_t cs = checksum();
    a(cs);
    return out;
}

bool Simulation::deserialize(const std::string& bytes, std::string* err) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    Archive a(bytes);
    uint32_t magic = 0, version = 0, rate = 0, tick = 0;
    uint64_t seed = 0, st = 0, inc = 0;
    a(magic, version, seed, rate);
    if (!a.ok() || magic != kSaveMagic) return fail("not a save state");
    if (version != kSaveVersion) return fail("save state version " + std::to_string(version) + ", this build reads " + std::to_string(kSaveVersion));
    if (rate != tickRate_) return fail("tick rate differs");
    std::vector<PlayerInfo> players;
    std::map<uint32_t, std::vector<Command>> pending;
    std::vector<Command> executed;
    std::map<uint8_t, uint32_t> seq;
    a(tick, st, inc, players, pending, executed, seq);
    World::EntityState es;
    a(es.gens, es.alive, es.free, es.pendingDestroy);
    if (!a.ok()) return fail("truncated save state");
    if (players.size() != players_.size()) return fail("player count differs (load into a simulation set up from the same map)");
    tick_ = tick;
    rng_.restore(st, inc);
    players_ = std::move(players);
    pending_ = std::move(pending);
    executed_ = std::move(executed);
    seqByPlayer_ = std::move(seq);
    world_.restoreEntityState(std::move(es));
    events_.clear();
    accumulator_ = 0.0;
    if (!module_->serializeState(*this, a) || !a.ok()) return fail("corrupt module state");
    uint64_t cs = 0;
    a(cs);
    if (!a.ok()) return fail("truncated save state");
    if (checksum() != cs) return fail("state checksum mismatch after loading (different rules or map?)");
    (void)seed;
    return true;
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
