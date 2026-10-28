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
    read_ = reader ? std::move(reader) : FileReader([](const std::string& p, std::string& out) { return platform::readContentFile(p, out); });
    if (!models_.count("shape:generic")) buildShapes(dev);
    looks_.clear();
    warnings_.clear();
    anims_.clear();
    for (auto& t : rules.types) {
        TypeLook l;
        buildLook(dev, t, l);
        looks_.push_back(std::move(l));
    }
    return true;
}

void RtsView::shutdown(gfx::Device& dev) {
    for (auto& [k, m] : models_) if (m) releaseModel(dev, *m);
    models_.clear(); looks_.clear(); anims_.clear();
    for (Mesh* m : {&terrain_, &water_, &projectile_}) { dev.destroyBuffer(m->vb); dev.destroyBuffer(m->ib); *m = Mesh{}; }
    hasTerrain_ = false;
}

float RtsView::selectionRadius(uint16_t type) const {
    const UnitType& t = rules_->types[type];
    if (t.structure) return std::max(t.footprintW, t.footprintH) * 0.62f;
    return float(t.radius.toDouble()) + 0.15f;
}

// heightAt / buildTerrain: RtsTerrain.cpp


void RtsView::draw(Renderer3D& r, const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid) {
    if (hasTerrain_) {
        r.drawMesh(terrain_, mat4{}, {1, 1, 1, 1}, kDrawNoCastShadow, 0.05f);
        bool anyWater = false;
        for (auto f : grid.flags) if ((f & kCellWater) && !(f & kCellGround)) { anyWater = true; break; }
        if (anyWater) r.drawMesh(water_, mat4{}, {0.22f, 0.45f, 0.62f, 0.78f}, kDrawNoCastShadow, 0.9f);
    }
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

    // fog of war: darken cells, merging runs per row to keep the triangle count low
    if (vision && !vision->empty()) {
        const int W = grid.w, H = grid.h;
        for (int y = 0; y < H; ++y) {
            int x = 0;
            while (x < W) {
                uint8_t v = (*vision)[size_t(y * W + x)];
                int x2 = x + 1;
                while (x2 < W && (*vision)[size_t(y * W + x2)] == v) ++x2;
                if (v != 2) {
                    uint32_t c = v == 0 ? rgba(8, 10, 14, 235) : rgba(8, 10, 14, 120);
                    float h = 1.3f;
                    r.triangle({float(x), h, float(y)}, {float(x), h, float(y + 1)}, {float(x2), h, float(y + 1)}, c);
                    r.triangle({float(x), h, float(y)}, {float(x2), h, float(y + 1)}, {float(x2), h, float(y)}, c);
                }
                x = x2;
            }
        }
    }
}

void RtsView::drawOverlays(const RTSCamera& cam, const RtsScene& s, float w, float h, float ox, float oy, void* drawList) {
    ImDrawList* dl = drawList ? static_cast<ImDrawList*>(drawList) : ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    char buf[32];
    for (const auto& o : s.objects) {
        if (!o.visible || o.isProjectile) continue;
        const UnitType& t = rules_->types[o.type];
        float top = t.structure ? 1.6f * looks_[o.type].scale * std::max(1.0f, std::max(t.footprintW, t.footprintH) * 0.6f) : 1.2f * looks_[o.type].scale;
        if (looks_[o.type].air) top += 2.4f;
        float2 sp;
        if (!cam.worldToScreen({o.pos.x, top, o.pos.z}, w, h, sp)) continue;
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
