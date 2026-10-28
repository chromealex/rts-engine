#include "RtsFx.h"
#include <algorithm>
#include <cmath>
#include "../RtsModule.h"
#include "../view/RtsView.h"   // SceneObject (plain struct, no link dependency)

namespace rts::game {

using fx::float3;
using fx::float4;

namespace {

constexpr const char* kCueNames[] = {
    "attack", "hit", "miss", "died", "spawned", "ready", "buildPlaced", "buildDone", "gathered", "captured",
    "levelUp", "ability", "upgraded", "sold", "regionClaimed", "wave", "trail", "damaged",
};
static_assert(sizeof(kCueNames) / sizeof(kCueNames[0]) == size_t(FxCue::Count), "cue names out of sync");
constexpr const char* kCategories[] = {"structure", "air", "unit"};

bool cueFromName(const std::string& s, FxCue& out) {
    for (size_t i = 0; i < size_t(FxCue::Count); ++i)
        if (s == kCueNames[i]) { out = FxCue(i); return true; }
    return false;
}

constexpr int kMaxCombatEffectsPerFrame = 48;   // big battles: bound the work, the budget does the rest

float fl(Fixed f) { return float(f.toDouble()); }
float3 toF(const Vec3& v) { return {fl(v.x), fl(v.y), fl(v.z)}; }

} // namespace

const char* fxCueName(FxCue c) { return size_t(c) < size_t(FxCue::Count) ? kCueNames[size_t(c)] : "?"; }

struct RtsFx::Collector {
    std::vector<SimEvent> events;
    bool enabled = true;
};

RtsFx::Binding RtsFx::resolve(const Json& v, const std::string& where) {
    Binding b;
    b.set = true;
    if (v.isNull()) return b;
    std::string name;
    if (v.isString()) name = v.str();
    else if (v.isObject()) {
        for (auto& [k, x] : v.members())
            if (k != "effect" && k != "height" && k != "scale" && k != "teamColor" && k != "below") warnings_.push_back(where + ": unknown field '" + k + "'");
        name = v["effect"].str();
        b.height = float(v.getFx("height").toDouble());
        b.scale = float(v.getFx("scale", Fixed::fromInt(1)).toDouble());
        b.below = float(v.getFx("below", Fixed::fromRatio(1, 2)).toDouble());
        b.teamColor = v.getBool("teamColor");
    }
    if (name.empty()) { warnings_.push_back(where + ": expected an effect name or { \"effect\": ... }"); return b; }
    const size_t before = ps_->errors().size();
    b.effect = ps_->effect(name);
    if (b.effect < 0) {
        warnings_.push_back(where + ": effect '" + name + "' is missing or invalid");
        for (size_t i = before; i < ps_->errors().size(); ++i) warnings_.push_back("  " + ps_->errors()[i]);
    }
    return b;
}

bool RtsFx::init(fx::ParticleSystem& ps, const Rules& rules, const Json& bank, std::string* error) {
    ps_ = &ps;
    rules_ = &rules;
    warnings_.clear();
    global_ = {};
    category_ = {};
    types_.assign(rules.types.size(), TypeFx{});
    auto fail = [&](const std::string& m) { if (error) *error = "fx.json: " + m; return false; };
    if (bank.isNull()) return true;   // a game without effects
    if (!bank.isObject()) return fail("expected an object");
    for (auto& [k, v] : bank.members())
        if (k != "events" && k != "types" && k != "weapons" && k != "abilities" && k != "comment") warnings_.push_back("fx.json: unknown section '" + k + "'");

    for (auto& [key, v] : bank["events"].members()) {
        const size_t dot = key.find('.');
        FxCue c;
        if (!cueFromName(key.substr(0, dot), c)) { warnings_.push_back("events: unknown cue '" + key + "'"); continue; }
        if (dot == std::string::npos) { global_[size_t(c)] = resolve(v, "events." + key); continue; }
        const std::string cat = key.substr(dot + 1);
        auto it = std::find_if(std::begin(kCategories), std::end(kCategories), [&](const char* n) { return cat == n; });
        if (it == std::end(kCategories)) { warnings_.push_back("events." + key + ": category must be structure, air or unit"); continue; }
        category_[size_t(c)][size_t(it - std::begin(kCategories))] = resolve(v, "events." + key);
    }

    const Json& weaponsById = bank["weapons"];
    const Json& abilitiesById = bank["abilities"];
    for (size_t t = 0; t < rules.types.size(); ++t) {
        const UnitType& ut = rules.types[t];
        TypeFx& tf = types_[t];
        tf.weapons.assign(ut.weapons.size(), {});
        tf.abilities.assign(ut.abilities.size(), {});
        for (size_t w = 0; w < ut.weapons.size(); ++w) {
            const Json& wj = weaponsById[ut.weapons[w].id];
            if (wj.isNull()) continue;
            const std::string where = "weapons." + ut.weapons[w].id;
            auto& wf = tf.weapons[w];
            if (wj.has("attack")) wf.attack = resolve(wj["attack"], where + ".attack");
            if (wj.has("hit")) wf.hit = resolve(wj["hit"], where + ".hit");
            if (wj.has("trail")) wf.trail = resolve(wj["trail"], where + ".trail");
        }
        for (size_t a = 0; a < ut.abilities.size(); ++a)
            if (abilitiesById.has(ut.abilities[a].id)) tf.abilities[a] = resolve(abilitiesById[ut.abilities[a].id], "abilities." + ut.abilities[a].id);
    }

    for (auto& [typeId, tj] : bank["types"].members()) {
        const int t = rules.findType(typeId);
        if (t < 0) { warnings_.push_back("types: unknown type '" + typeId + "'"); continue; }
        if (!tj.isObject()) return fail("types." + typeId + " must be an object");
        TypeFx& tf = types_[size_t(t)];
        const UnitType& ut = rules.types[size_t(t)];
        for (auto& [key, v] : tj.members()) {
            const std::string where = "types." + typeId + "." + key;
            FxCue c;
            if (key == "weapons") {
                for (auto& [wid, wj] : v.members()) {
                    auto it = std::find_if(ut.weapons.begin(), ut.weapons.end(), [&](auto& w) { return w.id == wid; });
                    if (it == ut.weapons.end()) { warnings_.push_back(where + ": type has no weapon '" + wid + "'"); continue; }
                    auto& wf = tf.weapons[size_t(it - ut.weapons.begin())];
                    if (wj.has("attack")) wf.attack = resolve(wj["attack"], where + "." + wid + ".attack");
                    if (wj.has("hit")) wf.hit = resolve(wj["hit"], where + "." + wid + ".hit");
                    if (wj.has("trail")) wf.trail = resolve(wj["trail"], where + "." + wid + ".trail");
                }
            } else if (key == "abilities") {
                for (auto& [aid, aj] : v.members()) {
                    auto it = std::find_if(ut.abilities.begin(), ut.abilities.end(), [&](auto& a) { return a.id == aid; });
                    if (it == ut.abilities.end()) { warnings_.push_back(where + ": type has no ability '" + aid + "'"); continue; }
                    tf.abilities[size_t(it - ut.abilities.begin())] = resolve(aj, where + "." + aid);
                }
            } else if (cueFromName(key, c)) {
                tf.cue[size_t(c)] = resolve(v, where);
            } else {
                warnings_.push_back(where + ": unknown cue");
            }
        }
    }
    return true;
}

const RtsFx::Binding& RtsFx::lookup(int type, FxCue c) const {
    if (type >= 0 && size_t(type) < types_.size()) {
        const Binding& b = types_[size_t(type)].cue[size_t(c)];
        if (b.set) return b;
        const UnitType& ut = rules_->types[size_t(type)];
        const size_t cat = ut.structure ? 0 : (ut.layer & kLayerAir) ? 1 : 2;
        if (category_[size_t(c)][cat].set) return category_[size_t(c)][cat];
    }
    return global_[size_t(c)];
}

const RtsFx::Binding& RtsFx::trailFor(int type) const {
    if (type >= 0 && size_t(type) < types_.size()) {
        const UnitType& ut = rules_->types[size_t(type)];
        for (size_t w = 0; w < ut.weapons.size(); ++w)   // the projectile weapon's trail
            if (ut.weapons[w].projectileSpeed > Fixed{} && types_[size_t(type)].weapons[w].trail.set) return types_[size_t(type)].weapons[w].trail;
    }
    return lookup(type, FxCue::Trail);
}

bool RtsFx::playAt(const Binding& b, const Vec3& pos, int type, float3 dir, int owner) {
    if (!b.valid() || !ps_) return false;
    float3 p = toF(pos);
    float ground = height_ ? height_(p.x, p.z) : 0.0f;
    if (type >= 0 && size_t(type) < rules_->types.size() && (rules_->types[size_t(type)].layer & kLayerAir)) ground += kAirHeight;
    p.y += ground + b.height;
    float4 tint{1, 1, 1, 1};
    if (b.teamColor && owner > 0 && playerColor_) tint = playerColor_(owner);
    if (!ps_->play(b.effect, p, dir, b.scale, tint).valid()) return false;
    ++played_;
    return true;
}

void RtsFx::attach(Simulation& sim, uint8_t viewer) {
    detach();
    viewer_ = viewer;
    collector_ = std::make_shared<Collector>();
    std::weak_ptr<Collector> weak = collector_;
    // Runs last in every tick and only copies events out: nothing is written into the simulation.
    sim.addSystem("fx.collect", [weak](Simulation& s) {
        if (auto c = weak.lock(); c && c->enabled) c->events.insert(c->events.end(), s.events().begin(), s.events().end());
    });
    played_ = 0;
}

void RtsFx::detach() {
    if (collector_) collector_->enabled = false;
    collector_.reset();
    if (ps_) {
        for (auto& [k, a] : trails_) ps_->stop(a.h);
        for (auto& [k, a] : damaged_) ps_->stop(a.h);
    }
    trails_.clear();
    damaged_.clear();
}

void RtsFx::handle(Simulation& sim, const SimEvent& ev) {
    auto* mod = dynamic_cast<RtsModule*>(&sim.module());
    if (!mod) return;
    World& w = sim.world();
    auto typeOf = [&](Entity e) -> int {
        auto* u = w.alive(e) ? w.tryGet<Unit>(e) : nullptr;
        return u ? int(u->type) : -1;
    };
    auto posOf = [&](Entity e, Vec3& out) {
        auto* t = w.alive(e) ? w.tryGet<Transform>(e) : nullptr;
        if (t) out = t->pos;
        return t != nullptr;
    };
    if (viewer_ != 0 && ev.player != viewer_ && !mod->isVisible(sim, viewer_, ev.pos)) return;   // fog of war
    const float3 up{0, 1, 0};
    auto toward = [&](Entity from, Entity to) {
        Vec3 a, b;
        if (!posOf(from, a) || !posOf(to, b)) return up;
        float3 d = toF(b) - toF(a);
        return render::length(d) > 1e-3f ? render::normalize(d) : up;
    };

    switch (ev.type) {
    case kEvAttack: {
        const int t = typeOf(ev.a);
        const Binding* b = nullptr;
        if (t >= 0 && ev.value >= 0 && size_t(ev.value) < types_[size_t(t)].weapons.size() && types_[size_t(t)].weapons[size_t(ev.value)].attack.set)
            b = &types_[size_t(t)].weapons[size_t(ev.value)].attack;
        playAt(b ? *b : lookup(t, FxCue::Attack), ev.pos, t, toward(ev.a, ev.b), ev.player);
        break;
    }
    case kEvHit: {   // a = target, b = source
        const int src = typeOf(ev.b);
        const Binding* b = nullptr;
        if (src >= 0) for (auto& wf : types_[size_t(src)].weapons) if (wf.hit.set) { b = &wf.hit; break; }
        playAt(b ? *b : lookup(src, FxCue::Hit), ev.pos, typeOf(ev.a), toward(ev.a, ev.b), ev.player);
        break;
    }
    case kEvMiss: playAt(lookup(typeOf(ev.a), FxCue::Miss), ev.pos, -1, up, ev.player); break;
    case kEvDied: playAt(lookup(int(ev.value), FxCue::Died), ev.pos, int(ev.value), up, ev.player); break;
    case kEvSpawned: playAt(lookup(int(ev.value), FxCue::Spawned), ev.pos, int(ev.value), up, ev.player); break;
    case kEvTrainDone: playAt(lookup(int(ev.value), FxCue::Ready), ev.pos, int(ev.value), up, ev.player); break;
    case kEvBuildPlaced: playAt(lookup(int(ev.value), FxCue::BuildPlaced), ev.pos, int(ev.value), up, ev.player); break;
    case kEvBuildDone: playAt(lookup(int(ev.value), FxCue::BuildDone), ev.pos, int(ev.value), up, ev.player); break;
    case kEvGathered: playAt(lookup(typeOf(ev.a), FxCue::Gathered), ev.pos, -1, up, ev.player); break;
    case kEvCaptured: playAt(lookup(typeOf(ev.a), FxCue::Captured), ev.pos, typeOf(ev.a), up, ev.player); break;
    case kEvLevelUp: playAt(lookup(typeOf(ev.a), FxCue::LevelUp), ev.pos, typeOf(ev.a), up, ev.player); break;
    case kEvAbility: {
        const int t = typeOf(ev.a);
        const Binding* b = nullptr;
        if (t >= 0 && ev.value >= 0 && size_t(ev.value) < types_[size_t(t)].abilities.size() && types_[size_t(t)].abilities[size_t(ev.value)].set)
            b = &types_[size_t(t)].abilities[size_t(ev.value)];
        playAt(b ? *b : lookup(t, FxCue::Ability), ev.pos, -1, up, ev.player);
        break;
    }
    case kEvUpgraded: playAt(lookup(int(ev.value), FxCue::Upgraded), ev.pos, int(ev.value), up, ev.player); break;
    case kEvSold: playAt(lookup(int(ev.value), FxCue::Sold), ev.pos, int(ev.value), up, ev.player); break;
    case kEvRegionClaimed: playAt(lookup(-1, FxCue::RegionClaimed), ev.pos, -1, up, ev.player); break;
    case kEvWave: if (ev.player != 0) playAt(lookup(-1, FxCue::Wave), ev.pos, -1, up, ev.player); break;
    default: break;
    }
}

void RtsFx::update(Simulation& sim, const std::vector<SceneObject>* scene) {
    if (!ps_) return;
    ++frame_;
    if (collector_) {
        std::vector<SimEvent> events;
        events.swap(collector_->events);
        int combat = 0;
        for (auto& ev : events) {
            const bool isCombat = ev.type == kEvAttack || ev.type == kEvHit || ev.type == kEvMiss;
            if (isCombat && ++combat > kMaxCombatEffectsPerFrame) continue;
            handle(sim, ev);
        }
    }
    if (!scene) return;

    World& w = sim.world();
    for (const SceneObject& o : *scene) {
        if (!o.entity.valid() || !o.visible) continue;
        const uint64_t key = o.entity.packed();
        if (o.isProjectile) {   // trail follows the interpolated projectile
            auto it = trails_.find(key);
            if (it == trails_.end()) {
                auto* pr = w.alive(o.entity) ? w.tryGet<Projectile>(o.entity) : nullptr;
                auto* su = pr && w.alive(pr->source) ? w.tryGet<Unit>(pr->source) : nullptr;
                const Binding& b = trailFor(su ? int(su->type) : -1);
                if (!b.valid()) { trails_[key] = {{}, frame_}; continue; }   // remembered: no lookup every frame
                trails_[key] = {ps_->play(b.effect, o.pos, {0, 1, 0}, b.scale), frame_};
            } else {
                it->second.frame = frame_;
                ps_->move(it->second.h, o.pos);
            }
            continue;
        }
        if (o.build < 1 || o.type >= types_.size()) continue;
        const Binding& b = lookup(int(o.type), FxCue::Damaged);
        if (!b.valid()) continue;
        auto it = damaged_.find(key);
        if (o.hp < b.below && o.hp > 0) {
            float3 p = o.pos;
            p.y += b.height;
            if (it == damaged_.end() || !ps_->playing(it->second.h)) damaged_[key] = {ps_->play(b.effect, p, {0, 1, 0}, b.scale), frame_};
            else { it->second.frame = frame_; ps_->move(it->second.h, p); }
        } else if (it != damaged_.end()) {
            ps_->stop(it->second.h);
            damaged_.erase(it);
        }
    }
    // gone this frame (hit the target, died, went into the fog): stop emitting, particles fade out
    for (auto* m : {&trails_, &damaged_})
        for (auto it = m->begin(); it != m->end();) {
            if (it->second.frame != frame_) { ps_->stop(it->second.h); it = m->erase(it); }
            else ++it;
        }
}

} // namespace rts::game
