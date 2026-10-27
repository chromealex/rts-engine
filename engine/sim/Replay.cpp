#include "Replay.h"
#include "../core/Hash.h"
#include <fstream>
#include <iterator>

namespace rts {

bool Replay::save(const std::string& path) const {
    ByteWriter w;
    w.u32(kMagic); w.u32(kVersion);
    w.str(module); w.str(rulesPath); w.str(mapPath);
    w.u64(dataHash); w.u64(seed); w.u32(tickRate); w.u32(inputDelay);
    w.u32(endTick); w.u64(finalChecksum); w.u32(checksumInterval);
    w.u32(uint32_t(commands.size()));
    for (auto& c : commands) writeCommand(w, c);
    w.u32(uint32_t(checksums.size()));
    for (auto& [t, h] : checksums) { w.u32(t); w.u64(h); }
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(w.buf.data()), std::streamsize(w.buf.size()));
    return bool(f);
}

bool Replay::load(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ByteReader r(data.data(), data.size());
    if (r.u32() != kMagic) { if (err) *err = "not a replay file"; return false; }
    if (r.u32() != kVersion) { if (err) *err = "unsupported replay version"; return false; }
    module = r.str(); rulesPath = r.str(); mapPath = r.str();
    dataHash = r.u64(); seed = r.u64(); tickRate = r.u32(); inputDelay = r.u32();
    endTick = r.u32(); finalChecksum = r.u64(); checksumInterval = r.u32();
    uint32_t n = r.u32();
    commands.clear();
    for (uint32_t i = 0; i < n && r.ok(); ++i) commands.push_back(readCommand(r));
    uint32_t m = r.u32();
    checksums.clear();
    for (uint32_t i = 0; i < m && r.ok(); ++i) { uint32_t t = r.u32(); uint64_t h = r.u64(); checksums.emplace_back(t, h); }
    if (!r.ok()) { if (err) *err = "truncated replay"; return false; }
    return true;
}

uint64_t hashFileContents(const std::vector<std::string>& paths) {
    Hasher h;
    for (auto& p : paths) {
        std::ifstream f(p, std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        h.str(s);
    }
    return h.value();
}

} // namespace rts
