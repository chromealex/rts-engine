// "view" blocks of types: model loading (with shape fallback), placement fix-up, material
// overrides, clip mapping and procedural animation presets. Problems become warnings with the
// type id, never errors: a broken model must not stop a game from starting.
#include "RtsView.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>

namespace rts::game {

using namespace render;

namespace {
constexpr float kDeg = 3.14159265358979f / 180.0f;

float num(const Json& j, float def) { return j.isNumber() ? float(j.toDouble(def)) : def; }
float3 vec3(const Json& j, float3 def) {
    return j.size() >= 3 ? float3{num(j[0], def.x), num(j[1], def.y), num(j[2], def.z)} : def;
}
std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
float srgb(float c) { return c * c; } // the engine's ~sRGB -> linear convention (lit shader squares colours)

uint32_t stateBit(const std::string& n) {
    if (n == "idle") return kProcIdle;
    if (n == "moving" || n == "move") return kProcMoving;
    if (n == "attacking" || n == "attack") return kProcAttacking;
    if (n == "gathering" || n == "gather") return kProcGathering;
    if (n == "building" || n == "build") return kProcBuilding;
    if (n == "carrying" || n == "carry") return kProcCarrying;
    if (n == "constructing" || n == "construct") return kProcConstructing;
    if (n == "producing" || n == "produce" || n == "work") return kProcProducing;
    if (n == "dead" || n == "die") return kProcDead;
    if (n == "air" || n == "flying") return kProcAir;
    return 0;
}
int eventIndex(const std::string& n) {
    if (n == "attack") return kProcEvAttack;
    if (n == "hit") return kProcEvHit;
    if (n == "spawn") return kProcEvSpawn;
    if (n == "death" || n == "die") return kProcEvDeath;
    return kProcEvNone;
}

// clip-name candidates for automatic mapping when a model has no "animations" block
const char* const kAuto[RtsView::kAnimCount][6] = {
    {"idle", "stand", "breath", nullptr}, {"walk", "run", "move", "locomotion", nullptr},
    {"attack", "shoot", "fire", "slash", "punch", nullptr}, {"gather", "harvest", "mine", "chop", "work", nullptr},
    {"build", "hammer", "repair", "work", nullptr}, {"carry", nullptr},
    {"death", "die", "dead", nullptr}, {"spawn", "appear", "birth", nullptr},
    {"hit", "hurt", "damage", nullptr}, {"working", "produce", "work", nullptr},
    {"construct", nullptr}, {"cast", "spell", nullptr},
};
} // namespace

const char* RtsView::animName(int k) {
    static const char* names[kAnimCount] = {"idle", "move", "attack", "gather", "build", "carry", "die", "spawn", "hit", "work", "construct", "cast"};
    return k >= 0 && k < kAnimCount ? names[k] : "?";
}

void RtsView::warn(const UnitType& t, const std::string& msg) {
    std::string w = "type '" + t.id + "': " + msg;
    if (std::find(warnings_.begin(), warnings_.end(), w) != warnings_.end()) return;
    std::fprintf(stderr, "view: %s\n", w.c_str());
    warnings_.push_back(std::move(w));
}

const Model* RtsView::modelOf(uint16_t type) const {
    return type < looks_.size() && !looks_[type].models.empty() ? looks_[type].models[0].model : nullptr;
}

void RtsView::buildLook(gfx::Device& dev, const UnitType& t, TypeLook& L) {
    const Json& v = t.view;
    L.structure = t.structure;
    L.air = t.layer & kLayerAir;
    L.scale = num(v["scale"], 1);
    const Json& c = v["color"];
    L.tint = c.size() >= 3 ? float4{srgb(num(c[0], 1)), srgb(num(c[1], 1)), srgb(num(c[2], 1)), 1} : float4{1, 1, 1, 1};
    L.sx = t.structure ? float(t.footprintW) : 1.0f;
    L.sz = t.structure ? float(t.footprintH) : 1.0f;
    L.corpse = num(v["corpse"], t.structure ? 3.0f : 2.5f);
    L.turnSpeed = num(v["turnSpeed"], t.structure ? 0.0f : 10.0f);
    L.flyHeight = num(v["flyHeight"], 2.4f);
    L.unitSpeed = float(t.speed.toDouble());
    std::vector<std::string> paths;
    if (v["model"].isString()) paths.push_back(v["model"].str());
    for (const auto& p : v["model"].items()) if (p.isString()) paths.push_back(p.str());
    for (const auto& p : paths) {
        ModelChoice ch;
        if (loadChoice(dev, t, v, p, ch)) L.models.push_back(std::move(ch));
    }
    L.shape = L.models.empty();
    if (L.shape) {
        std::string shape = v.getStr("shape", t.structure ? (t.resourceNode >= 0 ? "mine" : "structure") : "generic");
        if (!models_.count("shape:" + shape)) {
            warn(t, "unknown shape '" + shape + "'");
            shape = t.structure ? "structure" : "generic";
        }
        ModelChoice ch;
        ch.model = models_["shape:" + shape].get();
        ch.materials = ch.model->materials;
        configureChoice(dev, t, v, ch, true);
        L.models.push_back(std::move(ch));
    }
    auto procedural = [&](ModelChoice& ch) {
        if (!v.has("procedural")) defaultProcedural(t, L, ch);
        else {
            parseProcedural(t, v["procedural"], ch);
            if (v.getBool("proceduralDefaults")) defaultProcedural(t, L, ch);
        }
    };
    for (auto& ch : L.models) {
        procedural(ch);
        configureExtras(dev, t, v, ch);
        finishChoice(t, ch);
    }
    // ---- LOD: farther models share the clips (by name) and nodes (by name) of models[0]
    if (!L.shape) {
        for (const auto& lj : v["lod"].items()) {
            ModelChoice lc;
            if (!loadChoice(dev, t, v, lj.getStr("model"), lc)) continue;
            lc.lodDistance = num(lj["distance"], 30);
            const Model& base = *L.models[0].model;
            for (const auto& c0 : base.clips) lc.clipMap.push_back(lc.model->findClip(c0.name));
            for (const auto& n : lc.model->nodes) lc.nodeMap.push_back(base.findNode(n.name));
            for (int k = 0; k < kAnimCount; ++k) { lc.clips[k] = L.models[0].clips[k]; lc.hasClip[k] = L.models[0].hasClip[k]; }
            procedural(lc);
            configureExtras(dev, t, v, lc);
            lc.attach.clear(); lc.fx.clear(); lc.legs.clear(); lc.additive.clear(); // far away: no sockets, effects, IK
            finishChoice(t, lc);
            L.lods.push_back(std::move(lc));
        }
        std::sort(L.lods.begin(), L.lods.end(), [](const ModelChoice& x, const ModelChoice& y) { return x.lodDistance < y.lodDistance; });
    }
    // ---- stages: construction progress or damage swap the model / material variant
    bool buildStages = false;
    for (const auto& sj : v["stages"].items()) {
        Stage st;
        st.build = sj.has("build") ? num(sj["build"], 0) : -1;
        st.hp = sj.has("hp") ? num(sj["hp"], 1) : -1;
        if (st.build < 0 && st.hp < 0) { warn(t, "stages: each stage needs \"build\" or \"hp\""); continue; }
        if (sj.has("model")) {
            if (!loadChoice(dev, t, v, sj.getStr("model"), st.choice)) continue;
            procedural(st.choice);
        } else {
            st.choice = L.models[0];
        }
        if (sj.has("variant")) {
            st.choice.variant = st.choice.model->findVariant(sj.getStr("variant"));
            if (st.choice.variant < 0) warn(t, "stages: variant '" + sj.getStr("variant") + "' not found in " + st.choice.model->path);
        }
        configureExtras(dev, t, v, st.choice);
        if (sj.has("fx")) configureExtras(dev, t, sj, st.choice);
        finishChoice(t, st.choice);
        buildStages |= st.build >= 0;
        L.stages.push_back(std::move(st));
    }
    const std::string cons = v.getStr("construction", buildStages ? "stages" : "grow");
    L.construction = cons == "dissolve" ? Construction::Dissolve : cons == "stages" ? Construction::Stages : Construction::Grow;
    if (cons != "grow" && cons != "dissolve" && cons != "stages") warn(t, "construction must be grow, dissolve or stages");
    const bool dis = v.getBool("dissolve", false);
    L.dissolveSpawn = v.getBool("dissolveSpawn", dis);
    L.dissolveDeath = v.getBool("dissolveDeath", dis);
}

static uint32_t parseWhen(const Json& when, const std::function<void(const std::string&)>& bad) {
    std::vector<std::string> states;
    if (when.isString()) states.push_back(when.str());
    for (const auto& w : when.items()) states.push_back(w.str());
    uint32_t bits = 0;
    for (const auto& st : states) {
        uint32_t b = stateBit(st);
        if (!b) bad(st);
        bits |= b;
    }
    return bits;
}

Model* RtsView::loadModelCached(gfx::Device& dev, const UnitType& t, const std::string& path) {
    auto it = models_.find(path);
    if (it == models_.end()) {
        std::unique_ptr<Model> m;
        try {
            std::vector<std::string> w;
            m = loadModel(path, read_, &w);
            for (auto& s : w) warn(t, s);
            uploadModel(dev, *m);
        } catch (const std::exception& e) {
            warn(t, e.what());
            m.reset();
        }
        it = models_.emplace(path, std::move(m)).first;
    }
    return it->second.get();
}

void RtsView::configureExtras(gfx::Device& dev, const UnitType& t, const Json& v, ModelChoice& c) {
    const Model& m = *c.model;
    auto bad = [&](const char* what) { return [&, what](const std::string& st) { warn(t, std::string(what) + ": unknown state '" + st + "'"); }; };
    auto node = [&](const Json& j, const char* what) {
        if (!j.isString()) return -1;
        int n = m.findNode(j.str());
        if (n < 0) warn(t, std::string(what) + ": node '" + j.str() + "' not found in " + m.path);
        return n;
    };
    if (v.has("attach")) c.attach.clear();
    for (const auto& aj : v["attach"].items()) { // sockets: weapons, shields, carried cargo, banners
        Attachment at;
        at.model = loadModelCached(dev, t, aj.getStr("model"));
        if (!at.model) continue;
        at.materials = at.model->materials;
        for (auto& mt : at.materials) if (lower(mt.name).find("team") != std::string::npos && mt.team == TeamMode::None) mt.team = TeamMode::Tint;
        at.node = node(aj["node"], "attach");
        const float3 r = vec3(aj["rotate"], {0, 0, 0}) * kDeg;
        const float sc = num(aj["scale"], 1);
        at.local = mat4::translate(vec3(aj["offset"], {0, 0, 0})) * mat4::rotateY(r.y) * mat4::rotateX(r.x) * mat4::rotateZ(r.z) * mat4::scale({sc, sc, sc});
        at.when = parseWhen(aj["when"], bad("attach"));
        if (aj.has("carry")) {
            std::string res = aj.getStr("carry");
            at.carry = res == "any" || res.empty() ? -1 : -3;
            for (size_t i = 0; i < rules_->resources.size(); ++i) if (rules_->resources[i].id == res) at.carry = int(i);
            if (at.carry == -3) { warn(t, "attach: unknown resource '" + res + "'"); at.carry = -1; }
        }
        c.attach.push_back(std::move(at));
    }
    if (v.has("muzzle")) {
        const Json& mz = v["muzzle"];
        c.muzzle = node(mz.isObject() ? mz["node"] : mz, "muzzle");
        c.muzzleOffset = vec3(mz["offset"], {0, 0, 0});
    }
    if (v.has("fx")) c.fx.clear();
    for (const auto& fj : v["fx"].items()) { // effect anchors for particles
        Fx f;
        f.effect = fj.getStr("effect");
        if (f.effect.empty()) { warn(t, "fx: missing \"effect\""); continue; }
        f.node = node(fj["node"], "fx");
        f.offset = vec3(fj["offset"], {0, 0, 0});
        f.when = parseWhen(fj["when"], bad("fx"));
        f.hpBelow = num(fj["hpBelow"], 2);
        f.intensity = num(fj["intensity"], 1);
        c.fx.push_back(f);
    }
    if (v.has("additive")) c.additive.clear();
    for (const auto& aj : v["additive"].items()) { // additive clips layered on any action
        AdditiveSpec a;
        a.clip = m.findClip(aj.getStr("clip"));
        if (a.clip < 0) { warn(t, "additive: clip '" + aj.getStr("clip") + "' not found in " + m.path); continue; }
        a.when = parseWhen(aj["when"], bad("additive"));
        a.weight = num(aj["weight"], 1);
        c.additive.push_back(a);
    }
    if (v.has("footIK")) c.legs.clear();
    for (const auto& lj : v["footIK"].items()) { // two-bone legs planted on the terrain
        LegIK l{node(lj["hip"], "footIK"), node(lj["knee"], "footIK"), node(lj["foot"], "footIK")};
        if (l.hip >= 0 && l.knee >= 0 && l.foot >= 0) c.legs.push_back(l);
    }
}

void RtsView::finishChoice(const UnitType& t, ModelChoice& c) {
    for (auto& p : c.procs) {
        p.resolve(c.model);
        if (!p.nodeName.empty() && p.node < 0) warn(t, "procedural: node '" + p.nodeName + "' not found in " + c.model->path);
    }
    bool skinned = false;
    for (const auto& n : c.model->nodes) skinned |= n.skin >= 0 && n.mesh >= 0;
    bool anyClip = false, nodeProcs = false, overlays = false;
    for (bool h : c.hasClip) anyClip |= h;
    for (auto& p : c.procs) nodeProcs |= p.node >= 0;
    for (int k = 0; k < kAnimCount; ++k) overlays |= c.hasClip[k] && c.clips[k].layer > 0 && c.clips[k].mask >= 0;
    // Baked skinning draws crowds instanced; per-object bone edits (masked overlays, node
    // modifiers, additive clips, IK) need the CPU palette path.
    const std::string mode = t.view.getStr("skinning", "auto");
    if (mode != "auto" && mode != "baked" && mode != "palette") warn(t, "skinning must be auto, baked or palette");
    const bool palette = mode == "palette" || (mode == "auto" && (overlays || nodeProcs || !c.additive.empty() || !c.legs.empty()));
    c.baked = skinned && !palette && dev_ && bakeAnimations(*dev_, *c.model);
    if (c.baked) for (auto& cs : c.clips) { if (cs.layer > 0) { cs.layer = 0; cs.mask = -1; } } // one-shots on the base layer
    c.needsPose = (!c.baked && (anyClip || nodeProcs)) || !c.attach.empty() || c.muzzle >= 0 || !c.legs.empty();
    for (auto& f : c.fx) c.needsPose |= f.node >= 0;
}

std::vector<std::string> RtsView::modelFiles() const {
    std::vector<std::string> out;
    for (const auto& [path, m] : models_) if (path.rfind("shape:", 0) != 0) out.push_back(path);
    return out;
}

bool RtsView::loadChoice(gfx::Device& dev, const UnitType& t, const Json& view, const std::string& path, ModelChoice& out) {
    const bool known = models_.count(path) > 0;
    out.model = loadModelCached(dev, t, path);
    if (!out.model) {
        if (!known) warn(t, path + ": using the built-in shape");
        return false;
    }
    out.materials = out.model->materials;
    configureChoice(dev, t, view, out, false);
    return true;
}

int RtsView::loadTexture(gfx::Device& dev, const UnitType& t, Model& m, const std::string& path) {
    for (size_t i = 0; i < m.images.size(); ++i) if (m.images[i].name == path) return int(i);
    std::string bytes;
    Image img;
    img.name = path;
    if (!read_(path, bytes)) { warn(t, "texture '" + path + "' not found"); return -1; }
    if (!decodeImage(bytes, img)) { warn(t, "texture '" + path + "': unsupported format (PNG, JPEG, TGA)"); return -1; }
    return addImage(dev, m, std::move(img));
}

void RtsView::configureChoice(gfx::Device& dev, const UnitType& t, const Json& v, ModelChoice& c, bool isShape) {
    Model& m = *c.model;
    // ---- placement: centre on the ground, rotate the model's forward axis to +X, fit, offset
    const std::string fwd = v.getStr("forward", isShape ? "+x" : "+z");
    float yaw = fwd == "+z" ? 90 * kDeg : fwd == "-z" ? -90 * kDeg : fwd == "-x" ? 180 * kDeg : 0;
    if (fwd != "+x" && fwd != "-x" && fwd != "+z" && fwd != "-z") warn(t, "forward must be +x, -x, +z or -z");
    yaw += num(v["yaw"], 0) * kDeg;
    const float3 mn = m.boundsMin, mx = m.boundsMax, ext = mx - mn;
    mat4 center;
    if (!isShape && v.getBool("center", true)) center = mat4::translate({-(mn.x + mx.x) * 0.5f, -mn.y, -(mn.z + mx.z) * 0.5f});
    float fit = 1;
    const bool sideways = std::fabs(std::sin(yaw)) > 0.7f;
    if (v.has("height") && ext.y > 1e-5f) fit = num(v["height"], 1) / ext.y;
    else if (!isShape && t.structure && v.getBool("fit", true)) {
        const float ex = sideways ? ext.z : ext.x, ez = sideways ? ext.x : ext.z;
        if (ex > 1e-5f && ez > 1e-5f) fit = std::min(float(t.footprintW) / ex, float(t.footprintH) / ez);
    }
    c.fix = mat4::translate(vec3(v["offset"], {0, 0, 0})) * mat4::scale({fit, fit, fit}) * mat4::rotateY(yaw) * center;

    // ---- team colour: "team": "all" | "none" | [material names]; default: names containing "team"
    const Json& team = v["team"];
    for (auto& mt : c.materials) {
        bool on = mt.team != TeamMode::None;
        if (team.isString()) on = team.str() == "all";
        else if (team.isArray()) { on = false; for (const auto& n : team.items()) on |= n.str() == mt.name; }
        else if (!isShape && lower(mt.name).find("team") != std::string::npos) on = true;
        if (on && mt.team == TeamMode::None) mt.team = TeamMode::Tint;
        if (!on) mt.team = TeamMode::None;
    }
    for (const auto& n : team.items())
        if (m.findMaterial(n.str()) < 0) warn(t, "team: material '" + n.str() + "' not found in " + m.path);

    // ---- material overrides: "*" first, then by name
    auto apply = [&](Material& mt, const Json& o) {
        if (const Json& col = o["color"]; col.size() >= 3) mt.baseColor = {num(col[0], 1), num(col[1], 1), num(col[2], 1), col.size() > 3 ? num(col[3], 1) : mt.baseColor.w};
        mt.metallic = num(o["metallic"], mt.metallic);
        mt.roughness = num(o["roughness"], mt.roughness);
        if (o.has("emissive")) mt.emissive = vec3(o["emissive"], mt.emissive);
        mt.emissiveStrength = num(o["emissiveStrength"], mt.emissiveStrength);
        if (o.has("alpha")) {
            std::string a = o.getStr("alpha");
            mt.alpha = a == "mask" ? AlphaMode::Mask : a == "blend" ? AlphaMode::Blend : AlphaMode::Opaque;
            if (a != "mask" && a != "blend" && a != "opaque") warn(t, "alpha must be opaque, mask or blend");
        }
        mt.alphaCutoff = num(o["cutoff"], mt.alphaCutoff);
        mt.doubleSided = o.getBool("doubleSided", mt.doubleSided);
        if (o.has("unlit")) mt.shading = o.getBool("unlit") ? ShadingModel::Unlit : ShadingModel::Pbr;
        mt.castShadow = o.getBool("castShadow", mt.castShadow);
        mt.receiveShadow = o.getBool("receiveShadow", mt.receiveShadow);
        mt.vertexColors = o.getBool("vertexColors", mt.vertexColors);
        if (o.has("team")) {
            std::string tm = o.getStr("team");
            mt.team = tm == "tint" ? TeamMode::Tint : tm == "mask" ? TeamMode::Mask : tm == "alpha" ? TeamMode::BaseAlpha
                    : tm == "vertex" ? TeamMode::VertexAlpha : TeamMode::None;
            if (tm != "tint" && tm != "mask" && tm != "alpha" && tm != "vertex" && tm != "none") warn(t, "team must be tint, mask, alpha, vertex or none");
        }
        mt.teamStrength = num(o["teamStrength"], mt.teamStrength);
        if (o.has("teamMask")) { mt.teamTex = loadTexture(dev, t, m, o.getStr("teamMask")); if (mt.team == TeamMode::None || mt.team == TeamMode::Tint) mt.team = TeamMode::Mask; }
        if (o.has("texture")) mt.baseTex = loadTexture(dev, t, m, o.getStr("texture"));
        if (o.has("normalMap")) {
            mt.normalTex = loadTexture(dev, t, m, o.getStr("normalMap")); // tangents: from the file or generated for its own normal maps
        }
        if (o.has("emissiveMap")) { mt.emissiveTex = loadTexture(dev, t, m, o.getStr("emissiveMap")); if (!o.has("emissive")) mt.emissive = {1, 1, 1}; }
        if (const Json& uv = o["uvScale"]; uv.size() == 2) mt.uvScale = {num(uv[0], 1), num(uv[1], 1)};
    };
    const Json& mats = v["materials"];
    if (const Json& all = mats["*"]; all.isObject()) for (auto& mt : c.materials) apply(mt, all);
    for (const auto& [name, o] : mats.members()) {
        if (name == "*") continue;
        int i = m.findMaterial(name);
        if (i < 0) { warn(t, "materials: '" + name + "' not found in " + m.path); continue; }
        apply(c.materials[size_t(i)], o);
    }
    if (v.has("variant")) {
        c.variant = m.findVariant(v.getStr("variant"));
        if (c.variant < 0) warn(t, "variant '" + v.getStr("variant") + "' not found in " + m.path);
    }

    // ---- clips
    const Json& anims = v["animations"];
    for (int k = 0; k < kAnimCount; ++k) {
        ClipSpec& cs = c.clips[k];
        cs.loop = !(k == kAnimAttack || k == kAnimDie || k == kAnimSpawn || k == kAnimHit || k == kAnimCast);
        cs.layer = k == kAnimHit ? 2 : k == kAnimCast ? 1 : 0;
        if (k == kAnimMove || k == kAnimCarry) cs.speedRef = float(t.speed.toDouble());
        const Json& a = anims[animName(k)];
        std::vector<std::string> names;
        if (a.isString()) names.push_back(a.str());
        else if (a.isArray()) for (const auto& n : a.items()) names.push_back(n.str());
        else if (a.isObject()) {
            const Json& cl = a["clip"];
            if (cl.isString()) names.push_back(cl.str());
            for (const auto& n : cl.items()) names.push_back(n.str());
            cs.speed = num(a["speed"], cs.speed);
            cs.speedRef = num(a["speedRef"], cs.speedRef);
            cs.fade = num(a["fade"], cs.fade);
            cs.loop = a.getBool("loop", cs.loop);
            if (a.has("mask")) {
                cs.mask = m.findNode(a.getStr("mask"));
                if (cs.mask < 0) warn(t, "animations." + std::string(animName(k)) + ": mask node '" + a.getStr("mask") + "' not found");
                if (k == kAnimAttack) cs.layer = 1;
            }
            const Json& ly = a["layer"];
            if (ly.isString()) cs.layer = ly.str() == "base" ? 0 : ly.str() == "hit" ? 2 : 1;
            else if (ly.isNumber()) cs.layer = std::clamp(int(num(ly, 0)), 0, Animator::kLayers - 1);
            for (const auto& [at, name] : a["events"].members()) { // "events": {"0.35": "hit", "0.1": "step"}
                char* end = nullptr;
                const float tm = std::strtof(at.c_str(), &end);
                if (end == at.c_str()) { warn(t, "animations." + std::string(animName(k)) + ": event time '" + at + "' is not a number"); continue; }
                cs.events.push_back({tm, name.str()});
            }
            for (const auto& bp : a["blend"].items()) { // blend space: [{"clip": "Walk", "speed": 1.5}, {"clip": "Run", "speed": 4}]
                const int ci = m.findClip(bp.getStr("clip"));
                if (ci < 0) { warn(t, "animations." + std::string(animName(k)) + ": blend clip '" + bp.getStr("clip") + "' not found in " + m.path); continue; }
                cs.blend.push_back({ci, num(bp["speed"], 1)});
            }
            std::sort(cs.blend.begin(), cs.blend.end(), [](auto& x, auto& y) { return x.speed < y.speed; });
            if (!cs.blend.empty() && names.empty()) cs.clips.push_back(cs.blend[0].clip);
        }
        for (const auto& n : names) {
            int ci = m.findClip(n);
            if (ci >= 0) { cs.clips.push_back(ci); continue; }
            std::string avail;
            for (auto& cl : m.clips) avail += (avail.empty() ? "" : ", ") + cl.name;
            warn(t, "animations." + std::string(animName(k)) + ": clip '" + n + "' not found in " + m.path + (avail.empty() ? " (no clips)" : " (has: " + avail + ")"));
        }
        if (!anims.isObject() && names.empty() && !m.clips.empty()) { // automatic mapping by clip name
            for (int ci = 0; ci < int(m.clips.size()) && cs.clips.empty(); ++ci) {
                const std::string ln = lower(m.clips[size_t(ci)].name);
                for (const char* const* cand = kAuto[k]; *cand && cs.clips.empty(); ++cand)
                    if (ln.find(*cand) != std::string::npos) cs.clips.push_back(ci);
            }
        }
        c.hasClip[k] = !cs.clips.empty();
    }
}

namespace {
// Preset defaults; JSON fields override them.
void preset(ProcAnim& p, const std::string& type, bool& special, std::string& err) {
    special = true;
    if (type == "bob") { p.channel = ProcAnim::Translate; p.axis = {0, 1, 0}; p.amp = 0.05f; p.wave = ProcAnim::Abs; p.by = ProcAnim::ByDistance; p.freq = 1.2f; p.when = kProcMoving; }
    else if (type == "hover") { p.channel = ProcAnim::Translate; p.axis = {0, 1, 0}; p.amp = 0.12f; p.freq = 0.48f; }
    else if (type == "sway") { p.channel = ProcAnim::Rotate; p.axis = {1, 0, 0}; p.amp = 4 * kDeg; p.freq = 0.5f; }
    else if (type == "spin") { p.channel = ProcAnim::Rotate; p.axis = {0, 1, 0}; p.spin = 180 * kDeg; p.amp = 0; }
    else if (type == "wheel") { p.channel = ProcAnim::Rotate; p.axis = {0, 0, 1}; p.by = ProcAnim::ByDistance; p.spin = -1 / 0.2f; p.amp = 0; }
    else if (type == "pulse") { p.channel = ProcAnim::Scale; p.axis = {1, 1, 1}; p.amp = 0.05f; p.freq = 1; }
    else if (type == "recoil") { p.channel = ProcAnim::Translate; p.axis = {-1, 0, 0}; p.amp = 0.15f; p.wave = ProcAnim::Const; p.on = kProcEvAttack; p.attack = 0.02f; p.decay = 12; }
    else if (type == "lunge") { p.channel = ProcAnim::Translate; p.axis = {1, 0, 0}; p.amp = 0.12f; p.wave = ProcAnim::Const; p.on = kProcEvAttack; p.attack = 0.08f; p.decay = 8; }
    else if (type == "shake") { p.channel = ProcAnim::Translate; p.axis = {0, 0, 1}; p.amp = 0.05f; p.freq = 18; p.on = kProcEvHit; p.attack = 0.0f; p.decay = 10; }
    else if (type == "flap") { p.channel = ProcAnim::Rotate; p.axis = {1, 0, 0}; p.amp = 25 * kDeg; p.freq = 3; }
    else if (type == "pop") { p.channel = ProcAnim::Scale; p.axis = {1, 1, 1}; p.amp = -0.35f; p.wave = ProcAnim::Const; p.on = kProcEvSpawn; p.attack = 0; p.decay = 6; p.randomPhase = false; }
    else if (type == "squash") { p.channel = ProcAnim::Scale; p.axis = {0.5f, -1, 0.5f}; p.amp = 0.15f; p.wave = ProcAnim::Const; p.on = kProcEvAttack; p.attack = 0.03f; p.decay = 10; }
    else if (type == "lean") { p.channel = ProcAnim::Rotate; p.axis = {1, 0, 0}; p.special = ProcAnim::Lean; p.amp = 0.12f; p.limit = 15 * kDeg; p.when = kProcMoving; }
    else if (type == "aim") { p.channel = ProcAnim::Rotate; p.axis = {0, 1, 0}; p.special = ProcAnim::Aim; p.limit = 180 * kDeg; p.turnSpeed = 180 * kDeg; }
    else if (type == "aimPitch") { p.channel = ProcAnim::Rotate; p.axis = {0, 0, 1}; p.special = ProcAnim::AimPitch; p.limit = 30 * kDeg; p.turnSpeed = 90 * kDeg; }
    else if (type == "fall") { p.channel = ProcAnim::Rotate; p.axis = {1, 0, 0}; p.special = ProcAnim::Fall; p.amp = 90 * kDeg; p.on = kProcEvDeath; p.decay = 0.6f; }
    else if (type == "sink") { p.channel = ProcAnim::Translate; p.axis = {0, -1, 0}; p.special = ProcAnim::Fall; p.amp = 0.8f; p.on = kProcEvDeath; p.decay = 2.5f; }
    else if (type == "custom" || type.empty()) special = false;
    else err = "unknown procedural type '" + type + "'";
}
} // namespace

void RtsView::parseProcedural(const UnitType& t, const Json& list, ModelChoice& c) {
    if (!list.isArray()) { warn(t, "procedural must be an array"); return; }
    for (const auto& j : list.items()) {
        ProcAnim p;
        bool isPreset = false;
        std::string err;
        preset(p, j.getStr("type", "custom"), isPreset, err);
        if (!err.empty()) { warn(t, err + " (bob, hover, sway, spin, wheel, pulse, recoil, lunge, shake, flap, pop, squash, lean, aim, aimPitch, fall, sink, custom)"); continue; }
        p.nodeName = j.getStr("node");
        if (j.has("channel")) {
            std::string ch = j.getStr("channel");
            p.channel = ch == "rotate" ? ProcAnim::Rotate : ch == "scale" ? ProcAnim::Scale : ProcAnim::Translate;
        }
        p.axis = vec3(j["axis"], p.axis);
        const float angular = p.channel == ProcAnim::Rotate ? kDeg : 1.0f; // rotations are given in degrees
        if (j.has("amp")) p.amp = num(j["amp"], 0) * angular;
        if (j.has("radius") && num(j["radius"], 0) > 0) p.spin = -1 / num(j["radius"], 1); // wheels: rad per unit travelled
        if (j.has("spin")) p.spin = num(j["spin"], 0) * kDeg;
        std::string wave = j.getStr("wave");
        if (!wave.empty()) {
            p.wave = wave == "abs" ? ProcAnim::Abs : wave == "saw" ? ProcAnim::Saw : wave == "triangle" ? ProcAnim::Triangle
                   : wave == "square" ? ProcAnim::Square : wave == "const" ? ProcAnim::Const : ProcAnim::Sine;
        }
        p.freq = num(j["freq"], p.freq);
        if (j.has("by")) p.by = j.getStr("by") == "distance" ? ProcAnim::ByDistance : ProcAnim::ByTime;
        p.phase = num(j["phase"], p.phase);
        p.randomPhase = j.getBool("randomPhase", p.randomPhase);
        if (j.has("on")) {
            p.on = eventIndex(j.getStr("on"));
            if (p.on < 0) warn(t, "procedural: 'on' must be attack, hit, spawn or death");
        }
        p.attack = num(j["attack"], p.attack);
        p.decay = num(j["decay"], p.decay);
        if (j.has("duration")) p.decay = num(j["duration"], p.decay);
        const Json& when = j["when"];
        std::vector<std::string> states;
        if (when.isString()) states.push_back(when.str());
        for (const auto& w : when.items()) states.push_back(w.str());
        if (!states.empty()) p.when = 0;
        for (const auto& s : states) {
            uint32_t b = stateBit(s);
            if (!b) warn(t, "procedural: unknown state '" + s + "'");
            p.when |= b;
        }
        p.fade = num(j["fade"], p.fade);
        if (j.has("limit")) p.limit = num(j["limit"], 180) * kDeg;
        if (j.has("turnSpeed")) p.turnSpeed = num(j["turnSpeed"], 180) * kDeg;
        p.scaleBySpeed = j.getBool("scaleBySpeed", p.scaleBySpeed);
        p.speedRef = num(j["speedRef"], float(t.speed.toDouble()) > 0 ? float(t.speed.toDouble()) : 1.0f);
        c.procs.push_back(p);
    }
}

// Light motion for objects whose model has no clip for it, so shapes and static models feel alive.
void RtsView::defaultProcedural(const UnitType& t, const TypeLook& L, ModelChoice& c) {
    auto add = [&](const char* type, auto&& tweak) {
        ProcAnim p;
        bool sp = false;
        std::string err;
        preset(p, type, sp, err);
        tweak(p);
        c.procs.push_back(p);
    };
    if (t.structure) {
        if (!c.hasClip[kAnimDie]) add("sink", [&](ProcAnim& p) { p.amp = 1.2f; p.decay = L.corpse; });
        return;
    }
    if (L.air) add("hover", [](ProcAnim&) {});
    else {
        if (!c.hasClip[kAnimMove]) add("bob", [](ProcAnim&) {});
        if (!c.hasClip[kAnimDie]) {
            add("fall", [](ProcAnim&) {});
            add("sink", [&](ProcAnim& p) { p.amp = 0.6f; p.decay = L.corpse; });
        }
    }
    if (!c.hasClip[kAnimAttack]) add("lunge", [](ProcAnim&) {});
    if (!c.hasClip[kAnimSpawn]) add("pop", [](ProcAnim&) {});
}

} // namespace rts::game
