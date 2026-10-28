// HTTP(S) fetcher for the web build: the browser's fetch() through emscripten_fetch (no threads:
// callbacks arrive on the main loop). The chunk lands in the in-memory file system; persistence
// across sessions comes from the browser's HTTP cache.
#include <emscripten/fetch.h>

#include <cstdio>
#include <string>

#include "../Chunks.h"

namespace rts::pack {

namespace {
class WebDownload final : public Download {
public:
    std::string dest, err;
    bool finished = false, good = false;
    uint64_t got = 0;
    bool done() const override { return finished; }
    bool ok() const override { return good; }
    uint64_t bytes() const override { return got; }
    std::string error() const override { return err; }
    ~WebDownload() override { if (fetch && !finished) emscripten_fetch_close(fetch); }
    emscripten_fetch_t* fetch = nullptr;
};

void onSuccess(emscripten_fetch_t* f) {
    auto* d = static_cast<WebDownload*>(f->userData);
    std::FILE* out = std::fopen(d->dest.c_str(), "wb");
    d->good = out && std::fwrite(f->data, 1, size_t(f->numBytes), out) == size_t(f->numBytes);
    if (out) std::fclose(out);
    if (!d->good) d->err = "cannot write " + d->dest;
    d->got = f->numBytes;
    d->finished = true;
    d->fetch = nullptr;
    emscripten_fetch_close(f);
}
void onError(emscripten_fetch_t* f) {
    auto* d = static_cast<WebDownload*>(f->userData);
    d->err = "HTTP " + std::to_string(f->status);
    d->finished = true;
    d->fetch = nullptr;
    emscripten_fetch_close(f);
}
void onProgress(emscripten_fetch_t* f) { static_cast<WebDownload*>(f->userData)->got = f->dataOffset + f->numBytes; }
} // namespace

std::unique_ptr<Download> fetchHttp(const std::string& url, const std::string& destPath, std::string&) {
    auto d = std::make_unique<WebDownload>();
    d->dest = destPath;
    emscripten_fetch_attr_t a;
    emscripten_fetch_attr_init(&a);
    std::snprintf(a.requestMethod, sizeof a.requestMethod, "GET");
    a.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    a.onsuccess = onSuccess;
    a.onerror = onError;
    a.onprogress = onProgress;
    a.userData = d.get();
    d->fetch = emscripten_fetch(&a, url.c_str());
    return d;
}

void setAndroidJavaVm(void*) {}

} // namespace rts::pack
