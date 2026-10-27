// Builds .rpak files (host tools only: assetpipe, rtsbuild, tests). Output is reproducible:
// the same inputs and options always give byte-identical packs (no timestamps, sorted order,
// per-file compression independent of thread count).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Pack.h"

namespace rts::pack {

struct WriteOptions {
    int level = 19;          // zstd level (1..22); 19 is a good size/time trade-off for release
    unsigned threads = 0;    // 0 = hardware concurrency
    bool compress = true;    // false: everything stored (debugging, fastest builds)
};

struct WriteStats {
    struct Item { std::string path; uint64_t rawSize = 0, storedSize = 0; Codec codec = Codec::Stored; bool duplicate = false; };
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

class PackWriter {
public:
    // Adds one file; `packPath` is normalized. Returns false on a duplicate path.
    bool addFile(const std::string& packPath, const std::string& diskPath, std::string* error = nullptr);
    bool addData(const std::string& packPath, std::string data, std::string* error = nullptr);
    // Adds a directory tree under `prefix` ("" for the root, "engine" for engine assets).
    // Applies `<dir>/.rpakignore` on top of `rules`. Returns the number of files added, -1 on error.
    int addDirectory(const std::string& dir, const std::string& prefix, const IgnoreRules& rules, std::string* error = nullptr);

    bool write(const std::string& outPath, const WriteOptions& opt, WriteStats* stats = nullptr, std::string* error = nullptr);
    // Same, into memory (tests).
    bool writeToString(std::string& out, const WriteOptions& opt, WriteStats* stats = nullptr, std::string* error = nullptr);

    size_t size() const { return inputs_.size(); }

private:
    struct Input { std::string path, disk, data; bool inMemory = false; };
    std::vector<Input> inputs_;
};

// Human-readable size: 1.2 MB, 340 KB, 12 B.
std::string formatBytes(uint64_t n);
// Build-size report (user-facing, Russian): totals, savings, the `top` largest entries and
// per-folder totals, so it is obvious what makes a build big.
std::string sizeReport(const WriteStats& st, size_t top = 10);

} // namespace rts::pack
