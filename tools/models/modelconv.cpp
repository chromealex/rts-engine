// modelconv: converts model formats the runtime does not read into glTF binary sidecars
// (models/ship.lwo -> models/ship.lwo.glb), see tools/models/ModelConvert.h and docs/models.md.
//
//   modelconv <model> [out.glb] [--force] [--json]   one file (default output: the sidecar)
//   modelconv --project <game folder> [--force]       every convertible model of a game
//   modelconv --formats                               extensions that need conversion
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "ModelConvert.h"
#include "engine/data/Json.h"
#include "engine/render/Model.h"

using namespace rts;

namespace {

bool readDisk(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

int usage() {
    std::fprintf(stderr,
                 "usage: modelconv <model> [out.glb] [--force] [--json]\n"
                 "       modelconv --project <game folder> [--force] [--json]\n"
                 "       modelconv --formats\n");
    return 2;
}

// The sidecar must load in the engine exactly as the game will load it.
bool verify(const std::string& glbPath, Json& out, std::string& err) {
    try {
        std::vector<std::string> warn;
        auto m = render::loadModel(glbPath, readDisk, &warn);
        out.set("nodes", Json::number(int64_t(m->nodes.size())));
        out.set("meshes", Json::number(int64_t(m->meshes.size())));
        out.set("materials", Json::number(int64_t(m->materials.size())));
        out.set("skins", Json::number(int64_t(m->skins.size())));
        Json clips = Json::array();
        for (const auto& c : m->clips) clips.push(Json::string(c.name));
        out.set("clips", clips);
        Json w = Json::array();
        for (const auto& s : warn) w.push(Json::string(s));
        out.set("loadWarnings", w);
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string in, out, project;
    bool force = false, json = false, formats = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--force") force = true;
        else if (a == "--json") json = true;
        else if (a == "--formats") formats = true;
        else if ((a == "--project" || a == "--dir") && i + 1 < argc) project = argv[++i];
        else if (a == "-h" || a == "--help") return usage();
        else if (in.empty()) in = a;
        else if (out.empty()) out = a;
        else return usage();
    }
    if (formats) {
        Json r = Json::object();
        r.set("converter", Json::string(models::converterAvailable() ? models::converterVersion() : "none (built without assimp)"));
        Json e = Json::array();
        for (const auto& x : models::convertibleExtensions()) e.push(Json::string(x));
        r.set("extensions", e);
        std::printf("%s\n", r.dump(json ? -1 : 2).c_str());
        return 0;
    }
    if (!project.empty()) {
        models::ConvertReport rep;
        std::string err;
        if (!models::convertDirectory(project, force, rep, &err)) { std::fprintf(stderr, "modelconv: %s\n", err.c_str()); return 1; }
        int failed = 0;
        Json items = Json::array();
        for (const auto& it : rep.items) {
            failed += it.ok ? 0 : 1;
            if (json) {
                Json j = Json::object();
                j.set("path", Json::string(it.path));
                j.set("ok", Json::fromBool(it.ok));
                j.set("result", Json::string(it.result));
                items.push(j);
            } else {
                std::printf("%s %s: %s\n", it.ok ? "ok  " : "FAIL", it.path.c_str(), it.result.c_str());
            }
        }
        if (json) {
            Json r = Json::object();
            r.set("items", items);
            Json w = Json::array();
            for (const auto& s : rep.warnings) w.push(Json::string(s));
            r.set("warnings", w);
            std::printf("%s\n", r.dump(-1).c_str());
        } else {
            for (const auto& s : rep.warnings) std::printf("warning: %s\n", s.c_str());
            if (rep.items.empty()) std::printf("nothing to convert\n");
        }
        return failed ? 1 : 0;
    }
    if (in.empty()) return usage();
    models::ConvertInfo info;
    std::string err;
    bool did = false;
    const std::string dst = out.empty() ? models::sidecarPath(in) : out;
    if (!models::convertFile(in, dst, force, &did, &info, &err)) {
        if (json) {
            Json r = Json::object();
            r.set("ok", Json::fromBool(false));
            r.set("error", Json::string(err));
            std::printf("%s\n", r.dump(-1).c_str());
        } else {
            std::fprintf(stderr, "modelconv: %s\n", err.c_str());
        }
        return 1;
    }
    Json r = Json::object();
    r.set("ok", Json::fromBool(true));
    r.set("output", Json::string(dst));
    r.set("converted", Json::fromBool(did));
    Json w = Json::array();
    for (const auto& s : info.warnings) w.push(Json::string(s));
    r.set("warnings", w);
    std::string verr;
    // verify through the engine's loader (by the source path, so the sidecar lookup is exercised)
    const std::string probe = out.empty() ? in : dst;
    if (!verify(probe, r, verr)) {
        r.set("ok", Json::fromBool(false));
        r.set("error", Json::string("converted file does not load in the engine: " + verr));
    }
    if (json) std::printf("%s\n", r.dump(-1).c_str());
    else {
        std::printf("%s -> %s%s\n", in.c_str(), dst.c_str(), did ? "" : " (up to date)");
        std::printf("%s\n", r.dump(2).c_str());
    }
    return r.getBool("ok") ? 0 : 1;
}
