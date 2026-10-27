// assetpipe — builds and inspects resource packs (.rpak).
//
//   assetpipe pack -o <out.rpak> [--level N] [--store] [--threads N] [--ignore <glob>]... <dir>[=<prefix>]...
//   assetpipe list <pack>                 entries with sizes and codec
//   assetpipe verify <pack>...            decompress everything and check hashes
//   assetpipe extract <pack> <dir>        unpack to a folder
//   assetpipe diff <old.rpak> <new.rpak>  added / removed / changed files (patch planning)
//
// Example (what rtsbuild does):
//   assetpipe pack -o dist/content.rpak games/skirmish engine/assets=engine
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "engine/pack/PackWriter.h"

namespace fs = std::filesystem;
using namespace rts::pack;

namespace {

void usage() {
    std::cout << "usage:\n"
                 "  assetpipe pack -o <out.rpak> [--level N] [--store] [--threads N] [--ignore <glob>]... <dir>[=<prefix>]...\n"
                 "  assetpipe list <pack>\n"
                 "  assetpipe verify <pack>...\n"
                 "  assetpipe extract <pack> <dir>\n"
                 "  assetpipe diff <old.rpak> <new.rpak>\n";
}

int die(const std::string& s) { std::cout << "ОШИБКА: " << s << std::endl; return 1; }

std::unique_ptr<Pack> openOrDie(const std::string& path) {
    std::string err;
    auto p = Pack::openFile(path, &err);
    if (!p) { std::cout << "ОШИБКА: " << err << std::endl; std::exit(1); }
    return p;
}

char hex(unsigned v) { return "0123456789abcdef"[v & 15]; }
std::string hex64(uint64_t v) { std::string s(16, '0'); for (int i = 15; i >= 0; --i, v >>= 4) s[size_t(i)] = hex(unsigned(v)); return s; }

int cmdPack(int argc, char** argv) {
    std::string out;
    WriteOptions opt;
    IgnoreRules rules;
    std::vector<std::pair<std::string, std::string>> inputs;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
        if (a == "-o" || a == "--out") out = next();
        else if (a == "--level") opt.level = std::atoi(next().c_str());
        else if (a == "--threads") opt.threads = unsigned(std::atoi(next().c_str()));
        else if (a == "--store") opt.compress = false;
        else if (a == "--ignore") rules.add(next());
        else if (!a.empty() && a[0] == '-') { usage(); return 2; }
        else {
            size_t eq = a.find('=');
            inputs.emplace_back(a.substr(0, eq), eq == std::string::npos ? "" : a.substr(eq + 1));
        }
    }
    if (out.empty() || inputs.empty()) { usage(); return 2; }
    PackWriter w;
    std::string err;
    for (auto& [dir, prefix] : inputs) {
        std::error_code ec;
        if (fs::is_regular_file(dir, ec)) {
            if (!w.addFile(prefix.empty() ? fs::path(dir).filename().string() : prefix, dir, &err)) return die(err);
        } else if (w.addDirectory(dir, prefix, rules, &err) < 0) {
            return die(err);
        }
    }
    WriteStats st;
    if (!w.write(out, opt, &st, &err)) return die(err);
    for (auto& wmsg : st.warnings) std::cout << "предупреждение: " << wmsg << "\n";
    std::cout << out << "\n" << sizeReport(st) << "хеш контента: " << hex64(st.contentHash) << std::endl;
    return 0;
}

int cmdList(const std::string& path) {
    auto p = openOrDie(path);
    uint64_t raw = 0;
    for (size_t i = 0; i < p->size(); ++i) {
        const Entry& e = p->entry(i);
        raw += e.rawSize;
        std::cout << formatBytes(e.rawSize) << "\t" << formatBytes(e.storedSize) << "\t" << (e.codec == Codec::Zstd ? "zstd" : "store") << "\t" << e.path << "\n";
    }
    std::error_code ec;
    std::cout << p->size() << " файлов, " << formatBytes(raw) << " -> " << formatBytes(fs::file_size(path, ec))
              << ", хеш контента " << hex64(p->contentHash()) << std::endl;
    return 0;
}

int cmdVerify(int argc, char** argv) {
    int bad = 0;
    for (int i = 0; i < argc; ++i) {
        auto p = openOrDie(argv[i]);
        std::string data;
        size_t errors = 0;
        for (size_t k = 0; k < p->size(); ++k)
            if (!p->read(p->entry(k), data, true)) { std::cout << "  повреждён: " << p->lastError() << "\n"; ++errors; }
        std::cout << argv[i] << ": " << (errors ? "ОШИБКИ (" + std::to_string(errors) + ")" : std::string("OK")) << ", " << p->size() << " файлов" << std::endl;
        bad += errors ? 1 : 0;
    }
    return bad ? 1 : 0;
}

int cmdExtract(const std::string& path, const fs::path& dir) {
    auto p = openOrDie(path);
    std::string data;
    for (size_t k = 0; k < p->size(); ++k) {
        const Entry& e = p->entry(k);
        std::string rel(e.path);
        bool unsafe = rel.empty() || rel[0] == '/' || rel.find(':') != std::string::npos;
        for (size_t a = 0; !unsafe && a <= rel.size();) {
            size_t b = rel.find('/', a);
            if (b == std::string::npos) b = rel.size();
            unsafe = rel.compare(a, b - a, "..") == 0 && b - a == 2;
            a = b + 1;
        }
        if (unsafe) return die("небезопасный путь в паке: " + rel);
        if (!p->read(e, data, true)) return die(p->lastError());
        fs::path f = dir / rel;
        fs::create_directories(f.parent_path());
        std::ofstream o(f, std::ios::binary);
        o.write(data.data(), std::streamsize(data.size()));
        if (!o) return die("не удалось записать " + f.string());
    }
    std::cout << p->size() << " файлов -> " << dir.string() << std::endl;
    return 0;
}

int cmdDiff(const std::string& a, const std::string& b) {
    auto pa = openOrDie(a), pb = openOrDie(b);
    std::map<std::string, const Entry*> ma, mb;
    for (size_t i = 0; i < pa->size(); ++i) ma[std::string(pa->entry(i).path)] = &pa->entry(i);
    for (size_t i = 0; i < pb->size(); ++i) mb[std::string(pb->entry(i).path)] = &pb->entry(i);
    size_t added = 0, removed = 0, changed = 0;
    uint64_t patch = 0;
    for (auto& [path, e] : mb) {
        auto it = ma.find(path);
        if (it == ma.end()) { std::cout << "+ " << path << "\n"; ++added; patch += e->storedSize; }
        else if (it->second->hash != e->hash || it->second->rawSize != e->rawSize) { std::cout << "~ " << path << "\n"; ++changed; patch += e->storedSize; }
    }
    for (auto& [path, e] : ma) if (!mb.count(path)) { std::cout << "- " << path << "\n"; ++removed; }
    std::cout << "добавлено " << added << ", изменено " << changed << ", удалено " << removed << "; объём изменений ~" << formatBytes(patch) << std::endl;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    std::string cmd = argv[1];
    if (cmd == "pack") return cmdPack(argc - 2, argv + 2);
    if (cmd == "list" && argc == 3) return cmdList(argv[2]);
    if (cmd == "verify" && argc >= 3) return cmdVerify(argc - 2, argv + 2);
    if (cmd == "extract" && argc == 4) return cmdExtract(argv[2], argv[3]);
    if (cmd == "diff" && argc == 4) return cmdDiff(argv[2], argv[3]);
    if (cmd == "-h" || cmd == "--help") { usage(); return 0; }
    usage();
    return 2;
}
