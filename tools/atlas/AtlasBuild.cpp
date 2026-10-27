#include "AtlasBuild.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

#include "engine/pack/PackWriter.h"
#include "third_party/zstd/zstd.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace rts::atlas {

namespace {

std::string lower(std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
bool endsWith(const std::string& s, const std::string& e) { return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; }
std::string stem(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string n = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = n.find_last_of('.');
    return lower(dot == std::string::npos ? n : n.substr(0, dot));
}
std::string parentDir(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}
bool isPow2(int v) { return v > 0 && (v & (v - 1)) == 0; }
[[noreturn]] void bad(const std::string& m) { throw std::runtime_error("game.json atlas: " + m); }

// Usage folders -> default group. Everything else (3D model textures, terrain splat maps that
// tile, ...) stays a standalone texture unless the game says otherwise.
std::string usageGroup(const std::string& path) {
    std::string p = path.rfind("engine/", 0) == 0 ? path.substr(7) : path;
    size_t slash = p.find('/');
    if (slash == std::string::npos) return {};
    std::string top = lower(p.substr(0, slash));
    static const std::map<std::string, std::string> kMap = {
        {"ui", "ui"}, {"icons", "ui"}, {"portraits", "ui"}, {"cursors", "ui"}, {"hud", "ui"},
        {"sprites", "units"}, {"units", "units"}, {"buildings", "units"}, {"structures", "units"}, {"characters", "units"},
        {"tiles", "tiles"}, {"tilesets", "tiles"}, {"terrain", "tiles"},
        {"effects", "effects"}, {"fx", "effects"}, {"vfx", "effects"}, {"particles", "effects"}};
    auto it = kMap.find(top);
    return it == kMap.end() ? std::string() : it->second;
}

const char* kRuleKeys[] = {"group", "atlasGroup", "noAtlas", "wrap", "kind", "rotate", "trim", "mips", "splitAlpha", "padding", "extrude"};

void checkKeys(const Json& o, const std::string& where) {
    if (!o.isObject()) bad(where + " must be an object");
    for (auto& [k, v] : o.members()) {
        bool known = false;
        for (const char* r : kRuleKeys) known |= k == r;
        if (!known) bad(where + ": unknown key '" + k + "'");
    }
}

void applyProps(FileProps& p, const Json& o) {
    if (o.has("noAtlas")) p.noAtlas = o.getBool("noAtlas");
    if (o.has("wrap")) p.repeat = o.getStr("wrap") == "repeat";
    if (o.has("kind")) p.kind = o.getStr("kind");
    if (o.has("rotate")) p.rotate = o.getBool("rotate");
    if (o.has("trim")) p.trim = o.getBool("trim");
    if (o.has("mips")) p.mips = o.getBool("mips");
    if (o.has("splitAlpha")) p.splitAlpha = o.getBool("splitAlpha");
    if (o.has("padding")) p.padding = int(o.getInt("padding"));
    if (o.has("extrude")) p.extrude = int(o.getInt("extrude"));
}

struct Prepared {
    std::string path;
    FileProps props;
    Image trimmed;
    int offX = 0, offY = 0, srcW = 0, srcH = 0;
    uint64_t hash = 0, fileBytes = 0;
    bool opaque = true;
};

uint64_t imageHash(const Image& img) {
    uint64_t h = pack::hashBytes(img.rgba.data(), img.rgba.size());
    return h ^ (uint64_t(uint32_t(img.width)) << 32 | uint32_t(img.height)) * 0x9E3779B97F4A7C15ull;
}

Image trimImage(const Image& src, bool trim, int& ox, int& oy) {
    ox = oy = 0;
    if (!trim) return src;
    int x0 = src.width, y0 = src.height, x1 = -1, y1 = -1;
    for (int y = 0; y < src.height; ++y)
        for (int x = 0; x < src.width; ++x)
            if (src.px(x, y)[3] != 0) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
    if (x1 < 0) { x0 = y0 = x1 = y1 = 0; } // fully transparent: keep one texel
    Image out;
    out.width = x1 - x0 + 1;
    out.height = y1 - y0 + 1;
    out.rgba.resize(size_t(out.width) * size_t(out.height) * 4);
    for (int y = 0; y < out.height; ++y) std::memcpy(out.px(0, y), src.px(x0, y0 + y), size_t(out.width) * 4);
    ox = x0;
    oy = y0;
    return out;
}

// Content (rotated clockwise when asked) + `e` texels of edge extrusion around it.
void blit(Image& page, const Image& img, int x, int y, bool rotated, int e) {
    int pw = rotated ? img.height : img.width, ph = rotated ? img.width : img.height;
    for (int py = -e; py < ph + e; ++py)
        for (int px = -e; px < pw + e; ++px) {
            int dx = std::clamp(px, 0, pw - 1), dy = std::clamp(py, 0, ph - 1);
            // rotated: page (dx, dy) <- source (sx = dy, sy = h-1-dx)
            const uint8_t* s = rotated ? img.px(dy, img.height - 1 - dx) : img.px(dx, dy);
            std::memcpy(page.px(x + px, y + py), s, 4);
        }
}

size_t zstdSize(const std::string& s) {
    std::string z(ZSTD_compressBound(s.size()), '\0');
    size_t n = ZSTD_compress(z.data(), z.size(), s.data(), s.size(), 3);
    return ZSTD_isError(n) ? s.size() : n;
}

std::string pageName(const std::string& groupKey, int n, const std::string& ext) {
    std::string s;
    for (char c : groupKey) s += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : (c == '|' ? '-' : '_');
    return std::string(kAtlasDir) + s + "-" + std::to_string(n) + ext;
}

} // namespace

bool isImagePath(const std::string& path) {
    std::string l = lower(path);
    return endsWith(l, ".png") || endsWith(l, ".jpg") || endsWith(l, ".jpeg") || endsWith(l, ".tga");
}

std::string encodePng(const Image& img) {
    std::string out;
    stbi_write_png_to_func([](void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), size_t(size)); },
                           &out, img.width, img.height, 4, img.rgba.data(), img.width * 4);
    return out;
}

// ------------------------------------------------------------------ config
AtlasConfig AtlasConfig::fromGame(const Json& game, const std::string& profile) {
    AtlasConfig c;
    c.profile = profile;
    const Json& a = game["atlas"];
    bool lowEnd = profile == "web" || profile == "android" || profile.find("low") != std::string::npos;
    c.pack.maxPage = lowEnd ? 2048 : 4096;
    if (a.isNull()) return c;
    if (!a.isObject()) bad("must be an object");
    for (auto& [k, v] : a.members()) {
        static const std::set<std::string> kKeys = {"enable", "maxPage", "padding", "extrude", "trim", "pageSize", "pageFormat",
                                                    "maxTextureSize", "mipLevels", "groups", "files"};
        if (!kKeys.count(k)) bad("unknown key '" + k + "'");
    }
    c.enabled = a.getBool("enable", true);
    const Json& mp = a["maxPage"];
    if (mp.isNumber()) c.pack.maxPage = int(mp.integer());
    else if (mp.isObject()) {
        if (mp.has(profile)) c.pack.maxPage = int(mp.getInt(profile));
        else if (mp.has("default")) c.pack.maxPage = int(mp.getInt("default"));
    } else if (!mp.isNull()) bad("maxPage must be a number or {\"<profile>\": size, \"default\": size}");
    std::string ps = a.getStr("pageSize", "pow2");
    if (ps != "pow2" && ps != "mul4") bad("pageSize must be \"pow2\" or \"mul4\"");
    c.pack.pow2 = ps == "pow2";
    if (c.pack.maxPage < 64 || c.pack.maxPage > 16384 || (c.pack.pow2 ? !isPow2(c.pack.maxPage) : c.pack.maxPage % 4 != 0))
        bad("maxPage " + std::to_string(c.pack.maxPage) + " must be 64..16384 and a " + (c.pack.pow2 ? "power of two" : "multiple of 4"));
    c.pack.padding = int(a.getInt("padding", 2));
    c.pack.extrude = int(a.getInt("extrude", 1));
    if (c.pack.padding < 0 || c.pack.padding > 64 || c.pack.extrude < 0 || c.pack.extrude > 32) bad("padding must be 0..64, extrude 0..32");
    c.trim = a.getBool("trim", true);
    c.maxTextureSize = int(a.getInt("maxTextureSize", 0));
    c.mipLevels = int(a.getInt("mipLevels", 2));
    if (c.mipLevels < 0 || c.mipLevels > 5) bad("mipLevels must be 0..5");
    c.pageFormat = a.getStr("pageFormat", "rtex");
    if (c.pageFormat != "rtex" && c.pageFormat != "png") bad("pageFormat must be \"rtex\" or \"png\"");
    if (a.has("groups")) {
        c.groups = a["groups"];
        if (!c.groups.isObject()) bad("groups must be an object");
        for (auto& [g, o] : c.groups.members()) checkKeys(o, "groups['" + g + "']");
    }
    if (a.has("files")) {
        c.files = a["files"];
        if (!c.files.isObject()) bad("files must be an object of \"<glob>\": { ... }");
        for (auto& [g, o] : c.files.members()) checkKeys(o, "files['" + g + "']");
    }
    return c;
}

std::string AtlasConfig::key() const {
    if (!enabled) return "off";
    return "p" + std::to_string(pack.maxPage);
}

FileProps AtlasConfig::classify(const std::string& path) const {
    FileProps p;
    std::vector<const Json*> rules;
    std::string group = usageGroup(path);
    for (auto& [glob, o] : files.members())
        if (pack::globMatch(glob.c_str(), path.c_str())) {
            rules.push_back(&o);
            if (o.has("group")) group = o.getStr("group");
            if (o.has("atlasGroup")) group = o.getStr("atlasGroup");
        }
    p.group = group;
    if (group.empty()) return p;
    // built-in defaults per usage
    p.trim = trim;
    if (group == "tiles") { p.trim = false; p.splitAlpha = true; } // tilesets keep their grid; opaque ground draws without blending
    std::string s = stem(path);
    if (endsWith(s, "_n") || endsWith(s, "_nrm") || endsWith(s, "_normal") || endsWith(s, "_normals") || path.find("/normals/") != std::string::npos) p.kind = "normal";
    else if (endsWith(s, "_mask")) p.kind = "mask";
    if (endsWith(s, "_repeat") || endsWith(s, "_tiling") || endsWith(s, "_wrap")) p.repeat = true;
    if (groups.has(group)) applyProps(p, groups[group]);
    for (const Json* r : rules) applyProps(p, *r);
    if (p.kind != "color") p.trim = false; // alpha of normal maps / masks is data, not coverage
    return p;
}

// ------------------------------------------------------------------ build
bool buildAtlas(const std::vector<SourceImage>& input, const AtlasConfig& cfg, AtlasOutput& out, std::string* error) {
    out = AtlasOutput{};
    AtlasStats& st = out.stats;
    st.enabled = cfg.enabled;
    st.profile = cfg.profile;
    st.maxPage = cfg.pack.maxPage;
    if (!cfg.enabled) return true;
    std::vector<const SourceImage*> sorted;
    for (auto& s : input) sorted.push_back(&s);
    std::stable_sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->path < b->path; });

    int maxTex = cfg.maxTextureSize > 0 ? cfg.maxTextureSize : cfg.pack.maxPage / 4;
    std::map<std::string, std::vector<Prepared>> groups; // group key -> members, sorted by path
    for (const SourceImage* s : sorted) {
        FileProps p = cfg.classify(s->path);
        if (p.group.empty() || !isImagePath(s->path)) continue;
        ++st.candidates;
        if (p.noAtlas) { st.excluded.push_back({s->path, "noAtlas"}); continue; }
        if (p.repeat) { st.excluded.push_back({s->path, "wrap: repeat"}); continue; }
        Image img;
        if (!decodeImageFile(s->bytes, img)) { st.excluded.push_back({s->path, "cannot decode"}); st.warnings.push_back("atlas: cannot decode " + s->path); continue; }
        Prepared pr;
        pr.path = s->path;
        pr.props = p;
        pr.srcW = img.width;
        pr.srcH = img.height;
        pr.fileBytes = s->bytes.size();
        pr.trimmed = trimImage(img, p.trim, pr.offX, pr.offY);
        int pad = p.padding >= 0 ? p.padding : cfg.pack.padding, ext = p.extrude >= 0 ? p.extrude : cfg.pack.extrude;
        if (std::max(pr.trimmed.width, pr.trimmed.height) > std::min(maxTex, cfg.pack.maxPage - 2 * ext - (p.mips ? (1 << cfg.mipLevels) : 0)) || pad < 0) {
            st.excluded.push_back({s->path, "too large (" + std::to_string(img.width) + "x" + std::to_string(img.height) + " > " + std::to_string(maxTex) + ")"});
            continue;
        }
        for (size_t i = 3; i < pr.trimmed.rgba.size(); i += 4) if (pr.trimmed.rgba[i] != 255) { pr.opaque = false; break; }
        pr.hash = imageHash(pr.trimmed);
        std::string key = p.group;
        if (p.kind != "color") key += "|" + p.kind;
        if (p.splitAlpha) key += pr.opaque ? "|opaque" : "|alpha";
        if (p.mips) key += "|mips";
        groups[key].push_back(std::move(pr));
    }

    for (auto& [key, members] : groups) {
        // dedupe identical trimmed pixels
        std::vector<int> uniqueOf(members.size(), -1);
        std::vector<size_t> uniques;
        std::multimap<uint64_t, size_t> byHash;
        for (size_t i = 0; i < members.size(); ++i) {
            auto range = byHash.equal_range(members[i].hash);
            for (auto it = range.first; it != range.second; ++it) {
                const Image &a = members[it->second].trimmed, &b = members[i].trimmed;
                if (a.width == b.width && a.height == b.height && a.rgba == b.rgba) { uniqueOf[i] = int(it->second); break; }
            }
            if (uniqueOf[i] < 0) { uniqueOf[i] = int(i); byHash.emplace(members[i].hash, i); uniques.push_back(i); }
            else ++st.duplicates;
        }
        PackSettings ps = cfg.pack;
        bool mips = members[0].props.mips;
        ps.padding = 0;
        ps.extrude = 0;
        for (auto& m : members) {
            ps.padding = std::max(ps.padding, m.props.padding >= 0 ? m.props.padding : cfg.pack.padding);
            ps.extrude = std::max(ps.extrude, m.props.extrude >= 0 ? m.props.extrude : cfg.pack.extrude);
        }
        if (mips) { // mip-safe: neighbours stay apart and aligned down to the smallest used level
            int a = 1 << cfg.mipLevels;
            ps.align = a;
            ps.padding = std::max(ps.padding, a);
        }
        std::vector<size_t> itemOf(members.size(), 0);
        for (size_t k = 0; k < uniques.size(); ++k) itemOf[uniques[k]] = k;
        std::vector<PackItem> items;
        for (size_t u : uniques) {
            PackItem it;
            it.w = members[u].trimmed.width;
            it.h = members[u].trimmed.height;
            it.allowRotate = members[u].props.rotate;
            it.cluster = parentDir(members[u].path);
            it.key = members[u].path;
            items.push_back(std::move(it));
        }
        PackedPages pp = packRects(items, ps);
        if (!pp.ok) { if (error) *error = "atlas group '" + key + "': " + pp.error; return false; }

        GroupStats gs;
        gs.name = key;
        gs.textures = int(members.size());
        gs.unique = int(uniques.size());
        gs.pages = int(pp.pages.size());
        int firstPage = int(out.manifest.pages().size());
        std::vector<Image> imgs(pp.pages.size());
        std::vector<bool> alpha(pp.pages.size(), false);
        for (size_t p = 0; p < pp.pages.size(); ++p) {
            imgs[p].width = pp.pages[p].w;
            imgs[p].height = pp.pages[p].h;
            imgs[p].rgba.assign(size_t(imgs[p].width) * size_t(imgs[p].height) * 4, 0);
            gs.pageArea += uint64_t(imgs[p].width) * uint64_t(imgs[p].height);
        }
        for (size_t k = 0; k < uniques.size(); ++k) {
            const Prepared& m = members[uniques[k]];
            const Placement& pl = pp.placements[k];
            blit(imgs[size_t(pl.page)], m.trimmed, pl.x, pl.y, pl.rotated, ps.extrude);
            if (!m.opaque) alpha[size_t(pl.page)] = true;
            gs.usedArea += uint64_t(m.trimmed.width) * uint64_t(m.trimmed.height);
        }
        for (size_t p = 0; p < pp.pages.size(); ++p) {
            Page pg;
            pg.group = key;
            pg.width = imgs[p].width;
            pg.height = imgs[p].height;
            pg.alpha = alpha[p];
            pg.mips = mips;
            std::string bytes;
            if (cfg.pageFormat == "png") {
                pg.file = pageName(key, int(p), ".png");
                bytes = encodePng(imgs[p]);
            } else {
                pg.file = pageName(key, int(p), ".rtex");
                std::string plain = encodeRtex(imgs[p], pg.alpha, mips, false), delta = encodeRtex(imgs[p], pg.alpha, mips, true);
                bytes = zstdSize(delta) < zstdSize(plain) ? std::move(delta) : std::move(plain);
            }
            st.pageBytes += bytes.size();
            st.gpuBytesAfter += uint64_t(pg.width) * uint64_t(pg.height) * 4;
            out.files.push_back({pg.file, std::move(bytes)});
            out.manifest.pagesMut().push_back(pg);
            out.pageImages.push_back(std::move(imgs[p]));
        }
        for (size_t i = 0; i < members.size(); ++i) {
            const Prepared& m = members[i];
            const Placement& pl = pp.placements[itemOf[size_t(uniqueOf[i])]];
            Region r;
            r.page = firstPage + pl.page;
            r.x = pl.x; r.y = pl.y;
            r.w = m.trimmed.width; r.h = m.trimmed.height;
            r.rotated = pl.rotated;
            r.offsetX = m.offX; r.offsetY = m.offY;
            r.sourceW = m.srcW; r.sourceH = m.srcH;
            out.manifest.add(m.path, r);
            out.consumed.push_back(m.path);
            st.sourceBytes += m.fileBytes;
            st.gpuBytesBefore += uint64_t(m.srcW) * uint64_t(m.srcH) * 4;
        }
        st.atlased += gs.textures;
        st.pages += gs.pages;
        st.usedArea += gs.usedArea;
        st.pageArea += gs.pageArea;
        st.groups.push_back(gs);
    }
    if (!out.manifest.empty()) {
        std::string m = out.manifest.serialize();
        st.pageBytes += m.size();
        out.files.push_back({kManifestPath, std::move(m)});
    }
    std::sort(out.consumed.begin(), out.consumed.end());
    return true;
}

bool applyAtlas(pack::PackWriter& w, const AtlasConfig& cfg, AtlasStats& stats, std::string* error) {
    stats = AtlasStats{};
    stats.enabled = cfg.enabled;
    stats.profile = cfg.profile;
    stats.maxPage = cfg.pack.maxPage;
    if (!cfg.enabled) return true;
    std::vector<SourceImage> imgs;
    for (const std::string& p : w.paths()) {
        if (p.rfind(kAtlasDir, 0) == 0) { if (error) *error = "atlas: content already contains " + p + " (reserved folder _atlas/)"; return false; }
        if (!isImagePath(p) || cfg.classify(p).group.empty()) continue;
        SourceImage s;
        s.path = p;
        if (!w.readInput(p, s.bytes)) { if (error) *error = "atlas: cannot read " + p; return false; }
        imgs.push_back(std::move(s));
    }
    AtlasOutput out;
    if (!buildAtlas(imgs, cfg, out, error)) return false;
    stats = out.stats;
    if (out.consumed.empty()) return true;
    w.remove(out.consumed);
    for (auto& [path, bytes] : out.files)
        if (!w.addData(path, std::move(bytes), error)) return false;
    return true;
}

void finishStats(AtlasStats& st, const pack::WriteStats& ws) {
    st.storedBytes = 0;
    for (auto& it : ws.items)
        if (it.path.rfind(kAtlasDir, 0) == 0) st.storedBytes += it.storedSize;
}

// ------------------------------------------------------------------ reporting
Json AtlasStats::toJson() const {
    Json j = Json::object();
    j.set("enabled", Json::fromBool(enabled));
    j.set("profile", Json::string(profile));
    j.set("maxPage", Json::number(int64_t(maxPage)));
    j.set("candidates", Json::number(int64_t(candidates)));
    j.set("atlased", Json::number(int64_t(atlased)));
    j.set("duplicates", Json::number(int64_t(duplicates)));
    j.set("pages", Json::number(int64_t(pages)));
    j.set("occupancyPercent", Json::number(occupancy() * 100.0, 1));
    j.set("sourceBytes", Json::number(int64_t(sourceBytes)));
    j.set("atlasBytes", Json::number(int64_t(pageBytes)));
    j.set("storedBytes", Json::number(int64_t(storedBytes)));
    j.set("bytesSaved", Json::number(int64_t(sourceBytes) - int64_t(storedBytes ? storedBytes : pageBytes)));
    j.set("gpuBytesBefore", Json::number(int64_t(gpuBytesBefore)));
    j.set("gpuBytesAfter", Json::number(int64_t(gpuBytesAfter)));
    // texture switches when everything of a group is drawn: one per texture before, one per page after
    j.set("textureBindsBefore", Json::number(int64_t(atlased)));
    j.set("textureBindsAfter", Json::number(int64_t(pages)));
    Json g = Json::array();
    for (auto& s : groups) {
        Json o = Json::object();
        o.set("group", Json::string(s.name));
        o.set("textures", Json::number(int64_t(s.textures)));
        o.set("unique", Json::number(int64_t(s.unique)));
        o.set("pages", Json::number(int64_t(s.pages)));
        o.set("occupancyPercent", Json::number(s.pageArea ? 100.0 * double(s.usedArea) / double(s.pageArea) : 0.0, 1));
        g.push(std::move(o));
    }
    j.set("groups", std::move(g));
    Json ex = Json::array();
    for (auto& [p, r] : excluded) {
        Json o = Json::object();
        o.set("path", Json::string(p));
        o.set("reason", Json::string(r));
        ex.push(std::move(o));
    }
    j.set("excluded", std::move(ex));
    return j;
}

std::string AtlasStats::report() const {
    if (!enabled) return "атласы текстур: выключены (game.json atlas.enable = false)\n";
    if (atlased == 0 && excluded.empty()) return "атласы текстур: нет подходящих 2D-изображений\n";
    char buf[256];
    std::string s;
    std::snprintf(buf, sizeof buf, "атласы текстур (профиль %s, страница до %d):\n", profile.c_str(), maxPage); s += buf;
    std::snprintf(buf, sizeof buf, "  %d текстур -> %d стр., заполнение %.1f%%, дубликатов %d\n", atlased, pages, occupancy() * 100.0, duplicates); s += buf;
    uint64_t after = storedBytes ? storedBytes : pageBytes;
    s += "  в паке: было " + pack::formatBytes(sourceBytes) + " -> стало " + pack::formatBytes(after);
    s += sourceBytes >= after ? " (экономия " + pack::formatBytes(sourceBytes - after) + ")\n" : " (больше на " + pack::formatBytes(after - sourceBytes) + ")\n";
    s += "  видеопамять: было " + pack::formatBytes(gpuBytesBefore) + " -> стало " + pack::formatBytes(gpuBytesAfter) + "\n";
    for (auto& g : groups) {
        std::snprintf(buf, sizeof buf, "  группа %-16s %4d текстур (%d уник.) -> %d стр., %.1f%%\n", g.name.c_str(), g.textures, g.unique, g.pages,
                      g.pageArea ? 100.0 * double(g.usedArea) / double(g.pageArea) : 0.0);
        s += buf;
    }
    size_t shown = 0;
    for (auto& [p, r] : excluded) {
        if (shown++ == 10) { s += "  ... ещё " + std::to_string(excluded.size() - 10) + " не в атласе\n"; break; }
        s += "  не в атласе: " + p + " — " + r + "\n";
    }
    return s;
}

} // namespace rts::atlas
