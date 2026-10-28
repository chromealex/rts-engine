// Unpacker plugins: host-side support for archive formats the engine doesn't know (zip, tar,
// archives of other games, ...). A plugin is a folder with `unpacker.json` and a program in any
// language; see tools/unpackers/PROTOCOL.md. Plugins only run in host tools (assetpipe, rtsbuild,
// editor/MCP) to convert archives into .rpak — the game runtime reads .rpak only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "PackWriter.h"

namespace rts::pack {

constexpr int kUnpackerProtocol = 1;

struct UnpackerInfo {
    struct Magic { int64_t offset = 0; std::string bytes; };   // negative offset: from the end
    std::string name, title, version, license, dir, manifest;
    std::vector<std::string> extensions;   // lower-case, with dot
    std::vector<Magic> magic;
    std::vector<std::string> command;      // resolved: files inside the plugin dir made absolute
    std::vector<std::string> capabilities; // "probe", "list", "extract"
    bool external = false;                 // imported content is third-party: mark packs external
    bool builtin = false;                  // handled inside the tools (rpak), no process
    bool can(const std::string& cap) const;
};

struct ArchiveEntryInfo { std::string path; uint64_t size = 0; };

class UnpackerRegistry {
public:
    UnpackerRegistry();                    // registers the built-in "rpak" format
    // Scans `dir/unpacker.json` and `dir/*/unpacker.json`. Bad manifests go to errors().
    void addSearchDir(const std::string& dir);
    // Standard search path: <engine>/tools/unpackers, every dir in $RTS_UNPACKERS, then
    // <project>/unpackers. Later dirs win on name clashes (a game can override a plugin).
    void addDefaultDirs(const std::string& engineDir, const std::string& projectDir = "");
    bool loadManifest(const std::string& manifestPath, std::string* error = nullptr);

    const std::vector<UnpackerInfo>& all() const { return list_; }
    const std::vector<std::string>& errors() const { return errors_; }
    const UnpackerInfo* byName(const std::string& name) const;
    // Picks the plugin for a file: magic bytes first, then extension; when several match, asks
    // their `probe`. Returns nullptr with a readable reason if nothing fits.
    const UnpackerInfo* detect(const std::string& archive, std::string* why = nullptr) const;

    // Protocol calls (one process per call). Return false with a readable error.
    bool probe(const UnpackerInfo& u, const std::string& archive, bool& match, std::string* error = nullptr) const;
    bool list(const UnpackerInfo& u, const std::string& archive, std::vector<ArchiveEntryInfo>& out, std::string* error = nullptr) const;
    bool extract(const UnpackerInfo& u, const std::string& archive, const std::string& outDir, std::string* error = nullptr) const;
    // `info` handshake: checks the program runs and speaks our protocol version.
    bool handshake(const UnpackerInfo& u, std::string* error = nullptr) const;

private:
    std::vector<UnpackerInfo> list_;
    std::vector<std::string> errors_;
};

struct ImportOptions {
    std::string format;                    // plugin name; empty = detect
    std::string prefix;                    // where the files land inside the pack
    std::vector<std::string> include;      // glob filters on archive paths; empty = everything
    std::string tempDir;                   // scratch folder (created and removed); empty = system temp
    bool external = false;                 // force kFlagExternal (folders of third-party files)
};

struct ImportResult {
    std::string format;
    int files = 0;
    bool external = false;
};

// Unpacks `archive` (a file, or a plain folder) with the matching plugin and adds its files to `w` under opt.prefix.
// Symlinks and paths escaping the scratch folder are never followed.
bool importArchive(const UnpackerRegistry& reg, const std::string& archive, const ImportOptions& opt, PackWriter& w,
                   ImportResult* result = nullptr, std::string* error = nullptr);

// Writes a working plugin skeleton for a new format: manifest, Python script and a copy of the
// rtsunpack.py helper from <engineDir>/tools/unpackers/lib (so the plugin is self-contained).
bool scaffoldUnpacker(const std::string& dir, const std::string& name, const std::string& engineDir, std::string* error = nullptr);

// Minimal JSON string escaping for tool output (--json) and protocol requests.
std::string jsonEscape(const std::string& s);

} // namespace rts::pack
