// Lockstep with rollback: drives a Simulation from network input.
//
// The server is the authority on input: it "confirms" each tick with the full list of commands
// (everyone's input that arrived in time; late commands move to a later tick, so one lagging
// player never freezes the others). The client does not wait for that: it runs ahead with
// its own commands and predicts "no commands" for everyone else, saving a snapshot before every
// unconfirmed tick. When a confirmation differs from the prediction, it restores the snapshot and
// re-simulates up to where it was. Commands still execute inputDelay ticks after being issued, so
// with a normal ping most confirmations arrive before the tick is simulated and nothing rolls back.
//
// rollbackWindow limits how far ahead of confirmation the client may run (re-simulation cost on
// weak devices); 0 turns this into classic lockstep that waits for every tick.
//
// Presentation note: after a rollback, sim.events() of re-simulated ticks are not replayed, and
// predicted ticks may have produced events that "never happened"; sounds/VFX tolerate that.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <vector>
#include "../sim/Simulation.h"

namespace rts::net {

struct SessionConfig {
    uint32_t inputDelay = 2;
    uint32_t rollbackWindow = 8;
    uint32_t checksumInterval = 20;
    uint32_t catchUpBudgetMs = 12;   // max time per update() spent fast-forwarding when far behind
};

struct SessionStats {
    uint32_t rollbacks = 0, resimulatedTicks = 0, maxRollback = 0;
    uint32_t stallUpdates = 0;       // updates where the window blocked progress
    uint32_t predictedTicks = 0;     // ticks simulated ahead of confirmation
};

class RollbackSession {
public:
    using SendFrame = std::function<void(uint32_t tick, const std::vector<Command>& cmds)>;
    using SendChecksum = std::function<void(uint32_t tick, uint64_t hash)>;

    // controlled: player slots whose commands this client issues (empty = spectator).
    // Installs a command sink on the sim: from now on sim.submit() goes to the network.
    RollbackSession(Simulation& sim, SessionConfig cfg, std::vector<uint8_t> controlled, SendFrame sendFrame, SendChecksum sendChecksum);
    ~RollbackSession();
    RollbackSession(const RollbackSession&) = delete;
    RollbackSession& operator=(const RollbackSession&) = delete;

    // Confirmed input for ticks [first, first + count); must continue exactly at confirmedNext().
    void confirm(uint32_t first, uint32_t count, const std::vector<Command>& cmds);
    // Server clock sample: its nominal tick when the message left, plus our round-trip estimate.
    void serverTime(uint32_t serverTick, uint32_t rttMs, uint64_t nowMs);

    // Advance by real time. Returns ticks newly simulated (not counting re-simulation).
    int update(double seconds, uint64_t nowMs);

    void setControlled(std::vector<uint8_t> slots) { controlled_ = std::move(slots); }
    // After a reconnect: frames sent on the dead connection may never have arrived.
    void resendUnconfirmed() { for (auto& [tick, cmds] : local_) sendFrame_(tick, cmds); }
    const std::vector<uint8_t>& controlled() const { return controlled_; }
    uint32_t confirmedNext() const { return confirmedNext_; }
    uint32_t tick() const { return sim_.tick(); }
    // Ticks the sim runs ahead of the server's clock estimate (negative = behind).
    int32_t drift(uint64_t nowMs) const;
    // Winner team / -1 draw, once the result holds on confirmed state; 0 while undecided.
    int confirmedResult() const { return result_; }
    uint32_t resultTick() const { return resultTick_; }
    uint64_t resultHash() const { return resultHash_; }
    const SessionStats& stats() const { return stats_; }
    bool broken() const { return broken_; }    // internal invariant failed (should never happen)

private:
    bool canStep() const { return sim_.tick() < confirmedNext_ + window_; }
    void stepTick(bool resim);
    void rollback();
    uint32_t submitLocal(Command&& c);

    Simulation& sim_;
    SessionConfig cfg_;
    uint32_t window_;
    std::vector<uint8_t> controlled_;
    SendFrame sendFrame_;
    SendChecksum sendChecksum_;

    std::vector<Command> collecting_;                    // issued since the last frame was sent
    std::map<uint32_t, std::vector<Command>> local_;     // own sent frames, not yet confirmed
    std::map<uint32_t, std::vector<Command>> confirmed_; // confirmed but not yet simulated, or kept for rollback
    std::map<uint32_t, std::vector<Command>> applied_;   // what predicted ticks actually executed
    std::map<uint32_t, uint64_t> hashes_;                // checksum at tick K (state after K-1), not yet sent
    std::vector<SimSnapshot> ring_;
    std::vector<uint32_t> ringTick_;
    uint32_t confirmedNext_ = 0;
    uint32_t nextFrame_ = 0;                             // next frame tick to send
    uint32_t rollbackTo_ = UINT32_MAX;
    double acc_ = 0;
    uint32_t serverTick_ = 0, rtt_ = 0;
    uint64_t serverAt_ = 0;
    bool haveServerTime_ = false;
    int result_ = 0;                 // confirmed
    uint32_t resultTick_ = 0;
    uint64_t resultHash_ = 0;
    int candResult_ = 0;             // first tick with a result, possibly still predicted
    uint32_t candTick_ = 0;
    uint64_t candHash_ = 0;
    SessionStats stats_;
    bool broken_ = false;
};

} // namespace rts::net
