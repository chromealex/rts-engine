#include "Animation.h"

#include <algorithm>
#include <cmath>

namespace rts::render {

namespace {
constexpr float kPi = 3.14159265358979f;

float3 lerp3(float3 a, float3 b, float t) { return a + (b - a) * t; }

// Value of channel c at time t (3 or 4 components into out).
void sampleChannel(const AnimChannel& c, float t, float out[4]) {
    const int comps = c.path == AnimChannel::Rotation ? 4 : 3;
    const bool cubic = c.interp == AnimChannel::CubicSpline;
    const size_t stride = size_t(comps) * (cubic ? 3 : 1);
    const size_t valOff = cubic ? size_t(comps) : 0; // cubic keys: in-tangent, value, out-tangent
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
            const float m0 = c.values[k * stride + 2 * size_t(comps) + size_t(i)] * dt;   // out-tangent of k
            const float m1 = c.values[(k + 1) * stride + size_t(i)] * dt;                 // in-tangent of k+1
            out[i] = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
        }
        if (comps == 4) {
            quat q = qnormalize(quat{out[0], out[1], out[2], out[3]});
            out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
        }
        return;
    }
    if (comps == 4) {
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
    for (size_t i = 0; i < m.nodes.size(); ++i) pose[i] = {m.nodes[i].t, m.nodes[i].r, m.nodes[i].s};
}

void sampleClip(const AnimClip& clip, float t, Pose& pose, float w, const std::vector<uint8_t>* mask) {
    if (w <= 0) return;
    for (const auto& c : clip.channels) {
        if (c.node < 0 || c.node >= int(pose.size()) || c.times.empty()) continue;
        if (mask && (size_t(c.node) >= mask->size() || !(*mask)[size_t(c.node)])) continue;
        float v[4];
        sampleChannel(c, t, v);
        NodePose& p = pose[size_t(c.node)];
        switch (c.path) {
            case AnimChannel::Translation: p.t = w >= 1 ? float3{v[0], v[1], v[2]} : lerp3(p.t, {v[0], v[1], v[2]}, w); break;
            case AnimChannel::Scale: p.s = w >= 1 ? float3{v[0], v[1], v[2]} : lerp3(p.s, {v[0], v[1], v[2]}, w); break;
            case AnimChannel::Rotation: p.r = w >= 1 ? quat{v[0], v[1], v[2], v[3]} : nlerp(p.r, {v[0], v[1], v[2], v[3]}, w); break;
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
    t.time += dt * t.speed;
    if (dur <= 0) { t.time = 0; return; }
    if (t.loop) { t.time = std::fmod(t.time, dur); if (t.time < 0) t.time += dur; }
    else t.time = std::clamp(t.time, 0.0f, dur);
}

void Animator::update(float dt) {
    if (!model_) return;
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
            sampleClip(model_->clips[size_t(L.prev.clip)], L.prev.time, pose, i == 0 ? 1.0f : w * (1 - L.fade), mask);
            if (L.cur.clip >= 0) sampleClip(model_->clips[size_t(L.cur.clip)], L.cur.time, pose, w * L.fade, mask);
        } else if (L.cur.clip >= 0) {
            sampleClip(model_->clips[size_t(L.cur.clip)], L.cur.time, pose, w, mask);
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
