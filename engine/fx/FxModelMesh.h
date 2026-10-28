// Mesh particles / "mesh" shapes from model files: connects ParticleSystem::setMeshLoader to the engine's
// model loader (FBX / OBJ / glTF, render::loadModel). Header only: the apps that link the model library use it.
#pragma once
#include <exception>
#include <unordered_map>
#include "../render/Model.h"
#include "ParticleSystem.h"

namespace rts::fx {

// Every mesh of the model in its bind pose, in model space (Y up, right-handed, metres).
inline bool meshFromModelFile(const std::string& path, const render::FileReader& read, FxMesh& out) {
    std::unique_ptr<render::Model> m;
    try { m = render::loadModel(path, read); } catch (const std::exception&) { return false; }
    if (!m) return false;
    out = {};
    for (size_t n = 0; n < m->nodes.size(); ++n) {
        const auto& node = m->nodes[n];
        if (node.mesh < 0 || size_t(node.mesh) >= m->meshes.size()) continue;
        const render::mat4& xf = n < m->bindWorld.size() ? m->bindWorld[n] : render::mat4{};
        for (const auto& prim : m->meshes[size_t(node.mesh)].primitives) {
            // indices address the model's shared vertex list (vertex ranges are not filled by every loader)
            std::unordered_map<uint32_t, uint32_t> remap;
            for (uint32_t i = prim.firstIndex; i < prim.firstIndex + prim.indexCount && i < m->indices.size(); ++i) {
                const uint32_t v = m->indices[i];
                if (v >= m->vertices.size()) continue;
                auto it = remap.find(v);
                if (it == remap.end()) {
                    const auto& mv = m->vertices[v];
                    out.pos.push_back(xf.transformPoint(mv.pos));
                    const render::float4 nn = xf * render::float4{mv.nrm.x, mv.nrm.y, mv.nrm.z, 0};
                    out.nrm.push_back(render::normalize(render::float3{nn.x, nn.y, nn.z}));
                    out.uv.push_back(mv.uv);
                    it = remap.emplace(v, uint32_t(out.pos.size() - 1)).first;
                }
                out.idx.push_back(it->second);
            }
            out.idx.resize(out.idx.size() / 3 * 3);
        }
    }
    out.idx.resize(out.idx.size() / 3 * 3);
    return !out.pos.empty() && !out.idx.empty();
}

} // namespace rts::fx
