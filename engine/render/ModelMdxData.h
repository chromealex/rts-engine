// Warcraft III models: the intermediate structure shared by the .mdx (binary, ModelMdx.cpp) and
// .mdl (text, ModelMdl.cpp) readers. Internal to the loaders; values are in Warcraft III axes.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "Math3D.h"

namespace rts::render::mdx {

// ---------------------------------------------------------------- intermediate structure

struct Track {
    int interp = 1;              // 0 DontInterp, 1 Linear, 2 Hermite, 3 Bezier
    int globalSeq = -1;
    int comps = 3;
    std::vector<int32_t> frames;
    std::vector<float> values, inTans, outTans;   // comps per key
};
struct Sequence { std::string name; int32_t start = 0, end = 0; bool nonLooping = false; };
struct Texture { std::string file; uint32_t replaceable = 0, flags = 0; };
struct Layer {
    uint32_t filter = 0, shading = 0;
    int32_t texture = -1;
    float alpha = 1;
    std::vector<int32_t> textures;   // v1100: diffuse, normal, ORM, emissive, team colour, reflections
};
struct Mat { std::string shader; std::vector<Layer> layers; };
struct Geoset {
    std::vector<float3> pos, nrm;
    std::vector<float2> uv;
    std::vector<float4> tangents;
    std::vector<uint32_t> tris;
    std::vector<uint32_t> vgroup, groupSizes, groupIndices;
    std::vector<uint8_t> skin;   // 4 bones + 4 weights per vertex
    int32_t material = 0, lod = 0;
};
struct GeosetAnim { float alpha = 1; int32_t geoset = -1; Track alphaTrack; };
struct Object {
    enum Kind { Bone, Helper, Attachment, Other } kind = Other;
    std::string name;
    int32_t id = -1, parent = -1;
    uint32_t flags = 0;
    Track t, r, s;
};
struct Mdlx {
    uint32_t version = 800;
    std::vector<Sequence> seqs;
    std::vector<int32_t> globals;
    std::vector<Texture> textures;
    std::vector<Mat> materials;
    std::vector<Geoset> geosets;
    std::vector<GeosetAnim> geoAnims;
    std::vector<Object> objects;
    std::vector<float3> pivots;
    bool emitters = false;
    bool faceTypesSkipped = false;
};

// Polygon groups -> triangles (4 = triangles, 5 = strip, 6 = fan; others are not used by the game).
inline void appendFaces(Geoset& g, uint32_t type, const uint16_t* idx, size_t n, bool& skipped) {
    if (type == 4) for (size_t i = 0; i + 2 < n; i += 3) g.tris.insert(g.tris.end(), {idx[i], idx[i + 1], idx[i + 2]});
    else if (type == 5) for (size_t i = 0; i + 2 < n; ++i) {
        if (i & 1) g.tris.insert(g.tris.end(), {idx[i + 1], idx[i], idx[i + 2]});
        else g.tris.insert(g.tris.end(), {idx[i], idx[i + 1], idx[i + 2]});
    }
    else if (type == 6) for (size_t i = 1; i + 1 < n; ++i) g.tris.insert(g.tris.end(), {idx[0], idx[i], idx[i + 1]});
    else skipped = true;
}

// Parses .mdl text; throws std::runtime_error("path:line: what") on broken files.
Mdlx parseMdl(const std::string& path, const std::string& text);

} // namespace rts::render::mdx
