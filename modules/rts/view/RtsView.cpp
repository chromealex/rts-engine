#include "RtsView.h"
#include "../RtsModule.h"
#include "imgui.h"
#include "../../../engine/platform/Platform.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace rts::game {

using namespace render;

namespace {
float3 toF(const Vec3& v) { return {float(v.x.toDouble()), float(v.y.toDouble()), float(v.z.toDouble())}; }
} // namespace

float4 RtsView::playerColor(int p) {
    static const float4 colors[] = {
        {0.70f, 0.70f, 0.68f, 1}, // neutral
        {0.20f, 0.45f, 0.95f, 1}, // 1 blue
        {0.92f, 0.22f, 0.20f, 1}, // 2 red
        {0.25f, 0.80f, 0.30f, 1}, // 3 green
        {0.97f, 0.80f, 0.15f, 1}, // 4 yellow
        {0.68f, 0.35f, 0.90f, 1}, // 5 purple
        {0.15f, 0.82f, 0.85f, 1}, // 6 teal
        {0.98f, 0.55f, 0.15f, 1}, // 7 orange
        {0.95f, 0.45f, 0.72f, 1}, // 8 pink
    };
    return colors[p >= 0 && p < 9 ? p : 0];
}

bool RtsView::init(gfx::Device& dev, const Rules& rules, FileReader reader) {
    rules_ = &rules;
    dev_ = &dev;
    if (!fog_.ready()) fog_.init(dev);
    read_ = reader ? std::move(reader) : FileReader([](const std::string& p, std::string& out) { return platform::readContentFile(p, out); });
    {   // fog of war look: rules "fogOfWar" (visual keys), game.json "fogOfWar" overrides
        std::string text, rulesPath = "rules.json";
        Json game, rj;
        try { if (read_("game.json", text)) { game = Json::parse(text); rulesPath = game.getStr("rules", "rules.json"); } } catch (...) {}
        try { if (read_(rulesPath, text)) rj = Json::parse(text); } catch (...) {}
        fog_.setLook(parseFogLook(rj["fogOfWar"], game["fogOfWar"]));
        fogMeshDirty_ = true;
    }
    if (!models_.count("shape:generic")) buildShapes(dev);
    looks_.clear();
    warnings_.clear();
    anims_.clear();
    for (auto& t : rules.types) {
        TypeLook l;
        buildLook(dev, t, l);
        looks_.push_back(std::move(l));
    }
    twoD_ = std::make_unique<Rts2DView>();
    if (!twoD_->init(dev, rules, contentBase_)) twoD_.reset();
    lights_.init(rules);
    groundFx_.init(dev, rules, read_, contentBase_);
    terrain_.init(dev, rules, read_, contentBase_);
    return true;
}

void RtsView::shutdown(gfx::Device& dev) {
    for (auto& [k, m] : models_) if (m) releaseModel(dev, *m);
    models_.clear(); looks_.clear(); anims_.clear();
    for (Mesh* m : {&projectile_}) { dev.destroyBuffer(m->vb); dev.destroyBuffer(m->ib); *m = Mesh{}; }
    terrain_.shutdown(dev);
    fog_.shutdown();
    hasTerrain_ = false;
    if (twoD_) twoD_->shutdown(dev);
    twoD_.reset();
    groundFx_.shutdown(dev);
}

bool RtsView::spriteHit(const RTSCamera& cam, const SceneObject& o, const MapGrid& grid, float sx, float sy, float w, float h) const {
    return twoD_ && !o.isProjectile && twoD_->hasSprite(o.type) && twoD_->hit(cam, o, heightAt(grid, o.pos.x, o.pos.z), sx, sy, w, h);
}

float RtsView::selectionRadius(uint16_t type) const {
    const UnitType& t = rules_->types[type];
    if (t.structure) return std::max(t.footprintW, t.footprintH) * 0.62f;
    return float(t.radius.toDouble()) + 0.15f;
}

// heightAt / buildTerrain: RtsTerrain.cpp


void RtsView::draw(Renderer3D& r, const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid) {
    const bool tiles = twoD_ && twoD_->hasTiles();
    if (twoD_) twoD_->beginFrame(r.camera(), time);
    bars_.assign(s.objects.size(), {});
    if (hasTerrain_ && !tiles) terrain_.draw(r, grid, vision, groundFx_.drawsDig());
    if (!tiles) groundFx_.draw(r, s, time, vision, grid, *this);
    if (tiles || !groundFx_.drawsLinks())
        for (auto& [a, b] : s.roads) r.quadXZ(a, b, 0.5f, rgba(196, 164, 112, 235), 0.04f);

    const float dt = lastTime_ < 0 ? 0.0f : float(std::clamp(time - lastTime_, 0.0, 0.1));
    lastTime_ = time;
    ++frame_;
    viewProj_ = r.viewProj();
    camPos_ = r.cameraPos();
    events_.clear();
    effects_.clear();
    handleEvents(s);
    for (size_t i = 0; i < s.objects.size(); ++i) {
        const SceneObject& o = s.objects[i];
        if (o.isProjectile) {
            if (o.visible && o.entity.valid()) drawProjectile(r, o.entity.packed(), o, dt);
            else if (o.visible) r.drawMesh(projectile_, mat4::translate(o.pos), {1, 0.95f, 0.8f, 1}, kDrawUnlit);
            continue;
        }
        if (twoD_ && twoD_->hasSprite(o.type)) { // 2D games: sprites instead of models
            float g = heightAt(grid, o.pos.x, o.pos.z);
            twoD_->addObject(o, g);
            bars_[i] = twoD_->barPoint(o, g);
            if (o.visible && (o.selected || o.hovered)) {
                uint32_t col = o.selected ? (o.owner == 0 ? rgba(255, 255, 160, 230) : rgba(120, 255, 120, 230)) : rgba(255, 255, 255, 130);
                r.circleXZ({o.pos.x, 0, o.pos.z}, selectionRadius(o.type), col, 32, g + 0.05f);
            }
            continue;
        }
        if (o.type >= looks_.size()) continue;
        // editor placements have no entity: key them by position in the list
        const uint64_t key = o.entity.valid() ? o.entity.packed() : (1ull << 63) | uint64_t(i);
        EntityAnim& st = stateFor(key, o);
        st.last = o;
        if (!o.visible) continue;
        const TypeLook& L = looks_[o.type];
        const float ground = heightAt(grid, o.pos.x, o.pos.z);
        animate(st, o, dt, &grid);
        drawObject(r, st, o, ground, 1.0f);
        if (L.air) { // ground shadow blob helps reading the height
            r.circleXZ({o.pos.x, ground, o.pos.z}, 0.35f, rgba(0, 0, 0, 90), 16, ground + 0.03f);
        }
        if (o.selected || o.hovered) {
            float rad = selectionRadius(o.type);
            uint32_t col = o.selected ? (o.owner == 0 ? rgba(255, 255, 160, 230) : rgba(120, 255, 120, 230)) : rgba(255, 255, 255, 130);
            r.circleXZ({o.pos.x, 0, o.pos.z}, rad, col, 32, ground + 0.05f);
        }
    }
    // corpses (entity already removed) play their death and fade; stale states are dropped
    for (auto it = anims_.begin(); it != anims_.end();) {
        EntityAnim& st = it->second;
        if (st.seen == frame_ && !st.dead) { ++it; continue; }
        if (!st.dead || !st.initialized) {
            it = st.seen + 2 < frame_ ? anims_.erase(it) : std::next(it);
            continue;
        }
        st.deathAge += dt;
        if (st.type >= looks_.size() || st.deathAge > looks_[st.type].corpse) { it = anims_.erase(it); continue; }
        const TypeLook& L = looks_[st.type];
        if (st.seen != frame_ && st.last.visible) {
            SceneObject o = st.last;
            o.selected = o.hovered = false;
            animate(st, o, dt, &grid);
            drawObject(r, st, o, heightAt(grid, o.pos.x, o.pos.z), L.dissolveDeath ? 1.0f : std::clamp((L.corpse - st.deathAge) / 0.6f, 0.0f, 1.0f));
        }
        ++it;
    }
    for (auto& mk : s.moveMarkers) r.circleXZ(mk, 0.35f, rgba(120, 255, 120, 200), 16, 0.06f);
    if (twoD_) {
        twoD_->endFrame(grid);
        twoD_->submit(r, grid, rules_ ? float(rules_->levelHeight.toDouble()) : 1.5f);
    }

    // fog of war (RtsFog): faded, blurred field; a sheet draped over the ground, or the terrain shader
    // on heightfield maps (a sheet would cut the relief)
    if (vision && !vision->empty()) {
        if (fogMeshDirty_ && dev_) { fog_.build(*dev_, grid.w, grid.h, [&](float x, float z) { return heightAt(grid, x, z); }); fogMeshDirty_ = false; }
        const bool changed = fog_.update(*vision, grid.w, grid.h, time);
        const bool low = r.shadowQuality() == ShadowQuality::Off || r.shadowQuality() == ShadowQuality::Low;
        if (hasTerrain_ && !tiles && terrain_.shaderFog()) terrain_.setFog(&fog_.field(), fog_.look(), changed);
        else fog_.draw(r, time, low);
    }
    lights_.draw(r, s, time, vision, grid, *this);
}

void RtsView::drawOverlays(const RTSCamera& cam, const RtsScene& s, float w, float h, float ox, float oy, void* drawList) {
    ImDrawList* dl = drawList ? static_cast<ImDrawList*>(drawList) : ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    char buf[32];
    for (size_t oi = 0; oi < s.objects.size(); ++oi) {
        const auto& o = s.objects[oi];
        if (!o.visible || o.isProjectile) continue;
        const UnitType& t = rules_->types[o.type];
        float2 sp;
        if (twoD_ && twoD_->hasSprite(o.type) && oi < bars_.size()) {
            if (!cam.worldToScreen(bars_[oi], w, h, sp)) continue;
            sp.y -= 4;
        } else {
        float top = t.structure ? 1.6f * looks_[o.type].scale * std::max(1.0f, std::max(t.footprintW, t.footprintH) * 0.6f) : 1.2f * looks_[o.type].scale;
        if (looks_[o.type].air) top += 2.4f;
        if (!cam.worldToScreen({o.pos.x, top, o.pos.z}, w, h, sp)) continue;
        }
        sp.x += ox; sp.y += oy;
        if (o.garrison >= 0) {
            std::snprintf(buf, sizeof buf, "%d", o.garrison);
            float4 c = playerColor(o.owner);
            float size = fs * 1.2f;
            ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, buf);
            ImVec2 at(sp.x - ts.x * 0.5f, sp.y - ts.y * 0.5f);
            dl->AddRectFilled(ImVec2(at.x - 6, at.y - 1), ImVec2(at.x + ts.x + 6, at.y + ts.y + 1),
                              ImGui::ColorConvertFloat4ToU32(ImVec4(c.x * 0.45f, c.y * 0.45f, c.z * 0.45f, 0.85f)), 7);
            dl->AddText(font, size, at, IM_COL32_WHITE, buf);
            continue;
        }
        bool showHp = !t.invulnerable && (o.selected || o.hovered || o.hp < 0.999f);
        if (showHp) {
            float bw = t.structure ? 46.0f : 26.0f, bh = 4.0f;
            ImVec2 a(sp.x - bw * 0.5f, sp.y), b(sp.x + bw * 0.5f, sp.y + bh);
            dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(0, 0, 0, 170));
            float f = std::clamp(o.hp, 0.0f, 1.0f);
            ImU32 col = f > 0.6f ? IM_COL32(80, 220, 90, 255) : f > 0.3f ? IM_COL32(240, 200, 60, 255) : IM_COL32(230, 70, 60, 255);
            dl->AddRectFilled(a, ImVec2(a.x + bw * f, b.y), col);
        }
        if (o.build < 1.0f) {
            float bw = 46.0f;
            ImVec2 a(sp.x - bw * 0.5f, sp.y + 7);
            dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(a.x + bw + 1, a.y + 4), IM_COL32(0, 0, 0, 170));
            dl->AddRectFilled(a, ImVec2(a.x + bw * o.build, a.y + 3), IM_COL32(120, 190, 255, 255));
        }
    }
}

void sceneFromSim(Simulation& sim, RtsScene& out, float alpha, uint8_t viewer) {
    World& w = sim.world();
    auto* m = dynamic_cast<RtsModule*>(&sim.module());
    if (!m) return;
    const Rules& R = m->rules();
    GameState& gs = m->state(sim);
    out.objects.clear();
    out.roads.clear();
    out.events = sim.events();
    out.tick = sim.tick();
    out.viewer = viewer;
    out.lanes = R.lanes.enabled ? &gs.lanes : nullptr;
    out.laneSignature = gs.laneSignature;
    out.seconds = double(sim.tick()) / double(std::max(sim.tickRate(), 1u));
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
        SceneObject so;
        so.entity = e; so.type = u.type; so.owner = o.player;
        float3 a = toF(t.prevPos), b = toF(t.pos);
        so.pos = lerp(a, b, alpha);
        so.facing = toF(u.facing);
        const UnitType& ut = R.types[u.type];
        so.hp = ut.maxHp.raw > 0 ? float((u.hp / ut.maxHp).toDouble()) : 1.0f;
        if (auto* c = w.tryGet<Construction>(e)) so.build = float(c->progress.toDouble());
        if (auto* g = w.tryGet<Garrison>(e)) so.garrison = g->count;
        if (auto* c = w.tryGet<Cargo>(e)) { so.carry = float(c->amount.toDouble()); so.carryRes = c->res; }
        if (auto* od = w.tryGet<Orders>(e); od && !od->queue.empty()) so.order = uint8_t(od->queue.front().type) + 1;
        if (auto* mb = w.tryGet<Mobile>(e)) so.moving = mb->moving;
        if (auto* pr = w.tryGet<Production>(e)) so.producing = !pr->queue.empty();
        if (auto* cb = w.tryGet<Combat>(e); cb && cb->target.valid() && w.alive(cb->target)) {
            if (auto* tt = w.tryGet<Transform>(cb->target)) { so.hasTarget = true; so.target = lerp(toF(tt->prevPos), toF(tt->pos), alpha); }
        }
        if (so.moving) so.action = kActMove;
        else if (so.hasTarget) so.action = kActAttack;
        else if (so.order) {
            OrderType ot = OrderType(so.order - 1);
            if (ot == OrderType::Gather || ot == OrderType::Build || ot == OrderType::Repair) so.action = kActWork;
        }
        if (viewer != 0 && o.player != viewer && !sim.allied(o.player, viewer)) {
            // enemies only when visible; neutral structures (resources) once explored
            int x = int(t.pos.x.floorInt()), y = int(t.pos.z.floorInt());
            uint8_t v = gs.grid.inside(x, y) ? gs.vision[viewer][size_t(gs.grid.index(x, y))] : 0;
            so.visible = ut.structure && o.player == 0 ? v >= 1 : v == 2;
        }
        out.objects.push_back(so);
    });
    w.each<Projectile, Transform>([&](Entity e, Projectile& pr, Transform& t) {
        SceneObject so;
        so.entity = e; so.isProjectile = true; so.source = pr.source;
        if (auto* su = w.tryGet<Unit>(pr.source)) so.shooterType = su->type; // picks the projectile's light
        so.pos = lerp(toF(t.prevPos), toF(t.pos), alpha);
        if (viewer != 0) {
            int x = int(t.pos.x.floorInt()), y = int(t.pos.z.floorInt());
            so.visible = gs.grid.inside(x, y) && gs.vision[viewer][size_t(gs.grid.index(x, y))] == 2;
        }
        out.objects.push_back(so);
    });
    for (size_t i = 0; i < gs.roads.nodes.size(); ++i)
        for (uint16_t j : gs.roads.adj[i]) {
            if (j <= i) continue;
            auto* ta = w.tryGet<Transform>(gs.roads.nodes[i]);
            auto* tb = w.tryGet<Transform>(gs.roads.nodes[j]);
            if (ta && tb) out.roads.push_back({toF(ta->pos), toF(tb->pos)});
        }
}

} // namespace rts::game
