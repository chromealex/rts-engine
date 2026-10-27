// Replay = setup parameters + command stream + periodic checksums.
// The same format is what a lockstep client would log; a checksum mismatch pinpoints the
// first tick where two machines (or two builds) diverged.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include "Command.h"

namespace rts {

struct Replay {
    static constexpr uint32_t kMagic = 0x50525452; // "RTRP"
    static constexpr uint32_t kVersion = 1;

    std::string module;
    std::string rulesPath, mapPath;
    uint64_t dataHash = 0;     // hash of rules+map text: playback refuses mismatching data
    uint64_t seed = 0;
    uint32_t tickRate = 20;
    uint32_t inputDelay = 2;
    uint32_t endTick = 0;
    uint64_t finalChecksum = 0;
    uint32_t checksumInterval = 20;
    std::vector<Command> commands;
    std::vector<std::pair<uint32_t, uint64_t>> checksums; // (tick, checksum)

    bool save(const std::string& path) const;
    bool load(const std::string& path, std::string* err = nullptr);
};

uint64_t hashFileContents(const std::vector<std::string>& paths);

} // namespace rts
