#include "Unpacker.h"

#include "Chunks.h"
#include "FileCopy.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "engine/data/Json.h"

#if defined(_WIN32)
#define RTS_POPEN _popen
#define RTS_PCLOSE _pclose
#else
#include <sys/wait.h>
#define RTS_POPEN popen
#define RTS_PCLOSE pclose
#endif

namespace fs = std::filesystem;

namespace rts::pack {

namespace {

bool fail(std::string* err, const std::string& msg) { if (err) *err = msg; return false; }
std::string lower(std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }

std::string quoteArg(const std::string& a) {
#if defined(_WIN32)
    std::string o = "\"";
    for (char c : a) { if (c == '"') o += '\\'; o += c; }
    return o + "\"";
#else
    std::string o = "'";
    for (char c : a) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
#endif
}

bool hexDecode(const std::string& hex, std::string& out) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = char(std::tolower(static_cast<unsigned char>(c)));
        return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    std::string h;
    for (char c : hex) if (!std::isspace(static_cast<unsigned char>(c))) h += c;
    if (h.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < h.size(); i += 2) {
        int a = nib(h[i]), b = nib(h[i + 1]);
        if (a < 0 || b < 0) return false;
        out += char(a * 16 + b);
    }
    return true;
}

std::string readHead(const std::string& path, int64_t offset, size_t n) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    if (offset < 0) f.seekg(std::streamoff(offset), std::ios::end);
    else f.seekg(std::streamoff(offset));
    if (!f) return {};
    std::string s(n, '\0');
    f.read(s.data(), std::streamsize(n));
    s.resize(size_t(f.gcount()));
    return s;
}

bool magicMatches(const UnpackerInfo& u, const std::string& path) {
    for (auto& m : u.magic)
        if (!m.bytes.empty() && readHead(path, m.offset, m.bytes.size()) == m.bytes) return true;
    return false;
}

bool extMatches(const UnpackerInfo& u, const std::string& path) {
    std::string l = lower(path);
    for (auto& e : u.extensions)
        if (l.size() >= e.size() && l.compare(l.size() - e.size(), e.size(), e) == 0) return true;
    return false;
}

fs::path makeScratch(const std::string& base) {
    fs::path root = base.empty() ? fs::temp_directory_path() : fs::path(base);
    auto tick = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    for (int i = 0; i < 100; ++i) {
        fs::path p = root / ("rts-unpack-" + std::to_string(tick) + "-" + std::to_string(i));
        std::error_code ec;
        if (fs::create_directories(p, ec)) return p;
    }
    return {};
}

struct ScratchGuard {
    fs::path p;
    ~ScratchGuard() { std::error_code ec; if (!p.empty()) fs::remove_all(p, ec); }
};

// One protocol call: `<command...> <method> <request.json>`, response = JSON on stdout.
bool call(const UnpackerInfo& u, const std::string& method, const std::string& requestJson, Json& response, std::string* err) {
    if (u.command.empty()) return fail(err, u.name + ": no command in manifest");
    fs::path scratch = makeScratch("");
    ScratchGuard guard{scratch};
    if (scratch.empty()) return fail(err, "cannot create a temp folder");
    fs::path req = scratch / "request.json";
    {
        std::ofstream f(req, std::ios::binary);
        f << requestJson;
        if (!f) return fail(err, "cannot write " + req.string());
    }
    std::string cmd;
    for (auto& a : u.command) cmd += (cmd.empty() ? "" : " ") + quoteArg(a);
    cmd += " " + method + " " + quoteArg(req.string());
#if defined(_WIN32)
    cmd = "\"" + cmd + "\""; // cmd.exe strips one pair of outer quotes
#endif
    std::fflush(nullptr);
    std::FILE* p = RTS_POPEN(cmd.c_str(), "r");
    if (!p) return fail(err, u.name + ": cannot start " + u.command[0]);
    std::string out;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, p)) > 0;) out.append(buf, n);
    int rc = RTS_PCLOSE(p);
#if !defined(_WIN32)
    if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
    // The response is the last non-empty line, so stray prints from libraries don't break us.
    std::string line;
    {
        std::istringstream ss(out);
        for (std::string l; std::getline(ss, l);) {
            size_t b = l.find_first_not_of(" \t\r");
            if (b != std::string::npos) line = l.substr(b);
        }
    }
    if (line.empty()) return fail(err, u.name + " " + method + ": no response (exit code " + std::to_string(rc) + ")");
    try {
        response = Json::parse(line);
    } catch (const std::exception& e) {
        return fail(err, u.name + " " + method + ": response is not JSON (" + e.what() + ")");
    }
    if (response.has("error")) {
        const Json& e = response["error"];
        return fail(err, u.name + " " + method + ": " + (e.isObject() ? e.getStr("message", "error") : e.str("error")));
    }
    if (rc != 0) return fail(err, u.name + " " + method + ": exit code " + std::to_string(rc));
    return true;
}

std::string request(std::initializer_list<std::pair<const char*, std::string>> fields) {
    std::string s = "{\"protocol\":" + std::to_string(kUnpackerProtocol);
    for (auto& [k, v] : fields) s += ",\"" + std::string(k) + "\":" + v;
    return s + "}";
}
std::string jstr(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

} // namespace

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += char(c);
        }
    }
    return o;
}

bool UnpackerInfo::can(const std::string& cap) const { return std::find(capabilities.begin(), capabilities.end(), cap) != capabilities.end(); }

UnpackerRegistry::UnpackerRegistry() {
    UnpackerInfo r;
    r.name = "rpak"; r.title = "RTSEngine resource pack"; r.version = "1"; r.builtin = true;
    r.extensions = {".rpak"};
    r.magic = {{0, "RPAK"}};
    r.capabilities = {"probe", "list", "extract"};
    list_.push_back(r);
}

bool UnpackerRegistry::loadManifest(const std::string& manifestPath, std::string* error) {
    Json m;
    try {
        m = Json::parseFile(manifestPath);
    } catch (const std::exception& e) {
        return fail(error, manifestPath + ": " + e.what());
    }
    auto bad = [&](const std::string& why) { return fail(error, manifestPath + ": " + why); };
    UnpackerInfo u;
    u.manifest = manifestPath;
    u.dir = fs::absolute(fs::path(manifestPath)).parent_path().string();
    int proto = int(m.getInt("protocol", 0));
    if (proto != kUnpackerProtocol) return bad("unsupported protocol " + std::to_string(proto) + " (expected " + std::to_string(kUnpackerProtocol) + ")");
    u.name = m.getStr("name");
    if (u.name.empty()) return bad("missing \"name\"");
    for (char c : u.name) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return bad("name may contain only a-z, 0-9, - and _");
    if (u.name == "rpak") return bad("name 'rpak' is reserved");
    u.title = m.getStr("title", u.name);
    u.version = m.getStr("version", "0");
    u.license = m.getStr("license");
    u.external = m.getBool("external", true); // third-party data unless the plugin says otherwise
    for (auto& e : m["extensions"].items()) {
        std::string x = lower(e.str());
        if (x.empty()) continue;
        if (x[0] != '.') x = "." + x;
        u.extensions.push_back(x);
    }
    for (auto& mm : m["magic"].items()) {
        UnpackerInfo::Magic g;
        g.offset = mm.getInt("offset", 0);
        if (mm.has("hex")) { if (!hexDecode(mm.getStr("hex"), g.bytes)) return bad("bad hex in \"magic\""); }
        else g.bytes = mm.getStr("ascii");
        if (g.bytes.empty()) return bad("empty \"magic\" entry");
        u.magic.push_back(g);
    }
    for (auto& c : m["capabilities"].items()) u.capabilities.push_back(c.str());
    if (u.capabilities.empty()) u.capabilities = {"list", "extract"};
#if defined(_WIN32)
    const Json& cmd = m.has("command_windows") ? m["command_windows"] : m["command"];
#else
    const Json& cmd = m["command"];
#endif
    for (auto& c : cmd.items()) {
        std::string a = c.str();
        size_t at;
        while ((at = a.find("${dir}")) != std::string::npos) a.replace(at, 6, u.dir);
        std::error_code ec;
        fs::path local = fs::path(u.dir) / a;
        if (!a.empty() && fs::path(a).is_relative() && fs::exists(local, ec)) a = local.string();
        u.command.push_back(a);
    }
    if (u.command.empty()) return bad("missing \"command\"");
    if (u.extensions.empty() && u.magic.empty()) return bad("needs \"extensions\" or \"magic\" to be detectable");
    auto it = std::find_if(list_.begin(), list_.end(), [&](const UnpackerInfo& x) { return x.name == u.name; });
    if (it != list_.end()) *it = u; // later search dirs override
    else list_.push_back(u);
    return true;
}

void UnpackerRegistry::addSearchDir(const std::string& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<fs::path> manifests;
    if (fs::exists(fs::path(dir) / "unpacker.json", ec)) manifests.push_back(fs::path(dir) / "unpacker.json");
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_directory(ec) && fs::exists(e.path() / "unpacker.json", ec)) manifests.push_back(e.path() / "unpacker.json");
    std::sort(manifests.begin(), manifests.end());
    for (auto& m : manifests) {
        std::string err;
        if (!loadManifest(m.string(), &err)) errors_.push_back(err);
    }
}

void UnpackerRegistry::addDefaultDirs(const std::string& engineDir, const std::string& projectDir) {
    if (!engineDir.empty()) addSearchDir((fs::path(engineDir) / "tools" / "unpackers").string());
    if (const char* env = std::getenv("RTS_UNPACKERS")) {
#if defined(_WIN32)
        const char sep = ';';
#else
        const char sep = ':';
#endif
        std::stringstream ss(env);
        for (std::string d; std::getline(ss, d, sep);) if (!d.empty()) addSearchDir(d);
    }
    if (!projectDir.empty()) addSearchDir((fs::path(projectDir) / "unpackers").string());
}

const UnpackerInfo* UnpackerRegistry::byName(const std::string& name) const {
    for (auto& u : list_) if (u.name == name) return &u;
    return nullptr;
}

const UnpackerInfo* UnpackerRegistry::detect(const std::string& archive, std::string* why) const {
    std::error_code ec;
    if (!fs::is_regular_file(archive, ec)) { fail(why, "file not found: " + archive); return nullptr; }
    // Candidates: magic + extension, magic only, extension only. Each is confirmed by its probe,
    // so a weak signature (or an extension shared by unrelated formats, like .pac) can't misfire.
    std::vector<std::pair<int, const UnpackerInfo*>> cands;
    for (auto& u : list_) {
        int score = (magicMatches(u, archive) ? 2 : 0) + (extMatches(u, archive) ? 1 : 0);
        if (score) cands.emplace_back(-score, &u);
    }
    std::stable_sort(cands.begin(), cands.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::string lastErr;
    for (auto& [score, u] : cands) {
        bool match = false;
        std::string err;
        if (!probe(*u, archive, match, &err)) { lastErr = err; continue; }
        if (match) return u;
    }
    if (!lastErr.empty()) { fail(why, lastErr); return nullptr; }
    fail(why, "no unpacker knows this format: " + archive + " (see `assetpipe formats`; new plugin: `assetpipe unpackers new <dir> <name>`)");
    return nullptr;
}

bool UnpackerRegistry::handshake(const UnpackerInfo& u, std::string* error) const {
    if (u.builtin) return true;
    Json r;
    if (!call(u, "info", request({}), r, error)) return false;
    int proto = int(r.getInt("protocol", 0));
    if (proto != kUnpackerProtocol) return fail(error, u.name + ": program speaks protocol " + std::to_string(proto));
    if (r.getStr("name") != u.name) return fail(error, u.name + ": program reports name '" + r.getStr("name") + "'");
    return true;
}

bool UnpackerRegistry::probe(const UnpackerInfo& u, const std::string& archive, bool& match, std::string* error) const {
    match = false;
    if (u.builtin) { match = magicMatches(u, archive); return true; }
    if (!u.can("probe")) { match = magicMatches(u, archive) || extMatches(u, archive); return true; }
    Json r;
    if (!call(u, "probe", request({{"archive", jstr(fs::absolute(archive).string())}}), r, error)) return false;
    match = r.getBool("match");
    return true;
}

bool UnpackerRegistry::list(const UnpackerInfo& u, const std::string& archive, std::vector<ArchiveEntryInfo>& out, std::string* error) const {
    out.clear();
    if (u.builtin) {
        std::string err;
        auto p = Pack::openFile(archive, &err);
        if (!p) return fail(error, err);
        attachChunksNextTo(*p, archive);   // split pack: its chunks, if they are at hand
        for (size_t i = 0; i < p->size(); ++i) out.push_back({std::string(p->entry(i).path), p->entry(i).rawSize});
        return true;
    }
    if (!u.can("list")) return fail(error, u.name + ": plugin can't list");
    Json r;
    if (!call(u, "list", request({{"archive", jstr(fs::absolute(archive).string())}}), r, error)) return false;
    for (auto& e : r["entries"].items()) out.push_back({e.getStr("path"), uint64_t(e.getInt("size", 0))});
    return true;
}

bool UnpackerRegistry::extract(const UnpackerInfo& u, const std::string& archive, const std::string& outDir, std::string* error) const {
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (u.builtin) {
        std::string err, data;
        auto p = Pack::openFile(archive, &err);
        if (!p) return fail(error, err);
        attachChunksNextTo(*p, archive);   // split pack: its chunks, if they are at hand
        for (size_t i = 0; i < p->size(); ++i) {
            const Entry& e = p->entry(i);
            if (!p->read(e, data, true)) return fail(error, p->lastError());
            fs::path f = fs::path(outDir) / std::string(e.path);
            fs::create_directories(f.parent_path(), ec);
            std::ofstream o(f, std::ios::binary);
            o.write(data.data(), std::streamsize(data.size()));
            if (!o) return fail(error, "cannot write " + f.string());
        }
        return true;
    }
    if (!u.can("extract")) return fail(error, u.name + ": plugin can't extract");
    Json r;
    return call(u, "extract", request({{"archive", jstr(fs::absolute(archive).string())}, {"out", jstr(fs::absolute(outDir).string())}}), r, error);
}

namespace {
// Adds the files under `root` (never following symlinks) with opt.prefix / opt.include applied.
// Data is read immediately, so `root` may be a scratch folder removed right after.
int addTree(const fs::path& root, const ImportOptions& opt, PackWriter& w, bool applyIgnore, std::string* error) {
    IgnoreRules rules;
    std::error_code ec;
    std::vector<std::pair<std::string, std::string>> files;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_symlink(ec)) { it.disable_recursion_pending(); continue; }
        std::string rel = fs::relative(it->path(), root, ec).generic_string();
        bool isDir = it->is_directory(ec);
        if (applyIgnore && rules.ignored(rel, isDir)) { if (isDir) it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(ec)) continue;
        bool keep = opt.include.empty();
        for (auto& g : opt.include) keep = keep || globMatch(g.c_str(), rel.c_str());
        if (keep) files.emplace_back(rel, it->path().string());
    }
    if (ec) { fail(error, "cannot scan " + root.string() + ": " + ec.message()); return -1; }
    std::sort(files.begin(), files.end());
    for (auto& [rel, disk] : files) {
        std::ifstream f(disk, std::ios::binary);
        std::ostringstream ss; ss << f.rdbuf();
        if (!f && !f.eof()) { fail(error, "cannot read " + disk); return -1; }
        if (!w.addData(opt.prefix.empty() ? rel : opt.prefix + "/" + rel, ss.str(), error)) return -1;
    }
    return int(files.size());
}
} // namespace

bool importArchive(const UnpackerRegistry& reg, const std::string& archive, const ImportOptions& opt, PackWriter& w,
                   ImportResult* result, std::string* error) {
    std::error_code dec;
    if (fs::is_directory(archive, dec)) { // a plain folder (unpacked game, loose files)
        int n = addTree(archive, opt, w, true, error);
        if (n < 0) return false;
        if (opt.external) w.markExternal();
        if (result) { result->format = "dir"; result->files += n; result->external = opt.external; }
        return true;
    }
    std::string why;
    const UnpackerInfo* u = opt.format.empty() ? reg.detect(archive, &why) : reg.byName(opt.format);
    if (!u) return fail(error, opt.format.empty() ? why : "unknown format '" + opt.format + "' (assetpipe formats)");
    if (u->builtin) {
        std::string err;
        auto p = Pack::openFile(archive, &err);
        if (!p) return fail(error, err);
        attachChunksNextTo(*p, archive);   // split pack: its chunks, if they are at hand
        for (size_t i = 0; i < p->size(); ++i) {
            const Entry& e = p->entry(i);
            std::string path(e.path), data;
            bool keep = opt.include.empty();
            for (auto& g : opt.include) keep = keep || globMatch(g.c_str(), path.c_str());
            if (!keep) continue;
            if (!p->read(e, data, true)) return fail(error, p->lastError());
            if (!w.addData(opt.prefix.empty() ? path : opt.prefix + "/" + path, std::move(data), error)) return false;
            if (result) ++result->files;
        }
        bool external = p->external() || opt.external;
        if (external) w.markExternal();
        if (result) { result->format = u->name; result->external = external; }
        return true;
    }
    fs::path scratch = makeScratch(opt.tempDir);
    ScratchGuard guard{scratch};
    if (scratch.empty()) return fail(error, "cannot create a temp folder");
    fs::path out = scratch / "out";
    if (!reg.extract(*u, archive, out.string(), error)) return false;
    // The plugin's output is untrusted: scan it ourselves, skip symlinks, never trust reported paths.
    int n = addTree(out, opt, w, false, error);
    if (n < 0) return false;
    bool external = u->external || opt.external;
    if (external) w.markExternal();
    if (result) { result->format = u->name; result->files += n; result->external = external; }
    return true;
}

bool scaffoldUnpacker(const std::string& dir, const std::string& name, const std::string& engineDir, std::string* error) {
    for (char c : name) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return fail(error, "name may contain only a-z, 0-9, - and _");
    fs::path d = fs::path(dir) / name;
    std::error_code ec;
    if (fs::exists(d / "unpacker.json", ec)) return fail(error, (d / "unpacker.json").string() + " already exists");
    fs::create_directories(d, ec);
    if (ec) return fail(error, "cannot create " + d.string());
    std::ofstream m(d / "unpacker.json");
    m << "{\n"
         "  \"protocol\": 1,\n"
         "  \"name\": \"" << name << "\",\n"
         "  \"title\": \"TODO: format description\",\n"
         "  \"version\": \"0.1.0\",\n"
         "  \"extensions\": [\".todo\"],\n"
         "  \"magic\": [{\"offset\": 0, \"hex\": \"00000000\"}],\n"
         "  \"capabilities\": [\"probe\", \"list\", \"extract\"],\n"
         "  \"external\": true,\n"
         "  \"command\": [\"python3\", \"unpack.py\"],\n"
         "  \"command_windows\": [\"py\", \"-3\", \"unpack.py\"]\n"
         "}\n";
    std::ofstream s(d / "unpack.py");
    s << "#!/usr/bin/env python3\n"
         "\"\"\"Unpacker for " << name << " (RTSEngine unpacker protocol 1, see tools/unpackers/PROTOCOL.md).\"\"\"\n"
         "import os, sys\n"
         "sys.dont_write_bytecode = True\n"
         "HERE = os.path.dirname(os.path.abspath(__file__))\n"
         "sys.path[:0] = [HERE, os.path.join(HERE, '..', 'lib')]  # bundled copy first, then tools/unpackers/lib\n"
         "from rtsunpack import run, Entry\n"
         "\n"
         "NAME = '" << name << "'\n"
         "\n"
         "def probe(path):\n"
         "    with open(path, 'rb') as f:\n"
         "        return f.read(4) == b'\\x00\\x00\\x00\\x00'  # TODO: real signature check\n"
         "\n"
         "def entries(path):\n"
         "    \"\"\"Yield Entry(path, size, read) for every file in the archive. `read()` returns bytes.\"\"\"\n"
         "    raise NotImplementedError('TODO: parse the archive')\n"
         "\n"
         "if __name__ == '__main__':\n"
         "    run(NAME, '0.1.0', probe, entries)\n";
    if (!m || !s) return fail(error, "cannot write plugin files in " + d.string());
    // Bundle the helper so the plugin works from any folder (game repo, $RTS_UNPACKERS, shared).
    fs::path lib = fs::path(engineDir) / "tools" / "unpackers" / "lib" / "rtsunpack.py";
    if (!engineDir.empty() && fs::exists(lib, ec)) copyFileReplace(lib, d / "rtsunpack.py", ec);
    return true;
}

} // namespace rts::pack
