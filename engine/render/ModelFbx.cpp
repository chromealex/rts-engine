// FBX loader (binary and ASCII, FBX 6.x / 7.x) on top of ufbx (third_party/ufbx).
//
// ufbx converts the scene to the engine conventions while loading: Y up, right-handed, metres.
// The unit scale is applied to geometry and translations (UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY),
// so node scales stay clean; geometric transforms (pivots that only move the mesh) are baked into
// vertices, non-standard scale inheritance is compensated with helper nodes when needed.
//
// Supported: node hierarchy (local TRS), polygon meshes (triangulated), normals (generated when
// missing), UV0, vertex colours, tangents, one primitive per material, PBR material maps (base
// colour, roughness, metalness, normal, emission, opacity), external and embedded textures,
// skin deformers (4 strongest influences per vertex, split into kMaxPaletteJoints palettes),
// animation stacks baked to linear TRS channels. Not supported (warning, skipped): blend shapes,
// vertex caches, extra UV / colour sets, more than 4 influences, cameras, lights.
#include "Model.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"   // anonymous structs in unions
#endif
#include "../../third_party/ufbx/ufbx.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace rts::render {

namespace {

std::string dirOf(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}
std::string str(const ufbx_string& s) { return std::string(s.data, s.length); }
std::string slashes(std::string s) { std::replace(s.begin(), s.end(), '\\', '/'); return s; }
std::string baseName(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

float3 f3(const ufbx_vec3& v) { return {float(v.x), float(v.y), float(v.z)}; }
quat fq(const ufbx_quat& q) { return qnormalize(quat{float(q.x), float(q.y), float(q.z), float(q.w)}); }
mat4 fm(const ufbx_matrix& m) {
    mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 3; ++row) r.at(row, c) = float(m.cols[c].v[row]);
    return r;
}

struct Scene {   // owns the ufbx scene, frees it on every exit path
    ufbx_scene* s = nullptr;
    ~Scene() { if (s) ufbx_free_scene(s); }
};
struct Baked {
    ufbx_baked_anim* a = nullptr;
    ~Baked() { if (a) ufbx_free_baked_anim(a); }
};

// Vertex before palette assignment: global skin joint indices and float weights.
struct RawVertex {
    ModelVertex v;
    int joints[4] = {0, 0, 0, 0};
    float weights[4] = {0, 0, 0, 0};
};

class FbxReader {
public:
    FbxReader(const std::string& path, const FileReader& read, std::vector<std::string>* warnings)
        : path_(path), dir_(dirOf(path)), read_(read), warnings_(warnings) {}

    std::unique_ptr<Model> load(const std::string& bytes) {
        ufbx_load_opts opts{};
        opts.file_format = UFBX_FILE_FORMAT_FBX;
        opts.target_axes = ufbx_axes_right_handed_y_up;
        opts.target_unit_meters = 1;
        opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
        opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
        opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
        opts.generate_missing_normals = true;
        opts.normalize_normals = true;
        opts.ignore_missing_external_files = true;
        ufbx_error err{};
        Scene sc;
        sc.s = ufbx_load_memory(bytes.data(), bytes.size(), &opts, &err);
        if (!sc.s) {
            std::string msg = "FBX parse error: " + str(err.description);
            if (err.info_length) msg += " (" + std::string(err.info, err.info_length) + ")";
            fail(msg);
        }
        scene_ = sc.s;
        model_ = std::make_unique<Model>();
        loadNodes();
        loadMaterials();
        loadSkins();
        loadMeshes();
        loadAnimations();
        model_->finalize();
        return std::move(model_);
    }

private:
    [[noreturn]] void fail(const std::string& msg) const { throw std::runtime_error(path_ + ": " + msg); }
    void warn(const std::string& msg) {
        if (warnings_ && warned_.insert(msg).second) warnings_->push_back(path_ + ": " + msg);
    }

    // ufbx's implicit root node is dropped: its children become model roots.
    void loadNodes() {
        nodeMap_.assign(scene_->nodes.count, -1);
        for (size_t i = 0; i < scene_->nodes.count; ++i) {
            const ufbx_node* n = scene_->nodes.data[i];
            if (n->is_root) continue;
            nodeMap_[i] = int(model_->nodes.size());
            Node nd;
            nd.name = str(n->name);
            if (nd.name.empty()) nd.name = "node" + std::to_string(i);
            nd.t = f3(n->local_transform.translation);
            nd.r = fq(n->local_transform.rotation);
            nd.s = f3(n->local_transform.scale);
            model_->nodes.push_back(nd);
            if (n->light || n->camera) warn("cameras and lights are ignored");
        }
        for (size_t i = 0; i < scene_->nodes.count; ++i) {
            const ufbx_node* n = scene_->nodes.data[i];
            if (nodeMap_[i] < 0) continue;
            if (n->parent && !n->parent->is_root) model_->nodes[size_t(nodeMap_[i])].parent = nodeMap_[n->parent->typed_id];
        }
    }

    // ---- textures and materials ----

    int texture(const ufbx_texture* t, const std::string& where) {
        if (!t) return -1;
        if (t->type != UFBX_TEXTURE_FILE) {
            if (t->file_textures.count == 0) { warn(where + ": procedural / shader texture ignored"); return -1; }
            if (t->type == UFBX_TEXTURE_LAYERED && t->file_textures.count > 1) warn(where + ": layered texture, only the first layer is used");
            t = t->file_textures.data[0];
        }
        if (auto it = texCache_.find(t->element_id); it != texCache_.end()) return it->second;
        int& slot = texCache_[t->element_id];
        slot = -1;
        Image img;
        const std::string rel = slashes(str(t->relative_filename));
        img.name = !rel.empty() ? rel : !str(t->filename).empty() ? baseName(slashes(str(t->filename))) : "#" + std::to_string(t->typed_id);
        std::string bytes;
        if (t->content.size > 0) {
            bytes.assign(static_cast<const char*>(t->content.data), t->content.size);
        } else {
            // the relative path first, then the bare file name next to the .fbx (assets are often moved)
            std::vector<std::string> tries;
            if (!rel.empty()) tries.push_back(dir_ + rel);
            for (const ufbx_string* s : {&t->filename, &t->absolute_filename, &t->relative_filename})
                if (s->length) tries.push_back(dir_ + baseName(slashes(str(*s))));
            bool found = false;
            for (const auto& p : tries) if ((found = read_(p, bytes))) break;
            if (!found) { warn("texture '" + img.name + "' not found next to the model"); return -1; }
        }
        if (!decodeImage(bytes, img)) { warn("texture '" + img.name + "': unsupported format (PNG, JPEG, TGA only)"); return -1; }
        img.repeat = t->wrap_u == UFBX_WRAP_REPEAT || t->wrap_v == UFBX_WRAP_REPEAT;
        slot = int(model_->images.size());
        model_->images.push_back(std::move(img));
        return slot;
    }

    void loadMaterials() {
        for (size_t i = 0; i < scene_->materials.count; ++i) {
            const ufbx_material* fm = scene_->materials.data[i];
            const ufbx_material_pbr_maps& pbr = fm->pbr;
            Material m;
            m.name = str(fm->name);
            if (m.name.empty()) m.name = "material" + std::to_string(i);
            const std::string where = "material '" + m.name + "'";
            const float bf = pbr.base_factor.has_value ? float(pbr.base_factor.value_real) : 1.0f;
            if (pbr.base_color.has_value || pbr.base_color.value_components >= 3)
                m.baseColor = {float(pbr.base_color.value_vec4.x) * bf, float(pbr.base_color.value_vec4.y) * bf, float(pbr.base_color.value_vec4.z) * bf, 1};
            m.baseTex = texture(pbr.base_color.texture, where);
            if (m.baseTex >= 0 && !pbr.base_color.has_value) m.baseColor = {1, 1, 1, 1};
            if (const ufbx_texture* bt = pbr.base_color.texture; bt && bt->has_uv_transform) {
                const ufbx_matrix& u = bt->uv_to_texture;
                m.uvScale = {float(std::sqrt(u.m00 * u.m00 + u.m10 * u.m10)), float(std::sqrt(u.m01 * u.m01 + u.m11 * u.m11))};
                m.uvOffset = {float(u.m03), float(u.m13)};
                if (std::fabs(u.m10) > 1e-6 || std::fabs(u.m01) > 1e-6) warn(where + ": texture rotation is ignored");
            }
            // classic Lambert / Phong materials have no roughness / metalness: keep the engine defaults
            if (pbr.roughness.has_value) {
                float rough = float(pbr.roughness.value_real);
                if (fm->features.roughness_as_glossiness.enabled) rough = 1.0f - rough;
                m.roughness = std::clamp(rough, 0.0f, 1.0f);
            }
            if (pbr.metalness.has_value) m.metallic = std::clamp(float(pbr.metalness.value_real), 0.0f, 1.0f);
            if (pbr.roughness.texture || pbr.metalness.texture) warn(where + ": roughness / metalness textures are approximated by factors");
            m.normalTex = texture(pbr.normal_map.texture, where);
            const float ef = pbr.emission_factor.has_value ? float(pbr.emission_factor.value_real) : 1.0f;
            m.emissive = f3(pbr.emission_color.value_vec3) * ef;
            m.emissiveTex = texture(pbr.emission_color.texture, where);
            m.occlusionTex = texture(pbr.ambient_occlusion.texture, where);
            float opacity = 1;
            if (pbr.opacity.has_value) opacity = float(pbr.opacity.value_real);
            else if (fm->fbx.transparency_factor.has_value || fm->fbx.transparency_color.has_value) {
                // classic FBX: opacity = 1 - TransparencyFactor * TransparentColor (averaged)
                const ufbx_vec3& c = fm->fbx.transparency_color.value_vec3;
                const double avg = fm->fbx.transparency_color.has_value ? (c.x + c.y + c.z) / 3.0 : 1.0;
                const double f = fm->fbx.transparency_factor.has_value ? fm->fbx.transparency_factor.value_real : 1.0;
                opacity = float(1.0 - f * avg);
            }
            if (opacity < 0.999f) { m.alpha = AlphaMode::Blend; m.baseColor.w = std::clamp(opacity, 0.0f, 1.0f); }
            if (pbr.opacity.texture) {
                if (pbr.opacity.texture == pbr.base_color.texture) m.alpha = AlphaMode::Blend;
                else warn(where + ": separate opacity texture ignored (put alpha into the base colour texture)");
            }
            m.doubleSided = fm->features.double_sided.enabled;
            if (fm->features.unlit.enabled) m.shading = ShadingModel::Unlit;
            model_->materials.push_back(m);
        }
    }

    // ---- skins ----

    void loadSkins() {
        for (size_t i = 0; i < scene_->skin_deformers.count; ++i) {
            const ufbx_skin_deformer* d = scene_->skin_deformers.data[i];
            Skin s;
            s.name = str(d->name);
            if (s.name.empty()) s.name = "skin" + std::to_string(i);
            for (size_t c = 0; c < d->clusters.count; ++c) {
                const ufbx_skin_cluster* cl = d->clusters.data[c];
                int node = cl->bone_node && !cl->bone_node->is_root ? nodeMap_[cl->bone_node->typed_id] : -1;
                if (node < 0) { warn("skin '" + s.name + "': cluster without a bone node bound to the first node"); node = 0; }
                s.joints.push_back(node);
                s.inverseBind.push_back(fm(cl->geometry_to_bone));
            }
            if (d->skinning_method == UFBX_SKINNING_METHOD_DUAL_QUATERNION || d->skinning_method == UFBX_SKINNING_METHOD_BLENDED_DQ_LINEAR)
                warn("skin '" + s.name + "': dual quaternion skinning approximated by linear skinning");
            model_->skins.push_back(std::move(s));
        }
    }

    // ---- meshes ----

    void loadMeshes() {
        for (size_t mi = 0; mi < scene_->meshes.count; ++mi) {
            const ufbx_mesh* mesh = scene_->meshes.data[mi];
            MeshDef md;
            md.name = str(mesh->name);
            if (md.name.empty() && mesh->instances.count) md.name = str(mesh->instances.data[0]->name);
            if (md.name.empty()) md.name = "mesh" + std::to_string(mi);
            const ufbx_skin_deformer* skin = mesh->skin_deformers.count ? mesh->skin_deformers.data[0] : nullptr;
            int firstNode = -1;
            for (size_t k = 0; k < mesh->instances.count && firstNode < 0; ++k)
                if (!mesh->instances.data[k]->is_root) firstNode = nodeMap_[mesh->instances.data[k]->typed_id];
            loadMesh(*mesh, md, skin, firstNode);
            const int meshIndex = int(model_->meshes.size());
            for (size_t k = 0; k < mesh->instances.count; ++k) {
                const ufbx_node* n = mesh->instances.data[k];
                int ni = n->is_root ? -1 : nodeMap_[n->typed_id];
                if (ni < 0) continue;
                model_->nodes[size_t(ni)].mesh = meshIndex;
                if (skin) model_->nodes[size_t(ni)].skin = int(skin->typed_id);
            }
            model_->meshes.push_back(std::move(md));
        }
    }

    void loadMesh(const ufbx_mesh& mesh, MeshDef& md, const ufbx_skin_deformer* skin, int meshNode) {
        meshNode_ = meshNode;
        rigidJoint_ = -1;
        const std::string where = "mesh '" + md.name + "'";
        if (mesh.blend_deformers.count) warn(where + ": blend shapes are ignored");
        if (mesh.cache_deformers.count) warn(where + ": vertex caches are ignored");
        if (mesh.uv_sets.count > 1) warn(where + ": only the first UV set is used");
        if (mesh.color_sets.count > 1) warn(where + ": only the first vertex colour set is used");
        if (mesh.skin_deformers.count > 1) warn(where + ": only the first skin deformer is used");
        if (skin && skin->max_weights_per_vertex > 4) warn(where + ": more than 4 joint influences per vertex; the 4 strongest are kept");
        const bool hasTangents = mesh.vertex_tangent.exists && mesh.vertex_bitangent.exists;
        std::vector<uint32_t> tri(mesh.max_face_triangles * 3);
        for (size_t pi = 0; pi < mesh.material_parts.count; ++pi) {
            const ufbx_mesh_part& part = mesh.material_parts.data[pi];
            if (part.num_triangles == 0) continue;
            Primitive prim;
            prim.material = pi < mesh.materials.count && mesh.materials.data[pi] ? int(mesh.materials.data[pi]->typed_id) : defaultMaterial();
            // unique (index -> vertex) corners, deduplicated by content so shared corners stay shared
            std::vector<RawVertex> verts;
            std::vector<uint32_t> idx;
            std::unordered_map<std::string, uint32_t> dedup;
            for (size_t fi = 0; fi < part.face_indices.count; ++fi) {
                const ufbx_face face = mesh.faces.data[part.face_indices.data[fi]];
                const uint32_t n = ufbx_triangulate_face(tri.data(), tri.size(), &mesh, face);
                for (uint32_t c = 0; c < n * 3; ++c) {
                    RawVertex rv = corner(mesh, tri[c], skin, hasTangents);
                    std::string key(reinterpret_cast<const char*>(&rv), sizeof rv);
                    auto [it, inserted] = dedup.emplace(std::move(key), uint32_t(verts.size()));
                    if (inserted) verts.push_back(rv);
                    idx.push_back(it->second);
                }
            }
            const bool needTangents = !hasTangents && model_->materials[size_t(prim.material)].normalTex >= 0;
            if (!skin) { appendChunk(md, prim, verts, idx, false, needTangents); continue; }
            // split into chunks whose joints fit one palette, the same way as the glTF loader
            std::vector<uint32_t> chunk;
            std::set<int> used;
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                std::set<int> merged = used;
                for (int k = 0; k < 3; ++k) {
                    const RawVertex& v = verts[idx[t + size_t(k)]];
                    for (int e = 0; e < 4; ++e) if (v.weights[e] > 0 || e == 0) merged.insert(v.joints[e]);
                }
                if (int(merged.size()) > kMaxPaletteJoints && !chunk.empty()) {
                    appendChunk(md, prim, verts, chunk, true, needTangents);
                    chunk.clear();
                    merged.clear();
                    for (int k = 0; k < 3; ++k) {
                        const RawVertex& v = verts[idx[t + size_t(k)]];
                        for (int e = 0; e < 4; ++e) if (v.weights[e] > 0 || e == 0) merged.insert(v.joints[e]);
                    }
                }
                used = std::move(merged);
                chunk.insert(chunk.end(), {idx[t], idx[t + 1], idx[t + 2]});
            }
            if (!chunk.empty()) appendChunk(md, prim, verts, chunk, true, needTangents);
        }
    }

    RawVertex corner(const ufbx_mesh& mesh, uint32_t ix, const ufbx_skin_deformer* skin, bool hasTangents) {
        RawVertex rv;   // value-initialized: padding is zero, so the dedup key is stable
        std::memset(static_cast<void*>(&rv), 0, sizeof rv);
        ModelVertex& v = rv.v;
        v = ModelVertex{};
        v.pos = f3(ufbx_get_vertex_vec3(&mesh.vertex_position, ix));
        if (mesh.vertex_normal.exists) v.nrm = normalize(f3(ufbx_get_vertex_vec3(&mesh.vertex_normal, ix)));
        if (mesh.vertex_uv.exists) {
            const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh.vertex_uv, ix);
            v.uv = {float(uv.x), 1.0f - float(uv.y)};   // FBX UVs have the origin at the bottom left
        }
        if (hasTangents) {
            const float3 t = normalize(f3(ufbx_get_vertex_vec3(&mesh.vertex_tangent, ix)));
            const float3 b = f3(ufbx_get_vertex_vec3(&mesh.vertex_bitangent, ix));
            // the V flip above mirrors the tangent frame
            v.tangent = {t.x, t.y, t.z, dot(cross(v.nrm, t), b) < 0 ? 1.0f : -1.0f};
        }
        if (mesh.vertex_color.exists) {
            const ufbx_vec4 c = ufbx_get_vertex_vec4(&mesh.vertex_color, ix);
            auto q = [](double x) { return uint32_t(std::lround(std::clamp(x, 0.0, 1.0) * 255.0)); };
            v.color = q(c.x) | (q(c.y) << 8) | (q(c.z) << 16) | (q(c.w) << 24);
        }
        if (skin) {
            const uint32_t vert = mesh.vertex_indices.data[ix];
            const uint32_t nw = vert < skin->vertices.count ? skin->vertices.data[vert].num_weights : 0;
            if (nw == 0 && meshNode_ >= 0) {
                // unweighted vertices of a skinned mesh stay rigid with the mesh node (as in the FBX SDK):
                // an extra joint = mesh node with identity inverse bind
                if (rigidJoint_ < 0) {
                    if (mesh.instances.count > 1) warn("mesh '" + str(mesh.name) + "': instanced skinned mesh, unweighted vertices follow the first instance");
                    Skin& sk = model_->skins[skin->typed_id];
                    rigidJoint_ = int(sk.joints.size());
                    sk.joints.push_back(meshNode_);
                    sk.inverseBind.push_back(mat4{});
                }
                rv.joints[0] = rigidJoint_;
                rv.weights[0] = 1;
            } else if (nw > 0) {
                const ufbx_skin_vertex& sv = skin->vertices.data[vert];
                const uint32_t n = std::min<uint32_t>(sv.num_weights, 4);   // sorted by decreasing weight
                for (uint32_t k = 0; k < n; ++k) {
                    const ufbx_skin_weight& w = skin->weights.data[sv.weight_begin + k];
                    rv.joints[k] = int(w.cluster_index);
                    rv.weights[k] = std::max(0.0f, float(w.weight));
                }
            }
        }
        return rv;
    }

    void appendChunk(MeshDef& md, Primitive prim, const std::vector<RawVertex>& verts, const std::vector<uint32_t>& idx, bool skinned, bool needTangents) {
        Model& m = *model_;
        std::vector<int32_t> remap(verts.size(), -1);
        std::map<int, int> palette;
        const uint32_t base = uint32_t(m.vertices.size());
        prim.firstIndex = uint32_t(m.indices.size());
        prim.indexCount = uint32_t(idx.size());
        for (uint32_t i : idx) {
            if (remap[i] < 0) {
                remap[i] = int32_t(m.vertices.size() - base);
                const RawVertex& rv = verts[i];
                ModelVertex v = rv.v;
                if (skinned) {
                    float w[4], sum = 0;
                    for (int e = 0; e < 4; ++e) { w[e] = rv.weights[e]; sum += w[e]; }
                    if (sum <= 0) { w[0] = sum = 1; }   // unweighted vertex: follow its first joint
                    for (int e = 0; e < 4; ++e) {
                        if (w[e] <= 0) { v.joints[e] = 0; v.weights[e] = 0; continue; }
                        auto it = palette.find(rv.joints[e]);
                        if (it == palette.end()) it = palette.emplace(rv.joints[e], int(palette.size())).first;
                        v.joints[e] = uint8_t(it->second);
                        v.weights[e] = uint8_t(std::lround(w[e] / sum * 255.0f));
                    }
                }
                m.vertices.push_back(v);
            }
            m.indices.push_back(base + uint32_t(remap[i]));
        }
        if (skinned) {
            prim.jointPalette.resize(palette.size());
            for (auto& [j, slot] : palette) prim.jointPalette[size_t(slot)] = uint16_t(j);
            if (prim.jointPalette.empty()) prim.jointPalette.push_back(0);
        }
        if (needTangents) generateTangents(m.vertices, m.indices, prim.firstIndex, prim.indexCount);
        md.primitives.push_back(std::move(prim));
    }

    int defaultMaterial() {
        if (defaultMat_ < 0) {
            Material m; m.name = "default";
            defaultMat_ = int(model_->materials.size());
            model_->materials.push_back(m);
        }
        return defaultMat_;
    }

    // ---- animation ----

    void loadAnimations() {
        for (size_t si = 0; si < scene_->anim_stacks.count; ++si) {
            const ufbx_anim_stack* st = scene_->anim_stacks.data[si];
            AnimClip clip;
            clip.name = str(st->name);
            if (clip.name.empty()) clip.name = "animation" + std::to_string(si);
            ufbx_bake_opts bo{};
            bo.trim_start_time = true;
            bo.resample_rate = 30;
            bo.key_reduction_enabled = true;
            bo.key_reduction_rotation = true;
            ufbx_error err{};
            Baked baked;
            baked.a = ufbx_bake_anim(scene_, st->anim, &bo, &err);
            if (!baked.a) { warn("animation '" + clip.name + "': " + str(err.description) + "; skipped"); continue; }
            for (size_t k = 0; k < baked.a->nodes.count; ++k) {
                const ufbx_baked_node& bn = baked.a->nodes.data[k];
                if (bn.typed_id >= nodeMap_.size() || nodeMap_[bn.typed_id] < 0) continue;
                const int node = nodeMap_[bn.typed_id];
                const Node& nd = model_->nodes[size_t(node)];
                addVec3(clip, node, AnimChannel::Translation, bn.translation_keys, bn.constant_translation, nd.t);
                addRot(clip, node, bn.rotation_keys, bn.constant_rotation, nd.r);
                addVec3(clip, node, AnimChannel::Scale, bn.scale_keys, bn.constant_scale, nd.s);
            }
            clip.duration = std::max(clip.duration, float(baked.a->playback_duration));
            for (size_t e = 0; e < baked.a->elements.count; ++e)
                for (size_t p = 0; p < baked.a->elements.data[e].props.count; ++p)
                    if (!baked.a->elements.data[e].props.data[p].constant_value)
                        warn("animation '" + clip.name + "': property animation (" + str(baked.a->elements.data[e].props.data[p].name) + ") is ignored");
            if (!clip.channels.empty()) model_->clips.push_back(std::move(clip));
        }
    }

    // Constant channels that match the rest pose are dropped (exporters key every node).
    static void addVec3(AnimClip& clip, int node, AnimChannel::Path path, const ufbx_baked_vec3_list& keys, bool constant, float3 rest) {
        if (keys.count == 0) return;
        if (constant && length(f3(keys.data[0].value) - rest) < 1e-5f) return;
        AnimChannel c;
        c.node = node; c.path = path; c.interp = AnimChannel::Linear;
        for (size_t i = 0; i < keys.count; ++i) {
            c.times.push_back(float(keys.data[i].time));
            const float3 v = f3(keys.data[i].value);
            c.values.insert(c.values.end(), {v.x, v.y, v.z});
        }
        clip.duration = std::max(clip.duration, c.times.back());
        clip.channels.push_back(std::move(c));
    }
    static void addRot(AnimClip& clip, int node, const ufbx_baked_quat_list& keys, bool constant, quat rest) {
        if (keys.count == 0) return;
        if (constant && std::fabs(dot(fq(keys.data[0].value), rest)) > 1 - 1e-6f) return;
        AnimChannel c;
        c.node = node; c.path = AnimChannel::Rotation; c.interp = AnimChannel::Linear;
        quat prev{};
        for (size_t i = 0; i < keys.count; ++i) {
            c.times.push_back(float(keys.data[i].time));
            quat q = fq(keys.data[i].value);
            if (i > 0 && dot(prev, q) < 0) q = {-q.x, -q.y, -q.z, -q.w};   // shortest path between keys
            prev = q;
            c.values.insert(c.values.end(), {q.x, q.y, q.z, q.w});
        }
        clip.duration = std::max(clip.duration, c.times.back());
        clip.channels.push_back(std::move(c));
    }

    std::string path_, dir_;
    const FileReader& read_;
    std::vector<std::string>* warnings_;
    std::set<std::string> warned_;
    const ufbx_scene* scene_ = nullptr;
    std::unique_ptr<Model> model_;
    std::vector<int> nodeMap_;               // ufbx node typed_id -> model node (-1 = root)
    std::map<uint32_t, int> texCache_;       // ufbx texture element_id -> image index
    int defaultMat_ = -1;
    int meshNode_ = -1, rigidJoint_ = -1;   // current mesh: its node, extra joint for unweighted vertices
};

} // namespace

std::unique_ptr<Model> loadFbx(const std::string& path, const std::string& bytes, const FileReader& read, std::vector<std::string>* warnings) {
    FbxReader r(path, read, warnings);
    return r.load(bytes);
}

} // namespace rts::render
