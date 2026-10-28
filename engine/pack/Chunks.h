// Runtime downloads of pack chunks (split packs, see Pack.h) from any source.
//
// Sources are URL templates tried in order (mirrors): "https://cdn.example.com/game/{hash}/{file}",
// "file:///Volumes/DLC/{file}", a plain folder path, or any custom scheme a game registers
// ("steam://", "pad://" for Play Asset Delivery, a P2P layer...). Without "{file}" the chunk file
// name is appended after a '/'. "{hash}" is the base pack's content hash (16 hex digits), so one
// CDN folder can hold several builds side by side.
//
// Every chunk is checked against the size and SHA-256 recorded in the base pack before it is
// used, so an untrusted mirror can't inject data. Verified chunks are kept in a cache folder
// (<userData>/content-cache/<hash>/) and attached again on the next start without downloading.
//
// The manager is single-threaded and polled from the main loop: transfers run in the
// background (threads natively, browser fetch on the web) and SHA-256 is computed a few
// megabytes per poll, so nothing blocks a frame.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Pack.h"
#include "Sha256.h"

namespace rts::pack {

// One transfer into a local file. Implementations update their state from any thread; the
// manager only reads it from poll().
class Download {
public:
    virtual ~Download() = default;
    virtual bool done() const = 0;
    virtual bool ok() const = 0;
    virtual uint64_t bytes() const = 0;           // received so far
    virtual std::string error() const = 0;
};

// Starts a transfer of `url` into `destPath`. Returns nullptr if it can't start (the error says why).
using FetchFn = std::function<std::unique_ptr<Download>(const std::string& url, const std::string& destPath, std::string& error)>;

// Built-in fetchers.
std::unique_ptr<Download> fetchFile(const std::string& url, const std::string& destPath, std::string& error);  // file:// and paths
// HTTP(S) via the platform: libcurl (Linux, loaded at runtime), WinHTTP (Windows), NSURLSession
// (macOS/iOS), HttpURLConnection over JNI (Android), fetch() (web). nullptr where unavailable.
std::unique_ptr<Download> fetchHttp(const std::string& url, const std::string& destPath, std::string& error);
// Android: the JavaVM the HTTP fetcher attaches its threads to (set by the platform layer).
void setAndroidJavaVm(void* vm);

class ChunkManager {
public:
    enum class Status { NotDownloaded, Queued, Downloading, Verifying, Ready, Failed };
    struct ChunkView {
        const Pack* pack = nullptr;
        size_t index = 0;                          // 1-based chunk index in that pack
        const ChunkInfo* info = nullptr;
        Status status = Status::NotDownloaded;
        uint64_t bytes = 0;                        // downloaded or verified so far
        std::string error;
    };
    struct Progress {
        size_t chunks = 0, ready = 0, active = 0, failed = 0;
        uint64_t bytesTotal = 0, bytesReady = 0;   // over requested + ready chunks
        bool busy = false;
    };

    ChunkManager();
    void setCacheDir(std::string dir) { cacheDir_ = std::move(dir); }
    void setSources(std::vector<std::string> urls) { sources_ = std::move(urls); }
    const std::vector<std::string>& sources() const { return sources_; }
    void registerScheme(const std::string& scheme, FetchFn fn) { schemes_[scheme] = std::move(fn); }
    void setMaxParallel(int n) { maxParallel_ = n < 1 ? 1 : n; }
    void setVerifyBytesPerPoll(uint64_t n) { verifyStep_ = n < 65536 ? 65536 : n; }

    // Registers the chunks of a mounted pack and attaches those already in the cache.
    void addPack(Pack& pack);
    void clear();

    // Queue downloads. Chunks already ready or in progress are left alone; failed ones retry.
    void request(const Pack& pack, size_t chunkIndex);
    void requestAll();

    // Drives transfers, verification and attaching. Call every frame (cheap when idle).
    void poll();
    Progress progress() const;
    std::vector<ChunkView> chunks() const;
    Status status(const Pack& pack, size_t chunkIndex) const;

    std::string cachePath(const Pack& pack, size_t chunkIndex) const;
    std::string url(const std::string& source, const Pack& pack, size_t chunkIndex) const;

private:
    struct Item {
        Pack* pack = nullptr;
        size_t index = 0;
        Status status = Status::NotDownloaded;
        size_t sourceIndex = 0;
        std::unique_ptr<Download> dl;
        std::string partPath, error;
        std::unique_ptr<Sha256> sha;
        std::FILE* verifyFile = nullptr;
        uint64_t verified = 0;
    };
    void start(Item& it);
    void finishVerify(Item& it, bool ok, const std::string& err);
    void attach(Item& it, const std::string& path);
    FetchFn fetcherFor(const std::string& url) const;

    std::vector<std::unique_ptr<Item>> items_;
    std::vector<std::string> sources_;
    std::map<std::string, FetchFn> schemes_;
    std::string cacheDir_;
    int maxParallel_ = 2;
    uint64_t verifyStep_ = 8u << 20;
};

const char* statusName(ChunkManager::Status s);

// Host tools: attaches the chunks of a split pack found next to it (in ./chunks or the same
// folder) after checking their SHA-256. Returns how many were attached; problems are listed.
size_t attachChunksNextTo(Pack& pack, const std::string& packPath, std::vector<std::string>* problems = nullptr);

} // namespace rts::pack
