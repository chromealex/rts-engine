#include "RtsNative.h"
#include "RtsModule.h"
#include <set>
#include <stdexcept>

namespace rts::game {
namespace {
void hashState(Hasher& h, const NativeState& s) {
    h.u32(uint32_t(s.values.size()));
    for (const auto& [key, value] : s.values) { h.str(key); h.i64(value); }
    h.u32(uint32_t(s.arrays.size()));
    for (const auto& [key, values] : s.arrays) {
        h.str(key); h.u32(uint32_t(values.size()));
        for (auto v : values) h.i64(v);
    }
}
}
void NativeModules::load(RtsModule& rts, Simulation& sim, const Json& rules,
                         const std::vector<NativeDefinition>& definitions) {
    entries_.clear();
    if (!rules.has("nativeModules")) return;
    const auto& list = rules["nativeModules"];
    if (!list.isArray()) throw std::runtime_error("nativeModules: expected an array");
    std::set<std::string> ids;
    std::set<uint16_t> commands;
    for (const auto& item : list.items()) {
        const std::string id = item.getStr("id");
        if (!item.isObject() || id.empty()) throw std::runtime_error("nativeModules: expected object with id");
        if (!ids.insert(id).second) throw std::runtime_error("native module '" + id + "': duplicate id");
        const NativeDefinition* def = nullptr;
        for (const auto& d : definitions) if (d.id == id) {
            if (def) throw std::runtime_error("native module '" + id + "': duplicate factory");
            def = &d;
        }
        if (!def || !def->create) throw std::runtime_error("native module '" + id + "': not compiled into this build");
        Json config = item.has("config") ? item["config"] : Json::object();
        if (!config.isObject()) throw std::runtime_error("native module '" + id + "': config must be an object");
        // One command type per module, with param/flags for module-specific actions.
        int64_t command = item.getInt("command");
        if (item.has("command") && (!item["command"].isNumber() || command < 32768 || command > 65535 ||
                                    item["command"].fx() != Fixed::fromInt(command)))
            throw std::runtime_error("native module '" + id + "': command must be an integer in [32768, 65535]");
        if (command && !commands.insert(uint16_t(command)).second)
            throw std::runtime_error("native module '" + id + "': duplicate command");
        auto instance = def->create();
        if (!instance) throw std::runtime_error("native module '" + id + "': factory returned null");
        entries_.push_back({id, config.dump(-1), def->version, uint16_t(command), std::move(instance)});
    }
    if (entries_.empty()) return;
    auto& store = sim.world().setResource<NativeStore>();
    store.modules.resize(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i) {
        entries_[i].instance->setup(rts, sim, store.modules[i], Json::parse(entries_[i].config));
        sim.world().flush();
    }
    sim.addSystem("nativeModules", [this, &rts](Simulation& s) { tick(rts, s); });
}
void NativeModules::tick(RtsModule& rts, Simulation& sim) const {
    for (size_t i = 0; i < entries_.size(); ++i) {
        auto& state = sim.world().resource<NativeStore>()->modules[i];
        entries_[i].instance->tick(rts, sim, state);
        sim.world().flush();
    }
}
void NativeModules::command(RtsModule& rts, Simulation& sim, const Command& cmd) const {
    for (size_t i = 0; i < entries_.size(); ++i) if (entries_[i].command && entries_[i].command == cmd.type) {
        entries_[i].instance->command(rts, sim, sim.world().resource<NativeStore>()->modules[i], cmd);
        return;
    }
}
void NativeModules::hash(Simulation& sim, Hasher& h) const {
    if (entries_.empty()) return; // preserve existing games' checksums
    h.str("nativeModules"); h.u32(uint32_t(entries_.size()));
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        h.str(e.id); h.u32(e.version); h.str(e.config); h.u32(e.command);
        hashState(h, sim.world().resource<NativeStore>()->modules[i]);
    }
}
bool NativeModules::serialize(Simulation& sim, Archive& a) const {
    if (entries_.empty()) return a.ok(); // preserve old save format without extensions
    uint32_t count = uint32_t(entries_.size());
    a(count);
    if (count != entries_.size()) { a.fail(); return false; }
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        std::string id = e.id, config = e.config;
        uint32_t version = e.version;
        uint16_t command = e.command;
        a(id, version, config, command);
        if (id != e.id || version != e.version || config != e.config || command != e.command) { a.fail(); return false; }
        auto& s = sim.world().resource<NativeStore>()->modules[i];
        a(s.values, s.arrays);
    }
    return a.ok();
}
} // namespace rts::game
