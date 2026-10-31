#include "modules/rts/RtsNative.h"
#include "modules/rts/RtsModule.h"
#include <stdexcept>

namespace {
class Income final : public rts::game::NativeModule {
public:
    void setup(rts::game::RtsModule& game, rts::Simulation&, rts::game::NativeState& state,
               const rts::Json& config) const override {
        const auto resource = config.getStr("resource", "gold");
        const int id = game.rules().findResource(resource);
        if (id < 0) throw std::runtime_error("native module 'income': unknown resource '" + resource + "'");
        const rts::Fixed amount = config.getFx("amount", rts::Fixed::fromInt(5));
        if (amount.raw < 0) throw std::runtime_error("native module 'income': amount must be non-negative");
        state.values["resource"] = id;
        state.values["amount"] = amount.raw;
    }
    void tick(rts::game::RtsModule& game, rts::Simulation& sim, rts::game::NativeState& state) const override {
        if (sim.tick() % sim.tickRate() != 0) return;
        auto& players = game.state(sim).players;
        const size_t resource = size_t(state.values.at("resource"));
        const auto amount = rts::Fixed::fromRaw(state.values.at("amount"));
        for (const auto& player : sim.players()) {
            if (player.alive && player.slot < players.size()) players[player.slot].stock[resource] += amount;
        }
    }
};
}
std::unique_ptr<rts::game::NativeModule> createIncomeModule() { return std::make_unique<Income>(); }
