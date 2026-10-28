// Built-in procedural shapes: simple archetypes (units ~1 unit tall facing +X, structures on a
// 1x1 footprint) built with MeshBuilder and turned into Models, one node per part.
#include "RtsView.h"

#include <map>
#include <memory>

namespace rts::game {

using namespace render;

void RtsView::buildShapes(gfx::Device& dev) {
    const uint32_t wood = rgba(122, 86, 52), stone = rgba(150, 146, 140), darkStone = rgba(105, 102, 98), skin = rgba(233, 196, 160),
                   metal = rgba(170, 176, 186), leaf = rgba(52, 120, 48), leaf2 = rgba(70, 140, 60), gold = rgba(240, 196, 60),
                   roofTile = rgba(255, 255, 255), cream = rgba(236, 226, 200), dark = rgba(60, 60, 64);
    // parts are collected per shape, then each shape becomes one Model (one node per part), so
    // shapes take the same materials, team colours and procedural animation as imported models
    std::map<std::string, std::vector<std::pair<std::string, std::unique_ptr<MeshBuilder>>>> parts;
    std::map<std::string, std::vector<bool>> teamParts;
    auto add = [&](const std::string& shape, const std::string& part, bool team, auto&& build) {
        auto b = std::make_unique<MeshBuilder>();
        b->color(team ? 0xffffffffu : wood);
        build(*b);
        parts[shape].push_back({part, std::move(b)});
        teamParts[shape].push_back(team);
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
    // sandworm: a segmented back breaking the sand and a raised maw (Dune)
    add("worm", "body", false, [&](MeshBuilder& b) {
        const uint32_t sandSkin = rgba(196, 150, 96), maw = rgba(90, 40, 36);
        for (int k = 0; k < 6; ++k) b.color(k % 2 ? sandSkin : rgba(176, 132, 84)).sphere({-0.85f + 0.3f * k, 0.12f + 0.03f * k, 0}, 0.2f + 0.02f * k, 6, 10);
        b.color(sandSkin).cylinder({0.95f, 0.1f, 0}, 0.32f, 0.55f, 12, true, 0.36f);
        b.color(maw).cylinder({0.95f, 0.64f, 0}, 0.26f, 0.04f, 12);
        b.color(cream).cone({0.95f, 0.66f, 0.18f}, 0.04f, 0.14f, 5).cone({0.95f, 0.66f, -0.18f}, 0.04f, 0.14f, 5)
                      .cone({1.13f, 0.66f, 0}, 0.04f, 0.14f, 5).cone({0.77f, 0.66f, 0}, 0.04f, 0.14f, 5); });

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
    for (auto& [shape, list] : parts) {
        std::vector<ModelPart> mp;
        for (size_t i = 0; i < list.size(); ++i) {
            Material m;
            m.name = list[i].first;
            m.roughness = teamParts[shape][i] ? 0.55f : 0.8f;
            m.team = teamParts[shape][i] ? TeamMode::Tint : TeamMode::None;
            mp.push_back({list[i].first, list[i].second.get(), m, mat4{}});
        }
        auto model = modelFromParts("shape:" + shape, mp);
        uploadModel(dev, *model);
        models_["shape:" + shape] = std::move(model);
    }
    MeshBuilder pb; pb.color(rgba(255, 240, 200)).sphere({0, 0, 0}, 0.08f, 4, 6);
    projectile_ = pb.upload(dev);
}

} // namespace rts::game
