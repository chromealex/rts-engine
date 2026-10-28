// HTTP(S) fetcher for Linux and other POSIX desktops: libcurl loaded at runtime (dlopen), so the
// engine has no build-time dependency and runs (without downloads) where libcurl is missing.
#include <dlfcn.h>
#include <sys/stat.h>

#include <cstdio>

#include "ThreadDownload.h"

namespace rts::pack {

namespace {

// Stable libcurl ABI values (curl/curl.h).
enum : int {
    kOptWriteData = 10001, kOptUrl = 10002, kOptErrorBuffer = 10010, kOptUserAgent = 10018, kOptXferInfoData = 10057,
    kOptLowSpeedLimit = 19, kOptLowSpeedTime = 20, kOptNoProgress = 43, kOptFailOnError = 45, kOptFollowLocation = 52,
    kOptConnectTimeout = 78, kOptNoSignal = 99, kOptWriteFunction = 20011, kOptXferInfoFunction = 20219, kOptResumeFromLarge = 30116,
};
constexpr int kInfoResponseCode = 0x200000 + 2;

struct Curl {
    void* lib = nullptr;
    int (*globalInit)(long) = nullptr;
    void* (*init)() = nullptr;
    int (*setopt)(void*, int, ...) = nullptr;
    int (*perform)(void*) = nullptr;
    int (*getinfo)(void*, int, ...) = nullptr;
    void (*cleanup)(void*) = nullptr;
    const char* (*strerror)(int) = nullptr;
    bool ok = false;
};

const Curl& curl() {
    static Curl c = [] {
        Curl k;
        for (const char* name : {"libcurl.so.4", "libcurl.so", "libcurl-gnutls.so.4", "libcurl.4.dylib", "libcurl.dylib"})
            if ((k.lib = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
        if (!k.lib) return k;
        k.globalInit = reinterpret_cast<int (*)(long)>(dlsym(k.lib, "curl_global_init"));
        k.init = reinterpret_cast<void* (*)()>(dlsym(k.lib, "curl_easy_init"));
        k.setopt = reinterpret_cast<int (*)(void*, int, ...)>(dlsym(k.lib, "curl_easy_setopt"));
        k.perform = reinterpret_cast<int (*)(void*)>(dlsym(k.lib, "curl_easy_perform"));
        k.getinfo = reinterpret_cast<int (*)(void*, int, ...)>(dlsym(k.lib, "curl_easy_getinfo"));
        k.cleanup = reinterpret_cast<void (*)(void*)>(dlsym(k.lib, "curl_easy_cleanup"));
        k.strerror = reinterpret_cast<const char* (*)(int)>(dlsym(k.lib, "curl_easy_strerror"));
        k.ok = k.globalInit && k.init && k.setopt && k.perform && k.getinfo && k.cleanup && k.strerror && k.globalInit(3) == 0;
        return k;
    }();
    return c;
}

struct Sink {
    std::FILE* f = nullptr;
    std::string path;
    void* handle = nullptr;
    bool resumed = false, checked = false;
    ThreadDownload* dl = nullptr;
    uint64_t base = 0;
};

size_t onWrite(char* data, size_t size, size_t n, void* user) {
    auto* s = static_cast<Sink*>(user);
    if (s->resumed && !s->checked) {
        // A server that ignores Range answers 200 with the whole file: start over.
        long code = 0;
        curl().getinfo(s->handle, kInfoResponseCode, &code);
        if (code == 200) {
            std::fclose(s->f);
            s->f = std::fopen(s->path.c_str(), "wb");
            s->base = 0;
            if (!s->f) return 0;
        }
        s->checked = true;
    }
    size_t w = std::fwrite(data, size, n, s->f);
    s->dl->bytes_ += w * size;
    return w * size;
}

} // namespace

std::unique_ptr<Download> fetchHttp(const std::string& url, const std::string& destPath, std::string& error) {
    if (!curl().ok) { error = "HTTP downloads need libcurl (libcurl4), which was not found"; return nullptr; }
    auto dl = std::make_unique<ThreadDownload>();
    dl->run([url, destPath](ThreadDownload& d) {
        const Curl& c = curl();
        Sink s;
        s.dl = &d;
        s.path = destPath;
        struct stat st {};
        if (::stat(destPath.c_str(), &st) == 0 && st.st_size > 0) { s.resumed = true; s.base = uint64_t(st.st_size); }
        s.f = std::fopen(destPath.c_str(), s.resumed ? "ab" : "wb");
        if (!s.f) { d.fail("cannot write " + destPath); return; }
        d.bytes_ = s.base;
        void* h = c.init();
        s.handle = h;
        char err[256] = {0};
        c.setopt(h, kOptUrl, url.c_str());
        c.setopt(h, kOptWriteFunction, &onWrite);
        c.setopt(h, kOptWriteData, &s);
        c.setopt(h, kOptErrorBuffer, err);
        c.setopt(h, kOptFollowLocation, 1L);
        c.setopt(h, kOptFailOnError, 1L);
        c.setopt(h, kOptNoSignal, 1L);
        c.setopt(h, kOptConnectTimeout, 20L);
        c.setopt(h, kOptLowSpeedLimit, 1L);       // abort after 60 s without data
        c.setopt(h, kOptLowSpeedTime, 60L);
        c.setopt(h, kOptUserAgent, "RTSEngine/1");
        if (s.resumed) c.setopt(h, kOptResumeFromLarge, static_cast<long long>(s.base));
        int rc = c.perform(h);
        long code = 0;
        c.getinfo(h, kInfoResponseCode, &code);
        c.cleanup(h);
        if (std::fclose(s.f) != 0 && rc == 0) { d.fail("cannot write " + destPath); return; }
        if (rc == 33 /*CURLE_RANGE_ERROR*/ || code == 416) {   // nothing more to fetch or bad range: let verification decide
            d.succeed();
            return;
        }
        if (rc != 0) { d.fail(std::string(err[0] ? err : c.strerror(rc)) + (code ? " (HTTP " + std::to_string(code) + ")" : "")); return; }
        d.succeed();
    });
    return dl;
}

void setAndroidJavaVm(void*) {}

} // namespace rts::pack
