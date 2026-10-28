#include "RtsAudio.h"
#include <algorithm>
#include <cmath>
#include "../RtsModule.h"

namespace rts::game {

using audio::kNoSound;
using audio::SoundId;

namespace {

constexpr const char* kCueNames[] = {
    "attack", "hit", "miss", "died", "spawned", "trainStarted", "ready", "buildPlaced", "buildDone", "gathered",
    "captured", "lost", "playerDefeated", "victory", "defeat", "notEnough", "notEnoughSupply", "noRoom",
    "researched", "levelUp", "ability", "readyToPlace", "sold", "regionClaimed", "wave", "lowPower", "upgraded",
    "message", "underAttack", "baseUnderAttack", "select", "move", "attackOrder", "buildOrder", "click", "error",
};
static_assert(sizeof(kCueNames) / sizeof(kCueNames[0]) == size_t(Cue::Count), "cue names out of sync");

bool cueFromName(const std::string& s, Cue& out) {
    for (size_t i = 0; i < size_t(Cue::Count); ++i)
        if (s == kCueNames[i]) { out = Cue(i); return true; }
    return false;
}

constexpr double kAlertCooldown = 12.0;  // seconds between "under attack" announcements
constexpr int kMaxCombatSoundsPerFrame = 16;
constexpr float kBattleEnter = 8.0f;     // intensity = decaying count of hostile hits near the camera
constexpr double kBattleLeave = 15.0;    // seconds of calm before leaving battle music

constexpr float kFlyHeight = 2.4f;     // matches RtsView's flying height

float fl(Fixed f) { return float(f.toDouble()); }

} // namespace

const char* cueName(Cue c) { return size_t(c) < size_t(Cue::Count) ? kCueNames[size_t(c)] : "?"; }

// Shared with the system registered in the simulation, so a destroyed RtsAudio never dangles.
struct RtsAudio::Collector {
    std::vector<SimEvent> events;
    bool enabled = true;
};

SoundId RtsAudio::resolve(const Json& name, const std::string& where) {
    if (name.isNull()) return kNoSound;
    if (!name.isString()) { warnings_.push_back(where + ": expected a sound name"); return kNoSound; }
    SoundId s = engine_->find(name.str());
    if (s == kNoSound) warnings_.push_back(where + ": unknown sound '" + name.str() + "'");
    return s;
}

bool RtsAudio::init(audio::AudioEngine& engine, const Rules& rules, const Json& bank, std::string* error) {
    engine_ = &engine;
    rules_ = &rules;
    warnings_.clear();
    global_.fill(kNoSound);
    auto fail = [&](const std::string& m) { if (error) *error = "audio: " + m; return false; };
    for (const char* section : {"events", "types", "weapons", "abilities"})
        if (!bank[section].isNull() && !bank[section].isObject()) return fail(std::string("\"") + section + "\" must be an object");

    notEnoughRes_.assign(rules.resources.size(), kNoSound);
    for (auto& [key, v] : bank["events"].members()) {
        Cue c;
        if (key.rfind("notEnough.", 0) == 0) {
            const std::string res = key.substr(10);
            auto it = std::find_if(rules.resources.begin(), rules.resources.end(), [&](auto& r) { return r.id == res; });
            if (it == rules.resources.end()) { warnings_.push_back("events." + key + ": unknown resource '" + res + "'"); continue; }
            notEnoughRes_[size_t(it - rules.resources.begin())] = resolve(v, "events." + key);
        } else if (cueFromName(key, c)) {
            global_[size_t(c)] = resolve(v, "events." + key);
        } else {
            warnings_.push_back("events: unknown cue '" + key + "'");
        }
    }

    // weapons / abilities by id, shared by every type that has them
    const Json& weaponsById = bank["weapons"];
    const Json& abilitiesById = bank["abilities"];
    types_.assign(rules.types.size(), TypeSounds{});
    for (size_t t = 0; t < rules.types.size(); ++t) {
        const UnitType& ut = rules.types[t];
        TypeSounds& ts = types_[t];
        ts.cue.fill(kNoSound);
        ts.weapons.assign(ut.weapons.size(), WeaponSounds{});
        ts.abilities.assign(ut.abilities.size(), kNoSound);
        for (size_t w = 0; w < ut.weapons.size(); ++w) {
            const Json& wj = weaponsById[ut.weapons[w].id];
            if (wj.isNull()) continue;
            ts.weapons[w].attack = resolve(wj["attack"], "weapons." + ut.weapons[w].id + ".attack");
            ts.weapons[w].hit = resolve(wj["hit"], "weapons." + ut.weapons[w].id + ".hit");
        }
        for (size_t a = 0; a < ut.abilities.size(); ++a)
            ts.abilities[a] = resolve(abilitiesById[ut.abilities[a].id], "abilities." + ut.abilities[a].id);
    }

    for (auto& [typeId, tj] : bank["types"].members()) {
        const int t = rules.findType(typeId);
        if (t < 0) { warnings_.push_back("types: unknown type '" + typeId + "'"); continue; }
        if (!tj.isObject()) return fail("types." + typeId + " must be an object");
        TypeSounds& ts = types_[size_t(t)];
        const UnitType& ut = rules.types[size_t(t)];
        for (auto& [key, v] : tj.members()) {
            const std::string where = "types." + typeId + "." + key;
            Cue c;
            if (key == "weapons") {
                for (auto& [wid, wj] : v.members()) {
                    auto it = std::find_if(ut.weapons.begin(), ut.weapons.end(), [&](auto& w) { return w.id == wid; });
                    if (it == ut.weapons.end()) { warnings_.push_back(where + ": type has no weapon '" + wid + "'"); continue; }
                    WeaponSounds& ws = ts.weapons[size_t(it - ut.weapons.begin())];
                    if (wj.has("attack")) ws.attack = resolve(wj["attack"], where + "." + wid + ".attack");
                    if (wj.has("hit")) ws.hit = resolve(wj["hit"], where + "." + wid + ".hit");
                }
            } else if (key == "abilities") {
                for (auto& [aid, aj] : v.members()) {
                    auto it = std::find_if(ut.abilities.begin(), ut.abilities.end(), [&](auto& a) { return a.id == aid; });
                    if (it == ut.abilities.end()) { warnings_.push_back(where + ": type has no ability '" + aid + "'"); continue; }
                    ts.abilities[size_t(it - ut.abilities.begin())] = resolve(aj, where + "." + aid);
                }
            } else if (cueFromName(key, c)) {
                ts.cue[size_t(c)] = resolve(v, where);
            } else {
                warnings_.push_back(where + ": unknown cue");
            }
        }
    }
    return true;
}

SoundId RtsAudio::cueFor(int type, Cue c) const {
    if (type >= 0 && size_t(type) < types_.size()) {
        SoundId s = types_[size_t(type)].cue[size_t(c)];
        if (s != kNoSound) return s;
    }
    return global_[size_t(c)];
}

bool RtsAudio::playAt(SoundId s, const Vec3& pos, bool positional, float height) {
    if (s == kNoSound || !engine_) return false;
    audio::PlayParams p;
    p.positional = positional;
    p.x = fl(pos.x);
    p.y = height;
    p.z = fl(pos.z);
    if (engine_->play(s, p) == audio::kNoVoice) return false;
    ++played_;
    return true;
}

void RtsAudio::attach(Simulation& sim, uint8_t viewer) {
    detach();
    viewer_ = viewer;
    collector_ = std::make_shared<Collector>();
    std::weak_ptr<Collector> weak = collector_;
    // Runs last in every tick and only copies events out: nothing is written into the simulation,
    // so checksums and replays are unaffected (covered by a determinism test).
    sim.addSystem("audio.collect", [weak](Simulation& s) {
        if (auto c = weak.lock(); c && c->enabled) c->events.insert(c->events.end(), s.events().begin(), s.events().end());
    });
    time_ = 0;
    lastAlert_ = -1e9;
    intensity_ = 0;
    calmSince_ = 0;
    inBattle_ = false;
    played_ = 0;
    if (engine_ && !engine_->playMusic("match")) engine_->stopMusic(1.0f);
}

void RtsAudio::detach() {
    if (collector_) collector_->enabled = false;
    collector_.reset();
}

void RtsAudio::handle(Simulation& sim, const SimEvent& ev, bool worldSounds) {
    auto* mod = dynamic_cast<RtsModule*>(&sim.module());
    if (!mod) return;
    World& w = sim.world();
    const bool mine = viewer_ != 0 && ev.player == viewer_;
    auto typeOf = [&](Entity e) -> int {
        auto* u = w.alive(e) ? w.tryGet<Unit>(e) : nullptr;
        return u ? int(u->type) : -1;
    };
    auto ownerOf = [&](Entity e) -> int {
        auto* o = w.alive(e) ? w.tryGet<Owner>(e) : nullptr;
        return o ? int(o->player) : -1;
    };
    auto seen = [&](const Vec3& p) { return viewer_ == 0 || mine || mod->isVisible(sim, viewer_, p); };
    // the sim is flat: height comes from the cliff level (like the view draws it) and flying units
    const int heightType = ev.type == kEvDied ? int(ev.value) : typeOf(ev.a);
    float y = float(mod->levelAt(sim, ev.pos)) * fl(rules_->levelHeight);
    if (heightType >= 0 && size_t(heightType) < rules_->types.size() && (rules_->types[size_t(heightType)].layer & kLayerAir)) y += kFlyHeight;
    auto world = [&](SoundId s) { if (worldSounds && seen(ev.pos)) playAt(s, ev.pos, true, y); };
    auto own = [&](Cue c, int type, bool positional) { if (mine) playAt(cueFor(type, c), ev.pos, positional, y); };
    auto either = [&](Cue first, Cue fallback) { SoundId s = cueFor(-1, first); return s != kNoSound ? s : cueFor(-1, fallback); };

    switch (ev.type) {
    case kEvAttack: {
        const int t = typeOf(ev.a);
        SoundId s = kNoSound;
        if (t >= 0 && ev.value >= 0 && size_t(ev.value) < types_[size_t(t)].weapons.size()) s = types_[size_t(t)].weapons[size_t(ev.value)].attack;
        world(s != kNoSound ? s : cueFor(t, Cue::Attack));
        break;
    }
    case kEvHit: {
        const int src = typeOf(ev.b);
        SoundId s = kNoSound;
        if (src >= 0) for (auto& ws : types_[size_t(src)].weapons) if (ws.hit != kNoSound) { s = ws.hit; break; }
        world(s != kNoSound ? s : cueFor(src, Cue::Hit));
        if (!engine_) break;
        const float heard = engine_->audibility(audio::kNoSound, fl(ev.pos.x), y, fl(ev.pos.z));
        if (heard > 0.3f && seen(ev.pos)) intensity_ += 1.0f;
        // the viewer's unit is hit by an enemy somewhere the camera isn't looking
        const int victim = ownerOf(ev.a);
        if (viewer_ != 0 && victim == viewer_ && ev.player != viewer_ && mod->hostile(sim, viewer_, ev.player) &&
            heard < 0.99f && time_ - lastAlert_ >= kAlertCooldown) {
            const int vt = typeOf(ev.a);
            SoundId alert = vt >= 0 && rules_->types[size_t(vt)].structure ? cueFor(vt, Cue::BaseUnderAttack) : kNoSound;
            if (alert == kNoSound) alert = cueFor(vt, Cue::UnderAttack);
            if (playAt(alert, ev.pos, false)) lastAlert_ = time_;
        }
        break;
    }
    case kEvMiss: world(cueFor(typeOf(ev.a), Cue::Miss)); break;
    case kEvDied: world(cueFor(int(ev.value), Cue::Died)); break;
    case kEvSpawned: world(cueFor(int(ev.value), Cue::Spawned)); break;
    case kEvAbility: {
        const int t = typeOf(ev.a);
        SoundId s = kNoSound;
        if (t >= 0 && ev.value >= 0 && size_t(ev.value) < types_[size_t(t)].abilities.size()) s = types_[size_t(t)].abilities[size_t(ev.value)];
        world(s != kNoSound ? s : cueFor(t, Cue::Ability));
        break;
    }
    case kEvTrainStarted: own(Cue::TrainStarted, int(ev.value), false); break;
    case kEvTrainDone: own(Cue::Ready, int(ev.value), false); break;
    case kEvBuildPlaced: own(Cue::BuildPlaced, int(ev.value), true); break;
    case kEvBuildDone: own(Cue::BuildDone, int(ev.value), false); break;
    case kEvGathered: own(Cue::Gathered, typeOf(ev.a), true); break;
    case kEvResearched: own(Cue::Researched, -1, false); break;
    case kEvLevelUp: own(Cue::LevelUp, typeOf(ev.a), false); break;
    case kEvReadyToPlace: own(Cue::ReadyToPlace, int(ev.value), false); break;
    case kEvSold: own(Cue::Sold, int(ev.value), false); break;
    case kEvRegionClaimed: own(Cue::RegionClaimed, -1, false); break;
    case kEvLowPower: own(Cue::LowPower, -1, false); break;
    case kEvUpgraded: own(Cue::Upgraded, int(ev.value), false); break;
    case kEvNotEnough:
        if (!mine) break;
        if (ev.value == 255) playAt(either(Cue::NotEnoughSupply, Cue::NotEnough), ev.pos, false);
        else if (ev.value == 254) playAt(either(Cue::NoRoom, Cue::Error), ev.pos, false);
        else {
            SoundId s = ev.value >= 0 && size_t(ev.value) < notEnoughRes_.size() ? notEnoughRes_[size_t(ev.value)] : kNoSound;
            playAt(s != kNoSound ? s : cueFor(-1, Cue::NotEnough), ev.pos, false);
        }
        break;
    case kEvCaptured: // player = new owner, value = previous owner
        if (mine) playAt(cueFor(typeOf(ev.a), Cue::Captured), ev.pos, false);
        else if (viewer_ != 0 && ev.value == viewer_) playAt(cueFor(typeOf(ev.a), Cue::Lost), ev.pos, false);
        else world(cueFor(-1, Cue::Captured)); // a type's own "captured" is the viewer's announcement
        break;
    case kEvPlayerDefeated:
        if (ev.player != viewer_) playAt(cueFor(-1, Cue::PlayerDefeated), ev.pos, false);
        break;
    case kEvWave: playAt(cueFor(-1, Cue::Wave), ev.pos, false); break;
    case kEvMessage:
        if (ev.player == 0 || mine) playAt(cueFor(-1, Cue::Message), ev.pos, false);
        break;
    default: break;
    }
}

void RtsAudio::update(Simulation& sim, double dt) {
    time_ += dt;
    if (!collector_ || !engine_) return;
    std::vector<SimEvent> events;
    events.swap(collector_->events);
    // A flood (big battle, several ticks in one frame) is capped here only to bound the work per
    // frame; per-sound cooldowns and instance limits do the audible thinning.
    int combat = 0;
    for (auto& ev : events) {
        const bool isCombat = ev.type == kEvAttack || ev.type == kEvHit || ev.type == kEvMiss;
        handle(sim, ev, !isCombat || ++combat <= kMaxCombatSoundsPerFrame);
    }

    // battle music: in when fighting is heavy near the camera, out after a calm period
    intensity_ *= float(std::exp(-dt / 4.0));
    if (intensity_ > 1.0f) calmSince_ = time_;
    if (!inBattle_ && intensity_ > kBattleEnter && engine_->hasMusic("battle") && engine_->currentMusic() == "match") {
        inBattle_ = engine_->playMusic("battle", 2.0f);
    } else if (inBattle_ && time_ - calmSince_ > kBattleLeave) {
        inBattle_ = false;
        engine_->playMusic("match", 4.0f);
    }
}

void RtsAudio::onSelect(Simulation& sim, Entity unit) {
    auto* u = sim.world().alive(unit) ? sim.world().tryGet<Unit>(unit) : nullptr;
    if (u) playAt(cueFor(int(u->type), Cue::Select), {}, false);
}

void RtsAudio::onCommand(Simulation& sim, Entity firstUnit, uint16_t commandType) {
    auto* u = sim.world().alive(firstUnit) ? sim.world().tryGet<Unit>(firstUnit) : nullptr;
    if (!u) return;
    // only orders a unit would answer to; production, research, selling etc. have their own cues
    Cue c;
    switch (commandType) {
    case kCmdMove: case kCmdStop: case kCmdHold: case kCmdPatrol: case kCmdGather: case kCmdSmart:
    case kCmdSend: case kCmdDeploy: case kCmdRally: c = Cue::Move; break;
    case kCmdAttack: c = Cue::AttackOrder; break;
    case kCmdBuild: c = Cue::BuildOrder; break;
    default: return;
    }
    const int t = int(u->type);
    SoundId s = cueFor(t, c);
    if (s == kNoSound) s = cueFor(t, Cue::Move);
    playAt(s, {}, false);
}

void RtsAudio::onMatchEnd(bool won) {
    play(won ? Cue::Victory : Cue::Defeat);
    if (!engine_) return;
    const char* m = won ? "victory" : "defeat";
    if (engine_->hasMusic(m)) engine_->playMusic(m, 1.0f);
    else engine_->stopMusic(2.0f);
}

void RtsAudio::play(Cue c) { playAt(cueFor(-1, c), {}, false); }

audio::ListenerPose RtsAudio::cameraListener(const float target[3], const float eye[3], float referenceDistance) {
    float v[3] = {eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]};
    const float dist = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    audio::ListenerPose pose;
    if (dist < 1e-4f) { std::copy(target, target + 3, pose.pos); return pose; }
    for (float& c : v) c /= dist;
    const float h = std::clamp(dist * 0.55f, 3.0f, 60.0f);
    for (int i = 0; i < 3; ++i) { pose.pos[i] = target[i] + v[i] * h; pose.forward[i] = -v[i]; }
    pose.hearing = std::max(1.0f, std::sqrt(dist / std::max(1.0f, referenceDistance)));
    return pose;
}

} // namespace rts::game
