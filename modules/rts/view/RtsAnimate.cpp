// Per-object animation state: smoothed facing and speed, unit state -> clips on the Animator
// layers, sim events (attack, hit, spawn, death) -> one-shot clips and procedural envelopes,
// corpses that outlive their entity. Presentation only; reads the scene, never the simulation.
#include "RtsView.h"
#include "../RtsModule.h"

#include <algorithm>
#include <cmath>

namespace rts::game {

using namespace render;

namespace {
constexpr float kPi = 3.14159265358979f;
float wrap(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}
float yawOf(float3 f) { return std::atan2(-f.z, f.x); }
uint64_t mix64(uint64_t x) { // splitmix: stable per-object variety (variant, phase, clip pick)
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
bool fresh(float age, float dt) { return age <= dt + 1e-4f; }
} // namespace

void RtsView::handleEvents(const RtsScene& s) {
    if (s.tick == lastTick_) return;
    lastTick_ = s.tick;
    for (const auto& ev : s.events) {
        if (!ev.a.valid()) continue;
        const uint64_t key = ev.a.packed();
        auto it = anims_.find(key);
        switch (ev.type) {
            case kEvAttack: if (it != anims_.end()) it->second.eventAge[kProcEvAttack] = 0; break;
            case kEvHit: if (it != anims_.end()) it->second.eventAge[kProcEvHit] = 0; break;
            case kEvAbility: if (it != anims_.end()) it->second.castAge = 0; break;
            case kEvSpawned: case kEvTrainDone: {
                EntityAnim& st = anims_[key]; // may arrive before the object is first drawn
                st.eventAge[kProcEvSpawn] = 0;
                st.seen = frame_;
                break;
            }
            case kEvDied:
                if (it != anims_.end() && !it->second.dead) {
                    it->second.dead = true;
                    it->second.deathAge = 0;
                    it->second.eventAge[kProcEvDeath] = 0;
                }
                break;
            default: break;
        }
    }
}

RtsView::EntityAnim& RtsView::stateFor(uint64_t key, const SceneObject& o) {
    EntityAnim& st = anims_[key];
    if (!st.initialized || st.type != o.type) {
        const TypeLook& L = looks_[o.type];
        const uint64_t h = mix64(key);
        st.type = o.type;
        st.choice = int(h % L.models.size());
        st.anim.setModel(L.models[size_t(st.choice)].model);
        st.yaw = yawOf(o.facing);
        st.lastPos = o.pos;
        st.phase = float((h >> 20) & 0xffff) / 65536.0f;
        st.baseKind = -1;
        st.rt.clear();
        st.initialized = true;
    }
    st.seen = frame_;
    return st;
}

void RtsView::animate(EntityAnim& st, const SceneObject& o, float dt) {
    const TypeLook& L = looks_[o.type];
    const ModelChoice& ch = L.models[size_t(st.choice)];
    // ---- kinematics from the interpolated positions
    float3 d = o.pos - st.lastPos;
    d.y = 0;
    float dist = length(d);
    if (dist > 4.0f) dist = 0; // teleport (garrison exit, editor move)
    st.lastPos = o.pos;
    st.distance += dist;
    if (dt > 0) {
        const float inst = dist / dt;
        st.accel = (inst - st.speed) / dt;
        st.speed += (inst - st.speed) * std::min(1.0f, dt * 12.0f);
    }
    if (!st.dead) {
        const float diff = wrap(yawOf(o.facing) - st.yaw);
        const float step = L.turnSpeed > 0 ? L.turnSpeed * dt : 1e9f;
        const float turn = std::clamp(diff, -step, step);
        st.yaw = wrap(st.yaw + turn);
        st.turnRate += ((dt > 0 ? turn / dt : 0) - st.turnRate) * std::min(1.0f, dt * 8.0f);
    }
    for (float& a : st.eventAge) a += dt;
    st.castAge += dt;
    if (st.dead) st.eventAge[kProcEvDeath] = st.deathAge;

    // ---- state
    const bool moving = !st.dead && (o.moving || st.speed > 0.15f);
    uint32_t state = 0;
    if (st.dead) state |= kProcDead;
    if (moving) state |= kProcMoving;
    if (st.eventAge[kProcEvAttack] < 1.5f || (o.order == uint8_t(OrderType::Attack) + 1 && o.hasTarget)) state |= kProcAttacking;
    if (!moving && o.order == uint8_t(OrderType::Gather) + 1) state |= kProcGathering;
    if (!moving && (o.order == uint8_t(OrderType::Build) + 1 || o.order == uint8_t(OrderType::Repair) + 1)) state |= kProcBuilding;
    if (o.carry > 0) state |= kProcCarrying;
    if (o.build < 1) state |= kProcConstructing;
    if (o.producing) state |= kProcProducing;
    if (L.air) state |= kProcAir;
    if (!(state & (kProcDead | kProcMoving | kProcAttacking | kProcGathering | kProcBuilding | kProcConstructing | kProcProducing))) state |= kProcIdle;

    // ---- clips
    Animator& A = st.anim;
    const Model& m = *ch.model;
    if (!m.clips.empty()) {
        auto pick = [&](int kind) { const auto& v = ch.clips[kind].clips; return v[size_t(mix64(uint64_t(st.distance * 7) + uint64_t(kind)) % v.size())]; };
        auto play = [&](int kind, bool restart) {
            const ClipSpec& cs = ch.clips[kind];
            Animator::Play p;
            p.loop = cs.loop; p.speed = cs.speed; p.fade = cs.fade; p.restart = restart;
            A.play(cs.layer, pick(kind), p);
            A.setMask(cs.layer, cs.layer > 0 ? cs.mask : -1);
        };
        // one-shots from events
        if (fresh(st.eventAge[kProcEvAttack], dt) && ch.hasClip[kAnimAttack] && !st.dead) {
            if (ch.clips[kAnimAttack].layer > 0) play(kAnimAttack, true);
            else if (!moving) { play(kAnimAttack, true); st.baseKind = kAnimAttack; }
        }
        if (fresh(st.castAge, dt) && ch.hasClip[kAnimCast] && !st.dead) {
            play(kAnimCast, true);
            if (ch.clips[kAnimCast].layer == 0) st.baseKind = kAnimCast;
        }
        if (fresh(st.eventAge[kProcEvHit], dt) && ch.hasClip[kAnimHit] && !st.dead) play(kAnimHit, true);
        if (fresh(st.eventAge[kProcEvSpawn], dt) && ch.hasClip[kAnimSpawn]) {
            play(kAnimSpawn, true);
            if (ch.clips[kAnimSpawn].layer == 0) st.baseKind = kAnimSpawn;
        }
        for (int l = 1; l < Animator::kLayers; ++l) if (A.clip(l) >= 0 && A.finished(l)) A.stop(l, 0.15f);
        // base layer: a running one-shot (attack / spawn / cast) finishes first, unless the unit moves
        const bool oneShot = st.baseKind == kAnimAttack || st.baseKind == kAnimSpawn || st.baseKind == kAnimCast;
        if (st.dead) {
            if (ch.hasClip[kAnimDie] && st.baseKind != kAnimDie) { play(kAnimDie, true); st.baseKind = kAnimDie; }
        } else if (!(oneShot && !A.finished(0) && !moving)) {
            int kind = kAnimIdle;
            if ((state & kProcConstructing) && ch.hasClip[kAnimConstruct]) kind = kAnimConstruct;
            else if (moving) kind = (state & kProcCarrying) && ch.hasClip[kAnimCarry] ? kAnimCarry : kAnimMove;
            else if (state & kProcGathering) kind = kAnimGather;
            else if (state & kProcBuilding) kind = ch.hasClip[kAnimBuild] ? kAnimBuild : kAnimGather;
            else if (state & kProcProducing) kind = kAnimWork;
            if (!ch.hasClip[kind]) kind = kind == kAnimCarry ? kAnimMove : kAnimIdle;
            if (!ch.hasClip[kind]) {
                if (st.baseKind != -1) { A.stop(0, 0.25f); st.baseKind = -1; }
            } else if (kind != st.baseKind) {
                play(kind, false);
                st.baseKind = kind;
            }
            if (ch.hasClip[kind] && ch.clips[kind].speedRef > 0 && (kind == kAnimMove || kind == kAnimCarry))
                A.setSpeed(0, ch.clips[kind].speed * std::clamp(st.speed / ch.clips[kind].speedRef, 0.3f, 2.5f));
        }
        A.update(dt);
        A.evaluate(st.pose);
    } else if (ch.needsPose) {
        bindPose(m, st.pose);
    }

    // ---- procedural
    ProcInputs in;
    in.time = float(lastTime_);
    in.dt = dt;
    in.speed = st.speed;
    in.distance = st.distance;
    in.turnRate = st.turnRate;
    in.accel = st.accel;
    in.phase = st.phase;
    in.state = state;
    for (int i = 0; i < kProcEvCount; ++i) in.eventAge[i] = st.eventAge[i];
    if (!st.dead) in.eventAge[kProcEvDeath] = 1e9f;
    if (o.hasTarget) {
        float3 to = o.target - o.pos;
        in.hasTarget = true;
        in.aimYaw = wrap(std::atan2(-to.z, to.x) - st.yaw);
        in.aimPitch = std::atan2(to.y, std::max(0.01f, std::sqrt(to.x * to.x + to.z * to.z)));
    }
    st.objXf = mat4{};
    if (!ch.procs.empty()) applyProcedural(ch.procs, in, st.rt, st.pose, st.objXf);
    if (ch.needsPose) worldMatrices(m, st.pose, st.world);
}

void RtsView::drawObject(Renderer3D& r, EntityAnim& st, const SceneObject& o, float ground, float alpha) {
    const UnitType& t = rules_->types[o.type];
    const TypeLook& L = looks_[o.type];
    const ModelChoice& ch = L.models[size_t(st.choice)];
    const float k = L.scale;
    float3 p{o.pos.x, ground + (L.air ? L.flyHeight : 0.0f), o.pos.z};
    mat4 base;
    const float grow = std::clamp(o.build, 0.15f, 1.0f);
    if (t.structure && L.shape) {
        // shapes are built on a 1x1 footprint: stretch to the footprint, height follows the larger side
        base = mat4::translate(p) * mat4::scale({L.sx * k, k * std::max(L.sx, L.sz) * 0.75f * grow, L.sz * k});
    } else if (t.structure) {
        base = mat4::translate(p) * mat4::rotateY(st.yaw) * mat4::scale({k, k * grow, k});
    } else {
        base = mat4::translate(p) * mat4::rotateY(st.yaw) * mat4::scale({k, k, k});
    }
    ModelDraw d;
    d.model = ch.model;
    d.world = base * st.objXf * ch.fix;
    d.nodeWorld = ch.needsPose ? &st.world : nullptr;
    d.materials = &ch.materials;
    d.variant = ch.variant;
    const float4 pc = playerColor(o.owner);
    d.team = {pc.x * pc.x, pc.y * pc.y, pc.z * pc.z};
    d.tint = {L.tint.x, L.tint.y, L.tint.z, alpha};
    d.flash = st.eventAge[kProcEvHit] < 0.3f ? 0.3f * std::exp(-st.eventAge[kProcEvHit] * 12.0f) : 0.0f;
    if (o.build < 1.0f) { // under construction: washed out
        d.tint = {d.tint.x * 0.75f, d.tint.y * 0.75f, d.tint.z * 0.75f, alpha};
        d.flash += 0.06f;
    }
    r.drawModel(d);
}

} // namespace rts::game
