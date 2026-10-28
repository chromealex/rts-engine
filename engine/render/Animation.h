// Animation: skeletal clips (sampling, cross-fades, layers with node masks) and procedural
// modifiers (oscillators, event envelopes, lean, aim, fall) that work on any model, skinned or
// rigid, glTF or procedural. Presentation only: nothing here is deterministic or feeds the sim.
//
//   Animator   plays clips on layers; layer 0 is the base (locomotion), higher layers overlay
//              (attack while walking) and can be limited to a node subtree ("upper body").
//   ProcAnim   one procedural modifier on a node (or the whole object when node < 0), driven by
//              time, travelled distance or speed, gated by states and/or triggered by events.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Model.h"

namespace rts::render {

struct NodePose { float3 t; quat r; float3 s{1, 1, 1}; float w[kMaxMorphTargets] = {}; };
using Pose = std::vector<NodePose>;

void bindPose(const Model& m, Pose& pose);
// Blends `clip` at time `t` into `pose` with weight w (1 = replace). mask: per-node 0/1, null = all.
void sampleClip(const AnimClip& clip, float t, Pose& pose, float w = 1, const std::vector<uint8_t>* mask = nullptr);
void worldMatrices(const Model& m, const Pose& pose, std::vector<mat4>& world);
// Joint matrices of one primitive's palette (joint world * inverse bind).
void paletteMatrices(const Model& m, const Skin& skin, const Primitive& prim, const std::vector<mat4>& world, std::vector<mat4>& out);
std::vector<uint8_t> subtreeMask(const Model& m, int root);
// Adds clip(t) - clip(0) on top of `pose` (additive animation: breathing, recoil on any action).
void sampleClipAdditive(const AnimClip& clip, float t, Pose& pose, float w = 1);
// Two-bone IK (hip -> knee -> foot) in model space: rotates a and b so c reaches `target`, bending
// towards `pole`. `world` must match `pose`; both are updated for the chain.
void solveTwoBoneIK(const Model& m, Pose& pose, std::vector<mat4>& world, int a, int b, int c, float3 target, float3 pole, float weight = 1);
// Rotation part of a matrix (scale removed).
quat rotationOf(const mat4& m);

// Bakes every clip into Model::baked (joint matrices of the first skin, `fps` samples per second,
// at most maxRows rows: longer sets lower the rate). Needs the CPU model (nodes, clips, skins).
bool bakeAnimations(gfx::Device& dev, Model& m, float fps = 30, int maxRows = 2048);
// Row of the baked texture for clip / time (clip -1 = bind pose).
int bakedRow(const Model& m, int clip, float time, bool loop = true);
class Animator {
public:
    static constexpr int kLayers = 3;
    struct Play {
        bool loop = true;
        float speed = 1;
        float fade = 0.2f;      // cross-fade seconds
        bool restart = false;   // restart even if this clip is already playing
    };
    void setModel(const Model* m);
    const Model* model() const { return model_; }
    void play(int layer, int clip, const Play& p);
    void stop(int layer, float fade = 0.2f);
    void setSpeed(int layer, float speed) { if (valid(layer)) layers_[layer].cur.speed = speed; }
    void setMask(int layer, int rootNode);   // -1: whole model
    // Blend space: mix a second clip into the layer's current clip at the same phase (walk -> run).
    void setBlend(int layer, int clip2, float weight);
    // Additive clips on top of everything (index = slot, weight fades towards `weight`).
    void setAdditive(int slot, int clip, float weight, float fade = 0.3f);
    // What the base layer shows as two clips and a mix (for baked skinning): A -> B by w.
    struct BaseSample { int clipA = -1, clipB = -1; float timeA = 0, timeB = 0, w = 1; bool loopA = true, loopB = true; };
    BaseSample baseSample() const;
    void update(float dt);
    void evaluate(Pose& pose) const;         // bind pose + all layers
    int clip(int layer) const { return valid(layer) ? layers_[layer].cur.clip : -1; }
    float time(int layer) const { return valid(layer) ? layers_[layer].cur.time : 0; }
    bool finished(int layer) const;          // non-looping clip reached its end (or nothing playing)

private:
    struct Track { int clip = -1; float time = 0, speed = 1; bool loop = true; int clip2 = -1; float w2 = 0; };
    struct Additive { int clip = -1; float time = 0, weight = 0, target = 0, rate = 3; };
    struct Layer {
        Track cur, prev;
        float fade = 1, fadeDur = 0;     // cross-fade progress prev -> cur
        float weight = 0, target = 0, weightRate = 5; // layer weight (overlays fade in/out)
        std::vector<uint8_t> mask;
    };
    bool valid(int l) const { return l >= 0 && l < kLayers; }
    void advance(Track& t, float dt) const;
    void sampleTrack(const Track& t, Pose& pose, float w, const std::vector<uint8_t>* mask) const;
    const Model* model_ = nullptr;
    Layer layers_[kLayers];
    std::vector<Additive> additive_;
};

// ---------------------------------------------------------------- procedural

enum ProcState : uint32_t {          // bits for ProcAnim::when
    kProcIdle = 1, kProcMoving = 2, kProcAttacking = 4, kProcGathering = 8, kProcBuilding = 16,
    kProcCarrying = 32, kProcConstructing = 64, kProcProducing = 128, kProcDead = 256, kProcAir = 512,
};
enum ProcEvent : int { kProcEvNone = -1, kProcEvAttack = 0, kProcEvHit, kProcEvSpawn, kProcEvDeath, kProcEvCount };

struct ProcInputs {
    float time = 0, dt = 0;
    float speed = 0;          // world units / s
    float distance = 0;       // travelled so far
    float turnRate = 0;       // rad / s (+ = left)
    float accel = 0;          // units / s^2 along forward
    float aimYaw = 0;         // target direction relative to the body (rad), when hasTarget
    float aimPitch = 0;
    bool hasTarget = false;
    float3 groundNormal{0, 1, 0}; // terrain normal in object space (Align)
    float phase = 0;          // 0..1 per instance (desynchronizes identical units)
    uint32_t state = kProcIdle;
    float eventAge[kProcEvCount] = {1e9f, 1e9f, 1e9f, 1e9f}; // seconds since the last event
};

struct ProcAnim {
    enum Channel : uint8_t { Translate, Rotate, Scale };
    enum Wave : uint8_t { Sine, Abs, Saw, Triangle, Square, Const };
    enum Driver : uint8_t { ByTime, ByDistance };
    enum Special : uint8_t { None, Lean, Aim, AimPitch, Fall, Align };
    std::string nodeName;     // resolved against a model with resolve()
    int node = -1;            // -1: whole object
    Channel channel = Translate;
    float3 axis{0, 1, 0};
    float amp = 0;            // units / radians / scale fraction
    Wave wave = Sine;
    float freq = 1;           // Hz (ByTime) or cycles per travelled unit (ByDistance)
    Driver by = ByTime;
    float phase = 0;          // 0..1 added to the instance phase
    bool randomPhase = true;
    float spin = 0;           // rad / s (or rad per unit with ByDistance): continuous rotation
    int on = kProcEvNone;     // event trigger: amplitude follows an envelope of the event age
    float attack = 0.04f;     // envelope rise (s)
    float decay = 8;          // envelope fall rate (1/s); with Fall: duration
    uint32_t when = 0;        // state gate (0 = always), weight fades in/out
    float fade = 0.25f;
    Special special = None;
    float limit = 3.15f;      // Aim: max angle (rad); Lean: max lean
    float turnSpeed = 4;      // Aim: rad / s
    bool scaleBySpeed = false;// multiply the amplitude by speed / speedRef
    float speedRef = 1;
    void resolve(const Model* m) { node = (m && !nodeName.empty()) ? m->findNode(nodeName) : -1; }
};

struct ProcRuntime { float weight = 0, aim = 0, aimPitch = 0; quat align; };

// Applies modifiers to `pose` (nodes) and to `objectXf` (whole object, model space, post-multiplied).
// `rt` is per-instance state (resized to mods.size()).
void applyProcedural(const std::vector<ProcAnim>& mods, const ProcInputs& in, std::vector<ProcRuntime>& rt, Pose& pose, mat4& objectXf);

} // namespace rts::render
