#include "FileAssoc.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "engine/data/Json.h"
#include "tools/branding/ProjectIcon.h"

#if defined(_WIN32)
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600   // RegGetValueW (Vista+); older MinGW headers default lower
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#endif

namespace fs = std::filesystem;

namespace rts::editor {

namespace {

fs::path home() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (const char* h = std::getenv("USERPROFILE"); h && *h) return h;
    return {};
}

[[maybe_unused]] std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

[[maybe_unused]] bool writeText(const fs::path& p, const std::string& s) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f << s;
    return bool(f);
}

[[maybe_unused]] std::string shellQuote(const std::string& s) {
#if defined(_WIN32)
    return "\"" + s + "\"";
#else
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
#endif
}

[[maybe_unused]] bool inPath(const std::string& tool) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::stringstream ss(path);
    for (std::string dir; std::getline(ss, dir, ':');) {
        std::error_code ec;
        if (!dir.empty() && fs::exists(fs::path(dir) / tool, ec)) return true;
    }
    return false;
}

[[maybe_unused]] int run(const std::string& cmd) {
#if defined(_WIN32)
    return std::system((cmd + " >NUL 2>&1").c_str());
#else
    return std::system((cmd + " >/dev/null 2>&1").c_str());
#endif
}

bool automated() {
    if (const char* v = std::getenv("RTS_FILE_ASSOC"); v && (std::string(v) == "off" || std::string(v) == "0")) return true;
    return std::getenv("RTS_SCREENSHOT") || std::getenv("RTS_EXIT_AFTER_FRAMES");
}

[[maybe_unused]] fs::path iconsDir() { return home() / ".rtsengine" / "icons"; }

std::string& logoPath() { static std::string p; return p; }

// ------------------------------------------------------------------------------------- Linux
#if !defined(_WIN32) && !defined(__APPLE__)
const char* kIconName = "application-x-rtsengine-project";
const char* kAppIconName = "rtsengine-editor";
const char* kMimeXml = "rtsengine-project.xml";

fs::path desktopFile() { return fs::path(xdgDataHome()) / "applications" / kEditorDesktopId; }
fs::path mimeXmlFile() { return fs::path(xdgDataHome()) / "mime" / "packages" / kMimeXml; }
fs::path mimeAppsList() { return fs::path(xdgConfigHome()) / "mimeapps.list"; }

// first handler listed for our type in [Default Applications] of mimeapps.list ("" if none)
std::string defaultHandler() {
    std::stringstream ss(readText(mimeAppsList()));
    bool section = false;
    const std::string key = std::string(kProjectMimeType) + "=";
    for (std::string l; std::getline(ss, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty() && l[0] == '[') { section = l == "[Default Applications]"; continue; }
        if (section && l.rfind(key, 0) == 0) {
            std::string v = l.substr(key.size());
            return v.substr(0, v.find(';'));
        }
    }
    return {};
}

bool setDefaultHandler() {
    std::stringstream ss(readText(mimeAppsList()));
    std::vector<std::string> lines;
    for (std::string l; std::getline(ss, l);) { if (!l.empty() && l.back() == '\r') l.pop_back(); lines.push_back(l); }
    const std::string key = std::string(kProjectMimeType) + "=", entry = key + kEditorDesktopId + ";";
    int section = -1;
    for (size_t i = 0; i < lines.size(); ++i) if (lines[i] == "[Default Applications]") { section = int(i); break; }
    if (section < 0) {
        if (!lines.empty() && !lines.back().empty()) lines.push_back("");
        lines.push_back("[Default Applications]");
        lines.push_back(entry);
    } else {
        size_t i = size_t(section) + 1;
        bool done = false;
        for (; i < lines.size() && (lines[i].empty() || lines[i][0] != '['); ++i)
            if (lines[i].rfind(key, 0) == 0) { lines[i] = entry; done = true; }
        if (!done) lines.insert(lines.begin() + long(section) + 1, entry);
    }
    std::string out;
    for (auto& l : lines) out += l + "\n";
    return writeText(mimeAppsList(), out);
}

std::string mimeXml() {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<!-- RTSEngine project file; installed by the RTS editor (apps/editor/core/FileAssoc.cpp) -->
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type=")" + std::string(kProjectMimeType) + R"(">
    <comment>RTSEngine project</comment>
    <comment xml:lang="ru">Проект RTSEngine</comment>
    <sub-class-of type="application/json"/>
    <icon name=")" + kIconName + R"("/>
    <generic-icon name=")" + kIconName + R"("/>
    <glob pattern="*.rtsproj" weight="80"/>
  </mime-type>
</mime-info>
)";
}

std::string desktopEntry(const std::string& exe) {
    return "[Desktop Entry]\n"
           "Type=Application\n"
           "Version=1.0\n"
           "Name=RTS Editor\n"
           "Name[ru]=Редактор RTS Engine\n"
           "GenericName=Game Editor\n"
           "Comment=Edit RTSEngine game projects\n"
           "Comment[ru]=Редактор проектов игр RTSEngine\n"
           "Exec=" + desktopExec(exe) + "\n"
           "Icon=" + kAppIconName + "\n"
           "Terminal=false\n"
           "Categories=Development;Game;\n"
           "MimeType=" + kProjectMimeType + ";\n"
           "StartupWMClass=rts_editor\n"
           "X-RTSEngine-Exe=" + exe + "\n";
}

bool globsKnow() {   // update-mime-database ran over our package
    return readText(fs::path(xdgDataHome()) / "mime" / "globs2").find("rtsproj") != std::string::npos;
}

AssocStatus statusLinux(const std::string& exe) {
    AssocStatus s;
    s.supported = !home().empty() && !exe.empty();
    if (!s.supported) { s.detail = "нет домашней папки (HOME)"; return s; }
    std::error_code ec;
    const bool desk = readText(desktopFile()).find("X-RTSEngine-Exe=" + exe + "\n") != std::string::npos;
    const bool mime = fs::exists(mimeXmlFile(), ec);
    const std::string def = defaultHandler();
    if (!mime || !fs::exists(desktopFile(), ec)) s.detail = "тип .rtsproj не зарегистрирован";
    else if (!desk) s.detail = ".rtsproj открывает другой редактор: " + desktopFile().string();
    else if (def != kEditorDesktopId) {
        s.detail = def.empty() ? "редактор не выбран программой по умолчанию" : "по умолчанию .rtsproj открывает " + def;
        s.otherDefault = !def.empty();
    }
    else {
        s.registered = true;
        s.detail = globsKnow() ? ".rtsproj открывается этим редактором"
                               : ".rtsproj зарегистрирован; файловые менеджеры увидят тип после update-mime-database (пакет shared-mime-info)";
    }
    return s;
}

bool registerLinux(const std::string& exe, std::string* msg) {
    const fs::path data = xdgDataHome();
    branding::Image logo;
    const branding::Image* lp = !logoPath().empty() && branding::loadImageFile(logoPath(), logo) ? &logo : nullptr;
    // icons: hicolor theme sizes for the mime type and the launcher
    for (int px : {16, 24, 32, 48, 64, 128, 256}) {
        const fs::path dir = data / "icons" / "hicolor" / (std::to_string(px) + "x" + std::to_string(px));
        std::string err;
        if (!branding::writePng((dir / "mimetypes" / (std::string(kIconName) + ".png")).string(), branding::projectDocumentIcon(px, lp), &err) ||
            !branding::writePng((dir / "apps" / (std::string(kAppIconName) + ".png")).string(), branding::editorAppIcon(px, lp), &err)) {
            if (msg) *msg = err;
            return false;
        }
    }
    if (!writeText(mimeXmlFile(), mimeXml()) || !writeText(desktopFile(), desktopEntry(exe))) {
        if (msg) *msg = "не удалось записать файлы в " + data.string();
        return false;
    }
    std::vector<std::string> notes;
    if (inPath("update-mime-database")) run("update-mime-database " + shellQuote((data / "mime").string()));
    else notes.push_back("нет update-mime-database (shared-mime-info)");
    if (inPath("update-desktop-database")) run("update-desktop-database " + shellQuote((data / "applications").string()));
    if (inPath("gtk-update-icon-cache") && fs::exists(data / "icons" / "hicolor" / "index.theme"))
        run("gtk-update-icon-cache -q -t " + shellQuote((data / "icons" / "hicolor").string()));
    if (inPath("xdg-mime")) run(std::string("xdg-mime default ") + kEditorDesktopId + " " + kProjectMimeType);
    if (defaultHandler() != kEditorDesktopId && !setDefaultHandler()) {
        if (msg) *msg = "не удалось записать " + mimeAppsList().string();
        return false;
    }
    if (msg) {
        *msg = "Файлы .rtsproj открываются этим редактором (" + desktopFile().string() + ")";
        for (auto& n : notes) *msg += "; " + n;
    }
    return true;
}
#endif

// ------------------------------------------------------------------------------------- Windows
#if defined(_WIN32)
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}
std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring selfExe() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    buf.resize(n);
    return buf;
}
bool regSet(const std::wstring& sub, const wchar_t* name, const std::wstring& val) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    const LONG r = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()), DWORD((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}
std::wstring regGet(const std::wstring& sub, const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0) return {};
    std::wstring v(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), name, RRF_RT_REG_SZ, nullptr, v.data(), &size) != ERROR_SUCCESS) return {};
    v.resize(size / sizeof(wchar_t));
    while (!v.empty() && v.back() == L'\0') v.pop_back();
    return v;
}
const std::wstring kClasses = L"Software\\Classes\\";
std::wstring progId() { return utf8ToWide(kProjectProgId); }
std::wstring openCommand() { return L"\"" + selfExe() + L"\" \"%1\""; }

AssocStatus statusWindows() {
    AssocStatus s;
    s.supported = true;
    const std::wstring ext = regGet(kClasses + L".rtsproj", nullptr), cmd = regGet(kClasses + progId() + L"\\shell\\open\\command", nullptr);
    const std::wstring choice = regGet(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.rtsproj\\UserChoice", L"ProgId");
    if (ext != progId() || cmd.empty()) s.detail = "тип .rtsproj не зарегистрирован";
    else if (_wcsicmp(cmd.c_str(), openCommand().c_str()) != 0) s.detail = ".rtsproj открывает другой редактор: " + wideToUtf8(cmd);
    else if (!choice.empty() && choice != progId()) {
        s.otherDefault = true;
        s.detail = "в Windows для .rtsproj выбрана другая программа (" + wideToUtf8(choice) + "): «Открыть с помощью» → RTS Editor → «Всегда»";
    }
    else { s.registered = true; s.detail = ".rtsproj открывается этим редактором"; }
    return s;
}

bool registerWindows(std::string* msg) {
    const fs::path icons = iconsDir();
    std::string err;
    if (branding::writeEditorIcons(icons.string(), logoPath(), &err).empty()) { if (msg) *msg = err; return false; }
    const std::wstring pid = progId(), ico = (icons / "rtsproj.ico").wstring();
    bool ok = regSet(kClasses + L".rtsproj", nullptr, pid) &&
              regSet(kClasses + L".rtsproj", L"Content Type", utf8ToWide(kProjectMimeType)) &&
              regSet(kClasses + L".rtsproj\\OpenWithProgids", pid.c_str(), L"") &&
              regSet(kClasses + pid, nullptr, utf8ToWide("Проект RTSEngine")) &&
              regSet(kClasses + pid, L"FriendlyTypeName", utf8ToWide("Проект RTSEngine")) &&
              regSet(kClasses + pid + L"\\DefaultIcon", nullptr, L"\"" + ico + L"\",0") &&
              regSet(kClasses + pid + L"\\shell\\open", L"FriendlyAppName", L"RTS Editor") &&
              regSet(kClasses + pid + L"\\shell\\open\\command", nullptr, openCommand());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (msg) *msg = ok ? "Файлы .rtsproj открываются этим редактором (HKCU\\Software\\Classes\\.rtsproj)" : "не удалось записать реестр HKCU\\Software\\Classes";
    return ok;
}
#endif

// ------------------------------------------------------------------------------------- macOS
#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // LS*RoleHandler*: still the only C API for the default handler
std::string bundleOf(const std::string& exe) {
    const size_t at = exe.find(".app/Contents/MacOS/");
    return at == std::string::npos ? std::string() : exe.substr(0, at + 4);
}
fs::path stateFile() { return home() / ".rtsengine" / "fileassoc.json"; }
std::string registeredBundle() {
    try { return Json::parseFile(stateFile().string()).getStr("macos"); } catch (...) { return {}; }
}
std::string cf(CFStringRef s) {
    if (!s) return {};
    char buf[1024];
    return CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8) ? std::string(buf) : std::string();
}
std::string ownBundleId() {
    CFBundleRef b = CFBundleGetMainBundle();
    std::string id = b ? cf(CFBundleGetIdentifier(b)) : std::string();
    return id.empty() ? "com.rtsengine.editor" : id;
}
std::string defaultHandlerMac() {
    CFStringRef uti = CFStringCreateWithCString(nullptr, kProjectUti, kCFStringEncodingUTF8);
    CFStringRef h = LSCopyDefaultRoleHandlerForContentType(uti, kLSRolesAll);
    std::string r = cf(h);
    if (h) CFRelease(h);
    CFRelease(uti);
    return r;
}

AssocStatus statusMac(const std::string& exe) {
    AssocStatus s;
    const std::string bundle = bundleOf(exe);
    s.supported = !bundle.empty();
    if (!s.supported) { s.detail = "редактор запущен не из RTSEditor.app: тип .rtsproj объявляет Info.plist бандла"; return s; }
    const std::string h = defaultHandlerMac();
    if (registeredBundle() != bundle) s.detail = "бандл " + bundle + " ещё не зарегистрирован в LaunchServices";
    else if (h.empty() || h != ownBundleId()) {
        s.detail = h.empty() ? "у .rtsproj нет программы по умолчанию" : "по умолчанию .rtsproj открывает " + h;
        s.otherDefault = !h.empty();
    }
    else { s.registered = true; s.detail = ".rtsproj открывается этим редактором"; }
    return s;
}

bool registerMac(const std::string& exe, std::string* msg) {
    const std::string bundle = bundleOf(exe);
    if (bundle.empty()) { if (msg) *msg = "запустите редактор из RTSEditor.app"; return false; }
    const char* ls = "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister";
    std::error_code ec;
    if (fs::exists(ls, ec)) run(shellQuote(ls) + " -f " + shellQuote(bundle));
    else if (CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(bundle.c_str()), CFIndex(bundle.size()), true)) {
        LSRegisterURL(url, true);
        CFRelease(url);
    }
    CFStringRef uti = CFStringCreateWithCString(nullptr, kProjectUti, kCFStringEncodingUTF8);
    CFStringRef id = CFStringCreateWithCString(nullptr, ownBundleId().c_str(), kCFStringEncodingUTF8);
    const OSStatus st = LSSetDefaultRoleHandlerForContentType(uti, kLSRolesAll, id);
    CFRelease(id);
    CFRelease(uti);
    Json j = Json::object();
    j.set("macos", Json::string(bundle));
    writeText(stateFile(), j.dump(2) + "\n");
    if (msg) *msg = st == noErr ? "Файлы .rtsproj открываются " + bundle : "бандл зарегистрирован, но LaunchServices не принял программу по умолчанию (" + std::to_string(int(st)) + ")";
    return st == noErr;
}
#pragma clang diagnostic pop
#endif

} // namespace

void setFileAssocLogo(const std::string& pngPath) { logoPath() = pngPath; }

std::string xdgDataHome() {
    if (const char* v = std::getenv("XDG_DATA_HOME"); v && *v == '/') return v;
    return (home() / ".local" / "share").string();
}

std::string xdgConfigHome() {
    if (const char* v = std::getenv("XDG_CONFIG_HOME"); v && *v == '/') return v;
    return (home() / ".config").string();
}

std::string desktopExec(const std::string& exe) {
    // Desktop Entry spec: quote arguments with reserved characters; inside quotes escape " ` $ \ with
    // a backslash; then the whole value is a string where a backslash itself is written as \\.
    // A literal % is %% (field codes).
    bool quote = false;
    for (char c : exe) if (std::string(" \t\n\"'\\><~|&;$*?#()`").find(c) != std::string::npos) quote = true;
    std::string arg;
    for (char c : exe) {
        if (c == '%') { arg += "%%"; continue; }
        if (quote && (c == '"' || c == '`' || c == '$' || c == '\\')) arg += '\\';
        arg += c;
    }
    if (quote) arg = "\"" + arg + "\"";
    std::string out;
    for (char c : arg) { if (c == '\\') out += "\\\\"; else out += c; }
    return out + " %f";
}

AssocStatus fileAssociationStatus(const std::string& editorExe) {
#if defined(_WIN32)
    (void)editorExe;
    return statusWindows();
#elif defined(__APPLE__)
    return statusMac(editorExe);
#else
    return statusLinux(editorExe);
#endif
}

bool registerFileAssociation(const std::string& editorExe, std::string* message) {
#if defined(_WIN32)
    (void)editorExe;
    return registerWindows(message);
#elif defined(__APPLE__)
    return registerMac(editorExe, message);
#else
    return registerLinux(editorExe, message);
#endif
}

std::string ensureFileAssociation(const std::string& editorExe) {
    if (automated()) return {};
    const AssocStatus s = fileAssociationStatus(editorExe);
    if (!s.supported || s.registered || s.otherDefault) return {};
    std::string msg;
    registerFileAssociation(editorExe, &msg);
    return msg;
}

} // namespace rts::editor
