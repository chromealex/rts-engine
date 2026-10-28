// Builds .rpak files (host tools only: assetpipe, rtsbuild, tests). Output is reproducible:
// the same inputs and options always give byte-identical packs (no timestamps, sorted order,
// per-file compression independent of thread count).
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "Pack.h"

namespace rts::pack {

struct WriteOptions {
    int level = 19;          // zstd level (1..22); 19 is a good size/time trade-off for release
    unsigned threads = 0;    // 0 = hardware concurrency
    bool compress = true;    // false: everything stored (debugging, fastest builds)
    bool external = false;   // set kFlagExternal (also set automatically by addPack of an external pack)
    // Per-file overrides (build window / game.json packaging.files): the first glob matching a
    // pack path sets its zstd level; level 0 = store uncompressed. Deterministic like the rest.
    std::vector<std::pair<std::string, int>> levelRules;
};

struct WriteStats {
    struct Item { std::string path; uint64_t rawSize = 0, storedSize = 0; Codec codec = Codec::Stored; bool duplicate = false; uint8_t chunk = 0; };
    std::vector<Item> items;     // in pack order
    uint64_t rawBytes = 0;       // sum of all input files
    uint64_t packBytes = 0;      // size of the .rpak
    size_t duplicates = 0;
    uint64_t contentHash = 0;
    std::vector<std::string> warnings;
};

// Ignore rules for directory scans. Patterns use `*` and `?`; a pattern with `/` matches the
// whole relative path, otherwise the file or directory name. Hidden names (".git", ".DS_Store")
// are always skipped, as are replays and nested packs.
class IgnoreRules {
public:
    IgnoreRules();
    void add(const std::string& pattern);
    // Reads patterns from a `.rpakignore` file (one per line, `#` comments). Missing file is fine.
    void loadFile(const std::string& path);
    bool ignored(const std::string& relPath, bool isDir) const;

private:
    std::vector<std::string> patterns_;
};

bool globMatch(const char* pattern, const char* text);
// Files whose format is already compressed (images, audio, video, archives) are stored as is.
bool alreadyCompressed(const std::string& path);

// How to split content into a base pack (ships with the build) and downloadable chunks.
// Every choice is the user's: whether to split, the chunk size and the size limits.
struct SplitOptions {
    uint64_t chunkSize = 8u << 20;          // max bytes per chunk file
    uint64_t baseSize = 0;                  // max bytes of the base pack (0 = no limit)
    uint64_t totalBudget = 0;               // max bytes of base + all chunks (0 = no limit)
    bool fillBase = false;                  // false: base = only what must ship; true: fill up to baseSize
    std::vector<std::string> base;          // globs that always ship in the base (needed to start)
    std::vector<std::string> priority;      // globs in download order (earlier = sooner, and into base first)
    std::string chunkPrefix = "content";    // chunk file names: <prefix>.001.rpkc
};

struct SplitResult {
    struct Chunk { std::string file; uint64_t size = 0; size_t files = 0; std::string sha256; };
    uint64_t baseBytes = 0, totalBytes = 0;
    std::vector<Chunk> chunks;
};

bool writeFileAtomic(const std::string& path, const std::string& bytes, std::string* error = nullptr);

class PackWriter {
public:
    // Adds one file; `packPath` is normalized. Returns false on a duplicate path.
    bool addFile(const std::string& packPath, const std::string& diskPath, std::string* error = nullptr);
    bool addData(const std::string& packPath, std::string data, std::string* error = nullptr);
    // Adds a directory tree under `prefix` ("" for the root, "engine" for engine assets).
    // Applies `<dir>/.rpakignore` on top of `rules`. Returns the number of files added, -1 on error.
    // Symlinks are skipped when `followSymlinks` is false (untrusted trees, e.g. unpacker output).
    int addDirectory(const std::string& dir, const std::string& prefix, const IgnoreRules& rules, std::string* error = nullptr,
                     bool followSymlinks = true);
    // Adds every entry of an existing pack under `prefix` (merging imported or prebuilt packs).
    int addPack(Pack& pack, const std::string& prefix, std::string* error = nullptr);
    void markExternal() { external_ = true; }
    bool external() const { return external_; }

    bool write(const std::string& outPath, const WriteOptions& opt, WriteStats* stats = nullptr, std::string* error = nullptr);
    // Same, into memory (tests).
    bool writeToString(std::string& out, const WriteOptions& opt, WriteStats* stats = nullptr, std::string* error = nullptr);

    // Split build: base pack + chunk files (empty when everything fits the base).
    bool writeSplit(std::string& base, std::vector<std::string>& chunks, const WriteOptions& opt, const SplitOptions& split,
                    WriteStats* stats = nullptr, SplitResult* result = nullptr, std::string* error = nullptr);
    bool writeSplitFiles(const std::string& basePath, const std::string& chunkDir, const WriteOptions& opt, const SplitOptions& split,
                         WriteStats* stats = nullptr, SplitResult* result = nullptr, std::string* error = nullptr);

    size_t size() const { return inputs_.size(); }

    // Build steps that transform content before write() (texture atlases): list the inputs
    // (pack paths, insertion order), read one input's bytes, drop inputs by pack path.
    std::vector<std::string> paths() const;
    bool readInput(const std::string& packPath, std::string& out) const;
    size_t remove(const std::vector<std::string>& packPaths);

private:
    bool build(std::string& out, std::vector<std::string>& chunks, const WriteOptions& opt, const SplitOptions* split,
               WriteStats* stats, SplitResult* result, std::string* error);
    struct Input { std::string path, disk, data; bool inMemory = false; };
    std::vector<Input> inputs_;
    std::set<std::string> paths_;
    bool external_ = false;
};

// Human-readable size: 1.2 MB, 340 KB, 12 B.
std::string formatBytes(uint64_t n);
// Build-size report (user-facing, Russian): totals, savings, the `top` largest entries and
// per-folder totals, so it is obvious what makes a build big.
std::string sizeReport(const WriteStats& st, size_t top = 10);

} // namespace rts::pack
