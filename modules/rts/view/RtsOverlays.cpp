#include "RtsOverlays.h"
#include "RtsOverlayShaders.h"
#include "RtsView.h"   // SceneObject / RtsScene (data only: nothing here links the renderer)
#include "../RtsModule.h"
#include "../../../engine/platform/Platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rts::game {

using namespace render;

namespace {
float3 toF(const Vec3& v) { return {float(v.x.toDouble()), float(v.y.toDouble()), float(v.z.toDouble())}; }
bool contains(const std::vector<uint16_t>& v, uint16_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }
RtsModule* moduleOf(Simulation* sim) { return sim ? dynamic_cast<RtsModule*>(&sim->module()) : nullptr; }
// stable pseudo-random in [-1, 1] (popup jitter): presentation only
float hash01(uint32_t a, uint32_t b) {
    uint32_t h = a * 2654435761u ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2));
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return float(h & 0xffff) / 32767.5f - 1.0f;
}
void appendNumber(std::string& out, double v) {
    char buf[32];
    if (std::fabs(v - std::round(v)) < 0.05) std::snprintf(buf, sizeof buf, "%lld", (long long)std::llround(v));
    else std::snprintf(buf, sizeof buf, "%.1f", v);
    out += buf;
}
} // namespace

// ------------------------------------------------------------------ setup

void RtsOverlays::init(const Rules& rules, const Json& rulesOverlays, const Json& gameOverlays, const std::string& contentBase) {
    rules_ = &rules;
    base_ = contentBase;
    set_ = loadOverlaySet(rules, rulesOverlays, gameOverlays);
    state_.assign(set_.defs.size(), {});
    popups_.clear();
    lastTick_ = ~0u;
    worldDefs_.clear(); screenEntityDefs_.clear(); screenOtherDefs_.clear();
    for (size_t i = 0; i < set_.defs.size(); ++i) {
        const OverlayDef& d = set_.defs[i];
        const bool ent = d.anchor == OvAnchor::Selected || d.anchor == OvAnchor::Hovered || d.anchor == OvAnchor::Entities;
        if (d.style.space != OvSpace::Screen) worldDefs_.push_back(uint16_t(i));
        else if (ent) screenEntityDefs_.push_back(uint16_t(i));
        else screenOtherDefs_.push_back(uint16_t(i));
    }
    if (const char* q = std::getenv("RTS_OVERLAYS")) {
        std::string s = q;
        if (s == "low") setQuality(OverlayQuality::Low);
        else if (s == "medium") setQuality(OverlayQuality::Medium);
        else if (s == "high") setQuality(OverlayQuality::High);
    }
    images_.assign(set_.defs.size(), {});
}

// ------------------------------------------------------------------ frame state

void RtsOverlays::prepare(const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid, float zoom) {
    scene_ = &s; grid_ = &grid; vision_ = vision; time_ = time; zoom_ = zoom;
    selected_.clear();
    hovered_ = -1;
    bases_.clear();
    for (size_t i = 0; i < s.objects.size(); ++i) {
        const SceneObject& o = s.objects[i];
        if (o.isProjectile || !o.visible || o.type >= rules_->types.size()) continue;
        if (o.selected) selected_.push_back(int(i));
        if (o.hovered && hovered_ < 0) hovered_ = int(i);
        const UnitType& t = rules_->types[o.type];
        if (rules_->buildRadius > 0 && t.structure && t.extendsBase && o.owner == ctx_.player && ctx_.player != 0)
            bases_.push_back({o.pos.x, o.pos.z, float(rules_->buildRadius) + float(t.radius.toDouble())});
    }
    processEvents(s);
}

uint8_t RtsOverlays::relation(uint8_t owner) const {
    if (owner == 0) return kRelNeutral;
    if (ctx_.player != 0 && owner == ctx_.player) return kRelOwn;
    if (ctx_.player != 0 && owner != kHostileNeutral && ctx_.sim && ctx_.sim->allied(owner, ctx_.player)) return kRelAlly;
    return kRelEnemy;
}

uint32_t RtsOverlays::entityFlags(const SceneObject& o) const {
    if (o.type >= rules_->types.size()) return 0;
    const UnitType& t = rules_->types[o.type];
    uint32_t f = t.structure ? kOvStructure : kOvUnit;
    if (o.build < 1.0f) f |= kOvConstructing;
    if (o.hp < 0.999f) f |= kOvDamaged;
    if (o.garrison >= 0) f |= kOvGarrison;
    if (o.moving) f |= kOvMoving;
    if (o.producing) f |= kOvProducing;
    if (!o.moving && !o.order && !o.producing && !o.hasTarget) f |= kOvIdle;
    if (t.hero) f |= kOvHero;
    if (t.worker) f |= kOvWorker;
    if (t.invulnerable) f |= kOvInvulnerable;
    if (t.layer & kLayerAir) f |= kOvAir;
    if (o.carry > 0) f |= kOvCarrying;
    if (o.hasTarget) f |= kOvAttacking;
    if (t.resourceNode >= 0) f |= kOvResource;
    if (t.maxEnergy.raw > 0) f |= kOvEnergy;
    for (auto& wd : t.weapons) if (wd.range > Fixed::fromRatio(3, 2)) { f |= kOvRanged; break; }
    for (auto& ab : t.abilities) if (ab.aura) { f |= kOvAura; break; }
    return f;
}

bool RtsOverlays::matches(const OvFilter& f, const SceneObject& o) const {
    if (o.type >= rules_->types.size()) return false;
    if (!f.types.empty() || f.tags) {
        if (!contains(f.types, o.type) && !(f.tags & rules_->types[o.type].tags)) return false;
    }
    if (f.relations && !(f.relations & relation(o.owner))) return false;
    if (f.flagsOn || f.flagsOff) {
        const uint32_t fl = entityFlags(o);
        if ((fl & f.flagsOn) != f.flagsOn || (fl & f.flagsOff)) return false;
    }
    if (o.hp < f.hpMin || o.hp >= f.hpMax) return false;
    if (!f.statuses.empty()) {
        if (!ctx_.sim || !o.entity.valid()) return false;
        auto* st = ctx_.sim->world().tryGet<Statuses>(o.entity);
        if (!st) return false;
        bool any = false;
        for (auto& si : st->list) if (std::find(f.statuses.begin(), f.statuses.end(), int16_t(si.def)) != f.statuses.end()) { any = true; break; }
        if (!any) return false;
    }
    return true;
}

namespace {
bool keyHeld(const OverlayContext& c, uint16_t k) {
    using platform::Key;
    auto at = [&](Key key) { return int(key) < c.keyCount && c.keys[int(key)]; };
    if (!c.keys) return false;
    switch (k) {
    case kOvKeyShift: return at(Key::LeftShift) || at(Key::RightShift);
    case kOvKeyCtrl: return at(Key::LeftCtrl) || at(Key::RightCtrl);
    case kOvKeyAlt: return at(Key::LeftAlt) || at(Key::RightAlt);
    case kOvKeySuper: return at(Key::LeftSuper) || at(Key::RightSuper);
    default: return k < c.keyCount && c.keys[k];
    }
}
} // namespace

bool RtsOverlays::test(const OvCondition& c, const SceneObject* o) const {
    switch (c.kind) {
    case OvCondition::All: for (auto& k : c.kids) if (!test(k, o)) return false; return true;
    case OvCondition::Any: for (auto& k : c.kids) if (test(k, o)) return true; return false;
    case OvCondition::Not: return c.kids.empty() || !test(c.kids[0], o);
    case OvCondition::Always: return true;
    case OvCondition::Never: return false;
    case OvCondition::Placing: {
        if (ctx_.placeType < 0 || size_t(ctx_.placeType) >= rules_->types.size()) return false;
        if (c.filter.types.empty() && !c.filter.tags) return true;
        return contains(c.filter.types, uint16_t(ctx_.placeType)) || (c.filter.tags & rules_->types[size_t(ctx_.placeType)].tags);
    }
    case OvCondition::Selected: case OvCondition::Hovered: {
        const bool sel = c.kind == OvCondition::Selected;
        if (o) {
            const bool on = sel ? o->selected : o->hovered;
            if (!c.want) return !on;
            return on && matches(c.filter, *o);
        }
        bool any = false;
        if (scene_) {
            if (sel) { for (int i : selected_) if (matches(c.filter, scene_->objects[size_t(i)])) { any = true; break; } }
            else any = hovered_ >= 0 && matches(c.filter, scene_->objects[size_t(hovered_)]);
        }
        return any == c.want;
    }
    case OvCondition::Entity: {
        if (o) return matches(c.filter, *o);
        if (!scene_) return false;
        for (auto& so : scene_->objects) if (so.visible && !so.isProjectile && matches(c.filter, so)) return true;
        return false;
    }
    case OvCondition::Key: for (uint16_t k : c.keys) if (!keyHeld(ctx_, k)) return false; return true;
    case OvCondition::State: return (c.states & (ctx_.over ? 4 : ctx_.paused ? 2 : 1)) != 0;
    case OvCondition::Var: {
        double v = 0;
        if (RtsModule* m = moduleOf(ctx_.sim); m && !c.names.empty()) {
            auto& vars = m->state(*ctx_.sim).trig.vars;
            auto it = vars.find(c.names[0]);
            if (it != vars.end()) v = it->second.toDouble();
        }
        switch (c.cmp) {
        case 1: return v == c.value;
        case 2: return v != c.value;
        case 3: return v < c.value;
        case 4: return v <= c.value;
        case 5: return v > c.value;
        case 6: return v >= c.value;
        default: return v != 0;
        }
    }
    case OvCondition::Zoom: return zoom_ >= c.lo && zoom_ <= c.hi;
    case OvCondition::Cursor: return ctx_.hasCursor == c.want;
    case OvCondition::Mode: for (auto& n : c.names) if (n == (ctx_.mode ? ctx_.mode : "normal")) return true; return false;
    }
    return false;
}

bool RtsOverlays::cellTest(OvPred p, int x, int y) const {
    const MapGrid& g = *grid_;
    if (!g.inside(x, y)) return false;
    const size_t i = size_t(g.index(x, y));
    const uint8_t pl = ctx_.player;
    auto vis = [&]() -> uint8_t {
        if (!vision_ || vision_->size() <= i) return 2;
        return (*vision_)[i];
    };
    switch (p) {
    case OvPred::None: case OvPred::Ok: return true;
    case OvPred::Buildable: case OvPred::Blocked: {
        bool ok = g.areaBuildable(x, y, 1, 1) && vis() >= 1;
        if (ok && ctx_.placeType >= 0 && size_t(ctx_.placeType) < rules_->types.size()) {
            const UnitType& t = rules_->types[size_t(ctx_.placeType)];
            if (t.terrainReq == TerrainReq::Rock && !(g.flags[i] & kCellRock)) ok = false;
            if (t.terrainReq == TerrainReq::Creep && (g.creep.size() <= i || !g.creep[i])) ok = false;
        }
        if (ok && rules_->territory.enabled && rules_->territory.buildOnlyOwned && pl && g.territory.size() > i && g.territory[i] != pl) ok = false;
        if (ok && rules_->buildRadius > 0 && !bases_.empty()) ok = cellTest(OvPred::BuildRadius, x, y);
        return p == OvPred::Buildable ? ok : !ok;
    }
    case OvPred::BuildRadius: {
        if (rules_->buildRadius <= 0) return true;
        const float cx = x + 0.5f, cz = y + 0.5f;
        float extra = 0;
        if (ctx_.placeType >= 0 && size_t(ctx_.placeType) < rules_->types.size()) extra = float(rules_->types[size_t(ctx_.placeType)].radius.toDouble());
        for (auto& b : bases_) {
            const float dx = cx - b.x, dz = cz - b.z, r = b.r + extra;
            if (dx * dx + dz * dz <= r * r) return true;
        }
        return false;
    }
    case OvPred::Walkable: return (g.flags[i] & kCellGround) && (g.blocked.size() <= i || !g.blocked[i]);
    case OvPred::Water: return (g.flags[i] & kCellWater) != 0;
    case OvPred::Resource: return g.fieldRes.size() > i && g.fieldRes[i] >= 0 && g.fieldAmt[i].raw > 0 && vis() >= 1;
    case OvPred::Creep: return g.creep.size() > i && g.creep[i] != 0;
    case OvPred::Road: return (g.flags[i] & kCellRoad) != 0;
    case OvPred::Region: return g.region.size() > i && g.region[i] >= 0;
    case OvPred::Territory: return pl && g.territory.size() > i && g.territory[i] == pl && vis() >= 1;
    case OvPred::EnemyTerritory: return g.territory.size() > i && g.territory[i] && (relation(g.territory[i]) & kRelEnemy) && vis() >= 1;
    case OvPred::AnyTerritory: case OvPred::TerritoryEdge: return g.territory.size() > i && g.territory[i] && vis() >= 1;
    case OvPred::Visible: return vis() == 2;
    case OvPred::Explored: return vis() >= 1;
    case OvPred::Fogged: return vis() == 1;
    case OvPred::Unexplored: return vis() == 0;
    }
    return false;
}

// ------------------------------------------------------------------ events -> popups

void RtsOverlays::processEvents(const RtsScene& s) {
    // expire popups (compaction keeps the vector's storage)
    popups_.erase(std::remove_if(popups_.begin(), popups_.end(), [&](const Popup& p) {
        return p.def >= set_.defs.size() || time_ - p.born > double(set_.defs[p.def].event.lifetime); }), popups_.end());
    const size_t globalCap = quality_ == OverlayQuality::Low ? 24 : quality_ == OverlayQuality::Medium ? 96 : 256;
    auto spawn = [&](size_t di, uint16_t kind, uint8_t player, Entity a, int64_t value, Vec3 pos, uint32_t salt) {
        const OverlayDef& d = set_.defs[di];
        if (!d.enabled || !d.hasEvent || !contains(d.event.kinds, kind)) return;
        uint8_t owner = player;
        uint16_t type = kNoType;
        World* w = ctx_.sim ? &ctx_.sim->world() : nullptr;
        if (w && a.valid()) {
            if (auto* ow = w->tryGet<Owner>(a)) owner = ow->player;
            if (auto* u = w->tryGet<Unit>(a)) type = u->type;
            if (pos.x.raw == 0 && pos.z.raw == 0) if (auto* tr = w->tryGet<Transform>(a)) pos = tr->pos;
        }
        if (kind == kEvDied && value >= 0 && size_t(value) < rules_->types.size()) type = uint16_t(value);
        if (d.event.by && !(d.event.by & relation(player))) return;
        if (d.event.on && !(d.event.on & relation(owner))) return;
        if (!d.event.types.types.empty() || d.event.types.tags) {
            if (type >= rules_->types.size()) return;
            if (!contains(d.event.types.types, type) && !(d.event.types.tags & rules_->types[type].tags)) return;
        }
        if (grid_ && vision_ && ctx_.player) {   // never reveal what the fog hides
            const int cx = int(pos.x.floorInt()), cy = int(pos.z.floorInt());
            if (!grid_->inside(cx, cy) || vision_->size() <= size_t(grid_->index(cx, cy)) || (*vision_)[size_t(grid_->index(cx, cy))] != 2) return;
        }
        int live = 0;
        for (auto& p : popups_) if (p.def == di) ++live;
        if (live >= d.event.max || (!d.builtin && popups_.size() >= globalCap)) return;
        Popup p;
        p.def = uint16_t(di); p.kind = kind; p.born = time_; p.value = value; p.owner = owner; p.player = player; p.type = type;
        p.pos = toF(pos);
        if (kind == kEvHit) p.amount = float(Fixed::fromRaw(value).toDouble());
        if (kind == kEvGathered) {
            p.res = int16_t(value);
            auto it = carry_.find(a.packed());
            if (it != carry_.end()) p.amount = it->second.first;
        }
        if (d.event.spread > 0) { p.jx = hash01(salt, 1) * d.event.spread; p.jz = hash01(salt, 2) * d.event.spread; }
        popups_.push_back(p);
    };
    if (s.tick != lastTick_) {
        lastTick_ = s.tick;
        for (size_t ei = 0; ei < s.events.size(); ++ei) {
            const SimEvent& ev = s.events[ei];
            for (size_t di = 0; di < set_.defs.size(); ++di)
                if (set_.defs[di].hasEvent) spawn(di, ev.type, ev.player, ev.a, ev.value, ev.pos, s.tick * 977u + uint32_t(ei));
        }
    }
    if (ctx_.pings)
        for (size_t pi = 0; pi < ctx_.pings->size(); ++pi) {
            const OverlayPing& pg = (*ctx_.pings)[pi];
            Vec3 pos{Fixed::fromRatio(int64_t(pg.pos.x * 256), 256), Fixed{}, Fixed::fromRatio(int64_t(pg.pos.z * 256), 256)};
            for (size_t di = 0; di < set_.defs.size(); ++di)
                if (set_.defs[di].hasEvent) spawn(di, kOvEventPing, pg.player, kNullEntity, 0, pos, uint32_t(time_ * 1000) + uint32_t(pi));
        }
    carry_.clear();
    for (auto& o : s.objects) if (o.entity.valid() && o.carry > 0) carry_[o.entity.packed()] = {o.carry, o.carryRes};
    spawnPreviewPopups();
}

bool RtsOverlays::previewed(const OverlayDef& d) const {
    if (!ctx_.preview) return false;
    for (auto& id : *ctx_.preview) if (id == d.id) return true;
    return false;
}

// Editor preview of event overlays: a sample popup at the cursor (or the first object) every lifetime.
void RtsOverlays::spawnPreviewPopups() {
    if (!ctx_.preview || ctx_.preview->empty() || !scene_) return;
    for (size_t di = 0; di < set_.defs.size(); ++di) {
        const OverlayDef& d = set_.defs[di];
        if (!d.hasEvent || !previewed(d)) continue;
        bool live = false;
        for (auto& p : popups_) if (p.def == di) { live = true; break; }
        if (live) continue;
        Popup p;
        p.def = uint16_t(di); p.born = time_; p.kind = d.event.kinds.empty() ? 0 : d.event.kinds[0];
        p.value = 12; p.amount = 12; p.res = 0; p.owner = p.player = ctx_.player;
        if (ctx_.hasCursor) p.pos = ctx_.cursor;
        else if (!scene_->objects.empty()) { p.pos = scene_->objects[0].pos; p.type = scene_->objects[0].type; }
        else if (grid_) p.pos = {grid_->w * 0.5f, 0, grid_->h * 0.5f};
        popups_.push_back(p);
    }
}

// ------------------------------------------------------------------ anchors

size_t RtsOverlays::instCap(const OverlayDef& d) const {
    if (d.builtin) return SIZE_MAX;
    return quality_ == OverlayQuality::Low ? 48 : quality_ == OverlayQuality::Medium ? 192 : 1024;
}

bool RtsOverlays::defVisible(const OverlayDef& d, size_t di, bool cond, float& alpha) {
    DefState& st = state_[di];
    alpha = 1;
    if (cond) {
        if (!st.on) { st.on = true; st.since = time_; }
        st.lastTrue = time_;
        if (d.style.fadeIn > 0) alpha = float(std::clamp((time_ - st.since) / d.style.fadeIn, 0.0, 1.0));
        return true;
    }
    st.on = false;
    if (d.style.fadeOut > 0 && st.lastTrue >= 0 && time_ - st.lastTrue < d.style.fadeOut) {
        alpha = float(1.0 - (time_ - st.lastTrue) / d.style.fadeOut);
        return true;
    }
    return false;
}

bool RtsOverlays::gather(const OverlayDef& d, size_t di, bool screenPass) {
    insts_.clear();
    pathPts_.clear();
    if (!scene_ || !rules_) return false;
    const RtsScene& s = *scene_;
    const bool pv = previewed(d);
    const bool ent = d.anchor == OvAnchor::Selected || d.anchor == OvAnchor::Hovered || d.anchor == OvAnchor::Entities;
    float defAlpha = 1;
    if (!ent && d.anchor != OvAnchor::Event) {
        if (!defVisible(d, di, pv || test(d.when, nullptr), defAlpha)) return false;
    } else if (d.anchor == OvAnchor::Event) {
        if (!pv && !test(d.when, nullptr)) return false;
    }
    const size_t cap = instCap(d);
    auto addObj = [&](int i) {
        if (insts_.size() >= cap) return;
        const SceneObject& o = s.objects[size_t(i)];
        if (!matches(d.anchorFilter, o) || (!pv && !test(d.when, &o))) return;
        Inst in;
        in.pos = o.pos; in.obj = i; in.type = o.type; in.owner = o.owner;
        insts_.push_back(in);
    };
    Inst one;
    one.owner = ctx_.player;
    World* w = ctx_.sim ? &ctx_.sim->world() : nullptr;
    switch (d.anchor) {
    case OvAnchor::Selected:
        for (int i : selected_) addObj(i);
        if (pv && selected_.empty())
            for (size_t i = 0; i < s.objects.size(); ++i) if (s.objects[i].visible && !s.objects[i].isProjectile) addObj(int(i));
        break;
    case OvAnchor::Hovered: if (hovered_ >= 0) addObj(hovered_); break;
    case OvAnchor::Entities:
        for (size_t i = 0; i < s.objects.size(); ++i) if (s.objects[i].visible && !s.objects[i].isProjectile) addObj(int(i));
        break;
    case OvAnchor::Cursor:
        if (!ctx_.hasCursor) return false;
        one.pos = ctx_.cursor;
        if (d.snap) { one.pos.x = std::floor(one.pos.x) + 0.5f; one.pos.z = std::floor(one.pos.z) + 0.5f; }
        insts_.push_back(one);
        break;
    case OvAnchor::Ghost:
        if (ctx_.placeType < 0 || size_t(ctx_.placeType) >= rules_->types.size()) return false;
        one.pos = ctx_.placeAt; one.type = uint16_t(ctx_.placeType); one.ok = ctx_.placeOk;
        insts_.push_back(one);
        break;
    case OvAnchor::Area: {
        float r[4] = {d.rect[0], d.rect[1], d.rect[2], d.rect[3]};
        if (!d.areaName.empty()) {
            bool found = false;
            if (RtsModule* m = moduleOf(ctx_.sim)) {
                int ai = m->triggers().findArea(d.areaName);
                if (ai >= 0) {
                    const TrigArea& a = m->triggers().areas[size_t(ai)];
                    r[0] = float(a.x.toDouble()); r[1] = float(a.z.toDouble()); r[2] = float(a.w.toDouble()); r[3] = float(a.h.toDouble());
                    found = true;
                }
            } else if (ctx_.areas) {
                const Json& a = (*ctx_.areas)[d.areaName];
                if (a.isArray() && a.size() >= 2) {
                    r[0] = float(a[size_t(0)].toDouble()); r[1] = float(a[size_t(1)].toDouble());
                    r[2] = float(a.size() > 2 ? a[size_t(2)].toDouble(1) : 1); r[3] = float(a.size() > 3 ? a[size_t(3)].toDouble(1) : 1);
                    found = true;
                }
            }
            if (!found) return false;
        }
        std::memcpy(one.rect, r, sizeof r);
        one.pos = {r[0] + r[2] * 0.5f, 0, r[1] + r[3] * 0.5f};
        insts_.push_back(one);
        break;
    }
    case OvAnchor::Point: one.pos = {d.point[0], 0, d.point[1]}; insts_.push_back(one); break;
    case OvAnchor::Screen: one.sx = d.point[0]; one.sy = d.point[1]; insts_.push_back(one); break;
    case OvAnchor::Map: one.pos = camTarget_; insts_.push_back(one); break;
    case OvAnchor::Markers:
        for (auto& m : s.moveMarkers) { one.pos = m; insts_.push_back(one); if (insts_.size() >= cap) break; }
        break;
    case OvAnchor::Orders: case OvAnchor::Route: case OvAnchor::Rally: {
        if (!w) return false;
        for (int i : selected_) {
            if (insts_.size() >= cap) break;
            const SceneObject& o = s.objects[size_t(i)];
            if (!o.entity.valid() || !w->alive(o.entity) || (!pv && !test(d.when, &o))) continue;
            Inst in;
            in.obj = i; in.type = o.type; in.owner = o.owner; in.pos = o.pos;
            in.pathBegin = uint32_t(pathPts_.size());
            pathPts_.push_back(o.pos);
            if (d.anchor == OvAnchor::Orders) {
                if (auto* od = w->tryGet<Orders>(o.entity))
                    for (auto& ord : od->queue) {
                        Vec3 p = ord.point;
                        if (ord.target.valid()) if (auto* tt = w->tryGet<Transform>(ord.target)) p = tt->pos;
                        if (p.x.raw == 0 && p.z.raw == 0) continue;
                        pathPts_.push_back(toF(p));
                    }
            } else if (d.anchor == OvAnchor::Route) {
                if (auto* mb = w->tryGet<Mobile>(o.entity); mb && mb->moving)
                    for (size_t k = mb->pathPos; k < mb->path.size(); ++k) pathPts_.push_back(toF(mb->path[k]));
            } else if (auto* pr = w->tryGet<Production>(o.entity); pr && pr->hasRally) {
                Vec3 p = pr->rally;
                if (pr->rallyTarget.valid()) if (auto* tt = w->tryGet<Transform>(pr->rallyTarget)) p = tt->pos;
                pathPts_.push_back(toF(p));
            }
            in.pathCount = uint32_t(pathPts_.size()) - in.pathBegin;
            if (in.pathCount >= 2) insts_.push_back(in);
            else pathPts_.resize(in.pathBegin);
        }
        break;
    }
    case OvAnchor::Event:
        for (size_t pi = 0; pi < popups_.size(); ++pi) {
            const Popup& p = popups_[pi];
            if (p.def != di) continue;
            Inst in;
            in.popup = int(pi);
            in.age = float(time_ - p.born);
            in.pos = {p.pos.x + p.jx, p.pos.y, p.pos.z + p.jz};
            in.owner = p.owner; in.type = p.type;
            const float life = d.event.lifetime;
            in.alpha = 1;
            if (d.style.fadeOut > 0) in.alpha = std::clamp((life - in.age) / d.style.fadeOut, 0.0f, 1.0f);
            if (d.style.fadeIn > 0) in.alpha *= std::clamp(in.age / d.style.fadeIn, 0.0f, 1.0f);
            insts_.push_back(in);
        }
        break;
    }
    (void)screenPass;
    if (defAlpha < 1) for (auto& in : insts_) in.alpha *= defAlpha;
    return !insts_.empty();
}

float RtsOverlays::animAlpha(const OverlayDef& d, const Inst& in) const {
    float a = d.style.alpha * in.alpha;
    if (d.style.pulse > 0) {
        const float k = 0.5f + 0.5f * std::sin(float(time_) * 6.2831853f * d.style.pulse);
        a *= d.style.pulseMin + (1 - d.style.pulseMin) * k;
    }
    return a;
}

float4 RtsOverlays::colorFor(const OverlayDef& d, const Inst& in, int pred) const {
    const OvStyle& s = d.style;
    float4 c = s.color;
    if (s.playerColor) { float4 p = overlayPlayerColor(in.owner); c = {p.x, p.y, p.z, s.color.w}; }
    const uint8_t rel = relation(in.owner);
    const int rk = rel == kRelOwn ? kColOwn : rel == kRelAlly ? kColAlly : rel == kRelEnemy ? kColEnemy : kColNeutral;
    if (s.hasColor & (1u << rk)) c = s.colors[rk];
    if (pred >= 0 && (s.hasColor & (1u << (pred ? kColTrue : kColFalse)))) c = s.colors[pred ? kColTrue : kColFalse];
    c.w *= animAlpha(d, in);
    return c;
}

float RtsOverlays::valueOf(const OverlayDef& d, const SceneObject& o) const {
    const UnitType& t = rules_->types[o.type];
    World* w = ctx_.sim ? &ctx_.sim->world() : nullptr;
    switch (d.value) {
    case OvValue::Hp: return o.hp;
    case OvValue::Build: return o.build;
    case OvValue::Garrison: return t.garrisonMax > 0 && o.garrison >= 0 ? float(o.garrison) / float(t.garrisonMax) : 0;
    case OvValue::Energy:
        if (w && o.entity.valid() && t.maxEnergy.raw > 0) if (auto* u = w->tryGet<Unit>(o.entity)) return float((u->energy / t.maxEnergy).toDouble());
        return 0;
    case OvValue::Production:
        if (w && o.entity.valid()) if (auto* pr = w->tryGet<Production>(o.entity); pr && !pr->queue.empty()) return float(pr->queue.front().progress.toDouble());
        return 0;
    case OvValue::Carry: {
        float cap = 0;
        for (auto& g : t.gathers) cap = std::max(cap, float(g.carry.toDouble()));
        return cap > 0 ? o.carry / cap : 0;
    }
    case OvValue::Capture:
        if (w && o.entity.valid()) if (auto* cp = w->tryGet<Capture>(o.entity)) return float(cp->progress.toDouble());
        return 0;
    }
    return 0;
}

// Text with placeholders: {value} {amount} {resource} {name} {hp} {maxHp} {energy} {garrison} {build}
// {selected} {owner} {player} {var:NAME}. Unknown placeholders stay as written.
const std::string& RtsOverlays::format(const OverlayDef& d, const Inst& in) {
    text_.clear();
    const std::string& t = d.text;
    const SceneObject* o = in.obj >= 0 && scene_ ? &scene_->objects[size_t(in.obj)] : nullptr;
    const Popup* p = in.popup >= 0 ? &popups_[size_t(in.popup)] : nullptr;
    const UnitType* ty = in.type < rules_->types.size() ? &rules_->types[in.type] : nullptr;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] != '{') { text_ += t[i]; continue; }
        size_t end = t.find('}', i);
        if (end == std::string::npos) { text_.append(t, i, std::string::npos); break; }
        const std::string key = t.substr(i + 1, end - i - 1);
        const size_t before = text_.size();
        if (key == "value") { if (p) { if (p->kind == kEvHit) appendNumber(text_, p->amount); else appendNumber(text_, double(p->value)); } }
        else if (key == "amount") { if (p) appendNumber(text_, p->amount); else if (o) appendNumber(text_, o->carry); }
        else if (key == "resource") {
            int r = p ? p->res : o ? o->carryRes : -1;
            if (r >= 0 && size_t(r) < rules_->resources.size()) text_ += rules_->resources[size_t(r)].name;
        }
        else if (key == "name") { if (ty) text_ += ty->name; }
        else if (key == "hp") { if (o && ty) appendNumber(text_, std::round(o->hp * ty->maxHp.toDouble())); }
        else if (key == "maxHp") { if (ty) appendNumber(text_, ty->maxHp.toDouble()); }
        else if (key == "energy") {
            if (o && ctx_.sim && o->entity.valid()) if (auto* u = ctx_.sim->world().tryGet<Unit>(o->entity)) appendNumber(text_, std::floor(u->energy.toDouble()));
        }
        else if (key == "garrison") { if (o) appendNumber(text_, o->garrison); }
        else if (key == "build") { if (o) appendNumber(text_, std::floor(o->build * 100)); }
        else if (key == "selected") appendNumber(text_, double(selected_.size()));
        else if (key == "owner") appendNumber(text_, in.owner);
        else if (key == "player") appendNumber(text_, ctx_.player);
        else if (key.rfind("var:", 0) == 0) {
            double v = 0;
            if (RtsModule* m = moduleOf(ctx_.sim)) {
                auto& vars = m->state(*ctx_.sim).trig.vars;
                auto it = vars.find(key.substr(4));
                if (it != vars.end()) v = it->second.toDouble();
            }
            appendNumber(text_, v);
        } else text_.append(t, i, end - i + 1);
        (void)before;
        i = end;
    }
    return text_;
}

} // namespace rts::game
