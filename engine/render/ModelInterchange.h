// Internal helpers shared by the interchange loaders (ModelPly.cpp, Model3ds.cpp, ModelDae.cpp):
// texture lookup next to the model, Phong -> PBR conversion, normals, colour packing, bounds-checked
// binary reading. Not a public API.
#pragma once
#include "Model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace rts::render::interchange {

inline std::string dirOf(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}
inline std::string baseName(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}
inline std::string lowerStr(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline std::string upperStr(std::string s) {
    for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

inline uint32_t packColor(float r, float g, float b, float a = 1) {
    auto q = [](float x) { return uint32_t(std::lround(std::clamp(std::isfinite(x) ? x : 0.0f, 0.0f, 1.0f) * 255.0f)); };
    return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
}

// Blinn-Phong specular exponent -> roughness (same mapping as the OBJ loader).
inline float shininessToRoughness(float ns) { return std::clamp(std::sqrt(2.0f / (std::max(ns, 0.0f) + 2.0f)), 0.04f, 1.0f); }

// Area-weighted smooth normals for vertices [first, end) referenced by indices [i0, i1).
inline void smoothNormals(std::vector<ModelVertex>& v, const std::vector<uint32_t>& idx, size_t i0, size_t i1) {
    for (size_t i = i0; i < i1; ++i) v[idx[i]].nrm = {0, 0, 0};
    for (size_t i = i0; i + 2 < i1; i += 3) {
        ModelVertex &a = v[idx[i]], &b = v[idx[i + 1]], &c = v[idx[i + 2]];
        const float3 n = cross(b.pos - a.pos, c.pos - a.pos);   // length = 2 * area
        a.nrm += n; b.nrm += n; c.nrm += n;
    }
    for (size_t i = i0; i < i1; ++i) v[idx[i]].nrm = normalize(v[idx[i]].nrm);
}

inline float3 faceNormal(float3 a, float3 b, float3 c) { return normalize(cross(b - a, c - a)); }

// Z up (3DS, STL, PLY, Collada Z_UP) -> Y up, right-handed: (x, y, z) -> (x, z, -y).
inline float3 zUpToYUp(float3 p) { return {p.x, p.z, -p.y}; }

// Loads textures referenced by name relative to the model; tries the name as is, its base name, then
// lower / upper case (8.3 names of old exporters). Cached; -1 when missing (with a warning).
struct TextureLoader {
    std::string path, dir;
    const FileReader& read;
    std::vector<std::string>* warnings;
    Model& m;
    std::map<std::string, int> cache;

    void warn(const std::string& w) const { if (warnings) warnings->push_back(path + ": " + w); }

    int load(std::string file) {
        std::replace(file.begin(), file.end(), '\\', '/');
        if (file.rfind("file://", 0) == 0) file = file.substr(7);
        if (file.empty()) return -1;
        if (auto it = cache.find(file); it != cache.end()) return it->second;
        int& slot = cache[file];
        slot = -1;
        std::vector<std::string> tries;
        const std::string base = baseName(file);
        auto add = [&](const std::string& f) { if (std::find(tries.begin(), tries.end(), f) == tries.end()) tries.push_back(f); };
        if (!file.empty() && file[0] != '/' && file.find(':') == std::string::npos) add(dir + file);
        for (const std::string& b : {base, lowerStr(base), upperStr(base)}) add(dir + b);
        // mixed case of 8.3 names ("IMAGE1.jpg" for "IMAGE1.JPG"): stem and extension separately
        if (size_t dot = base.find_last_of('.'); dot != std::string::npos) {
            const std::string st = base.substr(0, dot), ext = base.substr(dot);
            for (const std::string& s : {st, lowerStr(st), upperStr(st)})
                for (const std::string& e : {lowerStr(ext), upperStr(ext)}) add(dir + s + e);
        }
        std::string bytes;
        bool found = false;
        for (const auto& p : tries) if ((found = read(p, bytes))) break;
        if (!found) { warn("texture '" + file + "' not found next to the model"); return -1; }
        Image img;
        img.name = file;
        if (!decodeImage(bytes, img)) { warn("texture '" + file + "': unsupported format"); return -1; }
        slot = int(m.images.size());
        m.images.push_back(std::move(img));
        return slot;
    }
};

// Bounds-checked little / big endian reader over a byte string.
class ByteReader {
public:
    ByteReader(const std::string& path, const std::string& b, size_t begin = 0, size_t end = std::string::npos)
        : path_(path), b_(b), pos_(begin), end_(std::min(end, b.size())) {}
    size_t pos() const { return pos_; }
    size_t end() const { return end_; }
    size_t left() const { return pos_ < end_ ? end_ - pos_ : 0; }
    void seek(size_t p) { if (p > end_) fail("offset past the end of the data"); pos_ = p; }
    void skip(size_t n) { need(n); pos_ += n; }
    void need(size_t n) const { if (n > left()) fail("unexpected end of file"); }
    [[noreturn]] void fail(const std::string& what) const { throw std::runtime_error(path_ + ": " + what); }
    const char* data() const { return b_.data() + pos_; }
    template <class T> T le() {
        need(sizeof(T));
        T v;
        std::memcpy(&v, b_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    template <class T> T be() {
        need(sizeof(T));
        unsigned char tmp[sizeof(T)];
        for (size_t i = 0; i < sizeof(T); ++i) tmp[i] = static_cast<unsigned char>(b_[pos_ + sizeof(T) - 1 - i]);
        T v;
        std::memcpy(&v, tmp, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    uint8_t u8() { return le<uint8_t>(); }
    uint16_t u16() { return le<uint16_t>(); }
    uint32_t u32() { return le<uint32_t>(); }
    float f32() { return le<float>(); }
    std::string cstr() {   // zero-terminated
        size_t e = pos_;
        while (e < end_ && b_[e] != '\0') ++e;
        if (e >= end_) fail("unterminated string");
        std::string s = b_.substr(pos_, e - pos_);
        pos_ = e + 1;
        return s;
    }

private:
    const std::string& path_;
    const std::string& b_;
    size_t pos_, end_;
};

} // namespace rts::render::interchange
