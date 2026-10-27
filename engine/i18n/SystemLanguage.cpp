// User's preferred UI languages from the OS. Kept out of the platform layer on purpose: it needs no
// window/app state, so tools and tests can use it too.
#include "Localization.h"

#include <cstdlib>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>
#elif defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rts::i18n {

namespace {

void addTag(std::vector<std::string>& out, const std::string& raw) {
    std::string t = normalizeTag(raw);
    if (t.empty()) return;
    for (auto& x : out) if (x == t) return;
    out.push_back(t);
}

void addList(std::vector<std::string>& out, const std::string& list, char sep) {
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(sep, i);
        if (j == std::string::npos) j = list.size();
        if (j > i) addTag(out, list.substr(i, j - i));
        i = j + 1;
    }
}

#if defined(__EMSCRIPTEN__)
EM_JS(char*, rtsBrowserLanguages, (), {
    var l = (navigator.languages && navigator.languages.length) ? navigator.languages : [navigator.language || ""];
    return stringToNewUTF8(l.join(","));
});
#endif

} // namespace

std::vector<std::string> systemLanguages() {
    std::vector<std::string> out;
    // Explicit override for testing any platform: RTS_LANG=de or RTS_LANG=uk,ru
    if (const char* forced = std::getenv("RTS_LANG"); forced && *forced) addList(out, forced, ',');

#if defined(__APPLE__)
    if (CFArrayRef langs = CFLocaleCopyPreferredLanguages()) {
        for (CFIndex i = 0, n = CFArrayGetCount(langs); i < n; ++i) {
            auto s = static_cast<CFStringRef>(CFArrayGetValueAtIndex(langs, i));
            char buf[64];
            if (s && CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) addTag(out, buf);
        }
        CFRelease(langs);
    }
#elif defined(_WIN32)
    ULONG count = 0, size = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size > 0) {
        std::wstring buf(size, L'\0');
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf.data(), &size)) {
            for (const wchar_t* p = buf.c_str(); *p; p += wcslen(p) + 1) {
                std::string a;
                for (const wchar_t* q = p; *q; ++q) a += char(*q < 128 ? *q : '?');
                addTag(out, a);
            }
        }
    }
#elif defined(__EMSCRIPTEN__)
    if (char* s = rtsBrowserLanguages()) { addList(out, s, ','); std::free(s); }
#elif defined(__ANDROID__)
    // System locale ("ru-RU"); older devices only have language + region properties.
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get("persist.sys.locale", v) > 0) addTag(out, v);
    else if (__system_property_get("ro.product.locale", v) > 0) addTag(out, v);
    else if (__system_property_get("persist.sys.language", v) > 0) addTag(out, v);
#else
    // POSIX: LANGUAGE is an ordered list ("uk:ru:en"); then the usual locale variables.
    if (const char* l = std::getenv("LANGUAGE"); l && *l) addList(out, l, ':');
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"})
        if (const char* l = std::getenv(var); l && *l) { addTag(out, l); break; }
#endif
    return out;
}

} // namespace rts::i18n
