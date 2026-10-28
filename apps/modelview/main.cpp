// rts_modelview: look at models the way the game renders them (materials, shadows, skinning,
// clips, variants, team colours, quality tiers). For artists checking an export and for CI
// screenshots of the model pipeline.
//
//   rts_modelview a.glb b.gltf@Walk c.obj [--clip Walk] [--time 0.4] [--variant winter] [--baked]
//                 [--team 0.9,0.2,0.2] [--quality low|medium|high] [--height 1.5] [--native] [--distance 6]
//                 [--yaw 20] [--pitch 30] [--spacing 2.2]
//   file@Clip plays that clip on that instance. Keys: Space next clip, Q quality, V variant,
//   P pause, T team colour; drag to orbit, wheel to zoom. RTS_SCREENSHOT works as in the client.
//
//   rts_modelview --game games/showcase [--types footman,catapult] [--start 4] [--fixed-dt 0.016]
//   gallery: every type of the game through the game's own view (view blocks, overrides,
//   procedural animation), cycling idle -> move -> attack -> death -> respawn.
#include "../../engine/platform/Platform.h"
#include "../../engine/render/Animation.h"
#include "../../engine/render/ImGuiLayer.h"
#include "../../engine/render/Renderer3D.h"
#include "imgui.h"
#include "Gallery.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

using namespace rts;
using namespace rts::render;

namespace {

struct Instance {
    Model* model = nullptr;
    std::string path;
    Animator anim;
    Pose pose;
    std::vector<mat4> world;
    int clip = -1, variant = -1;
    mat4 fix;
};

struct Viewer {
    std::vector<std::pair<std::string, std::string>> files;   // path, clip
    std::string clipAll, variantName;
    bool native = false;
    float seek = -1, height = 0, spacing = 2.2f, distance = 0, yawDeg = 25, pitchDeg = 32;
    float3 team{0.92f * 0.92f, 0.22f * 0.22f, 0.20f * 0.20f};
    MaterialQuality quality = MaterialQuality::High;
    bool qualitySet = false, paused = false, baked = false;
    std::string gameDir;
    std::vector<std::string> types;
    float start = 0, fixedDt = 0;
    std::unique_ptr<modelview::Gallery> gallery;

    gfx::Device* dev = nullptr;
    Renderer3D r3d;
    ImGuiLayer gui;
    RTSCamera cam;
    std::map<std::string, std::unique_ptr<Model>> models;
    std::vector<Instance> inst;
    std::vector<std::string> log;
    Mesh ground;
    double time = 0;
    int variant = -1, teamIdx = 0;
    bool dragging = false;
    float lastX = 0, lastY = 0;

    static bool readFile(const std::string& p, std::string& out) {
        std::ifstream f(p, std::ios::binary);
        if (f) { std::stringstream ss; ss << f.rdbuf(); out = ss.str(); return true; }
        return platform::readContentFile(p, out);
    }

    void init(gfx::Device& d) {
        dev = &d;
        r3d.init(d);
        r3d.setShadowQuality(ShadowQuality::High);
        r3d.setMaterialQuality(qualitySet ? quality : Renderer3D::defaultMaterialQualityFor(d, platform::isTouchDevice()));
        gui.init(d, "engine/fonts/ui.ttf", 15.0f);
        MeshBuilder gb;
        gb.color(rgba(120, 132, 110)).box({0, -0.05f, 0}, {60, 0.1f, 60});
        ground = gb.upload(d);
        time = start;
        if (!gameDir.empty()) {
            gallery = std::make_unique<modelview::Gallery>();
            std::string err;
            if (!gallery->init(d, gameDir, types, err)) { log.push_back(err); gallery.reset(); }
            else for (auto& w : gallery->warnings()) log.push_back(w);
            if (gallery) {
                cam.target = gallery->center();
                cam.distance = distance > 0 ? distance : gallery->extent() * 0.75f;
                cam.yaw = yawDeg * 3.14159265f / 180.0f;
                cam.pitch = pitchDeg * 3.14159265f / 180.0f;
                cam.minDistance = 1;
                for (auto& l : log) std::printf("%s\n", l.c_str());
                return;
            }
        }
        float x = -spacing * float(files.size() - 1) * 0.5f, maxH = 1;
        for (auto& [path, clip] : files) {
            auto it = models.find(path);
            if (it == models.end()) {
                std::unique_ptr<Model> m;
                try {
                    std::vector<std::string> warn;
                    m = loadModel(path, readFile, &warn);
                    for (auto& w : warn) log.push_back(w);
                    uploadModel(d, *m);
                // as the game view does by default: materials named "*team*" take the team colour
                for (auto& mt : m->materials) {
                    std::string n = mt.name;
                    for (auto& ch : n) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                    if (n.find("team") != std::string::npos && mt.team == TeamMode::None) mt.team = TeamMode::Tint;
                }
                    char buf[256];
                    std::snprintf(buf, sizeof buf, "%s: %zu nodes, %zu meshes, %zu materials, %zu textures, %zu skins, %zu clips",
                                  path.c_str(), m->nodes.size(), m->meshes.size(), m->materials.size(), m->images.size(), m->skins.size(), m->clips.size());
                    log.push_back(buf);
                    for (auto& c : m->clips) log.push_back("  clip '" + c.name + "' " + std::to_string(c.duration) + " s");
                    for (auto& im : m->images) if (im.gpuCompressed) log.push_back("  texture '" + im.name + "' stays GPU-compressed");
                    if (m->baked.tex.valid() || bakeAnimations(d, *m)) log.push_back("  clips baked: " + std::to_string(m->baked.rows) + " rows at " + std::to_string(int(m->baked.fps)) + " fps");
                    for (auto& v : m->variants) log.push_back("  variant '" + v + "'");
                } catch (const std::exception& e) { log.push_back(e.what()); }
                it = models.emplace(path, std::move(m)).first;
            }
            Instance in;
            in.model = it->second.get();
            in.path = path;
            if (in.model) {
                Model& m = *in.model;
                float3 mn = m.boundsMin, mx = m.boundsMax;
                float s = height > 0 && mx.y > mn.y ? height / (mx.y - mn.y) : 1.0f;
                // formats with their own units (voxels, Warcraft, Generals, Collada in cm) would put the
                // camera inside the model: frame anything far from game size at 2 units (--native keeps it)
                const float big = std::max({mx.x - mn.x, mx.y - mn.y, mx.z - mn.z});
                if (height <= 0 && !native && big > 1e-6f && (big > 6.0f || big < 0.2f)) {
                    s = 2.0f / big;
                    log.push_back(path + ": shown at " + std::to_string(s) + "x (native size " + std::to_string(big) + " units; --native to keep it)");
                }
                in.fix = mat4::translate({x, 0, 0}) * mat4::scale({s, s, s}) * mat4::translate({-(mn.x + mx.x) * 0.5f, -mn.y, -(mn.z + mx.z) * 0.5f});
                maxH = std::max(maxH, (mx.y - mn.y) * s);
                in.anim.setModel(&m);
                std::string want = clip.empty() ? clipAll : clip;
                in.clip = want.empty() ? (m.clips.empty() ? -1 : 0) : m.findClip(want);
                if (!want.empty() && in.clip < 0) log.push_back(path + ": no clip '" + want + "'");
                play(in);
                if (!variantName.empty()) in.variant = m.findVariant(variantName);
            }
            inst.push_back(std::move(in));
            x += spacing;
        }
        for (auto& l : log) std::printf("%s\n", l.c_str());
        cam.target = {0, maxH * 0.45f, 0};
        cam.distance = distance > 0 ? distance : std::max(4.0f, spacing * float(files.size()) * 1.1f + maxH);
        cam.yaw = yawDeg * 3.14159265f / 180.0f;
        cam.pitch = pitchDeg * 3.14159265f / 180.0f;
        cam.minDistance = 1;
    }

    void play(Instance& in) {
        if (!in.model || in.clip < 0) return;
        Animator::Play p;
        p.fade = 0;
        p.restart = true;
        in.anim.play(0, in.clip, p);
        if (seek >= 0) in.anim.update(seek);
    }

    void frame(gfx::Device& d, double dt) {
        if (fixedDt > 0) dt = fixedDt;
        if (!paused && seek < 0) time += dt;
        d.beginFrame(platform::framebufferWidth(), platform::framebufferHeight());
        gui.newFrame(dt);
        gfx::TextureHandle portrait = gallery ? gallery->portrait(time) : gfx::TextureHandle{}; // before the scene pass
        r3d.beginScene(cam, d.framebufferWidth(), d.framebufferHeight());
        if (gallery) { gallery->update(time); gallery->draw(r3d, time); }
        else r3d.drawMesh(ground, mat4{}, {1, 1, 1, 1}, kDrawNoCastShadow, 0.05f);
        for (auto& in : inst) {
            if (!in.model) continue;
            if (!paused && seek < 0) in.anim.update(float(dt));
            in.anim.evaluate(in.pose);
            worldMatrices(*in.model, in.pose, in.world);
            ModelDraw md;
            md.model = in.model;
            md.world = in.fix;
            md.nodeWorld = &in.world;
            md.pose = &in.pose;
            md.team = team;
            md.variant = variant >= 0 ? variant : in.variant;
            BakedSample bs;
            if (baked && in.model->baked.tex.valid()) { // skin from the baked clip texture, as crowds do
                const auto b = in.anim.baseSample();
                bs = {b.clipA, b.clipB, b.timeA, b.timeB, b.w, b.loopA, b.loopB};
                md.baked = &bs;
            }
            r3d.drawModel(md);
        }
        gfx::PassDesc pass;
        pass.clearColor[0] = 0.55f; pass.clearColor[1] = 0.66f; pass.clearColor[2] = 0.78f;
        r3d.render(pass);
        ImGui::SetNextWindowPos(ImVec2(8, 8));
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("info", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
        static const char* q[] = {"low", "medium", "high"};
        ImGui::Text("materials: %s   draw calls: %u", q[int(r3d.materialQuality())], r3d.drawCalls());
        for (auto& in : inst)
            if (in.model) ImGui::Text("%s  %s", in.path.c_str(), in.clip >= 0 ? in.model->clips[size_t(in.clip)].name.c_str() : "(bind pose)");
        if (portrait.valid()) {
            const bool flip = d.info().renderTargetFlipY;
            ImGui::Image(ImTextureRef(ImTextureID(portrait.id)), ImVec2(128, 128), ImVec2(0, flip ? 0.f : 1.f), ImVec2(1, flip ? 1.f : 0.f));
        }
        if (gallery) for (auto& ev : gallery->recentEvents()) ImGui::TextColored(ImVec4(1, 0.5f, 0.85f, 1), "event: %s", ev.c_str());
        if (!r3d.modelError().empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", r3d.modelError().c_str());
        ImGui::End();
        gui.render();
        d.endPass();
        d.endFrame();
    }

    void event(const platform::Event& e) {
        if (gui.handleEvent(e)) return;
        using platform::EventType;
        if (e.type == EventType::MouseDown) { dragging = true; lastX = e.x; lastY = e.y; }
        else if (e.type == EventType::MouseUp) dragging = false;
        else if (e.type == EventType::MouseMove && dragging) {
            cam.yaw -= (e.x - lastX) * 0.01f;
            cam.pitch = std::clamp(cam.pitch + (e.y - lastY) * 0.01f, 0.05f, 1.5f);
            lastX = e.x; lastY = e.y;
        } else if (e.type == EventType::MouseWheel) cam.distance = std::clamp(cam.distance * (e.wheelY > 0 ? 0.9f : 1.1f), 1.0f, 200.0f);
        else if (e.type == EventType::KeyDown) {
            using platform::Key;
            if (e.key == Key::Space) for (auto& in : inst) if (in.model && !in.model->clips.empty()) { in.clip = (in.clip + 1) % int(in.model->clips.size()); play(in); }
            if (e.key == Key::Q) r3d.setMaterialQuality(MaterialQuality((int(r3d.materialQuality()) + 1) % 3));
            if (e.key == Key::V) variant = variant + 1 >= 4 ? -1 : variant + 1;
            if (e.key == Key::P) paused = !paused;
            if (e.key == Key::T) {
                static const float3 teams[] = {{0.85f, 0.05f, 0.04f}, {0.04f, 0.2f, 0.9f}, {0.06f, 0.64f, 0.09f}, {0.94f, 0.64f, 0.02f}};
                teamIdx = (teamIdx + 1) % 4; team = teams[teamIdx];
            }
        }
    }

    void shutdown() {
        for (auto& [p, m] : models) if (m) releaseModel(*dev, *m);
        if (gallery) gallery->shutdown(*dev);
        dev->destroyBuffer(ground.vb); dev->destroyBuffer(ground.ib);
        gui.shutdown();
        r3d.shutdown();
    }
};

Viewer* g_viewer = nullptr;

float3 parse3(const std::string& s) {
    float3 v;
    std::sscanf(s.c_str(), "%f,%f,%f", &v.x, &v.y, &v.z);
    return {v.x * v.x, v.y * v.y, v.z * v.z}; // given like player colours (~sRGB)
}

} // namespace

int rtsMain(int argc, char** argv) {
    static Viewer v;
    g_viewer = &v;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--clip") v.clipAll = next();
        else if (a == "--game") v.gameDir = next();
        else if (a == "--types") {
            std::stringstream ss(next());
            for (std::string t; std::getline(ss, t, ',');) v.types.push_back(t);
        }
        else if (a == "--start") v.start = std::stof(next());
        else if (a == "--baked") v.baked = true;
        else if (a == "--fixed-dt") v.fixedDt = std::stof(next());
        else if (a == "--time") v.seek = std::stof(next());
        else if (a == "--variant") v.variantName = next();
        else if (a == "--team") v.team = parse3(next());
        else if (a == "--height") v.height = std::stof(next());
        else if (a == "--spacing") v.spacing = std::stof(next());
        else if (a == "--distance") v.distance = std::stof(next());
        else if (a == "--native") v.native = true;
        else if (a == "--yaw") v.yawDeg = std::stof(next());
        else if (a == "--pitch") v.pitchDeg = std::stof(next());
        else if (a == "--quality") {
            std::string q = next();
            v.quality = q == "low" ? MaterialQuality::Low : q == "medium" ? MaterialQuality::Medium : MaterialQuality::High;
            v.qualitySet = true;
        } else {
            size_t at = a.find('@');
            v.files.push_back({a.substr(0, at), at == std::string::npos ? std::string() : a.substr(at + 1)});
        }
    }
    if (v.files.empty() && v.gameDir.empty()) {
        std::printf("usage: rts_modelview model.glb [model2.gltf@Clip ...] [--clip C] [--time T] [--variant V] [--quality low|medium|high]\n"
                    "       rts_modelview --game games/<id> [--types a,b] [--start T] [--fixed-dt S]\n");
        return 1;
    }
    platform::AppConfig cfg;
    cfg.title = "RTSEngine model viewer";
    cfg.width = 1280; cfg.height = 720;
    platform::AppCallbacks cb;
    cb.init = [](gfx::Device& d) { g_viewer->init(d); };
    cb.frame = [](gfx::Device& d, double dt) { g_viewer->frame(d, dt); };
    cb.event = [](const platform::Event& e) { g_viewer->event(e); };
    cb.shutdown = [] { g_viewer->shutdown(); };
    return platform::run(cfg, cb);
}
