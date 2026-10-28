#include "Chunks.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace rts::pack {

namespace {

std::string hashHex(uint64_t v) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

void replaceAll(std::string& s, const std::string& a, const std::string& b) {
    for (size_t p = 0; (p = s.find(a, p)) != std::string::npos; p += b.size()) s.replace(p, a.size(), b);
}

class Finished final : public Download {
public:
    Finished(bool ok, uint64_t bytes, std::string err) : ok_(ok), bytes_(bytes), err_(std::move(err)) {}
    bool done() const override { return true; }
    bool ok() const override { return ok_; }
    uint64_t bytes() const override { return bytes_; }
    std::string error() const override { return err_; }

private:
    bool ok_;
    uint64_t bytes_;
    std::string err_;
};

} // namespace

const char* statusName(ChunkManager::Status s) {
    switch (s) {
    case ChunkManager::Status::NotDownloaded: return "not downloaded";
    case ChunkManager::Status::Queued: return "queued";
    case ChunkManager::Status::Downloading: return "downloading";
    case ChunkManager::Status::Verifying: return "verifying";
    case ChunkManager::Status::Ready: return "ready";
    case ChunkManager::Status::Failed: return "failed";
    }
    return "?";
}

std::unique_ptr<Download> fetchFile(const std::string& url, const std::string& destPath, std::string& error) {
    std::string path = url.rfind("file://", 0) == 0 ? url.substr(7) : url;
#if defined(_WIN32)
    if (path.size() > 2 && path[0] == '/' && path[2] == ':') path.erase(0, 1);   // file:///C:/...
#endif
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) { error = "not found: " + path; return nullptr; }
    fs::copy_file(path, destPath, fs::copy_options::overwrite_existing, ec);
    if (ec) return std::make_unique<Finished>(false, 0, "copy " + path + ": " + ec.message());
    return std::make_unique<Finished>(true, uint64_t(fs::file_size(destPath, ec)), "");
}

ChunkManager::ChunkManager() {
    registerScheme("file", fetchFile);
    registerScheme("http", fetchHttp);
    registerScheme("https", fetchHttp);
}

void ChunkManager::clear() {
    for (auto& it : items_) if (it->verifyFile) std::fclose(it->verifyFile);
    items_.clear();
}

std::string ChunkManager::cachePath(const Pack& pack, size_t index) const {
    return (fs::path(cacheDir_) / hashHex(pack.contentHash()) / pack.chunks()[index - 1].file).string();
}

std::string ChunkManager::url(const std::string& source, const Pack& pack, size_t index) const {
    std::string u = source;
    const std::string& file = pack.chunks()[index - 1].file;
    replaceAll(u, "{hash}", hashHex(pack.contentHash()));
    if (u.find("{file}") != std::string::npos) replaceAll(u, "{file}", file);
    else u += (u.empty() || u.back() == '/' || u.back() == '\\' ? "" : "/") + file;
    return u;
}

FetchFn ChunkManager::fetcherFor(const std::string& u) const {
    size_t p = u.find("://");
    std::string scheme = p == std::string::npos ? "file" : u.substr(0, p);
    for (char& c : scheme) c = char(std::tolower(static_cast<unsigned char>(c)));
    auto it = schemes_.find(scheme);
    return it == schemes_.end() ? FetchFn{} : it->second;
}

void ChunkManager::addPack(Pack& pack) {
    for (size_t i = 1; i <= pack.chunks().size(); ++i) {
        auto it = std::make_unique<Item>();
        it->pack = &pack;
        it->index = i;
        if (!cacheDir_.empty()) {
            // A cached chunk was verified when it was downloaded; size + header catch truncation
            // and leftovers of other builds.
            std::string path = cachePath(pack, i);
            std::error_code ec;
            if (fs::is_regular_file(path, ec)) {
                auto src = openFileSource(path);
                std::string err;
                if (src && pack.attachChunk(i, std::move(src), &err)) it->status = Status::Ready;
                else fs::remove(path, ec);
            }
        }
        items_.push_back(std::move(it));
    }
}

void ChunkManager::request(const Pack& pack, size_t index) {
    for (auto& it : items_)
        if (it->pack == &pack && it->index == index && (it->status == Status::NotDownloaded || it->status == Status::Failed)) {
            it->status = Status::Queued;
            it->sourceIndex = 0;
            it->error.clear();
        }
}

void ChunkManager::requestAll() {
    for (auto& it : items_) request(*it->pack, it->index);
}

void ChunkManager::start(Item& it) {
    const ChunkInfo& info = it.pack->chunks()[it.index - 1];
    if (cacheDir_.empty()) { it.status = Status::Failed; it.error = "no cache folder for downloads"; return; }
    std::string final = cachePath(*it.pack, it.index);
    std::error_code ec;
    fs::create_directories(fs::path(final).parent_path(), ec);
    it.partPath = final + ".part";
    while (it.sourceIndex < sources_.size()) {
        std::string u = url(sources_[it.sourceIndex], *it.pack, it.index);
        FetchFn f = fetcherFor(u);
        std::string err;
        it.dl = f ? f(u, it.partPath, err) : nullptr;
        if (it.dl) { it.status = Status::Downloading; return; }
        it.error = info.file + ": " + (f ? err : "no fetcher for " + u);
        ++it.sourceIndex;
    }
    it.status = Status::Failed;
    if (it.error.empty()) it.error = info.file + ": no content sources configured";
}

void ChunkManager::attach(Item& it, const std::string& path) {
    std::string err;
    auto src = openFileSource(path);
    if (src && it.pack->attachChunk(it.index, std::move(src), &err)) { it.status = Status::Ready; return; }
    it.status = Status::Failed;
    it.error = err.empty() ? "cannot open " + path : err;
}

void ChunkManager::finishVerify(Item& it, bool ok, const std::string& err) {
    if (it.verifyFile) { std::fclose(it.verifyFile); it.verifyFile = nullptr; }
    it.sha.reset();
    std::error_code ec;
    if (ok) {
        std::string final = cachePath(*it.pack, it.index);
        fs::rename(it.partPath, final, ec);
        if (ec) { fs::remove(final, ec); fs::rename(it.partPath, final, ec); }
        if (!ec) { attach(it, final); return; }
        it.error = "cannot store " + final;
    } else {
        it.error = err;
    }
    fs::remove(it.partPath, ec);       // bad data: never resume from it
    ++it.sourceIndex;                  // next mirror
    it.status = Status::Queued;
    if (it.sourceIndex >= sources_.size()) it.status = Status::Failed;
}

void ChunkManager::poll() {
    int active = 0;
    for (auto& p : items_) if (p->status == Status::Downloading || p->status == Status::Verifying) ++active;
    for (auto& p : items_) {
        Item& it = *p;
        const ChunkInfo& info = it.pack->chunks()[it.index - 1];
        switch (it.status) {
        case Status::Queued:
            if (active < maxParallel_) { start(it); if (it.status == Status::Downloading) ++active; }
            break;
        case Status::Downloading:
            if (!it.dl->done()) break;
            if (!it.dl->ok()) {
                it.error = info.file + ": " + it.dl->error();
                it.dl.reset();
                ++it.sourceIndex;
                it.status = it.sourceIndex < sources_.size() ? Status::Queued : Status::Failed;
                --active;
                break;
            }
            it.dl.reset();
            it.verifyFile = std::fopen(it.partPath.c_str(), "rb");
            if (!it.verifyFile) { finishVerify(it, false, info.file + ": downloaded file vanished"); break; }
            it.sha = std::make_unique<Sha256>();
            it.verified = 0;
            it.status = Status::Verifying;
            [[fallthrough]];
        case Status::Verifying: {
            std::vector<char> buf(1u << 16);
            uint64_t budget = verifyStep_;
            while (budget) {
                size_t n = std::fread(buf.data(), 1, std::min<uint64_t>(buf.size(), budget), it.verifyFile);
                if (n == 0) break;
                it.sha->update(buf.data(), n);
                it.verified += n;
                budget -= n;
                if (it.verified > info.size) break;
            }
            if (budget && (std::feof(it.verifyFile) || it.verified > info.size)) {
                if (it.verified != info.size) finishVerify(it, false, info.file + ": size " + std::to_string(it.verified) + " != " + std::to_string(info.size));
                else if (it.sha->finish() != info.sha256) finishVerify(it, false, info.file + ": SHA-256 mismatch (corrupt or tampered)");
                else finishVerify(it, true, "");
                if (it.status != Status::Verifying) --active;
            }
            break;
        }
        default: break;
        }
    }
}

ChunkManager::Progress ChunkManager::progress() const {
    Progress pr;
    for (auto& p : items_) {
        const ChunkInfo& info = p->pack->chunks()[p->index - 1];
        ++pr.chunks;
        switch (p->status) {
        case Status::Ready: ++pr.ready; pr.bytesTotal += info.size; pr.bytesReady += info.size; break;
        case Status::Queued: pr.bytesTotal += info.size; pr.busy = true; break;
        case Status::Downloading:
            ++pr.active; pr.busy = true; pr.bytesTotal += info.size;
            pr.bytesReady += std::min(info.size, p->dl ? p->dl->bytes() : 0) / 2;   // download = first half
            break;
        case Status::Verifying:
            ++pr.active; pr.busy = true; pr.bytesTotal += info.size;
            pr.bytesReady += info.size / 2 + std::min(info.size, p->verified) / 2;
            break;
        case Status::Failed: ++pr.failed; break;
        default: break;
        }
    }
    return pr;
}

std::vector<ChunkManager::ChunkView> ChunkManager::chunks() const {
    std::vector<ChunkView> out;
    for (auto& p : items_) {
        ChunkView v;
        v.pack = p->pack;
        v.index = p->index;
        v.info = &p->pack->chunks()[p->index - 1];
        v.status = p->status;
        v.bytes = p->status == Status::Ready ? v.info->size : p->status == Status::Verifying ? p->verified : p->dl ? p->dl->bytes() : 0;
        v.error = p->error;
        out.push_back(v);
    }
    return out;
}

size_t attachChunksNextTo(Pack& p, const std::string& packPath, std::vector<std::string>* problems) {
    size_t attached = 0;
    fs::path dir = fs::path(packPath).parent_path();
    for (size_t i = 1; i <= p.chunks().size(); ++i) {
        const ChunkInfo& c = p.chunks()[i - 1];
        std::error_code ec;
        fs::path f = dir / "chunks" / c.file;
        if (!fs::exists(f, ec)) f = dir / c.file;
        if (!fs::exists(f, ec)) { if (problems) problems->push_back(c.file + ": not found next to the pack"); continue; }
        std::FILE* in = std::fopen(f.string().c_str(), "rb");
        if (!in) { if (problems) problems->push_back(c.file + ": cannot open"); continue; }
        Sha256 sha;
        std::vector<char> buf(1 << 16);
        for (size_t n; (n = std::fread(buf.data(), 1, buf.size(), in)) > 0;) sha.update(buf.data(), n);
        std::fclose(in);
        if (sha.finish() != c.sha256) { if (problems) problems->push_back(c.file + ": SHA-256 mismatch"); continue; }
        std::string err;
        if (!p.attachChunk(i, openFileSource(f.string()), &err)) { if (problems) problems->push_back(err); continue; }
        ++attached;
    }
    return attached;
}

ChunkManager::Status ChunkManager::status(const Pack& pack, size_t index) const {
    for (auto& p : items_) if (p->pack == &pack && p->index == index) return p->status;
    return Status::NotDownloaded;
}

} // namespace rts::pack
