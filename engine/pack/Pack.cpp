#include "Pack.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "third_party/zstd/zstd.h"

namespace rts::pack {

namespace {

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32; }

class FileSource final : public Source {
public:
    explicit FileSource(std::FILE* f) : f_(f) {
        std::fseek(f_, 0, SEEK_END);
        long n = std::ftell(f_);
        size_ = n > 0 ? uint64_t(n) : 0;
    }
    ~FileSource() override { std::fclose(f_); }
    uint64_t size() const override { return size_; }
    bool read(uint64_t off, void* dst, size_t n) override {
        if (off > size_ || n > size_ - off) return false;
        // fseek takes long; packs > 2 GB on 32-bit hosts are out of scope.
        if (std::fseek(f_, long(off), SEEK_SET) != 0) return false;
        return std::fread(dst, 1, n, f_) == n;
    }

private:
    std::FILE* f_;
    uint64_t size_ = 0;
};

class MemorySource final : public Source {
public:
    explicit MemorySource(std::string b) : b_(std::move(b)) {}
    uint64_t size() const override { return b_.size(); }
    bool read(uint64_t off, void* dst, size_t n) override {
        if (off > b_.size() || n > b_.size() - off) return false;
        std::memcpy(dst, b_.data() + off, n);
        return true;
    }
    const uint8_t* mapped() const override { return reinterpret_cast<const uint8_t*>(b_.data()); }

private:
    std::string b_;
};

bool fail(std::string* err, const std::string& msg) { if (err) *err = msg; return false; }

} // namespace

std::unique_ptr<Source> openFileSource(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    return std::make_unique<FileSource>(f);
}

std::unique_ptr<Source> memorySource(std::string bytes) { return std::make_unique<MemorySource>(std::move(bytes)); }

std::string normalizePath(std::string_view p) {
    std::string o;
    o.reserve(p.size());
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = i;
        while (j < p.size() && p[j] != '/' && p[j] != '\\') ++j;
        std::string_view seg = p.substr(i, j - i);
        if (!seg.empty() && seg != ".") {
            if (!o.empty()) o += '/';
            o += seg;
        }
        i = j + 1;
    }
    return o;
}

uint64_t hashBytes(const void* data, size_t n) {
    auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

std::unique_ptr<Pack> Pack::openFile(const std::string& path, std::string* error) {
    auto src = openFileSource(path);
    if (!src) { fail(error, "cannot open " + path); return nullptr; }
    auto p = open(std::move(src), error);
    if (p) p->setName(path);
    else if (error) *error = path + ": " + *error;
    return p;
}

std::unique_ptr<Pack> Pack::open(std::unique_ptr<Source> src, std::string* error) {
    if (!src) { fail(error, "no source"); return nullptr; }
    uint8_t h[kHeaderSize];
    if (src->size() < kHeaderSize || !src->read(0, h, kHeaderSize)) { fail(error, "file too small for a pack header"); return nullptr; }
    if (std::memcmp(h, "RPAK", 4) != 0) { fail(error, "not a .rpak file (bad magic)"); return nullptr; }
    uint32_t version = rd32(h + 4), flags = rd32(h + 8), count = rd32(h + 12);
    if (version != kPackVersion) { fail(error, "unsupported pack version " + std::to_string(version)); return nullptr; }
    uint64_t tocOff = rd64(h + 16), tocStored = rd64(h + 24), tocRaw = rd64(h + 32), tocHash = rd64(h + 40);
    const uint64_t total = src->size();
    if (tocOff > total || tocStored > total - tocOff) { fail(error, "truncated pack (TOC out of range)"); return nullptr; }
    if (tocRaw < uint64_t(count) * kEntrySize || tocRaw > (uint64_t(1) << 31)) { fail(error, "corrupt TOC size"); return nullptr; }

    std::unique_ptr<Pack> p(new Pack());
    std::string stored(size_t(tocStored), '\0');
    if (!src->read(tocOff, stored.data(), stored.size())) { fail(error, "cannot read TOC"); return nullptr; }
    if (flags & kFlagTocZstd) {
        p->toc_.resize(size_t(tocRaw));
        size_t r = ZSTD_decompress(p->toc_.data(), p->toc_.size(), stored.data(), stored.size());
        if (ZSTD_isError(r) || r != tocRaw) { fail(error, "corrupt TOC (decompression failed)"); return nullptr; }
    } else {
        if (tocStored != tocRaw) { fail(error, "corrupt TOC size"); return nullptr; }
        p->toc_ = std::move(stored);
    }
    if (hashBytes(p->toc_.data(), p->toc_.size()) != tocHash) { fail(error, "corrupt TOC (hash mismatch)"); return nullptr; }

    const auto* t = reinterpret_cast<const uint8_t*>(p->toc_.data());
    const size_t strBase = size_t(count) * kEntrySize, strSize = p->toc_.size() - strBase;
    p->entries_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* r = t + size_t(i) * kEntrySize;
        Entry e;
        e.offset = rd64(r); e.storedSize = rd64(r + 8); e.rawSize = rd64(r + 16); e.hash = rd64(r + 24);
        uint32_t po = rd32(r + 32);
        uint16_t pl = uint16_t(r[36] | r[37] << 8);
        e.codec = Codec(r[38]);
        if (po > strSize || pl > strSize - po) { fail(error, "corrupt TOC (path out of range)"); return nullptr; }
        if (e.offset > total || e.storedSize > total - e.offset) { fail(error, "truncated pack (entry data out of range)"); return nullptr; }
        if (e.codec != Codec::Stored && e.codec != Codec::Zstd) { fail(error, "unknown codec " + std::to_string(int(e.codec))); return nullptr; }
        if (e.codec == Codec::Stored && e.storedSize != e.rawSize) { fail(error, "corrupt entry size"); return nullptr; }
        e.path = std::string_view(p->toc_.data() + strBase + po, pl);
        if (!p->entries_.empty() && !(p->entries_.back().path < e.path)) { fail(error, "corrupt TOC (paths not sorted)"); return nullptr; }
        p->entries_.push_back(e);
    }
    p->contentHash_ = rd64(h + 48);
    p->flags_ = flags;
    p->src_ = std::move(src);
    return p;
}

Pack::~Pack() { if (dctx_) ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(dctx_)); }

const Entry* Pack::find(std::string_view path) const {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), path, [](const Entry& e, std::string_view k) { return e.path < k; });
    return it != entries_.end() && it->path == path ? &*it : nullptr;
}

bool Pack::read(std::string_view path, std::string& out, bool verify) {
    const Entry* e = find(normalizePath(path));
    if (!e) return false;
    return read(*e, out, verify);
}

bool Pack::read(const Entry& e, std::string& out, bool verify) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto failRead = [&](const char* why) { error_ = std::string(e.path) + ": " + why; return false; };
    if (e.rawSize > (uint64_t(1) << 40)) return failRead("entry too large");
    std::string data(size_t(e.rawSize), '\0');
    const uint8_t* m = src_->mapped();
    if (e.codec == Codec::Stored) {
        if (m) std::memcpy(data.data(), m + e.offset, data.size());
        else if (!src_->read(e.offset, data.data(), data.size())) return failRead("read error");
    } else {
        const void* in = nullptr;
        if (m) in = m + e.offset;
        else {
            staging_.resize(size_t(e.storedSize));
            if (!src_->read(e.offset, staging_.data(), staging_.size())) return failRead("read error");
            in = staging_.data();
        }
        if (!dctx_) dctx_ = ZSTD_createDCtx();
        size_t r = ZSTD_decompressDCtx(static_cast<ZSTD_DCtx*>(dctx_), data.data(), data.size(), in, size_t(e.storedSize));
        if (staging_.capacity() > (size_t(4) << 20)) std::string().swap(staging_); // don't pin big buffers
        if (ZSTD_isError(r) || r != e.rawSize) return failRead("corrupt data (decompression failed)");
    }
    if (verify && hashBytes(data.data(), data.size()) != e.hash) return failRead("corrupt data (hash mismatch)");
    out = std::move(data);
    return true;
}

bool PackSet::contains(std::string_view path) const {
    std::string n = normalizePath(path);
    for (auto it = packs_.rbegin(); it != packs_.rend(); ++it) if ((*it)->find(n)) return true;
    return false;
}

bool PackSet::read(std::string_view path, std::string& out) {
    std::string n = normalizePath(path);
    for (auto it = packs_.rbegin(); it != packs_.rend(); ++it) {
        if (const Entry* e = (*it)->find(n)) return (*it)->read(*e, out);
    }
    return false;
}

} // namespace rts::pack
