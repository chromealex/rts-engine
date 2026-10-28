// Resource packs (.rpak): read-only archives of content files, one per game build.
//
// Layout (little-endian, every offset absolute):
//
//   Header, 64 bytes
//     0  char[4] "RPAK"
//     4  u32     version (1; 2 = split pack with downloadable chunks)
//     8  u32     flags (kFlagTocZstd, kFlagExternal)
//    12  u32     entry count
//    16  u64     TOC offset
//    24  u64     TOC stored size
//    32  u64     TOC raw size
//    40  u64     TOC hash   (FNV-1a 64 of the raw TOC)
//    48  u64     content hash (rts::Hasher over str(path), u64(raw hash) of every entry, TOC order)
//    56  u64     v2: offset of the chunk table inside the raw TOC (0 = none)
//   Blobs, each 16-byte aligned (identical files share one blob)
//   TOC: entry records (40 bytes each, sorted by path bytes) followed by the path string table
//     u64 offset, u64 storedSize, u64 rawSize, u64 hash (FNV-1a 64 of raw bytes),
//     u32 pathOffset (into string table), u16 pathLength, u8 codec, u8 chunk (v2; 0 = this file)
//   v2 chunk table (inside the raw TOC): u32 count, then per chunk:
//     u64 size, u8 sha256[32], u16 nameLength, name ("content.001.rpkc")
//
// Split packs: the base pack holds the TOC of *all* files plus the data needed to start; other
// data lives in chunk files that can be downloaded later from anywhere (CDN, mirror, peer, local
// DLC folder). Entry offsets of chunked files point into their chunk. A chunk file is:
//     char[4] "RPKC", u32 version (1), u32 chunk index (1-based), u32 0, u64 base content hash,
//     u64 0, then the blobs (16-byte aligned).
// Downloaded chunks are verified against size + SHA-256 before they are attached.
//
// Packs never influence the simulation by themselves: they return exactly the bytes of the
// original files. The content hash identifies the content set (e.g. to detect peers running
// different data in multiplayer).
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rts::pack {

constexpr uint32_t kPackVersion = 1;       // written when nothing is split
constexpr uint32_t kPackVersionSplit = 2;
constexpr size_t kChunkHeaderSize = 32;
constexpr size_t kHeaderSize = 64;
constexpr size_t kEntrySize = 40;

enum class Codec : uint8_t { Stored = 0, Zstd = 1 };

constexpr uint32_t kFlagTocZstd = 1u << 0;
// Pack contains content imported from third-party archives (e.g. the user's own copy of a
// commercial game). Such packs must never be committed or redistributed.
constexpr uint32_t kFlagExternal = 1u << 1;

// Random-access byte source behind a pack (file, Android asset, memory).
class Source {
public:
    virtual ~Source() = default;
    virtual uint64_t size() const = 0;
    virtual bool read(uint64_t offset, void* dst, size_t n) = 0;
    // Whole source mapped in memory, or nullptr. Lets stored entries be copied without staging.
    virtual const uint8_t* mapped() const { return nullptr; }
};

std::unique_ptr<Source> openFileSource(const std::string& path);
std::unique_ptr<Source> memorySource(std::string bytes);

struct Entry {
    std::string_view path;
    uint64_t offset = 0, storedSize = 0, rawSize = 0, hash = 0;
    Codec codec = Codec::Stored;
    uint8_t chunk = 0;             // 0 = data in this pack, N = in chunk N (split packs)
};

struct ChunkInfo {
    std::string file;              // "content.001.rpkc"
    uint64_t size = 0;
    std::array<uint8_t, 32> sha256{};
};

// Checks a chunk file's header against its pack (index, base content hash, size).
bool checkChunkHeader(Source& src, const ChunkInfo& info, size_t index, uint64_t contentHash, std::string* error = nullptr);

// "./a\\b//c" -> "a/b/c". Paths in packs are stored normalized and are case-sensitive.
std::string normalizePath(std::string_view path);
uint64_t hashBytes(const void* data, size_t n);

class Pack {
public:
    // Returns nullptr and fills `error` with a readable reason on failure (corrupt/truncated file).
    static std::unique_ptr<Pack> open(std::unique_ptr<Source> src, std::string* error = nullptr);
    static std::unique_ptr<Pack> openFile(const std::string& path, std::string* error = nullptr);
    ~Pack();

    const std::string& name() const { return name_; }
    void setName(std::string n) { name_ = std::move(n); }
    uint64_t contentHash() const { return contentHash_; }
    uint32_t flags() const { return flags_; }
    bool external() const { return (flags_ & kFlagExternal) != 0; }
    size_t size() const { return entries_.size(); }
    const Entry& entry(size_t i) const { return entries_[i]; }
    const Entry* find(std::string_view normalizedPath) const;
    bool contains(std::string_view path) const { return find(normalizePath(path)) != nullptr; }

    // Split packs: chunk table and attached chunk sources (index is 1-based like Entry::chunk).
    const std::vector<ChunkInfo>& chunks() const { return chunks_; }
    bool chunkAttached(size_t index) const;
    // Attaches downloaded chunk data (header checked; the caller verified SHA-256 on download).
    bool attachChunk(size_t index, std::unique_ptr<Source> src, std::string* error = nullptr);
    // True when the entry's data is available now (in this file or an attached chunk).
    bool ready(const Entry& e) const { return e.chunk == 0 || chunkAttached(e.chunk); }

    // Reads and decompresses one file. `verify` also checks the raw hash (zstd frames carry
    // their own checksum regardless).
    bool read(std::string_view path, std::string& out, bool verify = false);
    bool read(const Entry& e, std::string& out, bool verify = false);
    const std::string& lastError() const { return error_; }

private:
    Pack() = default;
    std::unique_ptr<Source> src_;
    std::string toc_;            // raw TOC; entry paths point into it
    std::vector<ChunkInfo> chunks_;
    std::vector<std::unique_ptr<Source>> chunkSrc_;
    std::vector<Entry> entries_;
    uint64_t contentHash_ = 0;
    uint32_t flags_ = 0;
    std::string name_, error_;
    std::mutex mutex_;           // guards the decoder context and staging buffer
    void* dctx_ = nullptr;
    std::string staging_;
};

// Ordered set of mounted packs. Later mounts take priority (patches, DLC, mods).
class PackSet {
public:
    void mount(std::unique_ptr<Pack> p) { packs_.push_back(std::move(p)); }
    void clear() { packs_.clear(); }
    bool empty() const { return packs_.empty(); }
    size_t count() const { return packs_.size(); }
    Pack& at(size_t i) { return *packs_[i]; }
    bool contains(std::string_view path) const;
    bool read(std::string_view path, std::string& out);
    // Where a file is: the pack that has it (latest mount first) and whether its data is here.
    enum class State { Missing, Ready, NotDownloaded };
    State state(std::string_view path, Pack** pack = nullptr, const Entry** entry = nullptr);

private:
    std::vector<std::unique_ptr<Pack>> packs_;
};

} // namespace rts::pack
