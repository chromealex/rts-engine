#include "Animation.h"

#include <algorithm>
#include <cmath>

namespace rts::render {

namespace {
constexpr float kPi = 3.14159265358979f;

float3 lerp3(float3 a, float3 b, float t) { return a + (b - a) * t; }

constexpr int kMaxComps = 64;
int channelComps(const AnimChannel& c) {
    return c.path == AnimChannel::Rotation ? 4 : c.path == AnimChannel::Weights ? std::min<int>(c.comps, kMaxComps) : 3;
}

// Value of channel c at time t (channelComps(c) floats into out).
void sampleChannel(const AnimChannel& c, float t, float* out) {
    const int comps = channelComps(c);
    const int stored = c.path == AnimChannel::Weights ? c.comps : comps;
    const bool cubic = c.interp == AnimChannel::CubicSpline;
    const size_t stride = size_t(stored) * (cubic ? 3 : 1);
    const size_t valOff = cubic ? size_t(stored) : 0; // cubic keys: in-tangent, value, out-tangent
    const size_t n = c.times.size();
    auto val = [&](size_t k, int i) { return c.values[k * stride + valOff + size_t(i)]; };
    if (t <= c.times[0] || n == 1) { for (int i = 0; i < comps; ++i) out[i] = val(0, i); return; }
    if (t >= c.times[n - 1]) { for (int i = 0; i < comps; ++i) out[i] = val(n - 1, i); return; }
    size_t k = size_t(std::upper_bound(c.times.begin(), c.times.end(), t) - c.times.begin()) - 1;
    const float t0 = c.times[k], t1 = c.times[k + 1], dt = t1 - t0;
    const float u = dt > 0 ? (t - t0) / dt : 0;
    if (c.interp == AnimChannel::Step) { for (int i = 0; i < comps; ++i) out[i] = val(k, i); return; }
    if (cubic) {
        const float u2 = u * u, u3 = u2 * u;
        const float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        for (int i = 0; i < comps; ++i) {
            const float p0 = val(k, i), p1 = val(k + 1, i);
            const float m0 = c.values[k * stride + 2 * size_t(stored) + size_t(i)] * dt;   // out-tangent of k
            const float m1 = c.values[(k + 1) * stride + size_t(i)] * dt;                 // in-tangent of k+1
            out[i] = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
        }
        if (c.path == AnimChannel::Rotation) {
            quat q = qnormalize(quat{out[0], out[1], out[2], out[3]});
            out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
        }
        return;
    }
    if (c.path == AnimChannel::Rotation) {
        quat q = nlerp({val(k, 0), val(k, 1), val(k, 2), val(k, 3)}, {val(k + 1, 0), val(k + 1, 1), val(k + 1, 2), val(k + 1, 3)}, u);
        out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
        return;
    }
    for (int i = 0; i < comps; ++i) out[i] = val(k, i) + (val(k + 1, i) - val(k, i)) * u;
}

float wrapAngle(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}
} // namespace

void bindPose(const Model& m, Pose& pose) {
    pose.resize(m.nodes.size());
    for (size_t i = 0; i < m.nodes.size(); ++i) {
        pose[i].t = m.nodes[i].t; pose[i].r = m.nodes[i].r; pose[i].s = m.nodes[i].s;
        for (int k = 0; k < kMaxMorphTargets; ++k) pose[i].w[k] = m.nodes[i].weights[k];
    }
}

void sampleClip(const AnimClip& clip, float t, Pose& pose, float w, const std::vector<uint8_t>* mask) {
    if (w <= 0) return;
    for (const auto& c : clip.channels) {
        if (c.node < 0 || c.node >= int(pose.size()) || c.times.empty()) continue;
        if (mask && (size_t(c.node) >= mask->size() || !(*mask)[size_t(c.node)])) continue;
        float v[kMaxComps];
        sampleChannel(c, t, v);
        NodePose& p = pose[size_t(c.node)];
        switch (c.path) {
            case AnimChannel::Translation: p.t = w >= 1 ? float3{v[0], v[1], v[2]} : lerp3(p.t, {v[0], v[1], v[2]}, w); break;
            case AnimChannel::Scale: p.s = w >= 1 ? float3{v[0], v[1], v[2]} : lerp3(p.s, {v[0], v[1], v[2]}, w); break;
            case AnimChannel::Rotation: p.r = w >= 1 ? quat{v[0], v[1], v[2], v[3]} : nlerp(p.r, {v[0], v[1], v[2], v[3]}, w); break;
            case AnimChannel::Weights:
                for (int k = 0; k < std::min<int>(channelComps(c), kMaxMorphTargets); ++k) p.w[k] += (v[k] - p.w[k]) * std::min(w, 1.0f);
                break;
        }
    }
}

void worldMatrices(const Model& m, const Pose& pose, std::vector<mat4>& world) {
    world.resize(m.nodes.size());
    for (int n : m.order) {
        const NodePose& p = pose[size_t(n)];
        mat4 l = mat4::trs(p.t, p.r, p.s);
        const int parent = m.nodes[size_t(n)].parent;
        world[size_t(n)] = parent >= 0 ? world[size_t(parent)] * l : l;
    }
}

void paletteMatrices(const Model& m, const Skin& skin, const Primitive& prim, const std::vector<mat4>& world, std::vector<mat4>& out) {
    out.resize(prim.jointPalette.size());
    for (size_t i = 0; i < prim.jointPalette.size(); ++i) {
        const size_t j = prim.jointPalette[i];
        if (j >= skin.joints.size()) { out[i] = mat4{}; continue; }
        const int node = skin.joints[j];
        out[i] = (node >= 0 && size_t(node) < world.size() ? world[size_t(node)] : mat4{}) * skin.inverseBind[j];
    }
    (void)m;
}

std::vector<uint8_t> subtreeMask(const Model& m, int root) {
    std::vector<uint8_t> mask(m.nodes.size(), 0);
    if (root < 0 || root >= int(m.nodes.size())) { std::fill(mask.begin(), mask.end(), 1); return mask; }
    std::vector<int> stack{root};
    while (!stack.empty()) {
        int n = stack.back(); stack.pop_back();
        if (mask[size_t(n)]) continue;
        mask[size_t(n)] = 1;
        for (int c : m.nodes[size_t(n)].children) stack.push_back(c);
    }
    return mask;
}

void sampleClipAdditive(const AnimClip& clip, float t, Pose& pose, float w) {
    if (w <= 0) return;
    for (const auto& c : clip.channels) {
        if (c.node < 0 || c.node >= int(pose.size()) || c.times.empty()) continue;
        float v[kMaxComps], r[kMaxComps];
        sampleChannel(c, t, v);
        sampleChannel(c, c.times[0], r);
        NodePose& p = pose[size_t(c.node)];
        switch (c.path) {
            case AnimChannel::Translation: p.t += float3{v[0] - r[0], v[1] - r[1], v[2] - r[2]} * w; break;
            case AnimChannel::Scale:
                p.s = mul(p.s, float3{1 + (v[0] / (r[0] != 0 ? r[0] : 1) - 1) * w, 1 + (v[1] / (r[1] != 0 ? r[1] : 1) - 1) * w, 1 + (v[2] / (r[2] != 0 ? r[2] : 1) - 1) * w});
                break;
            case AnimChannel::Rotation: { // delta = inverse(ref) * value, scaled by w
                quat ref{-r[0], -r[1], -r[2], r[3]}, val{v[0], v[1], v[2], v[3]};
                quat d = nlerp(quat{}, ref * val, w);
                p.r = qnormalize(p.r * d);
                break;
            }
            case AnimChannel::Weights:
                for (int k = 0; k < std::min<int>(channelComps(c), kMaxMorphTargets); ++k) p.w[k] += (v[k] - r[k]) * w;
                break;
        }
    }
}

quat rotationOf(const mat4& m) {
    float3 c0 = normalize(float3{m.at(0, 0), m.at(1, 0), m.at(2, 0)}), c1 = normalize(float3{m.at(0, 1), m.at(1, 1), m.at(2, 1)}),
           c2 = normalize(float3{m.at(0, 2), m.at(1, 2), m.at(2, 2)});
    const float tr = c0.x + c1.y + c2.z;
    quat r;
    if (tr > 0) { float k = std::sqrt(tr + 1) * 2; r = {(c1.z - c2.y) / k, (c2.x - c0.z) / k, (c0.y - c1.x) / k, k / 4}; }
    else if (c0.x > c1.y && c0.x > c2.z) { float k = std::sqrt(1 + c0.x - c1.y - c2.z) * 2; r = {k / 4, (c1.x + c0.y) / k, (c2.x + c0.z) / k, (c1.z - c2.y) / k}; }
    else if (c1.y > c2.z) { float k = std::sqrt(1 + c1.y - c0.x - c2.z) * 2; r = {(c1.x + c0.y) / k, k / 4, (c2.y + c1.z) / k, (c2.x - c0.z) / k}; }
    else { float k = std::sqrt(1 + c2.z - c0.x - c1.y) * 2; r = {(c2.x + c0.z) / k, (c2.y + c1.z) / k, k / 4, (c0.y - c1.x) / k}; }
    return qnormalize(r);
}

namespace {
float3 posOf(const mat4& m) { return {m.at(0, 3), m.at(1, 3), m.at(2, 3)}; }
quat inverse(const quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
float safeAcos(float x) { return std::acos(std::clamp(x, -1.0f, 1.0f)); }
// world matrices of a node and its subtree after its local pose changed
void refreshSubtree(const Model& m, const Pose& pose, std::vector<mat4>& world, int root) {
    std::vector<int> stack{root};
    while (!stack.empty()) {
        int n = stack.back(); stack.pop_back();
        const NodePose& p = pose[size_t(n)];
        const int parent = m.nodes[size_t(n)].parent;
        mat4 l = mat4::trs(p.t, p.r, p.s);
        world[size_t(n)] = parent >= 0 ? world[size_t(parent)] * l : l;
        for (int c : m.nodes[size_t(n)].children) stack.push_back(c);
    }
}
} // namespace

void solveTwoBoneIK(const Model& m, Pose& pose, std::vector<mat4>& world, int a, int b, int c, float3 target, float3 pole, float weight) {
    const int n = int(pose.size());
    if (a < 0 || b < 0 || c < 0 || a >= n || b >= n || c >= n || world.size() != pose.size() || weight <= 0) return;
    const float3 pa = posOf(world[size_t(a)]), pb = posOf(world[size_t(b)]), pc = posOf(world[size_t(c)]);
    const float lab = length(pb - pa), lcb = length(pb - pc);
    if (lab < 1e-5f || lcb < 1e-5f) return;
    const float lat = std::clamp(length(target - pa), 1e-4f, lab + lcb - 1e-4f);
    // current and wanted interior angles (law of cosines)
    const float acab0 = safeAcos(dot(normalize(pc - pa), normalize(pb - pa)));
    const float babc0 = safeAcos(dot(normalize(pa - pb), normalize(pc - pb)));
    const float acat0 = safeAcos(dot(normalize(pc - pa), normalize(target - pa)));
    const float acab1 = safeAcos((lcb * lcb - lab * lab - lat * lat) / (-2 * lab * lat));
    const float babc1 = safeAcos((lat * lat - lab * lab - lcb * lcb) / (-2 * lab * lcb));
    float3 axis0 = cross(pc - pa, pole - pa);
    if (dot(axis0, axis0) < 1e-10f) axis0 = cross(pc - pa, pb - pa);
    if (dot(axis0, axis0) < 1e-10f) return;
    axis0 = normalize(axis0);
    float3 axis1 = cross(pc - pa, target - pa);
    const bool turn = dot(axis1, axis1) > 1e-10f;
    if (turn) axis1 = normalize(axis1);
    const quat ga = rotationOf(world[size_t(a)]), gb = rotationOf(world[size_t(b)]);
    auto local = [](const quat& g, float3 axis) { return inverse(g).rotate(axis); };
    quat ra = quat::axisAngle(local(ga, axis0), (acab1 - acab0) * weight);
    if (turn) ra = ra * quat::axisAngle(local(ga, axis1), acat0 * weight);
    const quat rb = quat::axisAngle(local(gb, axis0), (babc1 - babc0) * weight);
    pose[size_t(a)].r = qnormalize(pose[size_t(a)].r * ra);
    pose[size_t(b)].r = qnormalize(pose[size_t(b)].r * rb);
    refreshSubtree(m, pose, world, a);
}

bool bakeAnimations(gfx::Device& dev, Model& m, float fps, int maxRows) {
    int skin = -1;
    for (const auto& nd : m.nodes) if (nd.skin >= 0 && nd.mesh >= 0) { skin = nd.skin; break; }
    if (skin < 0 || m.baked.tex.valid()) return m.baked.tex.valid();
    const Skin& sk = m.skins[size_t(skin)];
    const int joints = int(sk.joints.size()), width = joints * 3;
    if (joints == 0 || width > 2048) return false;
    float total = 0;
    for (const auto& c : m.clips) total += c.duration;
    const int fixedRows = 1 + 2 * int(m.clips.size());
    if (total > 0 && fixedRows + int(std::ceil(total * fps)) > maxRows) fps = std::max(4.0f, float(maxRows - fixedRows) / total);
    Model::Baked b;
    b.skin = skin; b.joints = joints; b.fps = fps;
    int rows = 1;
    for (const auto& c : m.clips) {
        Model::Baked::Clip bc;
        bc.firstRow = rows;
        bc.frames = std::max(1, int(std::ceil(c.duration * fps)) + 1);
        rows += bc.frames;
        b.clips.push_back(bc);
    }
    if (rows > maxRows) return false;
    b.rows = rows;
    std::vector<float> data(size_t(rows) * size_t(width) * 4, 0.0f);
    Pose pose;
    std::vector<mat4> world;
    auto writeRow = [&](int row) {
        for (int j = 0; j < joints; ++j) {
            const int node = sk.joints[size_t(j)];
            const mat4 jm = (node >= 0 && size_t(node) < world.size() ? world[size_t(node)] : mat4{}) * sk.inverseBind[size_t(j)];
            float* dst = &data[(size_t(row) * size_t(width) + size_t(j) * 3) * 4];
            for (int r = 0; r < 3; ++r)
                for (int col = 0; col < 4; ++col) dst[r * 4 + col] = jm.at(r, col);
        }
    };
    world = m.bindWorld;
    writeRow(0);
    for (size_t ci = 0; ci < m.clips.size(); ++ci) {
        const AnimClip& clip = m.clips[ci];
        for (int f = 0; f < b.clips[ci].frames; ++f) {
            bindPose(m, pose);
            sampleClip(clip, std::min(float(f) / fps, clip.duration), pose);
            worldMatrices(m, pose, world);
            writeRow(b.clips[ci].firstRow + f);
        }
    }
    gfx::TextureDesc td;
    td.width = width; td.height = rows;
    td.format = gfx::PixelFormat::RGBA32F;
    td.filter = gfx::Filter::Nearest;
    td.data = data.data();
    td.label = "baked animation";
    b.tex = dev.createTexture(td);
    if (!b.tex.valid()) return false;
    m.baked = std::move(b);
    return true;
}

int bakedRow(const Model& m, int clip, float time, bool loop) {
    if (clip < 0 || clip >= int(m.baked.clips.size())) return 0;
    const auto& c = m.baked.clips[size_t(clip)];
    int f = int(time * m.baked.fps + 0.5f);
    f = loop && c.frames > 1 ? f % (c.frames - 1) : std::min(f, c.frames - 1); // last frame == first frame for loops
    return c.firstRow + std::max(0, f);
}

// ---------------------------------------------------------------- Animator

void Animator::setModel(const Model* m) {
    if (m == model_) return;
    model_ = m;
    for (auto& l : layers_) l = Layer{};
}

void Animator::play(int layer, int clip, const Play& p) {
    if (!valid(layer) || !model_) return;
    if (clip >= int(model_->clips.size())) clip = -1;
    Layer& L = layers_[layer];
    L.target = 1;
    L.weightRate = p.fade > 0 ? 1.0f / p.fade : 1000.0f;
    if (layer == 0) L.weight = 1;
    if (clip == L.cur.clip && clip >= 0 && !p.restart) {
        L.cur.loop = p.loop; L.cur.speed = p.speed;
        return;
    }
    if (L.cur.clip >= 0 && L.weight > 0.01f) { L.prev = L.cur; L.fade = 0; L.fadeDur = p.fade; }
    else { L.prev = Track{}; L.fade = 1; }
    if (p.fade <= 0) L.fade = 1;
    L.cur = Track{clip, 0, p.speed, p.loop};
}

void Animator::stop(int layer, float fade) {
    if (!valid(layer)) return;
    Layer& L = layers_[layer];
    if (layer == 0) { // base layer: fade back to the bind pose
        if (L.cur.clip >= 0) { L.prev = L.cur; L.fade = 0; L.fadeDur = fade; if (fade <= 0) L.fade = 1; }
        L.cur = Track{};
        return;
    }
    L.target = 0;
    L.weightRate = fade > 0 ? 1.0f / fade : 1000.0f;
}

void Animator::setMask(int layer, int rootNode) {
    if (!valid(layer) || !model_) return;
    layers_[layer].mask = rootNode < 0 ? std::vector<uint8_t>{} : subtreeMask(*model_, rootNode);
}

void Animator::advance(Track& t, float dt) const {
    if (t.clip < 0) return;
    const float dur = model_->clips[size_t(t.clip)].duration;
    float rate = 1;
    if (t.clip2 >= 0 && t.w2 > 0 && dur > 0) { // phase-synchronized blend: the mixed cycle length drives both
        const float d2 = model_->clips[size_t(t.clip2)].duration;
        const float mixed = dur + (d2 - dur) * t.w2;
        if (mixed > 0) rate = dur / mixed;
    }
    t.time += dt * t.speed * rate;
    if (dur <= 0) { t.time = 0; return; }
    if (t.loop) { t.time = std::fmod(t.time, dur); if (t.time < 0) t.time += dur; }
    else t.time = std::clamp(t.time, 0.0f, dur);
}

void Animator::setBlend(int layer, int clip2, float weight) {
    if (!valid(layer) || !model_) return;
    Track& t = layers_[layer].cur;
    t.clip2 = clip2 >= 0 && clip2 < int(model_->clips.size()) ? clip2 : -1;
    t.w2 = std::clamp(weight, 0.0f, 1.0f);
}

void Animator::setAdditive(int slot, int clip, float weight, float fade) {
    if (slot < 0 || !model_) return;
    if (int(additive_.size()) <= slot) additive_.resize(size_t(slot) + 1);
    Additive& a = additive_[size_t(slot)];
    if (a.clip != clip) { a.clip = clip >= 0 && clip < int(model_->clips.size()) ? clip : -1; a.time = 0; }
    a.target = weight;
    a.rate = fade > 0 ? 1.0f / fade : 1000.0f;
}

Animator::BaseSample Animator::baseSample() const {
    BaseSample s;
    const Layer& L = layers_[0];
    auto t2 = [&](const Track& t) { // time in clip2 at the same phase
        const float d1 = model_->clips[size_t(t.clip)].duration, d2 = model_->clips[size_t(t.clip2)].duration;
        return d1 > 0 ? t.time / d1 * d2 : 0.0f;
    };
    if (L.prev.clip >= 0 && L.fade < 1) {
        s.clipA = L.prev.clip; s.timeA = L.prev.time; s.loopA = L.prev.loop;
        s.clipB = L.cur.clip; s.timeB = L.cur.time; s.loopB = L.cur.loop; s.w = L.fade;
    } else if (L.cur.clip >= 0 && L.cur.clip2 >= 0 && L.cur.w2 > 0) {
        s.clipA = L.cur.clip; s.timeA = L.cur.time; s.clipB = L.cur.clip2; s.timeB = t2(L.cur); s.w = L.cur.w2;
    } else {
        s.clipA = s.clipB = L.cur.clip; s.timeA = s.timeB = L.cur.time; s.loopA = s.loopB = L.cur.loop; s.w = 1;
    }
    return s;
}

void Animator::sampleTrack(const Track& t, Pose& pose, float w, const std::vector<uint8_t>* mask) const {
    if (t.clip < 0) return;
    sampleClip(model_->clips[size_t(t.clip)], t.time, pose, w, mask);
    if (t.clip2 >= 0 && t.w2 > 0) {
        const float d1 = model_->clips[size_t(t.clip)].duration, d2 = model_->clips[size_t(t.clip2)].duration;
        sampleClip(model_->clips[size_t(t.clip2)], d1 > 0 ? t.time / d1 * d2 : 0.0f, pose, w * t.w2, mask);
    }
}

void Animator::update(float dt) {
    if (!model_) return;
    for (auto& a : additive_) {
        if (a.clip >= 0) {
            const float dur = model_->clips[size_t(a.clip)].duration;
            a.time = dur > 0 ? std::fmod(a.time + dt, dur) : 0;
        }
        const float d = a.target - a.weight, step = dt * a.rate;
        a.weight = std::fabs(d) <= step ? a.target : a.weight + (d > 0 ? step : -step);
    }
    for (int i = 0; i < kLayers; ++i) {
        Layer& L = layers_[i];
        advance(L.cur, dt);
        advance(L.prev, dt);
        if (L.fade < 1) {
            L.fade = L.fadeDur > 0 ? std::min(1.0f, L.fade + dt / L.fadeDur) : 1.0f;
            if (L.fade >= 1) L.prev = Track{};
        }
        if (i > 0) {
            float d = L.target - L.weight, step = dt * L.weightRate;
            L.weight = std::fabs(d) <= step ? L.target : L.weight + (d > 0 ? step : -step);
            if (L.weight <= 0 && L.target <= 0) { L.cur = Track{}; L.prev = Track{}; }
        }
    }
}

void Animator::evaluate(Pose& pose) const {
    if (!model_) return;
    bindPose(*model_, pose);
    for (int i = 0; i < kLayers; ++i) {
        const Layer& L = layers_[i];
        const float w = i == 0 ? 1.0f : L.weight;
        if (w <= 0.001f) continue;
        const std::vector<uint8_t>* mask = L.mask.empty() ? nullptr : &L.mask;
        if (L.prev.clip >= 0 && L.fade < 1) {
            sampleTrack(L.prev, pose, i == 0 ? 1.0f : w * (1 - L.fade), mask);
            sampleTrack(L.cur, pose, w * L.fade, mask);
        } else {
            sampleTrack(L.cur, pose, w, mask);
        }
        if (i == 0 && L.cur.clip < 0 && L.prev.clip >= 0 && L.fade < 1) {
            // fading out to bind: blend the bind values back in
            Pose bind;
            bindPose(*model_, bind);
            for (size_t n = 0; n < pose.size(); ++n) {
                pose[n].t = lerp3(pose[n].t, bind[n].t, L.fade);
                pose[n].s = lerp3(pose[n].s, bind[n].s, L.fade);
                pose[n].r = nlerp(pose[n].r, bind[n].r, L.fade);
            }
        }
    }
    for (const auto& a : additive_)
        if (a.clip >= 0 && a.weight > 0.001f) sampleClipAdditive(model_->clips[size_t(a.clip)], a.time, pose, a.weight);
}

bool Animator::finished(int layer) const {
    if (!valid(layer) || !model_) return true;
    const Track& t = layers_[layer].cur;
    if (t.clip < 0) return true;
    return !t.loop && t.time >= model_->clips[size_t(t.clip)].duration - 1e-4f;
}

// ---------------------------------------------------------------- procedural

void applyProcedural(const std::vector<ProcAnim>& mods, const ProcInputs& in, std::vector<ProcRuntime>& rt, Pose& pose, mat4& objectXf) {
    rt.resize(mods.size());
    for (size_t i = 0; i < mods.size(); ++i) {
        const ProcAnim& a = mods[i];
        ProcRuntime& r = rt[i];
        const bool active = a.when == 0 || (in.state & a.when);
        {
            const float target = active ? 1.0f : 0.0f, step = a.fade > 0 ? in.dt / a.fade : 1.0f;
            r.weight = std::fabs(target - r.weight) <= step ? target : r.weight + (target > r.weight ? step : -step);
        }
        float env = 1;
        if (a.on >= 0 && a.on < kProcEvCount) {
            const float age = in.eventAge[a.on];
            if (age >= 1e8f) env = 0;
            else if (a.special == ProcAnim::Fall) env = std::clamp(age / std::max(a.decay, 0.01f), 0.0f, 1.0f);
            else env = age < a.attack ? age / std::max(a.attack, 1e-4f) : std::exp(-(age - a.attack) * a.decay);
        }
        const float x = a.by == ProcAnim::ByDistance ? in.distance : in.time;
        float v = 0, angle = 0;
        switch (a.special) {
            case ProcAnim::Aim:
            case ProcAnim::AimPitch: {
                const float want = a.special == ProcAnim::Aim ? in.aimYaw : in.aimPitch;
                const float target = in.hasTarget ? std::clamp(wrapAngle(want), -a.limit, a.limit) : 0.0f;
                const float d = wrapAngle(target - r.aim), step = a.turnSpeed * in.dt;
                r.aim = std::fabs(d) <= step ? target : r.aim + (d > 0 ? step : -step);
                angle = v = r.aim;
                break;
            }
            case ProcAnim::Lean:
                v = angle = std::clamp(in.turnRate * a.amp, -a.limit, a.limit) * r.weight;
                break;
            case ProcAnim::Fall:
                v = angle = a.amp * env * env * r.weight;
                break;
            case ProcAnim::Align: { // tilt the object onto the terrain (vehicles, buildings on slopes)
                float3 n = normalize(in.groundNormal), up{0, 1, 0};
                float3 ax = cross(up, n);
                const float sn = length(ax);
                quat target = sn > 1e-5f ? quat::axisAngle(ax * (1 / sn), std::atan2(sn, dot(up, n)) * (a.amp > 0 ? a.amp : 1) * r.weight) : quat{};
                r.align = nlerp(r.align, target, std::min(1.0f, in.dt * (a.turnSpeed > 0 ? a.turnSpeed : 8)));
                if (a.node >= 0 && a.node < int(pose.size())) pose[size_t(a.node)].r = qnormalize(r.align * pose[size_t(a.node)].r);
                else if (a.node < 0) objectXf = objectXf * mat4::rotate(r.align);
                continue;
            }
            case ProcAnim::None: {
                if (r.weight <= 0 && a.spin == 0) continue;
                const float cyc = x * a.freq + a.phase + (a.randomPhase ? in.phase : 0.0f);
                const float f = cyc - std::floor(cyc);
                float w = 0;
                switch (a.wave) {
                    case ProcAnim::Sine: w = std::sin(2 * kPi * cyc); break;
                    case ProcAnim::Abs: w = std::fabs(std::sin(kPi * cyc)); break;
                    case ProcAnim::Saw: w = f * 2 - 1; break;
                    case ProcAnim::Triangle: w = 1 - 4 * std::fabs(f - 0.5f); break;
                    case ProcAnim::Square: w = f < 0.5f ? 1.0f : -1.0f; break;
                    case ProcAnim::Const: w = 1; break;
                }
                float amp = a.amp * env * r.weight;
                if (a.scaleBySpeed) amp *= std::min(in.speed / std::max(a.speedRef, 1e-3f), 2.0f);
                v = amp * w;
                angle = v + a.spin * x;
                break;
            }
        }
        if (a.node >= 0 && a.node < int(pose.size())) {
            NodePose& np = pose[size_t(a.node)];
            switch (a.channel) {
                case ProcAnim::Translate: np.t += a.axis * v; break;
                case ProcAnim::Rotate: np.r = qnormalize(np.r * quat::axisAngle(a.axis, angle)); break;
                case ProcAnim::Scale: np.s = mul(np.s, float3{1, 1, 1} + a.axis * v); break;
            }
        } else if (a.node < 0) {
            switch (a.channel) {
                case ProcAnim::Translate: objectXf = objectXf * mat4::translate(a.axis * v); break;
                case ProcAnim::Rotate: objectXf = objectXf * mat4::rotate(quat::axisAngle(a.axis, angle)); break;
                case ProcAnim::Scale: objectXf = objectXf * mat4::scale(float3{1, 1, 1} + a.axis * v); break;
            }
        }
    }
}

} // namespace rts::render
