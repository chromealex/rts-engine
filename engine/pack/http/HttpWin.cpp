// HTTP(S) fetcher for Windows: WinHTTP (system TLS, proxy settings honoured).
#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <vector>

#include "ThreadDownload.h"

namespace rts::pack {

namespace {
std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}
std::string lastError(const char* what) { return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")"; }
} // namespace

std::unique_ptr<Download> fetchHttp(const std::string& url, const std::string& destPath, std::string& error) {
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { error = "bad URL " + url; return nullptr; }
    std::wstring h(host, uc.dwHostNameLength), p(path, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength && uc.lpszExtraInfo) p += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    const bool tls = uc.nScheme == INTERNET_SCHEME_HTTPS;
    const INTERNET_PORT port = uc.nPort;
    auto dl = std::make_unique<ThreadDownload>();
    dl->run([h, p, tls, port, destPath](ThreadDownload& d) {
        // Resume a partial file with a Range request.
        uint64_t have = 0;
        if (std::FILE* f = std::fopen(destPath.c_str(), "rb")) { std::fseek(f, 0, SEEK_END); have = uint64_t(std::ftell(f)); std::fclose(f); }
        HINTERNET s = WinHttpOpen(L"RTSEngine/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s) s = WinHttpOpen(L"RTSEngine/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET c = s ? WinHttpConnect(s, h.c_str(), port, 0) : nullptr;
        HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", p.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, tls ? WINHTTP_FLAG_SECURE : 0) : nullptr;
        auto closeAll = [&] { if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); if (s) WinHttpCloseHandle(s); };
        if (!r) { d.fail(lastError("WinHttpOpenRequest")); closeAll(); return; }
        std::wstring range;
        if (have) range = L"Range: bytes=" + std::to_wstring(have) + L"-";
        if (!WinHttpSendRequest(r, range.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : range.c_str(), DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(r, nullptr)) {
            d.fail(lastError("HTTP request")); closeAll(); return;
        }
        DWORD code = 0, len = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        if (code == 416) { d.succeed(); closeAll(); return; }          // already complete: verification decides
        if (code != 200 && code != 206) { d.fail("HTTP " + std::to_string(code)); closeAll(); return; }
        if (code == 200) have = 0;                                      // server ignored Range
        std::FILE* f = std::fopen(destPath.c_str(), have ? "ab" : "wb");
        if (!f) { d.fail("cannot write " + destPath); closeAll(); return; }
        d.bytes_ = have;
        std::vector<char> buf(1 << 16);
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(r, buf.data(), DWORD(buf.size()), &got)) { d.fail(lastError("WinHttpReadData")); break; }
            if (got == 0) { d.succeed(); break; }
            if (std::fwrite(buf.data(), 1, got, f) != got) { d.fail("cannot write " + destPath); break; }
            d.bytes_ += got;
        }
        std::fclose(f);
        closeAll();
    });
    return dl;
}

void setAndroidJavaVm(void*) {}

} // namespace rts::pack
