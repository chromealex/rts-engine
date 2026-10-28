// assetpipe — builds and inspects resource packs (.rpak) and imports foreign archives.
//
//   assetpipe pack -o <out.rpak> [pack options] <dir>[=<prefix>]...
//   assetpipe import -o <out.rpak> [pack options] [--format F] [--include <glob>]... [--external] <archive|dir>[=<prefix>]...
//   assetpipe list <archive>               entries (any known format; .rpak also shows codec)
//   assetpipe verify <pack>...             decompress everything and check hashes
//   assetpipe extract <archive> <dir>      unpack any known format to a folder
//   assetpipe diff <old.rpak> <new.rpak>   added / removed / changed files (patch planning)
//   assetpipe formats                      built-in formats and unpacker plugins found
//   assetpipe probe <file>...              which format a file is
//   assetpipe unpackers new <dir> <name>   skeleton of a new unpacker plugin
//   assetpipe unpackers check [<name>...]  validate manifests and run the protocol handshake
//   assetpipe atlas [--profile P] -o <dir> <dir>[=<prefix>]...
//                                          texture atlas preview: pages as PNG + atlas.json + report
//
// pack options: --level N, --store, --threads N, --ignore <glob>, --atlas <profile> (pack 2D images
//               into texture atlases like rtsbuild; settings from game.json "atlas" of the root input)
// global: --json (one JSON object on stdout, for the MCP server / editor), --unpackers <dir>
//         (extra plugin folder, repeatable), --project <dir> (also loads <dir>/unpackers),
//         --engine <dir> (default: source tree; plugins in <engine>/tools/unpackers)
//
// Example (what rtsbuild does):
//   assetpipe pack -o dist/content.rpak games/skirmish engine/assets=engine
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "engine/pack/PackWriter.h"
#include "engine/pack/Chunks.h"
#include "engine/pack/Sha256.h"
#include "engine/data/Json.h"
#include "engine/pack/Unpacker.h"
#include "tools/atlas/AtlasBuild.h"

#ifndef RTS_SOURCE_DIR
#define RTS_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;
using namespace rts::pack;
using rts::Json;

namespace {

struct Global {
    bool json = false;
    std::string engine = RTS_SOURCE_DIR, project;
    std::vector<std::string> unpackerDirs;
};
Global G;

void usage() {
    std::cout << "usage:\n"
                 "  assetpipe pack -o <out.rpak> [--level N] [--store] [--threads N] [--ignore <glob>]... <dir>[=<prefix>]...\n"
                 "       split: --chunk-size 8MB [--base-size 40MB] [--total-size 1GB] [--fill-base] [--base <glob>]... [--order <glob>]... [--chunks-dir D]\n"
                 "  assetpipe import -o <out.rpak> [--format F] [--include <glob>]... [--external] <archive|dir>[=<prefix>]...\n"
                 "  assetpipe list <archive>\n"
                 "  assetpipe verify <pack>...\n"
                 "  assetpipe extract <archive> <dir>\n"
                 "  assetpipe diff <old.rpak> <new.rpak>\n"
                 "  assetpipe formats\n"
                 "  assetpipe probe <file>...\n"
                 "  assetpipe unpackers new <dir> <name>\n"
                 "  assetpipe unpackers check [<name>...]\n"
                 "  assetpipe atlas [--profile P] -o <dir> <dir>[=<prefix>]...\n"
                 "pack/import also take --atlas <profile>\n"
                 "global: --json  --unpackers <dir>  --project <dir>  --engine <dir>\n";
}

std::string q(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

int die(const std::string& s) {
    if (G.json) std::cout << "{\"ok\":false,\"error\":" << q(s) << "}" << std::endl;
    else std::cout << "ОШИБКА: " << s << std::endl;
    return 1;
}

std::unique_ptr<Pack> openPack(const std::string& path, std::string& err) { return Pack::openFile(path, &err); }

char hex(unsigned v) { return "0123456789abcdef"[v & 15]; }
std::string hex64(uint64_t v) { std::string s(16, '0'); for (int i = 15; i >= 0; --i, v >>= 4) s[size_t(i)] = hex(unsigned(v)); return s; }

UnpackerRegistry registry() {
    UnpackerRegistry r;
    r.addDefaultDirs(G.engine, G.project);
    for (auto& d : G.unpackerDirs) r.addSearchDir(d);
    if (!G.json) for (auto& e : r.errors()) std::cerr << "предупреждение: плагин пропущен: " << e << "\n";
    return r;
}

std::pair<std::string, std::string> splitPrefix(const std::string& a) {
    // "path=prefix"; a '=' inside the file name itself is rare, but respect existing files.
    size_t eq = a.rfind('=');
    std::error_code ec;
    if (eq == std::string::npos || fs::exists(a, ec)) return {a, ""};
    return {a.substr(0, eq), a.substr(eq + 1)};
}

struct PackArgs {
    std::string out, format, atlas;
    WriteOptions opt;
    IgnoreRules rules;
    std::vector<std::string> include;
    bool external = false;
    std::vector<std::pair<std::string, std::string>> inputs;
    bool split = false;              // --chunk-size given: base pack + chunks
    SplitOptions so;
    std::string chunksDir;
};

uint64_t parseSize(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
    double v = std::atof(s.substr(0, i).c_str());
    std::string u;
    for (char c : s.substr(i)) u += char(std::toupper(static_cast<unsigned char>(c)));
    double m = u.empty() || u == "B" ? 1 : u == "K" || u == "KB" ? 1024.0 : u == "M" || u == "MB" ? 1048576.0 : u == "G" || u == "GB" ? 1073741824.0 : -1;
    if (i == 0 || m < 0) { std::cout << "ОШИБКА: размер \"" << s << "\" — ожидается число с KB/MB/GB" << std::endl; std::exit(2); }
    return uint64_t(v * m);
}

bool parsePackArgs(int argc, char** argv, PackArgs& a, bool importMode) {
    for (int i = 0; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
        if (s == "-o" || s == "--out") a.out = next();
        else if (s == "--level") a.opt.level = std::atoi(next().c_str());
        else if (s == "--threads") a.opt.threads = unsigned(std::atoi(next().c_str()));
        else if (s == "--store") a.opt.compress = false;
        else if (s == "--ignore") a.rules.add(next());
        else if (s == "--atlas") a.atlas = next();
        else if (s == "--chunk-size") { a.split = true; a.so.chunkSize = parseSize(next()); }
        else if (s == "--base-size") a.so.baseSize = parseSize(next());
        else if (s == "--total-size") a.so.totalBudget = parseSize(next());
        else if (s == "--fill-base") a.so.fillBase = true;
        else if (s == "--base") a.so.base.push_back(next());
        else if (s == "--order") a.so.priority.push_back(next());
        else if (s == "--chunks-dir") a.chunksDir = next();
        else if (importMode && s == "--format") a.format = next();
        else if (importMode && s == "--include") a.include.push_back(next());
        else if (importMode && s == "--external") a.external = true;
        else if (!s.empty() && s[0] == '-') return false;
        else a.inputs.push_back(splitPrefix(s));
    }
    return !a.out.empty() && !a.inputs.empty();
}

// game.json of the input mounted at the pack root ("" prefix), or an empty object.
Json rootGameJson(const std::vector<std::pair<std::string, std::string>>& inputs) {
    for (auto& [dir, prefix] : inputs) {
        std::error_code ec;
        if (prefix.empty() && fs::exists(fs::path(dir) / "game.json", ec)) return Json::parseFile((fs::path(dir) / "game.json").string());
    }
    return Json::object();
}

int finishPack(PackWriter& w, const PackArgs& a, const std::string& importsJson) {
    WriteStats st;
    std::string err;
    WriteOptions opt = a.opt;
    opt.external = opt.external || a.external;
    rts::atlas::AtlasStats as;
    if (!a.atlas.empty()) {
        try {
            auto cfg = rts::atlas::AtlasConfig::fromGame(rootGameJson(a.inputs), a.atlas);
            if (!rts::atlas::applyAtlas(w, cfg, as, &err)) return die(err);
        } catch (const std::exception& e) { return die(e.what()); }
    }
    SplitResult sr;
    std::string chunksDir = a.chunksDir.empty() ? (fs::path(a.out).parent_path() / "chunks").string() : a.chunksDir;
    if (a.split) {
        if (!w.writeSplitFiles(a.out, chunksDir, opt, a.so, &st, &sr, &err)) return die(err);
    } else if (!w.write(a.out, opt, &st, &err)) {
        return die(err);
    }
    rts::atlas::finishStats(as, st);
    bool external = opt.external || w.external();
    if (G.json) {
        std::cout << "{\"ok\":true,\"out\":" << q(a.out) << ",\"files\":" << st.items.size() << ",\"rawBytes\":" << st.rawBytes
                  << ",\"packBytes\":" << st.packBytes << ",\"duplicates\":" << st.duplicates << ",\"external\":" << (external ? "true" : "false")
                  << ",\"contentHash\":" << q(hex64(st.contentHash)) << ",\"imports\":[" << importsJson << "]";
        if (!a.atlas.empty()) std::cout << ",\"atlas\":" << as.toJson().dump(-1);
        if (a.split) {
            std::cout << ",\"split\":{\"baseBytes\":" << sr.baseBytes << ",\"totalBytes\":" << sr.totalBytes << ",\"chunksDir\":" << q(chunksDir) << ",\"chunks\":[";
            for (size_t i = 0; i < sr.chunks.size(); ++i)
                std::cout << (i ? "," : "") << "{\"file\":" << q(sr.chunks[i].file) << ",\"bytes\":" << sr.chunks[i].size << ",\"files\":" << sr.chunks[i].files
                          << ",\"sha256\":" << q(sr.chunks[i].sha256) << "}";
            std::cout << "]}";
        }
        std::cout << ",\"warnings\":[";
        for (size_t i = 0; i < st.warnings.size(); ++i) std::cout << (i ? "," : "") << q(st.warnings[i]);
        std::cout << "]}" << std::endl;
        return 0;
    }
    for (auto& m : st.warnings) std::cout << "предупреждение: " << m << "\n";
    for (auto& m : as.warnings) std::cout << "предупреждение: " << m << "\n";
    std::cout << a.out << "\n" << sizeReport(st) << (a.atlas.empty() ? std::string() : as.report()) << "хеш контента: " << hex64(st.contentHash) << std::endl;
    if (a.split) {
        std::cout << "разбиение: в паке " << formatBytes(sr.baseBytes) << ", частей " << sr.chunks.size() << " в " << chunksDir << ", всего " << formatBytes(sr.totalBytes) << "\n";
        for (auto& c : sr.chunks) std::cout << "  " << c.file << "  " << formatBytes(c.size) << ", файлов " << c.files << "\n";
    }
    if (external) std::cout << "ВНИМАНИЕ: пак содержит внешний (сторонний) контент — не коммитить и не распространять." << std::endl;
    return 0;
}

int cmdPack(int argc, char** argv) {
    PackArgs a;
    if (!parsePackArgs(argc, argv, a, false)) { usage(); return 2; }
    PackWriter w;
    std::string err;
    for (auto& [src, prefix] : a.inputs) {
        std::error_code ec;
        if (fs::is_regular_file(src, ec)) {
            if (!w.addFile(prefix.empty() ? fs::path(src).filename().string() : prefix, src, &err)) return die(err);
        } else if (w.addDirectory(src, prefix, a.rules, &err) < 0) {
            return die(err);
        }
    }
    return finishPack(w, a, "");
}

int cmdAtlas(int argc, char** argv) {
    std::string out, profile = "linux";
    std::vector<std::pair<std::string, std::string>> inputs;
    for (int i = 0; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
        if (s == "-o" || s == "--out") out = next();
        else if (s == "--profile") profile = next();
        else if (!s.empty() && s[0] == '-') { usage(); return 2; }
        else inputs.push_back(splitPrefix(s));
    }
    if (out.empty() || inputs.empty()) { usage(); return 2; }
    PackWriter w;
    IgnoreRules rules;
    std::string err;
    for (auto& [dir, prefix] : inputs) if (w.addDirectory(dir, prefix, rules, &err) < 0) return die(err);
    std::vector<rts::atlas::SourceImage> imgs;
    rts::atlas::AtlasOutput res;
    try {
        auto cfg = rts::atlas::AtlasConfig::fromGame(rootGameJson(inputs), profile);
        rts::atlas::scanSheets(w, cfg);
        for (const std::string& p : w.paths()) {
            if (!rts::atlas::isImagePath(p)) continue;
            rts::atlas::SourceImage src;
            src.path = p;
            if (!w.readInput(p, src.bytes)) return die("не удалось прочитать " + p);
            imgs.push_back(std::move(src));
        }
        if (!rts::atlas::buildAtlas(imgs, cfg, res, &err)) return die(err);
    } catch (const std::exception& e) { return die(e.what()); }
    auto save = [&](const fs::path& f, const std::string& data) {
        fs::create_directories(f.parent_path());
        std::ofstream o(f, std::ios::binary);
        o.write(data.data(), std::streamsize(data.size()));
    };
    for (size_t i = 0; i < res.pageImages.size(); ++i) {
        fs::path f = fs::path(out) / res.manifest.pages()[i].file;
        save(f.replace_extension(".png"), rts::atlas::encodePng(res.pageImages[i]));
    }
    save(fs::path(out) / rts::atlas::kManifestPath, res.manifest.serialize());
    save(fs::path(out) / "atlas-report.json", res.stats.toJson().dump(2) + "\n");
    if (G.json) std::cout << "{\"ok\":true,\"out\":" << q(out) << ",\"atlas\":" << res.stats.toJson().dump(-1) << "}" << std::endl;
    else std::cout << res.stats.report() << "-> " << out << std::endl;
    return 0;
}

int cmdImport(int argc, char** argv) {
    PackArgs a;
    if (!parsePackArgs(argc, argv, a, true)) { usage(); return 2; }
    UnpackerRegistry reg = registry();
    PackWriter w;
    std::string err, imports;
    for (auto& [src, prefix] : a.inputs) {
        ImportOptions io;
        io.format = a.format; io.prefix = prefix; io.include = a.include; io.external = a.external;
        ImportResult r;
        if (!importArchive(reg, src, io, w, &r, &err)) return die(src + ": " + err);
        if (!G.json) std::cout << src << ": " << r.format << ", файлов " << r.files << (r.external ? " (внешний контент)" : "") << "\n";
        imports += std::string(imports.empty() ? "" : ",") + "{\"source\":" + q(src) + ",\"format\":" + q(r.format) + ",\"prefix\":" + q(prefix) +
                   ",\"files\":" + std::to_string(r.files) + ",\"external\":" + (r.external ? "true" : "false") + "}";
    }
    return finishPack(w, a, imports);
}

int cmdList(const std::string& path) {
    std::string err;
    UnpackerRegistry reg = registry();
    const UnpackerInfo* u = reg.detect(path, &err);
    if (!u) return die(err);
    if (u->builtin) {
        auto p = openPack(path, err);
        if (!p) return die(err);
        uint64_t raw = 0;
        if (G.json) std::cout << "{\"ok\":true,\"format\":\"rpak\",\"external\":" << (p->external() ? "true" : "false") << ",\"contentHash\":" << q(hex64(p->contentHash())) << ",\"entries\":[";
        for (size_t i = 0; i < p->size(); ++i) {
            const Entry& e = p->entry(i);
            raw += e.rawSize;
            if (G.json) std::cout << (i ? "," : "") << "{\"path\":" << q(std::string(e.path)) << ",\"size\":" << e.rawSize << ",\"stored\":" << e.storedSize
                                  << ",\"codec\":\"" << (e.codec == Codec::Zstd ? "zstd" : "store") << "\"}";
            else std::cout << formatBytes(e.rawSize) << "\t" << formatBytes(e.storedSize) << "\t" << (e.codec == Codec::Zstd ? "zstd" : "store")
                           << (e.chunk ? "\tчасть " + std::to_string(e.chunk) : std::string("\t")) << "\t" << e.path << "\n";
        }
        if (G.json) { std::cout << "]}" << std::endl; return 0; }
        std::error_code ec;
        std::cout << p->size() << " файлов, " << formatBytes(raw) << " -> " << formatBytes(fs::file_size(path, ec)) << ", хеш контента " << hex64(p->contentHash())
                  << (p->external() ? ", ВНЕШНИЙ контент" : "") << std::endl;
        return 0;
    }
    std::vector<ArchiveEntryInfo> entries;
    if (!reg.list(*u, path, entries, &err)) return die(err);
    uint64_t raw = 0;
    if (G.json) std::cout << "{\"ok\":true,\"format\":" << q(u->name) << ",\"external\":" << (u->external ? "true" : "false") << ",\"entries\":[";
    for (size_t i = 0; i < entries.size(); ++i) {
        raw += entries[i].size;
        if (G.json) std::cout << (i ? "," : "") << "{\"path\":" << q(entries[i].path) << ",\"size\":" << entries[i].size << "}";
        else std::cout << formatBytes(entries[i].size) << "\t" << entries[i].path << "\n";
    }
    if (G.json) { std::cout << "]}" << std::endl; return 0; }
    std::cout << entries.size() << " файлов, " << formatBytes(raw) << ", формат " << u->name << " (" << u->title << ")" << std::endl;
    return 0;
}

size_t attachLocalChunks(Pack& p, const std::string& packPath, std::vector<std::string>& problems) {
    return attachChunksNextTo(p, packPath, &problems);
}

int cmdVerify(int argc, char** argv) {
    int bad = 0;
    if (G.json) std::cout << "{\"ok\":";
    std::string results;
    for (int i = 0; i < argc; ++i) {
        std::string err;
        auto p = openPack(argv[i], err);
        size_t errors = 0;
        std::string firstErr = err;
        if (p) {
            std::vector<std::string> problems;
            attachLocalChunks(*p, argv[i], problems);
            for (auto& m : problems) {
                if (!G.json) std::cout << "  часть: " << m << "\n";
                if (firstErr.empty()) firstErr = m;
                ++errors;
            }
            std::string data;
            for (size_t k = 0; k < p->size(); ++k) {
                if (!p->ready(p->entry(k))) continue;   // its chunk was already reported
                if (!p->read(p->entry(k), data, true)) {
                    if (!G.json) std::cout << "  повреждён: " << p->lastError() << "\n";
                    if (firstErr.empty()) firstErr = p->lastError();
                    ++errors;
                }
            }
        } else {
            errors = 1;
        }
        if (!G.json) std::cout << argv[i] << ": " << (errors ? "ОШИБКИ (" + std::to_string(errors) + ") " + firstErr : std::string("OK")) << std::endl;
        results += std::string(results.empty() ? "" : ",") + "{\"pack\":" + q(argv[i]) + ",\"errors\":" + std::to_string(errors) + (firstErr.empty() ? "" : ",\"error\":" + q(firstErr)) + "}";
        bad += errors ? 1 : 0;
    }
    if (G.json) std::cout << (bad ? "false" : "true") << ",\"results\":[" << results << "]}" << std::endl;
    return bad ? 1 : 0;
}

int cmdExtract(const std::string& path, const std::string& dir) {
    std::string err;
    UnpackerRegistry reg = registry();
    const UnpackerInfo* u = reg.detect(path, &err);
    if (!u) return die(err);
    if (!reg.extract(*u, path, dir, &err)) return die(err);
    if (G.json) std::cout << "{\"ok\":true,\"format\":" << q(u->name) << ",\"out\":" << q(dir) << "}" << std::endl;
    else std::cout << path << " (" << u->name << ") -> " << dir << std::endl;
    return 0;
}

int cmdDiff(const std::string& a, const std::string& b) {
    std::string err;
    auto pa = openPack(a, err);
    if (!pa) return die(err);
    auto pb = openPack(b, err);
    if (!pb) return die(err);
    std::map<std::string, const Entry*> ma, mb;
    for (size_t i = 0; i < pa->size(); ++i) ma[std::string(pa->entry(i).path)] = &pa->entry(i);
    for (size_t i = 0; i < pb->size(); ++i) mb[std::string(pb->entry(i).path)] = &pb->entry(i);
    std::vector<std::pair<char, std::string>> changes;
    uint64_t patch = 0;
    for (auto& [path, e] : mb) {
        auto it = ma.find(path);
        if (it == ma.end()) { changes.emplace_back('+', path); patch += e->storedSize; }
        else if (it->second->hash != e->hash || it->second->rawSize != e->rawSize) { changes.emplace_back('~', path); patch += e->storedSize; }
    }
    for (auto& [path, e] : ma) if (!mb.count(path)) changes.emplace_back('-', path);
    size_t n[3] = {0, 0, 0};
    for (auto& c : changes) ++n[c.first == '+' ? 0 : c.first == '~' ? 1 : 2];
    if (G.json) {
        std::cout << "{\"ok\":true,\"added\":" << n[0] << ",\"changed\":" << n[1] << ",\"removed\":" << n[2] << ",\"patchBytes\":" << patch << ",\"changes\":[";
        for (size_t i = 0; i < changes.size(); ++i) std::cout << (i ? "," : "") << "{\"op\":\"" << changes[i].first << "\",\"path\":" << q(changes[i].second) << "}";
        std::cout << "]}" << std::endl;
        return 0;
    }
    for (auto& c : changes) std::cout << c.first << " " << c.second << "\n";
    std::cout << "добавлено " << n[0] << ", изменено " << n[1] << ", удалено " << n[2] << "; объём изменений ~" << formatBytes(patch) << std::endl;
    return 0;
}

std::string infoJson(const UnpackerInfo& u) {
    std::string s = "{\"name\":" + q(u.name) + ",\"title\":" + q(u.title) + ",\"version\":" + q(u.version) + ",\"builtin\":" + (u.builtin ? "true" : "false") +
                    ",\"external\":" + (u.external ? "true" : "false") + ",\"manifest\":" + q(u.manifest) + ",\"extensions\":[";
    for (size_t i = 0; i < u.extensions.size(); ++i) s += (i ? "," : "") + q(u.extensions[i]);
    s += "],\"capabilities\":[";
    for (size_t i = 0; i < u.capabilities.size(); ++i) s += (i ? "," : "") + q(u.capabilities[i]);
    return s + "]}";
}

int cmdFormats() {
    UnpackerRegistry reg = registry();
    if (G.json) {
        std::cout << "{\"ok\":true,\"protocol\":" << kUnpackerProtocol << ",\"formats\":[";
        for (size_t i = 0; i < reg.all().size(); ++i) std::cout << (i ? "," : "") << infoJson(reg.all()[i]);
        std::cout << "],\"errors\":[";
        for (size_t i = 0; i < reg.errors().size(); ++i) std::cout << (i ? "," : "") << q(reg.errors()[i]);
        std::cout << "]}" << std::endl;
        return 0;
    }
    std::cout << "формат\tрасширения\tописание\n";
    for (auto& u : reg.all()) {
        std::string ext;
        for (auto& e : u.extensions) ext += (ext.empty() ? "" : " ") + e;
        std::cout << u.name << "\t" << (ext.empty() ? "-" : ext) << "\t" << u.title << (u.builtin ? "  [встроенный]" : "") << "\n";
    }
    std::cout << "\nплагины ищутся в: " << (fs::path(G.engine) / "tools" / "unpackers").string() << ", $RTS_UNPACKERS, <проект>/unpackers, --unpackers" << std::endl;
    return 0;
}

int cmdProbe(int argc, char** argv) {
    UnpackerRegistry reg = registry();
    int unknown = 0;
    std::string results;
    for (int i = 0; i < argc; ++i) {
        std::string why;
        std::error_code ec;
        const UnpackerInfo* u = fs::is_directory(argv[i], ec) ? nullptr : reg.detect(argv[i], &why);
        std::string fmt = fs::is_directory(argv[i], ec) ? "dir" : u ? u->name : "";
        if (fmt.empty()) ++unknown;
        if (!G.json) std::cout << argv[i] << ": " << (fmt.empty() ? "неизвестный формат" : fmt) << "\n";
        results += std::string(results.empty() ? "" : ",") + "{\"file\":" + q(argv[i]) + ",\"format\":" + (fmt.empty() ? "null" : q(fmt)) + "}";
    }
    if (G.json) std::cout << "{\"ok\":true,\"results\":[" << results << "]}" << std::endl;
    return unknown ? 1 : 0;
}

int cmdUnpackers(int argc, char** argv) {
    if (argc >= 1 && std::string(argv[0]) == "new") {
        if (argc != 3) { usage(); return 2; }
        std::string err;
        if (!scaffoldUnpacker(argv[1], argv[2], G.engine, &err)) return die(err);
        fs::path d = fs::path(argv[1]) / argv[2];
        if (G.json) std::cout << "{\"ok\":true,\"dir\":" << q(d.string()) << "}" << std::endl;
        else std::cout << "создан плагин: " << d.string() << " (unpacker.json + unpack.py; протокол: tools/unpackers/PROTOCOL.md)" << std::endl;
        return 0;
    }
    if (argc >= 1 && std::string(argv[0]) == "check") {
        UnpackerRegistry reg = registry();
        std::vector<std::string> names;
        for (int i = 1; i < argc; ++i) {
            std::error_code ec;
            if (fs::exists(fs::path(argv[i]) / "unpacker.json", ec) || fs::path(argv[i]).filename() == "unpacker.json") {
                fs::path m = fs::path(argv[i]).filename() == "unpacker.json" ? fs::path(argv[i]) : fs::path(argv[i]) / "unpacker.json";
                std::string err;
                if (!reg.loadManifest(m.string(), &err)) return die(err);
                Json probeName = Json::parseFile(m.string());
                names.push_back(probeName.getStr("name"));
            } else {
                names.push_back(argv[i]);
            }
        }
        if (names.empty()) for (auto& u : reg.all()) if (!u.builtin) names.push_back(u.name);
        int bad = 0;
        std::string results;
        for (auto& n : names) {
            const UnpackerInfo* u = reg.byName(n);
            std::string err;
            bool ok = u && reg.handshake(*u, &err);
            if (!u) err = "not found";
            if (!ok) ++bad;
            if (!G.json) std::cout << n << ": " << (ok ? "OK" : "ОШИБКА — " + err) << "\n";
            results += std::string(results.empty() ? "" : ",") + "{\"name\":" + q(n) + ",\"ok\":" + (ok ? "true" : "false") + (ok ? "" : ",\"error\":" + q(err)) + "}";
        }
        for (auto& e : reg.errors()) { ++bad; if (!G.json) std::cout << "ОШИБКА манифеста: " << e << "\n"; }
        if (G.json) {
            std::cout << "{\"ok\":" << (bad ? "false" : "true") << ",\"results\":[" << results << "],\"manifestErrors\":[";
            for (size_t i = 0; i < reg.errors().size(); ++i) std::cout << (i ? "," : "") << q(reg.errors()[i]);
            std::cout << "]}" << std::endl;
        }
        return bad ? 1 : 0;
    }
    if (argc >= 1 && std::string(argv[0]) == "list") return cmdFormats();
    usage();
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    // Pull global options out first so they may appear anywhere.
    std::vector<char*> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
        if (a == "--json") G.json = true;
        else if (a == "--unpackers") G.unpackerDirs.push_back(next());
        else if (a == "--project") G.project = next();
        else if (a == "--engine") G.engine = next();
        else args.push_back(argv[i]);
    }
    if (args.empty()) { usage(); return 2; }
    std::string cmd = args[0];
    int n = int(args.size()) - 1;
    char** rest = args.data() + 1;
    if (cmd == "pack") return cmdPack(n, rest);
    if (cmd == "import") return cmdImport(n, rest);
    if (cmd == "list" && n == 1) return cmdList(rest[0]);
    if (cmd == "verify" && n >= 1) return cmdVerify(n, rest);
    if (cmd == "extract" && n == 2) return cmdExtract(rest[0], rest[1]);
    if (cmd == "diff" && n == 2) return cmdDiff(rest[0], rest[1]);
    if (cmd == "formats") return cmdFormats();
    if (cmd == "probe" && n >= 1) return cmdProbe(n, rest);
    if (cmd == "unpackers") return cmdUnpackers(n, rest);
    if (cmd == "atlas") return cmdAtlas(n, rest);
    if (cmd == "-h" || cmd == "--help") { usage(); return 0; }
    usage();
    return 2;
}
