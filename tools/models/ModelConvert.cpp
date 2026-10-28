#include "ModelConvert.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

#include "engine/pack/PackWriter.h"

#if defined(RTS_HAVE_ASSIMP) && !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define RTS_CONVERT_FORK 1
#include <sys/wait.h>
#include <unistd.h>
#else
#define RTS_CONVERT_FORK 0
#endif

#if defined(RTS_HAVE_ASSIMP)
#include <assimp/Exporter.hpp>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>
#include <assimp/MemoryIOWrapper.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/version.h>
#endif

namespace fs = std::filesystem;

namespace rts::models {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extOf(const std::string& path) {
    const size_t dot = path.find_last_of('.'), slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lower(path.substr(dot));
}

// Read natively by render::loadModel (engine/render/Model.cpp, kFormats); never converted.
const std::set<std::string> kNative = {".gltf", ".glb", ".obj", ".fbx", ".vox", ".vxl", ".ply", ".stl", ".3ds",
                                       ".dae", ".mdx", ".mdl", ".3do", ".w3d"};
// Model formats assimp imports, recognised even in builds without assimp (so packing can leave the
// sources out and report them). Ambiguous extensions (.raw, .ter, .xml, .txt, .pk3) are omitted.
const char* const kKnown[] = {".blend", ".lwo", ".lws", ".lxo", ".x", ".ms3d", ".ase", ".ask", ".ac", ".ac3d", ".acc",
                              ".md2", ".md3", ".md5mesh", ".mdc", ".smd", ".vta", ".b3d", ".irr", ".irrmesh", ".off",
                              ".ogex", ".cob", ".scn", ".q3o", ".q3s", ".nff", ".enff", ".xgl", ".zgl", ".3mf", ".amf",
                              ".bvh", ".csm", ".x3d", ".x3db", ".sib", ".m3d", ".iqm", ".mot", ".ndo", ".hmp", ".mesh"};

bool readDisk(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

#if defined(RTS_HAVE_ASSIMP)
std::string normalizePath(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    std::vector<std::string> parts;
    std::stringstream ss(p);
    std::string part;
    const bool abs = !p.empty() && p[0] == '/';
    while (std::getline(ss, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == ".." && !parts.empty() && parts.back() != "..") parts.pop_back();
        else parts.push_back(part);
    }
    std::string out = abs ? "/" : "";
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? "/" : "") + parts[i];
    return out;
}

std::string dirOf(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}

// assimp reads the model and everything it references (.mtl, textures, .bin) through ReadFn, so the
// same code converts files on disk and inputs of a pack being built.
class ReadIO final : public Assimp::IOSystem {
public:
    explicit ReadIO(const ReadFn& read) : read_(read) {}
    bool Exists(const char* file) const override {
        std::string data;
        return read_(normalizePath(file), data);
    }
    char getOsSeparator() const override { return '/'; }
    Assimp::IOStream* Open(const char* file, const char* mode) override {
        if (mode && (std::strchr(mode, 'w') || std::strchr(mode, 'a'))) return nullptr;
        std::string data;
        if (!read_(normalizePath(file), data)) return nullptr;
        auto* buf = new uint8_t[data.size() + 1];
        std::memcpy(buf, data.data(), data.size());
        buf[data.size()] = 0;
        return new Assimp::MemoryIOStream(buf, data.size(), true);
    }
    void Close(Assimp::IOStream* s) override { delete s; }

private:
    ReadFn read_;
};

// Texture references become paths relative to the model (where the sidecar sits too): absolute
// paths and backslashes from the authoring machine are reduced to what exists next to the model.
void fixTexturePaths(aiScene* scene, const std::string& modelPath, const ReadFn& read, ConvertInfo* info) {
    const std::string dir = dirOf(modelPath);
    std::set<std::string> missing;
    for (unsigned mi = 0; mi < scene->mNumMaterials; ++mi) {
        aiMaterial* mat = scene->mMaterials[mi];
        for (int t = aiTextureType_NONE; t <= aiTextureType_UNKNOWN; ++t) {
            const auto type = aiTextureType(t);
            for (unsigned k = 0; k < mat->GetTextureCount(type); ++k) {
                aiString p;
                if (mat->GetTexture(type, k, &p) != AI_SUCCESS) continue;
                std::string s = p.C_Str();
                if (s.empty() || s[0] == '*') continue;   // embedded
                std::replace(s.begin(), s.end(), '\\', '/');
                std::vector<std::string> cands{s};
                const size_t slash = s.find_last_of('/');
                if (slash != std::string::npos) cands.push_back(s.substr(slash + 1));
                std::string found;
                std::string tmp;
                for (const auto& c : cands)
                    if (read(normalizePath(dir + c), tmp)) { found = c; break; }
                if (found.empty()) { missing.insert(s); continue; }
                if (found != p.C_Str()) {
                    aiString np(found);
                    mat->AddProperty(&np, AI_MATKEY_TEXTURE(type, k));
                }
            }
        }
    }
    if (info)
        for (const auto& m : missing) info->warnings.push_back(modelPath + ": texture '" + m + "' not found next to the model");
}
bool convertInProcess(const std::string& path, const ReadFn& read, std::string& glb, ConvertInfo* info, std::string* error) {
    const std::string np = normalizePath(path);
    Assimp::Importer imp;
    imp.SetIOHandler(new ReadIO(read));
    imp.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);
    imp.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS, 4);
    const unsigned flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices |
                           aiProcess_LimitBoneWeights | aiProcess_SortByPType | aiProcess_ValidateDataStructure |
                           aiProcess_FindInvalidData | aiProcess_RemoveRedundantMaterials | aiProcess_PopulateArmatureData;
    const aiScene* cs = imp.ReadFile(np, flags);
    if (!cs || !cs->mRootNode || (cs->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
        if (error) {
            std::string why = imp.GetErrorString();
            if (extOf(np) == ".blend")
                why += " (assimp reads Blender files up to 2.7x; in newer Blender use File > Export > glTF 2.0)";
            *error = path + ": " + (why.empty() ? std::string("cannot convert") : why);
        }
        return false;
    }
    aiScene* scene = imp.GetOrphanedScene();
    std::unique_ptr<aiScene> own(scene);
    if (scene->mNumMeshes == 0) {
        if (error) *error = path + ": no meshes";
        return false;
    }
    fixTexturePaths(scene, np, read, info);
    // bones must name nodes of the hierarchy (assimp's glTF exporter crashes otherwise): a mesh with
    // dangling bones is exported unskinned
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        aiMesh* mesh = scene->mMeshes[i];
        bool ok = true;
        for (unsigned b = 0; b < mesh->mNumBones && ok; ++b) ok = scene->mRootNode->FindNode(mesh->mBones[b]->mName) != nullptr;
        if (ok) continue;
        for (unsigned b = 0; b < mesh->mNumBones; ++b) delete mesh->mBones[b];
        delete[] mesh->mBones;
        mesh->mBones = nullptr;
        mesh->mNumBones = 0;
        if (info) info->warnings.push_back(path + ": mesh '" + std::string(mesh->mName.C_Str()) + "' has bones outside the hierarchy; exported without skinning");
    }
    for (unsigned i = 0; i < scene->mNumAnimations; ++i)   // glTF needs names to tell clips apart
        if (scene->mAnimations[i]->mName.length == 0) scene->mAnimations[i]->mName = aiString(i == 0 ? "Default" : "Clip" + std::to_string(i));
    Assimp::Exporter ex;
    const aiExportDataBlob* blob = ex.ExportToBlob(scene, "glb2", 0);
    if (!blob || !blob->data || blob->size == 0) {
        if (error) *error = path + ": glTF export failed: " + ex.GetErrorString();
        return false;
    }
    glb.assign(static_cast<const char*>(blob->data), blob->size);
    if (info) {
        info->meshes = int(scene->mNumMeshes);
        info->materials = int(scene->mNumMaterials);
        info->textures = int(scene->mNumTextures);
        info->clips = int(scene->mNumAnimations);
        std::set<std::string> bones;
        for (unsigned i = 0; i < scene->mNumMeshes; ++i)
            for (unsigned b = 0; b < scene->mMeshes[i]->mNumBones; ++b) bones.insert(scene->mMeshes[i]->mBones[b]->mName.C_Str());
        info->bones = int(bones.size());
        std::vector<const aiNode*> stack{scene->mRootNode};
        while (!stack.empty()) {
            const aiNode* n = stack.back();
            stack.pop_back();
            ++info->nodes;
            for (unsigned c = 0; c < n->mNumChildren; ++c) stack.push_back(n->mChildren[c]);
        }
    }
    return true;
}

#if RTS_CONVERT_FORK
// assimp has crashing corner cases on broken files; a child process keeps rtsbuild / modelconv
// alive and turns a crash into a readable error.
void putU32(std::string& o, uint32_t v) { o.append(reinterpret_cast<const char*>(&v), 4); }
void putStr(std::string& o, const std::string& s) { putU32(o, uint32_t(s.size())); o += s; }
struct Cursor {
    const std::string& s; size_t at = 0; bool ok = true;
    uint32_t u32() { uint32_t v = 0; if (at + 4 > s.size()) { ok = false; return 0; } std::memcpy(&v, s.data() + at, 4); at += 4; return v; }
    std::string str() { uint32_t n = u32(); if (!ok || at + n > s.size()) { ok = false; return {}; } std::string r = s.substr(at, n); at += n; return r; }
};

bool convertIsolated(const std::string& path, const ReadFn& read, std::string& glb, ConvertInfo* info, std::string* error) {
    int fd[2];
    if (pipe(fd) != 0) return convertInProcess(path, read, glb, info, error);
    std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return convertInProcess(path, read, glb, info, error); }
    if (pid == 0) {
        close(fd[0]);
        ConvertInfo ci;
        std::string out, err;
        const bool ok = convertInProcess(path, read, out, &ci, &err);
        std::string msg;
        putU32(msg, ok ? 1u : 0u);
        putStr(msg, out);
        putStr(msg, err);
        for (int v : {ci.meshes, ci.materials, ci.textures, ci.clips, ci.nodes, ci.bones}) putU32(msg, uint32_t(v));
        putU32(msg, uint32_t(ci.warnings.size()));
        for (const auto& w : ci.warnings) putStr(msg, w);
        size_t done = 0;
        while (done < msg.size()) {
            const ssize_t n = write(fd[1], msg.data() + done, msg.size() - done);
            if (n <= 0) break;
            done += size_t(n);
        }
        close(fd[1]);
        _exit(0);
    }
    close(fd[1]);
    std::string msg;
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd[0], buf, sizeof buf);
        if (n <= 0) break;
        msg.append(buf, size_t(n));
    }
    close(fd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    Cursor c{msg};
    const bool ok = c.u32() == 1;
    std::string out = c.str(), err = c.str();
    ConvertInfo ci;
    for (int* v : {&ci.meshes, &ci.materials, &ci.textures, &ci.clips, &ci.nodes, &ci.bones}) *v = int(c.u32());
    const uint32_t nw = c.u32();
    for (uint32_t i = 0; i < nw && c.ok && i < 100000; ++i) ci.warnings.push_back(c.str());
    if (!c.ok || !WIFEXITED(status)) {
        if (error) *error = path + ": the converter crashed on this file (assimp); re-export it to glTF from the authoring tool";
        return false;
    }
    if (info) *info = ci;
    if (!ok) { if (error) *error = err; return false; }
    glb = std::move(out);
    return true;
}
#endif
#endif

} // namespace

bool converterAvailable() {
#if defined(RTS_HAVE_ASSIMP)
    return true;
#else
    return false;
#endif
}

std::string converterVersion() {
#if defined(RTS_HAVE_ASSIMP)
    return "assimp " + std::to_string(aiGetVersionMajor()) + "." + std::to_string(aiGetVersionMinor()) + "." + std::to_string(aiGetVersionPatch());
#else
    return {};
#endif
}

std::vector<std::string> convertibleExtensions() {
    std::set<std::string> exts(std::begin(kKnown), std::end(kKnown));
#if defined(RTS_HAVE_ASSIMP)
    Assimp::Importer imp;
    std::string list;
    imp.GetExtensionList(list);   // "*.3ds;*.obj;..."
    std::stringstream ss(list);
    for (std::string e; std::getline(ss, e, ';');) {
        if (e.size() > 2 && e[0] == '*') exts.insert(lower(e.substr(1)));
    }
    // assimp lists some non-model or ambiguous ones
    for (const char* skip : {".raw", ".ter", ".xml", ".txt", ".pk3", ".ifc", ".ifczip", ".stp", ".step", ".json", ".assbin", ".assjson",
                             ".uc", ".prj", ".md5anim", ".md5camera", ".mesh.xml", ".dxf"})
        exts.erase(skip);
#endif
    std::vector<std::string> out;
    for (const auto& e : exts) if (!kNative.count(e)) out.push_back(e);
    return out;
}

bool needsConversion(const std::string& path) {
    const std::string e = extOf(path);
    if (e.empty() || kNative.count(e)) return false;
    static const std::vector<std::string> exts = convertibleExtensions();
    return std::find(exts.begin(), exts.end(), e) != exts.end();
}

std::string sidecarPath(const std::string& path) { return path + ".glb"; }

bool convertModel(const std::string& path, const ReadFn& read, std::string& glb, ConvertInfo* info, std::string* error) {
#if defined(RTS_HAVE_ASSIMP)
#if RTS_CONVERT_FORK
    return convertIsolated(path, read, glb, info, error);
#else
    return convertInProcess(path, read, glb, info, error);
#endif
#else
    (void)read; (void)glb; (void)info;
    if (error) *error = path + ": this build has no model converter (configure with -DRTS_ASSIMP=ON)";
    return false;
#endif
}

bool convertFile(const std::string& in, const std::string& out, bool force, bool* converted, ConvertInfo* info, std::string* error) {
    if (converted) *converted = false;
    const std::string dst = out.empty() ? sidecarPath(in) : out;
    std::error_code ec;
    if (!fs::exists(in, ec)) {
        if (error) *error = in + ": file not found";
        return false;
    }
    if (!force && fs::exists(dst, ec) && fs::last_write_time(dst, ec) >= fs::last_write_time(in, ec)) return true;
    std::string glb;
    if (!convertModel(in, readDisk, glb, info, error)) return false;
    if (!pack::writeFileAtomic(dst, glb, error)) return false;
    if (converted) *converted = true;
    return true;
}

bool convertDirectory(const std::string& dir, bool force, ConvertReport& report, std::string* error) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        if (error) *error = dir + ": not a directory";
        return false;
    }
    std::vector<std::string> files;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory() && (name.empty() || name[0] == '.' || name == "build" || name == "dist")) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file() && needsConversion(it->path().string())) files.push_back(it->path().generic_string());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        ConvertInfo info;
        std::string err;
        bool did = false;
        const std::string rel = fs::path(f).lexically_relative(dir).generic_string();
        if (convertFile(f, "", force, &did, &info, &err))
            report.items.push_back({rel, did ? "converted: " + std::to_string(info.meshes) + " meshes, " + std::to_string(info.clips) + " clips" : "up to date", true});
        else
            report.items.push_back({rel, err, false});
        report.warnings.insert(report.warnings.end(), info.warnings.begin(), info.warnings.end());
    }
    return true;
}

bool applyModelConversion(pack::PackWriter& w, ConvertReport& report, std::string* error) {
    const std::vector<std::string> paths = w.paths();
    const std::set<std::string> have(paths.begin(), paths.end());
    std::vector<std::string> drop;
    for (const auto& p : paths) {
        if (!needsConversion(p)) continue;
        drop.push_back(p);
        const std::string side = sidecarPath(p);
        if (have.count(side)) { report.items.push_back({p, "uses " + side, true}); continue; }
        ConvertInfo info;
        std::string glb, err;
        auto read = [&w](const std::string& path, std::string& out) { return w.readInput(path, out); };
        if (!convertModel(p, read, glb, &info, &err)) {
            report.items.push_back({p, err, false});
            continue;
        }
        if (!w.addData(side, std::move(glb), error)) return false;
        report.items.push_back({p, "converted to " + side, true});
        report.warnings.insert(report.warnings.end(), info.warnings.begin(), info.warnings.end());
    }
    w.remove(drop);
    return true;
}

} // namespace rts::models
