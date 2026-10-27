#include "Rollback.h"
#include <algorithm>
#include <chrono>

namespace rts::net {

static bool canonicalLess(const Command& a, const Command& b) {
    return a.player != b.player ? a.player < b.player : a.seq < b.seq;
}

RollbackSession::RollbackSession(Simulation& sim, SessionConfig cfg, std::vector<uint8_t> controlled, SendFrame sendFrame,
                                 SendChecksum sendChecksum)
    : sim_(sim), cfg_(cfg), window_(controlled.empty() ? 0 : cfg.rollbackWindow), controlled_(std::move(controlled)),
      sendFrame_(std::move(sendFrame)), sendChecksum_(std::move(sendChecksum)) {
    ring_.resize(window_ + 2);
    ringTick_.assign(ring_.size(), UINT32_MAX);
    confirmedNext_ = sim_.tick();
    nextFrame_ = sim_.tick() + cfg_.inputDelay;
    sim_.setCommandSink([this](Command&& c) { return submitLocal(std::move(c)); });
}

RollbackSession::~RollbackSession() { sim_.setCommandSink(nullptr); }

uint32_t RollbackSession::submitLocal(Command&& c) {
    if (std::find(controlled_.begin(), controlled_.end(), c.player) == controlled_.end()) return 0; // not ours: ignore
    collecting_.push_back(std::move(c));
    return std::max(nextFrame_, sim_.tick() + cfg_.inputDelay);
}

void RollbackSession::confirm(uint32_t first, uint32_t count, const std::vector<Command>& cmds) {
    if (first + count <= confirmedNext_) return;           // duplicate (e.g. history after a resume)
    if (first > confirmedNext_) { broken_ = true; return; }
    std::map<uint32_t, std::vector<Command>> byTick;
    for (auto& c : cmds) if (c.tick >= first && c.tick < first + count) byTick[c.tick].push_back(c);
    for (uint32_t t = confirmedNext_; t < first + count; ++t) {
        auto& list = byTick[t];
        std::sort(list.begin(), list.end(), canonicalLess);
        if (t < sim_.tick()) {
            auto it = applied_.find(t);
            const std::vector<Command> none;
            if (!(list == (it != applied_.end() ? it->second : none))) rollbackTo_ = std::min(rollbackTo_, t);
            if (it != applied_.end()) applied_.erase(it);
        }
        confirmed_[t] = std::move(list);
        local_.erase(t);
    }
    confirmedNext_ = first + count;
    // Confirmed inputs below the oldest tick we could still roll back to are no longer needed.
    const uint32_t keepFrom = std::min({sim_.tick(), rollbackTo_, confirmedNext_});
    while (!confirmed_.empty() && confirmed_.begin()->first + 1 < keepFrom) confirmed_.erase(confirmed_.begin());
}

void RollbackSession::serverTime(uint32_t serverTick, uint32_t rttMs, uint64_t nowMs) {
    serverTick_ = serverTick; rtt_ = rttMs; serverAt_ = nowMs; haveServerTime_ = true;
}

int32_t RollbackSession::drift(uint64_t nowMs) const {
    if (!haveServerTime_) return 0;
    const double tickMs = 1000.0 / sim_.tickRate();
    const double est = serverTick_ + (double(nowMs - serverAt_) + rtt_ * 0.5) / tickMs;
    return int32_t(double(sim_.tick()) - est);
}

void RollbackSession::stepTick(bool resim) {
    const uint32_t s = sim_.tick();
    if (!resim) {
        // Close the frame for tick s + delay: everything issued until now executes then.
        const uint32_t f = s + cfg_.inputDelay;
        if (f >= nextFrame_ && !controlled_.empty()) {
            std::map<uint8_t, uint32_t> seq;
            for (auto& c : collecting_) { c.tick = f; c.seq = ++seq[c.player]; }
            std::sort(collecting_.begin(), collecting_.end(), canonicalLess);
            sendFrame_(f, collecting_);
            if (f >= confirmedNext_) local_[f] = collecting_;
            collecting_.clear();
            nextFrame_ = f + 1;
        }
    }
    std::vector<Command> inputs;
    if (s < confirmedNext_) {
        if (auto it = confirmed_.find(s); it != confirmed_.end()) inputs = it->second;
    } else {
        const size_t slot = s % ring_.size();
        sim_.saveState(ring_[slot]);
        ringTick_[slot] = s;
        if (auto it = local_.find(s); it != local_.end()) inputs = it->second;
        applied_[s] = inputs;
        ++stats_.predictedTicks;
    }
    for (auto& c : inputs) sim_.inject(c);
    sim_.step();
    const uint32_t now = sim_.tick();
    if (cfg_.checksumInterval && now % cfg_.checksumInterval == 0) hashes_[now] = sim_.checksum();
    if (!result_ && !candResult_) {
        if (int r = sim_.result()) { candResult_ = r; candTick_ = now; candHash_ = sim_.checksum(); }
    }
}

void RollbackSession::rollback() {
    const uint32_t from = rollbackTo_;
    rollbackTo_ = UINT32_MAX;
    const uint32_t target = sim_.tick();
    if (from >= target) return;
    const size_t slot = from % ring_.size();
    if (ringTick_[slot] != from) { broken_ = true; return; }
    sim_.loadState(ring_[slot]);
    ++stats_.rollbacks;
    stats_.maxRollback = std::max(stats_.maxRollback, target - from);
    for (auto it = hashes_.begin(); it != hashes_.end();) it = it->first > from ? hashes_.erase(it) : std::next(it);
    if (candResult_ && candTick_ > from) candResult_ = 0;
    while (sim_.tick() < target) { stepTick(true); ++stats_.resimulatedTicks; }
}

int RollbackSession::update(double seconds, uint64_t nowMs) {
    if (broken_) return 0;
    if (rollbackTo_ != UINT32_MAX) rollback();

    const double tickLen = 1.0 / sim_.tickRate();
    // Pace to the server clock: speed up when behind, slow down when ahead.
    const int32_t d = drift(nowMs);
    double speed = 1.0;
    if (d < -2) speed = std::min(1.0 + 0.1 * double(-d), 2.0);
    else if (d > 2) speed = std::max(1.0 - 0.1 * double(d), 0.5);
    acc_ += seconds * speed;

    int stepped = 0;
    const bool farBehind = d < -int32_t(sim_.tickRate()); // more than a second: reconnect / resume
    if (farBehind) {
        const auto t0 = std::chrono::steady_clock::now();
        while (canStep() && drift(nowMs) < 0) {
            stepTick(false); ++stepped;
            if ((stepped & 15) == 0 && std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(cfg_.catchUpBudgetMs)) break;
        }
        acc_ = 0;
    } else {
        while (acc_ >= tickLen && stepped < 8) {
            if (!canStep()) { ++stats_.stallUpdates; acc_ = std::min(acc_, tickLen); break; }
            stepTick(false); ++stepped;
            acc_ -= tickLen;
        }
        if (acc_ > tickLen * 4) acc_ = tickLen * 4;
    }

    if (!result_ && candResult_ && candTick_ <= confirmedNext_ && rollbackTo_ == UINT32_MAX) {
        result_ = candResult_; resultTick_ = candTick_; resultHash_ = candHash_;
    }
    // Checksums of states that can no longer change.
    while (!hashes_.empty() && hashes_.begin()->first <= confirmedNext_ && rollbackTo_ == UINT32_MAX) {
        sendChecksum_(hashes_.begin()->first, hashes_.begin()->second);
        hashes_.erase(hashes_.begin());
    }
    return stepped;
}

} // namespace rts::net
