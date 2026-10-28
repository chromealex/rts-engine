#include "RtsView.h"
#include "../RtsModule.h"
#include "imgui.h"

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

Mesh& RtsView::mesh(gfx::Device& dev, const std::string& key, const MeshBuilder& b) {
    auto it = meshes_.find(key);
    if (it != meshes_.end()) return it->second;
    return meshes_[key] = b.upload(dev);
}

void RtsView::buildShapes(gfx::Device& dev) {
    const uint32_t wood = rgba(122, 86, 52), stone = rgba(150, 146, 140), darkStone = rgba(105, 102, 98), skin = rgba(233, 196, 160),
                   metal = rgba(170, 176, 186), leaf = rgba(52, 120, 48), leaf2 = rgba(70, 140, 60), gold = rgba(240, 196, 60),
                   roofTile = rgba(255, 255, 255), cream = rgba(236, 226, 200), dark = rgba(60, 60, 64);
    auto add = [&](const std::string& shape, const std::string& part, bool team, auto&& build) {
        MeshBuilder b;
        b.color(team ? 0xffffffffu : wood);
        build(b);
        shapes_[shape].parts.push_back({&mesh(dev, shape + "/" + part, b), mat4{}, {1, 1, 1, 1}, team});
    };
    // ---- units (≈1 unit tall, facing +X)
    add("worker", "body", true, [&](MeshBuilder& b) { b.cylinder({0, 0.0f, 0}, 0.22f, 0.5f, 12, true, 0.17f); });
    add("worker", "head", false, [&](MeshBuilder& b) { b.color(skin).sphere({0, 0.66f, 0}, 0.15f, 6, 10); b.color(wood).box({0.18f, 0.35f, 0.12f}, {0.06f, 0.5f, 0.06f}); b.color(metal).box({0.18f, 0.6f, 0.12f}, {0.22f, 0.07f, 0.07f}); });
    add("soldier", "body", true, [&](MeshBuilder& b) { b.box({0, 0.32f, 0}, {0.36f, 0.5f, 0.42f}); });
    add("soldier", "rest", false, [&](MeshBuilder& b) {
        b.color(metal).sphere({0, 0.7f, 0}, 0.16f, 6, 10); b.color(dark).box({0, 0.04f, 0.1f}, {0.14f, 0.12f, 0.12f}).box({0, 0.04f, -0.1f}, {0.14f, 0.12f, 0.12f});
        b.color(metal).box({0.08f, 0.38f, -0.27f}, {0.38f, 0.42f, 0.05f}); b.color(metal).box({0.3f, 0.5f, 0.24f}, {0.55f, 0.05f, 0.05f}); });
    add("ranged", "body", true, [&](MeshBuilder& b) { b.cylinder({0, 0, 0}, 0.2f, 0.55f, 10, true, 0.14f); b.cone({0, 0.8f, 0}, 0.18f, 0.25f, 10); });
    add("ranged", "rest", false, [&](MeshBuilder& b) { b.color(skin).sphere({0, 0.68f, 0}, 0.13f, 6, 10); b.color(wood).cylinder({0.18f, 0.15f, 0.1f}, 0.025f, 0.7f, 6); });
    add("vehicle", "base", false, [&](MeshBuilder& b) {
        b.color(wood).box({0, 0.32f, 0}, {1.0f, 0.22f, 0.7f});
        for (float x : {-0.32f, 0.32f}) for (float z : {-0.38f, 0.38f}) b.color(dark).box({x, 0.14f, z}, {0.3f, 0.28f, 0.1f});
        b.color(wood).box({-0.1f, 0.65f, 0}, {0.12f, 0.55f, 0.12f}); });
    add("vehicle", "team", true, [&](MeshBuilder& b) { b.box({-0.35f, 0.5f, 0}, {0.3f, 0.12f, 0.72f}); });
    add("tank", "base", false, [&](MeshBuilder& b) { b.color(dark).box({0, 0.15f, 0.3f}, {1.1f, 0.3f, 0.25f}).box({0, 0.15f, -0.3f}, {1.1f, 0.3f, 0.25f}); b.color(metal).box({0.55f, 0.55f, 0}, {0.7f, 0.08f, 0.08f}); });
    add("tank", "team", true, [&](MeshBuilder& b) { b.box({0, 0.36f, 0}, {1.0f, 0.25f, 0.62f}); b.cylinder({0, 0.48f, 0}, 0.26f, 0.18f, 12); });
    add("flyer", "body", true, [&](MeshBuilder& b) { b.transform(mat4::scale({1.4f, 0.7f, 0.8f})).sphere({0, 0, 0}, 0.3f, 6, 12); b.transform(mat4{}); b.box({0, 0.05f, 0}, {0.3f, 0.04f, 1.5f}); });
    add("flyer", "rest", false, [&](MeshBuilder& b) { b.color(cream).sphere({0.45f, 0.08f, 0}, 0.14f, 5, 8); b.color(gold).cone({0.62f, 0.05f, 0}, 0.06f, 0.12f, 6); });
    add("mushroom", "stem", false, [&](MeshBuilder& b) { b.color(cream).cylinder({0, 0, 0}, 0.14f, 0.35f, 8, true, 0.11f); });
    add("mushroom", "cap", true, [&](MeshBuilder& b) { b.transform(mat4::translate({0, 0.36f, 0}) * mat4::scale({1, 0.6f, 1})).sphere({0, 0, 0}, 0.24f, 5, 10); });
    add("generic", "body", true, [&](MeshBuilder& b) { b.cylinder({0, 0, 0}, 0.3f, 0.7f, 10); });

    // ---- structures (sized to a 1x1 footprint; scaled by the footprint at draw time)
    add("hall", "base", false, [&](MeshBuilder& b) { b.color(stone).box({0, 0.35f, 0}, {0.9f, 0.7f, 0.9f}); b.color(wood).box({0.46f, 0.18f, 0}, {0.04f, 0.35f, 0.2f}); });
    add("hall", "roof", true, [&](MeshBuilder& b) { b.cone({0, 0.7f, 0}, 0.66f, 0.45f, 4); b.cylinder({0.25f, 0.7f, 0.25f}, 0.08f, 0.5f, 8); });
    add("house", "base", false, [&](MeshBuilder& b) { b.color(cream).box({0, 0.3f, 0}, {0.8f, 0.6f, 0.8f}); b.color(wood).box({0.41f, 0.15f, 0}, {0.04f, 0.3f, 0.18f}); });
    add("house", "roof", true, [&](MeshBuilder& b) { b.cone({0, 0.6f, 0}, 0.62f, 0.45f, 4); });
    add("barracks", "base", false, [&](MeshBuilder& b) { b.color(darkStone).box({0, 0.3f, 0}, {0.9f, 0.6f, 0.9f}); b.color(wood).box({0.46f, 0.2f, 0}, {0.04f, 0.4f, 0.3f}); });
    add("barracks", "roof", true, [&](MeshBuilder& b) { b.box({0, 0.64f, 0}, {0.95f, 0.08f, 0.95f}); b.cylinder({-0.3f, 0.6f, -0.3f}, 0.03f, 0.6f, 6); b.box({-0.2f, 1.08f, -0.3f}, {0.2f, 0.14f, 0.02f}); });
    add("workshop", "base", false, [&](MeshBuilder& b) { b.color(wood).box({0, 0.28f, 0}, {0.9f, 0.56f, 0.8f}); b.color(darkStone).cylinder({0.25f, 0.5f, -0.2f}, 0.09f, 0.55f, 8); });
    add("workshop", "roof", true, [&](MeshBuilder& b) { b.transform(mat4::rotateY(0.785f)).cone({0, 0.56f, 0}, 0.62f, 0.3f, 4); });
    add("factory", "base", false, [&](MeshBuilder& b) { b.color(metal).box({0, 0.3f, 0}, {0.92f, 0.6f, 0.92f}); b.color(dark).cylinder({0.3f, 0.6f, 0.3f}, 0.08f, 0.6f, 8).cylinder({0.1f, 0.6f, 0.3f}, 0.08f, 0.5f, 8); });
    add("factory", "roof", true, [&](MeshBuilder& b) { b.box({0, 0.62f, -0.2f}, {0.9f, 0.06f, 0.45f}); });
    add("tower", "base", false, [&](MeshBuilder& b) { b.color(stone).cylinder({0, 0, 0}, 0.3f, 1.1f, 12, true, 0.24f); b.color(darkStone).box({0, 1.12f, 0}, {0.62f, 0.1f, 0.62f}); });
    add("tower", "roof", true, [&](MeshBuilder& b) { b.cone({0, 1.17f, 0}, 0.4f, 0.5f, 12); });
    add("mine", "base", false, [&](MeshBuilder& b) {
        b.color(darkStone).transform(mat4::scale({1, 0.55f, 1})).sphere({0, 0.3f, 0}, 0.48f, 6, 10); b.transform(mat4{});
        b.color(dark).box({0.38f, 0.18f, 0}, {0.1f, 0.3f, 0.28f});
        b.color(gold).box({0.1f, 0.42f, 0.2f}, {0.12f, 0.1f, 0.12f}).box({-0.15f, 0.4f, -0.1f}, {0.1f, 0.12f, 0.1f}).box({0.2f, 0.35f, -0.2f}, {0.12f, 0.08f, 0.1f}); });
    add("tree", "base", false, [&](MeshBuilder& b) { b.color(wood).cylinder({0, 0, 0}, 0.07f, 0.45f, 6); b.color(leaf).cone({0, 0.3f, 0}, 0.42f, 0.7f, 8); b.color(leaf2).cone({0, 0.65f, 0}, 0.3f, 0.6f, 8); });
    add("rock", "base", false, [&](MeshBuilder& b) { b.color(darkStone).transform(mat4::scale({1, 0.6f, 0.9f})).sphere({0, 0.2f, 0}, 0.45f, 5, 7); });
    add("mushroomhouse", "stem", false, [&](MeshBuilder& b) { b.color(cream).cylinder({0, 0, 0}, 0.2f, 0.55f, 14, true, 0.17f); b.color(wood).box({0.19f, 0.15f, 0}, {0.03f, 0.25f, 0.12f}); });
    add("mushroomhouse", "cap", true, [&](MeshBuilder& b) { b.transform(mat4::translate({0, 0.55f, 0}) * mat4::scale({1, 0.5f, 1})).sphere({0, 0, 0}, 0.45f, 7, 16); });
    add("structure", "base", false, [&](MeshBuilder& b) { b.color(stone).box({0, 0.3f, 0}, {0.9f, 0.6f, 0.9f}); });
    add("structure", "roof", true, [&](MeshBuilder& b) { b.box({0, 0.63f, 0}, {0.95f, 0.06f, 0.95f}); });
    (void)roofTile;
    MeshBuilder pb; pb.color(rgba(255, 240, 200)).sphere({0, 0, 0}, 0.08f, 4, 6);
    projectile_ = pb.upload(dev);
}

bool RtsView::init(gfx::Device& dev, const Rules& rules) {
    rules_ = &rules;
    if (shapes_.empty()) buildShapes(dev);
    looks_.clear();
    for (auto& t : rules.types) {
        TypeLook l;
        std::string shape = t.view.getStr("shape", t.structure ? (t.resourceNode >= 0 ? "mine" : "structure") : "generic");
        if (!shapes_.count(shape)) shape = t.structure ? "structure" : "generic";
        l.shape = &shapes_[shape];
        l.scale = float(t.view.getFx("scale", Fixed::fromInt(1)).toDouble());
        const Json& c = t.view["color"];
        l.tint = c.size() >= 3 ? float4{float(c[0].fx().toDouble()), float(c[1].fx().toDouble()), float(c[2].fx().toDouble()), 1} : float4{1, 1, 1, 1};
        l.air = t.layer & kLayerAir;
        l.sx = t.structure ? float(t.footprintW) : float(t.radius.toDouble()) * 2.6f;
        l.sz = t.structure ? float(t.footprintH) : l.sx;
        if (!t.structure) l.sx = l.sz = 1.0f;
        looks_.push_back(l);
    }
    lights_.init(rules);
    return true;
}

void RtsView::shutdown(gfx::Device& dev) {
    for (auto& [k, m] : meshes_) { dev.destroyBuffer(m.vb); dev.destroyBuffer(m.ib); }
    meshes_.clear(); shapes_.clear();
    for (Mesh* m : {&terrain_, &water_, &projectile_}) { dev.destroyBuffer(m->vb); dev.destroyBuffer(m->ib); }
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

    for (const auto& o : s.objects) {
        if (!o.visible) continue;
        if (o.isProjectile) { r.drawMesh(projectile_, mat4::translate(o.pos), {1, 0.95f, 0.8f, 1}, kDrawUnlit); continue; }
        const UnitType& t = rules_->types[o.type];
        const TypeLook& L = looks_[o.type];
        float4 team = playerColor(o.owner);
        float ground = heightAt(grid, o.pos.x, o.pos.z);
        float3 p{o.pos.x, ground + (L.air ? 2.4f + 0.12f * float(std::sin(time * 3 + o.pos.x)) : 0.0f), o.pos.z};
        float yaw = std::atan2(-o.facing.z, o.facing.x);
        float k = L.scale;
        mat4 m;
        if (t.structure) {
            float grow = std::clamp(o.build, 0.15f, 1.0f);
            m = mat4::translate(p) * mat4::scale({L.sx * k, k * std::max(L.sx, L.sz) * 0.75f * grow, L.sz * k});
        } else {
            m = mat4::translate(p) * mat4::rotateY(yaw) * mat4::scale({k, k, k});
        }
        bool building = o.build < 1.0f;
        for (const auto& part : L.shape->parts) {
            float4 c = part.team ? float4{team.x * L.tint.x, team.y * L.tint.y, team.z * L.tint.z, 1} : L.tint;
            if (building) c = {c.x * 0.75f + 0.2f, c.y * 0.75f + 0.18f, c.z * 0.75f + 0.12f, 1};
            r.drawMesh(*part.mesh, m * part.xf, c, 0, part.team ? 0.35f : 0.15f);
        }
        if (L.air) { // ground shadow blob helps reading the height
            r.circleXZ({p.x, ground, p.z}, 0.35f, rgba(0, 0, 0, 90), 16, ground + 0.03f);
        }
        if (o.selected || o.hovered) {
            float rad = selectionRadius(o.type);
            uint32_t col = o.selected ? (o.owner == 0 ? rgba(255, 255, 160, 230) : rgba(120, 255, 120, 230)) : rgba(255, 255, 255, 130);
            r.circleXZ({p.x, 0, p.z}, rad, col, 32, ground + 0.05f);
        }
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
    lights_.draw(r, s, time, vision, grid, *this);
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
        if (auto* c = w.tryGet<Cargo>(e)) so.carry = float(c->amount.toDouble());
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
        so.entity = e; so.isProjectile = true;
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
