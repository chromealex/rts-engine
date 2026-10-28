#include "PackWriter.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#include "Sha256.h"
#include "engine/core/Hash.h"
#include "third_party/zstd/zstd.h"

namespace fs = std::filesystem;

namespace rts::pack {

namespace {

void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s += char(uint8_t(v >> (8 * i))); }
void put64(std::string& s, uint64_t v) { for (int i = 0; i < 8; ++i) s += char(uint8_t(v >> (8 * i))); }
bool fail(std::string* err, const std::string& msg) { if (err) *err = msg; return false; }

bool readAll(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    out = ss.str();
    return bool(f) || f.eof();
}

std::string lower(std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }

// Compressing tiny files or files that barely shrink only costs decode time at load.
constexpr size_t kMinCompressSize = 64;
bool worthIt(size_t raw, size_t packed) { return packed + raw / 32 < raw; }

} // namespace

bool writeFileAtomic(const std::string& path, const std::string& bytes, std::string* error) {
    std::error_code ec;
    fs::path out(path);
    if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);
    // Write to a temp file and rename so a failed build never leaves a half-written file behind.
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), std::streamsize(bytes.size()));
        if (!f) return fail(error, "cannot write " + tmp);
    }
    fs::rename(tmp, out, ec);
    if (ec) { fs::remove(out, ec); fs::rename(tmp, out, ec); }
    if (ec) return fail(error, "cannot write " + path + ": " + ec.message());
    return true;
}

bool globMatch(const char* p, const char* t) {
    // Iterative wildcard match with single backtrack point (linear for typical patterns).
    const char *star = nullptr, *mark = nullptr;
    while (*t) {
        if (*p == '?' || *p == *t) { ++p; ++t; }
        else if (*p == '*') { star = p++; mark = t; }
        else if (star) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (*p == '*') ++p;
    return *p == 0;
}

bool alreadyCompressed(const std::string& path) {
    static const char* kExt[] = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".ogg", ".opus", ".mp3", ".m4a", ".aac",
                                 ".flac", ".mp4", ".webm", ".mkv", ".zip", ".gz", ".zst", ".7z", ".rpak", ".woff2", ".basis"};
    std::string l = lower(path);
    for (const char* e : kExt) {
        size_t n = std::strlen(e);
        if (l.size() >= n && l.compare(l.size() - n, n, e) == 0) return true;
    }
    return false;
}

IgnoreRules::IgnoreRules() {
    for (const char* p : {"*.rpl", "*.rpak", "*~", "Thumbs.db", "desktop.ini", "*.tmp", "*.swp"}) add(p);
}

void IgnoreRules::add(const std::string& pattern) {
    std::string p = pattern;
    while (!p.empty() && std::isspace(static_cast<unsigned char>(p.back()))) p.pop_back();
    size_t b = 0;
    while (b < p.size() && std::isspace(static_cast<unsigned char>(p[b]))) ++b;
    p = p.substr(b);
    if (p.empty() || p[0] == '#') return;
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p[0] == '/') p.erase(0, 1);
    patterns_.push_back(p);
}

void IgnoreRules::loadFile(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) add(line);
}

bool IgnoreRules::ignored(const std::string& rel, bool isDir) const {
    std::string name = rel.substr(rel.find_last_of('/') == std::string::npos ? 0 : rel.find_last_of('/') + 1);
    if (!name.empty() && name[0] == '.') return true;
    for (const std::string& p : patterns_) {
        bool dirOnly = !p.empty() && p.back() == '/';
        std::string q = dirOnly ? p.substr(0, p.size() - 1) : p;
        if (dirOnly && !isDir) continue;
        if (q.find('/') != std::string::npos ? globMatch(q.c_str(), rel.c_str()) : globMatch(q.c_str(), name.c_str())) return true;
    }
    return false;
}

bool PackWriter::addFile(const std::string& packPath, const std::string& diskPath, std::string* error) {
    Input in;
    in.path = normalizePath(packPath);
    in.disk = diskPath;
    if (in.path.empty()) return fail(error, "empty pack path for " + diskPath);
    if (in.path.size() > 0xffff) return fail(error, "path too long: " + in.path);
    if (!paths_.insert(in.path).second) return fail(error, "duplicate path in pack: " + in.path);
    inputs_.push_back(std::move(in));
    return true;
}

bool PackWriter::addData(const std::string& packPath, std::string data, std::string* error) {
    if (!addFile(packPath, "<memory>", error)) return false;
    inputs_.back().data = std::move(data);
    inputs_.back().inMemory = true;
    return true;
}

std::vector<std::string> PackWriter::paths() const {
    std::vector<std::string> v;
    v.reserve(inputs_.size());
    for (const Input& in : inputs_) v.push_back(in.path);
    return v;
}

bool PackWriter::readInput(const std::string& packPath, std::string& out) const {
    std::string p = normalizePath(packPath);
    for (const Input& in : inputs_)
        if (in.path == p) { if (in.inMemory) { out = in.data; return true; } return readAll(in.disk, out); }
    return false;
}

size_t PackWriter::remove(const std::vector<std::string>& packPaths) {
    std::vector<std::string> drop;
    for (auto& p : packPaths) drop.push_back(normalizePath(p));
    std::sort(drop.begin(), drop.end());
    size_t before = inputs_.size();
    inputs_.erase(std::remove_if(inputs_.begin(), inputs_.end(), [&](const Input& in) { return std::binary_search(drop.begin(), drop.end(), in.path); }), inputs_.end());
    return before - inputs_.size();
}

int PackWriter::addDirectory(const std::string& dir, const std::string& prefix, const IgnoreRules& base, std::string* error,
                             bool followSymlinks) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) { fail(error, "not a directory: " + dir); return -1; }
    IgnoreRules rules = base;
    rules.loadFile((fs::path(dir) / ".rpakignore").string());
    // Collect and sort first: directory iteration order differs between file systems.
    std::vector<std::pair<std::string, std::string>> files;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::string rel = fs::relative(it->path(), dir, ec).generic_string();
        if (!followSymlinks && it->is_symlink(ec)) { it.disable_recursion_pending(); continue; }
        bool isDir = it->is_directory(ec);
        if (rules.ignored(rel, isDir)) { if (isDir) it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(ec)) continue;
        files.emplace_back(rel, it->path().string());
    }
    if (ec) { fail(error, "cannot scan " + dir + ": " + ec.message()); return -1; }
    std::sort(files.begin(), files.end());
    for (auto& [rel, disk] : files)
        if (!addFile(prefix.empty() ? rel : prefix + "/" + rel, disk, error)) return -1;
    return int(files.size());
}

int PackWriter::addPack(Pack& pack, const std::string& prefix, std::string* error) {
    std::string data;
    for (size_t i = 0; i < pack.size(); ++i) {
        const Entry& e = pack.entry(i);
        if (!pack.read(e, data, true)) { fail(error, pack.name() + ": " + pack.lastError()); return -1; }
        std::string path = prefix.empty() ? std::string(e.path) : prefix + "/" + std::string(e.path);
        if (!addData(path, std::move(data), error)) return -1;
    }
    if (pack.external()) external_ = true;
    return int(pack.size());
}

bool PackWriter::write(const std::string& outPath, const WriteOptions& opt, WriteStats* stats, std::string* error) {
    std::string bytes;
    if (!writeToString(bytes, opt, stats, error)) return false;
    return writeFileAtomic(outPath, bytes, error);
}

bool PackWriter::writeToString(std::string& out, const WriteOptions& opt, WriteStats* stats, std::string* error) {
    std::vector<std::string> chunks;
    return build(out, chunks, opt, nullptr, stats, nullptr, error);
}

bool PackWriter::writeSplit(std::string& base, std::vector<std::string>& chunks, const WriteOptions& opt, const SplitOptions& split,
                            WriteStats* stats, SplitResult* result, std::string* error) {
    return build(base, chunks, opt, &split, stats, result, error);
}

bool PackWriter::writeSplitFiles(const std::string& basePath, const std::string& chunkDir, const WriteOptions& opt, const SplitOptions& split,
                                 WriteStats* stats, SplitResult* result, std::string* error) {
    std::string base;
    std::vector<std::string> chunks;
    SplitResult local;
    SplitResult& r = result ? *result : local;
    if (!build(base, chunks, opt, &split, stats, &r, error)) return false;
    if (!writeFileAtomic(basePath, base, error)) return false;
    std::error_code ec;
    if (!chunks.empty()) fs::create_directories(chunkDir, ec);
    for (size_t i = 0; i < chunks.size(); ++i)
        if (!writeFileAtomic((fs::path(chunkDir) / r.chunks[i].file).string(), chunks[i], error)) return false;
    return true;
}

bool PackWriter::build(std::string& out, std::vector<std::string>& chunkData, const WriteOptions& opt, const SplitOptions* split,
                       WriteStats* stats, SplitResult* result, std::string* error) {
    WriteStats local;
    WriteStats& st = stats ? *stats : local;
    st = WriteStats{};
    chunkData.clear();

    std::vector<size_t> order(inputs_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return inputs_[a].path < inputs_[b].path; });

    // Case-only differences break on case-insensitive file systems (macOS, Windows) during
    // development, so flag them even though packs themselves are case-sensitive.
    {
        std::map<std::string, std::string> seen;
        for (size_t i : order) {
            auto [it, fresh] = seen.emplace(lower(inputs_[i].path), inputs_[i].path);
            if (!fresh) st.warnings.push_back("paths differ only in case: " + it->second + " / " + inputs_[i].path);
        }
    }

    struct Blob { std::string raw, stored; uint64_t hash = 0; Codec codec = Codec::Stored; long dupOf = -1; };
    std::vector<Blob> blobs(order.size());
    for (size_t k = 0; k < order.size(); ++k) {
        Input& in = inputs_[order[k]];
        if (in.inMemory) blobs[k].raw = in.data;
        else if (!readAll(in.disk, blobs[k].raw)) return fail(error, "cannot read " + in.disk);
        blobs[k].hash = hashBytes(blobs[k].raw.data(), blobs[k].raw.size());
    }
    // Dedup: identical bytes share the first blob (compared fully, not just by hash).
    {
        std::map<std::pair<uint64_t, uint64_t>, std::vector<size_t>> byHash;
        for (size_t k = 0; k < blobs.size(); ++k) {
            auto& bucket = byHash[{blobs[k].hash, blobs[k].raw.size()}];
            for (size_t j : bucket) if (blobs[j].raw == blobs[k].raw) { blobs[k].dupOf = long(j); break; }
            if (blobs[k].dupOf < 0) bucket.push_back(k);
        }
    }

    // Compress in parallel; each file is compressed independently, so output doesn't depend on
    // the thread count.
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    auto worker = [&]() {
        ZSTD_CCtx* c = ZSTD_createCCtx();
        ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, std::clamp(opt.level, 1, ZSTD_maxCLevel()));
        ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 1);
        for (size_t k; (k = next++) < blobs.size();) {
            Blob& b = blobs[k];
            if (b.dupOf >= 0 || !opt.compress || b.raw.size() < kMinCompressSize || alreadyCompressed(inputs_[order[k]].path)) continue;
            std::string z(ZSTD_compressBound(b.raw.size()), '\0');
            size_t n = ZSTD_compress2(c, z.data(), z.size(), b.raw.data(), b.raw.size());
            if (ZSTD_isError(n)) { failed = true; break; }
            if (worthIt(b.raw.size(), n)) { z.resize(n); b.stored = std::move(z); b.codec = Codec::Zstd; }
        }
        ZSTD_freeCCtx(c);
    };
    unsigned nt = opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
    nt = unsigned(std::min<size_t>(nt, std::max<size_t>(1, blobs.size())));
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < nt; ++i) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();
    if (failed) return fail(error, "compression failed");
    auto storedOf = [&](const Blob& b) -> const std::string& { return b.codec == Codec::Zstd ? b.stored : b.raw; };

    // ---- placement: chunk index per unique blob (0 = base)
    std::vector<uint8_t> chunkOf(blobs.size(), 0);
    std::vector<uint64_t> chunkBytes;                 // payload sizes as we fill
    if (split) {
        if (split->chunkSize < 1024) return fail(error, "split: chunk size must be at least 1 KB");
        auto matches = [](const std::vector<std::string>& globs, const std::string& path) -> int {
            for (size_t g = 0; g < globs.size(); ++g) if (globMatch(globs[g].c_str(), path.c_str())) return int(g);
            return -1;
        };
        // Rank every unique blob by the best of the paths that share it.
        struct Ranked { size_t blob; bool mandatory; int prio; size_t first; };
        std::vector<Ranked> ranks;
        std::vector<long> rankIndex(blobs.size(), -1);
        for (size_t k = 0; k < blobs.size(); ++k) {
            size_t owner = blobs[k].dupOf >= 0 ? size_t(blobs[k].dupOf) : k;
            const std::string& path = inputs_[order[k]].path;
            bool mand = matches(split->base, path) >= 0;
            int pr = matches(split->priority, path);
            if (pr < 0) pr = int(split->priority.size());
            if (rankIndex[owner] < 0) { rankIndex[owner] = long(ranks.size()); ranks.push_back({owner, mand, pr, k}); }
            else { Ranked& r = ranks[size_t(rankIndex[owner])]; r.mandatory = r.mandatory || mand; r.prio = std::min(r.prio, pr); }
        }
        std::stable_sort(ranks.begin(), ranks.end(), [](const Ranked& a, const Ranked& b) {
            if (a.mandatory != b.mandatory) return a.mandatory;
            if (a.prio != b.prio) return a.prio < b.prio;
            return a.first < b.first;
        });
        // Base estimate: header + uncompressed TOC (an upper bound) + aligned blobs.
        uint64_t toc = 4;
        for (size_t k = 0; k < blobs.size(); ++k) toc += kEntrySize + inputs_[order[k]].path.size();
        uint64_t base = kHeaderSize + toc + 16 * 60;   // + room for a chunk table of a few entries
        auto aligned = [](uint64_t n) { return (n + 15) & ~uint64_t(15); };
        std::vector<std::pair<uint64_t, std::string>> mandatoryList;
        bool baseOpen = split->fillBase;
        for (const Ranked& r : ranks) {
            uint64_t sz = aligned(storedOf(blobs[r.blob]).size());
            if (r.mandatory) {
                base += sz;
                mandatoryList.emplace_back(sz, inputs_[order[r.first]].path);
                continue;
            }
            if (baseOpen && (split->baseSize == 0 || base + sz <= split->baseSize)) { base += sz; continue; }
            baseOpen = false; // keep download order: once something is out, everything after is out
            if (chunkBytes.empty() || chunkBytes.back() + sz > split->chunkSize - kChunkHeaderSize) {
                if (chunkBytes.size() >= 255) return fail(error, "split: more than 255 chunks — raise the chunk size");
                if (sz > split->chunkSize - kChunkHeaderSize)
                    st.warnings.push_back(inputs_[order[r.first]].path + " (" + formatBytes(sz) + ") is larger than the chunk size: it gets a chunk of its own");
                chunkBytes.push_back(0);
            }
            chunkBytes.back() += sz;
            chunkOf[r.blob] = uint8_t(chunkBytes.size());
        }
        if (split->baseSize && base > split->baseSize) {
            std::sort(mandatoryList.rbegin(), mandatoryList.rend());
            std::string msg = "split: the files that must ship with the build take " + formatBytes(base) + ", more than the limit " +
                              formatBytes(split->baseSize) + "; largest:";
            for (size_t i = 0; i < mandatoryList.size() && i < 5; ++i) msg += " " + mandatoryList[i].second + " (" + formatBytes(mandatoryList[i].first) + ")";
            return fail(error, msg);
        }
    }
    for (size_t k = 0; k < blobs.size(); ++k) if (blobs[k].dupOf >= 0) chunkOf[k] = chunkOf[size_t(blobs[k].dupOf)];

    // ---- data: base blobs into `out`, chunked blobs into their chunk files
    out.assign(kHeaderSize, '\0');
    chunkData.assign(chunkBytes.size(), std::string(kChunkHeaderSize, '\0'));
    std::vector<uint64_t> offsets(blobs.size());
    for (size_t k = 0; k < blobs.size(); ++k) {
        Blob& b = blobs[k];
        if (b.dupOf >= 0) { offsets[k] = offsets[size_t(b.dupOf)]; continue; }
        std::string& dst = chunkOf[k] ? chunkData[chunkOf[k] - 1u] : out;
        while (dst.size() % 16) dst += '\0';
        offsets[k] = dst.size();
        dst += storedOf(b);
    }

    std::string toc, strings;
    Hasher content;
    for (size_t k = 0; k < blobs.size(); ++k) {
        const Blob& src = blobs[k].dupOf >= 0 ? blobs[size_t(blobs[k].dupOf)] : blobs[k];
        const std::string& path = inputs_[order[k]].path;
        uint64_t stored = storedOf(src).size();
        put64(toc, offsets[k]); put64(toc, stored); put64(toc, src.raw.size()); put64(toc, src.hash);
        put32(toc, uint32_t(strings.size()));
        toc += char(uint8_t(path.size())); toc += char(uint8_t(path.size() >> 8));
        toc += char(src.codec); toc += char(chunkOf[k]);
        strings += path;
        content.str(path); content.u64(src.hash);

        WriteStats::Item it{path, src.raw.size(), stored, src.codec, blobs[k].dupOf >= 0};
        it.chunk = chunkOf[k];
        st.rawBytes += it.rawSize;
        if (it.duplicate) ++st.duplicates;
        st.items.push_back(std::move(it));
    }
    toc += strings;

    // Chunk headers need the content hash; the chunk table needs the finished chunks.
    uint64_t chunkTable = 0;
    if (!chunkData.empty()) {
        if (result) *result = SplitResult{};
        chunkTable = toc.size();
        put32(toc, uint32_t(chunkData.size()));
        for (size_t c = 0; c < chunkData.size(); ++c) {
            std::string h = "RPKC";
            put32(h, 1); put32(h, uint32_t(c + 1)); put32(h, 0); put64(h, content.value()); put64(h, 0);
            std::memcpy(chunkData[c].data(), h.data(), kChunkHeaderSize);
            char name[64];
            std::snprintf(name, sizeof name, "%s.%03zu.rpkc", split->chunkPrefix.c_str(), c + 1);
            auto sha = sha256(chunkData[c].data(), chunkData[c].size());
            put64(toc, chunkData[c].size());
            toc.append(reinterpret_cast<const char*>(sha.data()), sha.size());
            std::string nm = name;
            toc += char(uint8_t(nm.size())); toc += char(uint8_t(nm.size() >> 8));
            toc += nm;
            if (result) {
                SplitResult::Chunk ch{nm, chunkData[c].size(), 0, toHex(sha.data(), sha.size())};
                for (auto& it : st.items) if (it.chunk == c + 1 && !it.duplicate) ++ch.files;
                result->chunks.push_back(ch);
            }
        }
    }

    uint32_t flags = (opt.external || external_) ? kFlagExternal : 0;
    std::string tocStored = toc;
    if (opt.compress && toc.size() >= kMinCompressSize) {
        std::string z(ZSTD_compressBound(toc.size()), '\0');
        size_t n = ZSTD_compress(z.data(), z.size(), toc.data(), toc.size(), std::clamp(opt.level, 1, ZSTD_maxCLevel()));
        if (!ZSTD_isError(n) && n < toc.size()) { z.resize(n); tocStored = std::move(z); flags |= kFlagTocZstd; }
    }
    while (out.size() % 16) out += '\0';
    uint64_t tocOff = out.size();
    out += tocStored;

    std::string h = "RPAK";
    put32(h, chunkData.empty() ? kPackVersion : kPackVersionSplit); put32(h, flags); put32(h, uint32_t(blobs.size()));
    put64(h, tocOff); put64(h, tocStored.size()); put64(h, toc.size());
    put64(h, hashBytes(toc.data(), toc.size()));
    put64(h, content.value()); put64(h, chunkTable);
    std::memcpy(out.data(), h.data(), kHeaderSize);

    st.packBytes = out.size();
    st.contentHash = content.value();
    if (result) {
        result->baseBytes = out.size();
        result->totalBytes = out.size();
        for (auto& c : chunkData) result->totalBytes += c.size();
    }
    if (split && split->totalBudget && result && result->totalBytes > split->totalBudget)
        return fail(error, "split: the whole content takes " + formatBytes(result->totalBytes) + ", more than the limit " + formatBytes(split->totalBudget));
    return true;
}

std::string formatBytes(uint64_t n) {
    char buf[32];
    if (n >= (uint64_t(1) << 30)) std::snprintf(buf, sizeof buf, "%.2f GB", double(n) / double(uint64_t(1) << 30));
    else if (n >= (uint64_t(1) << 20)) std::snprintf(buf, sizeof buf, "%.1f MB", double(n) / double(1 << 20));
    else if (n >= 1024) std::snprintf(buf, sizeof buf, "%.1f KB", double(n) / 1024.0);
    else std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(n));
    return buf;
}

std::string sizeReport(const WriteStats& st, size_t top) {
    std::ostringstream o;
    double pct = st.rawBytes ? 100.0 * double(st.packBytes) / double(st.rawBytes) : 100.0;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f%%", pct);
    o << "файлов: " << st.items.size() << ", исходный размер " << formatBytes(st.rawBytes) << " -> пак " << formatBytes(st.packBytes)
      << " (" << buf << ")";
    if (st.duplicates) o << ", дубликатов: " << st.duplicates;
    o << "\n";
    std::map<std::string, std::pair<uint64_t, uint64_t>> dirs; // top-level folder -> raw, stored
    for (auto& it : st.items) {
        size_t s = it.path.find('/');
        auto& d = dirs[s == std::string::npos ? "(корень)" : it.path.substr(0, s) + "/"];
        d.first += it.rawSize;
        if (!it.duplicate) d.second += it.storedSize;
    }
    if (dirs.size() > 1) {
        o << "по папкам (в паке):\n";
        std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> v(dirs.begin(), dirs.end());
        std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.second > b.second.second; });
        for (auto& [name, sz] : v) o << "  " << formatBytes(sz.second) << "\t" << name << "  (исходно " << formatBytes(sz.first) << ")\n";
    }
    std::vector<const WriteStats::Item*> big;
    for (auto& it : st.items) if (!it.duplicate) big.push_back(&it);
    std::stable_sort(big.begin(), big.end(), [](auto* a, auto* b) { return a->storedSize > b->storedSize; });
    if (top && !big.empty()) {
        o << "самые большие файлы (в паке):\n";
        for (size_t i = 0; i < big.size() && i < top; ++i)
            o << "  " << formatBytes(big[i]->storedSize) << "\t" << big[i]->path << (big[i]->codec == Codec::Zstd ? "" : "  [без сжатия]") << "\n";
    }
    return o.str();
}

} // namespace rts::pack
