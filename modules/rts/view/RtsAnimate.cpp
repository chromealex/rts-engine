// Per-object animation state: smoothed facing and speed, unit state -> clips on the Animator
// layers (blend spaces, additive clips), sim events (attack, hit, spawn, death) -> one-shot clips
// and procedural envelopes, clip events, foot IK, animation LOD, corpses that outlive their
// entity. Presentation only; reads the scene, never the simulation.
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
                    EntityAnim& st = it->second;
                    st.dead = true;
                    st.deathAge = 0;
                    st.eventAge[kProcEvDeath] = 0;
                    if (st.initialized) events_.push_back({"death", ev.a, st.type, st.last.owner, st.last.pos});
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
        st.poseValid = false;
        st.initialized = true;
        if (st.eventAge[kProcEvSpawn] < 1) events_.push_back({"spawn", o.entity, o.type, o.owner, o.pos});
    }
    st.seen = frame_;
    return st;
}

uint32_t RtsView::stateBits(const EntityAnim& st, const SceneObject& o, bool moving) const {
    const TypeLook& L = looks_[o.type];
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
    return state;
}

void RtsView::emitClipEvents(EntityAnim& st, const ModelChoice& ch, const SceneObject& o) {
    for (int l = 0; l < Animator::kLayers; ++l) {
        const int clip = st.anim.clip(l);
        const float t = st.anim.time(l);
        if (clip >= 0 && clip == st.prevClip[l]) {
            const float t0 = st.prevTime[l];
            for (int k = 0; k < kAnimCount; ++k) {
                const ClipSpec& cs = ch.clips[k];
                if (cs.events.empty() || std::find(cs.clips.begin(), cs.clips.end(), clip) == cs.clips.end()) continue;
                for (const auto& [at, name] : cs.events) {
                    const bool crossed = t >= t0 ? (at > t0 && at <= t) : (at > t0 || at <= t); // wrapped loop
                    if (crossed) events_.push_back({name, o.entity, o.type, o.owner, o.pos});
                }
            }
        }
        st.prevClip[l] = clip;
        st.prevTime[l] = t;
    }
}

void RtsView::animate(EntityAnim& st, const SceneObject& o, float dt, const MapGrid* grid, const float3* viewPos) {
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

    const bool moving = !st.dead && (o.moving || st.speed > 0.15f);
    const uint32_t state = stateBits(st, o, moving);

    // ---- animation LOD: off screen -> clips only keep time; far away -> pose updated less often
    bool updatePose = true;
    if (animLod_ && st.poseValid) {
        const float3 p = viewPos ? *viewPos : o.pos;
        const float4 c = viewProj_ * float4{p.x, p.y + 0.5f, p.z, 1};
        const bool onScreen = c.w > 0 && std::fabs(c.x) < c.w * 1.2f && std::fabs(c.y) < c.w * 1.2f;
        const float camDist = length(camPos_ - p);
        const float interval = !onScreen ? 1e9f : camDist > 70 ? 0.2f : camDist > 40 ? 0.1f : 0.0f;
        st.lodAccum += dt;
        if (st.lodAccum < interval) updatePose = false;
        else st.lodAccum = 0;
    }

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
        if (fresh(st.eventAge[kProcEvHit], dt) && ch.hasClip[kAnimHit] && !st.dead && ch.clips[kAnimHit].layer > 0) play(kAnimHit, true);
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
            const ClipSpec& cs = ch.clips[kind];
            if (ch.hasClip[kind] && (kind == kAnimMove || kind == kAnimCarry)) {
                if (cs.blend.size() >= 2) { // blend space: pick the bracket around the current speed
                    size_t hi = 1;
                    while (hi + 1 < cs.blend.size() && st.speed > cs.blend[hi].speed) ++hi;
                    const auto& a = cs.blend[hi - 1];
                    const auto& b = cs.blend[hi];
                    const float w = std::clamp((st.speed - a.speed) / std::max(b.speed - a.speed, 1e-3f), 0.0f, 1.0f);
                    if (A.clip(0) != a.clip) { Animator::Play p; p.fade = cs.fade; A.play(0, a.clip, p); }
                    A.setBlend(0, b.clip, w);
                    const float ref = a.speed + (b.speed - a.speed) * w;
                    A.setSpeed(0, cs.speed * std::clamp(st.speed / std::max(ref, 1e-3f), 0.3f, 2.5f));
                } else if (cs.speedRef > 0) {
                    A.setSpeed(0, cs.speed * std::clamp(st.speed / cs.speedRef, 0.3f, 2.5f));
                }
            } else {
                A.setBlend(0, -1, 0);
            }
        }
        for (size_t i = 0; i < ch.additive.size(); ++i) {
            const AdditiveSpec& ad = ch.additive[i];
            A.setAdditive(int(i), ad.clip, (ad.when == 0 || (state & ad.when)) ? ad.weight : 0.0f);
        }
        A.update(dt);
        emitClipEvents(st, ch, o);
        if (ch.needsPose && updatePose) A.evaluate(st.pose);
    } else if (ch.needsPose && (updatePose || st.pose.empty())) {
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
    if (grid && grid->w > 0) { // terrain normal around the object, rotated into object space
        const float e = 0.6f;
        const float hx = heightAt(*grid, o.pos.x + e, o.pos.z) - heightAt(*grid, o.pos.x - e, o.pos.z);
        const float hz = heightAt(*grid, o.pos.x, o.pos.z + e) - heightAt(*grid, o.pos.x, o.pos.z - e);
        const float3 n = normalize(float3{-hx, 2 * e, -hz});
        in.groundNormal = mat4::rotateY(-st.yaw).transformDir(n);
    }
    st.objXf = mat4{};
    if (!ch.procs.empty()) {
        // node modifiers act on the pose: skip them with the pose when LOD postponed it
        static Pose none; // a postponed pose must not take the node modifiers twice
        applyProcedural(ch.procs, in, st.rt, ch.needsPose && updatePose ? st.pose : none, st.objXf);
    }
    if (ch.needsPose && updatePose) {
        worldMatrices(m, st.pose, st.world);
        // foot IK: plant two-bone legs on the terrain under each foot (ramps, cliffs, slopes)
        if (grid && grid->w > 0 && !ch.legs.empty() && !moving) {
            const float scale = L.scale, ground = heightAt(*grid, o.pos.x, o.pos.z);
            const mat4 toWorld = mat4::translate({o.pos.x, ground, o.pos.z}) * mat4::rotateY(st.yaw) * mat4::scale({scale, scale, scale}) * ch.fix;
            const mat4 toModel = toWorld.inverse();
            for (const auto& leg : ch.legs) {
                const float3 foot = {st.world[size_t(leg.foot)].at(0, 3), st.world[size_t(leg.foot)].at(1, 3), st.world[size_t(leg.foot)].at(2, 3)};
                float3 fw = toWorld.transformPoint(foot);
                const float delta = heightAt(*grid, fw.x, fw.z) - ground;
                if (std::fabs(delta) < 0.01f) continue;
                fw.y += delta;
                const float3 target = toModel.transformPoint(fw);
                const float3 knee = {st.world[size_t(leg.knee)].at(0, 3), st.world[size_t(leg.knee)].at(1, 3), st.world[size_t(leg.knee)].at(2, 3)};
                solveTwoBoneIK(m, st.pose, st.world, leg.hip, leg.knee, leg.foot, target, knee + float3{0, 0, 0.5f});
            }
        }
        st.poseValid = true;
    }
}

} // namespace rts::game
