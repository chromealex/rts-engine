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
    for (const Input& o : inputs_) if (o.path == in.path) return fail(error, "duplicate path in pack: " + in.path);
    inputs_.push_back(std::move(in));
    return true;
}

bool PackWriter::addData(const std::string& packPath, std::string data, std::string* error) {
    if (!addFile(packPath, "<memory>", error)) return false;
    inputs_.back().data = std::move(data);
    inputs_.back().inMemory = true;
    return true;
}

int PackWriter::addDirectory(const std::string& dir, const std::string& prefix, const IgnoreRules& base, std::string* error) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) { fail(error, "not a directory: " + dir); return -1; }
    IgnoreRules rules = base;
    rules.loadFile((fs::path(dir) / ".rpakignore").string());
    // Collect and sort first: directory iteration order differs between file systems.
    std::vector<std::pair<std::string, std::string>> files;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::string rel = fs::relative(it->path(), dir, ec).generic_string();
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

bool PackWriter::write(const std::string& outPath, const WriteOptions& opt, WriteStats* stats, std::string* error) {
    std::string bytes;
    if (!writeToString(bytes, opt, stats, error)) return false;
    std::error_code ec;
    fs::path out(outPath);
    if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);
    // Write to a temp file and rename so a failed build never leaves a half-written pack behind.
    std::string tmp = outPath + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), std::streamsize(bytes.size()));
        if (!f) return fail(error, "cannot write " + tmp);
    }
    fs::rename(tmp, out, ec);
    if (ec) { fs::remove(out, ec); fs::rename(tmp, out, ec); }
    if (ec) return fail(error, "cannot write " + outPath + ": " + ec.message());
    return true;
}

bool PackWriter::writeToString(std::string& out, const WriteOptions& opt, WriteStats* stats, std::string* error) {
    WriteStats local;
    WriteStats& st = stats ? *stats : local;
    st = WriteStats{};

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

    out.assign(kHeaderSize, '\0');
    std::vector<uint64_t> offsets(blobs.size());
    for (size_t k = 0; k < blobs.size(); ++k) {
        Blob& b = blobs[k];
        if (b.dupOf >= 0) { offsets[k] = offsets[size_t(b.dupOf)]; continue; }
        while (out.size() % 16) out += '\0';
        offsets[k] = out.size();
        out += b.codec == Codec::Zstd ? b.stored : b.raw;
    }

    std::string toc, strings;
    Hasher content;
    for (size_t k = 0; k < blobs.size(); ++k) {
        const Blob& src = blobs[k].dupOf >= 0 ? blobs[size_t(blobs[k].dupOf)] : blobs[k];
        const std::string& path = inputs_[order[k]].path;
        uint64_t stored = src.codec == Codec::Zstd ? src.stored.size() : src.raw.size();
        put64(toc, offsets[k]); put64(toc, stored); put64(toc, src.raw.size()); put64(toc, src.hash);
        put32(toc, uint32_t(strings.size()));
        toc += char(uint8_t(path.size())); toc += char(uint8_t(path.size() >> 8));
        toc += char(src.codec); toc += '\0';
        strings += path;
        content.str(path); content.u64(src.hash);

        WriteStats::Item it{path, src.raw.size(), stored, src.codec, blobs[k].dupOf >= 0};
        st.rawBytes += it.rawSize;
        if (it.duplicate) ++st.duplicates;
        st.items.push_back(std::move(it));
    }
    toc += strings;

    uint32_t flags = 0;
    std::string tocStored = toc;
    if (opt.compress && toc.size() >= kMinCompressSize) {
        std::string z(ZSTD_compressBound(toc.size()), '\0');
        size_t n = ZSTD_compress(z.data(), z.size(), toc.data(), toc.size(), std::clamp(opt.level, 1, ZSTD_maxCLevel()));
        if (!ZSTD_isError(n) && n < toc.size()) { z.resize(n); tocStored = std::move(z); flags |= 1u; }
    }
    while (out.size() % 16) out += '\0';
    uint64_t tocOff = out.size();
    out += tocStored;

    std::string h = "RPAK";
    put32(h, kPackVersion); put32(h, flags); put32(h, uint32_t(blobs.size()));
    put64(h, tocOff); put64(h, tocStored.size()); put64(h, toc.size());
    put64(h, hashBytes(toc.data(), toc.size()));
    put64(h, content.value()); put64(h, 0);
    std::memcpy(out.data(), h.data(), kHeaderSize);

    st.packBytes = out.size();
    st.contentHash = content.value();
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
