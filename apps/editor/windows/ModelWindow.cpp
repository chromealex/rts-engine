// Models: the selected type as the game draws it (portrait with its idle clip), with what its model
// contains — clips, nodes, materials, variants, baked skinning — and the warnings of its view
// block. Model files are watched: saving a model file reloads the views. Formats the runtime does
// not read (.blend, .lwo, .x, ...) are converted to a "<file>.glb" sidecar with modelconv (next to
// rtsbuild) when they are first used and whenever they change. Agents get the same information
// through the `model_info` operation.
#include <cstdio>
#include <filesystem>
#include "../Editor.h"
#include "modules/rts/view/RtsPortrait.h"

namespace fs = std::filesystem;

namespace rts::editor {

namespace {
Json modelInfo(const render::Model& m) {
    Json r = Json::object();
    r.set("path", Json::string(m.path));
    Json clips = Json::array();
    for (const auto& c : m.clips) {
        Json cj = Json::object();
        cj.set("name", Json::string(c.name));
        cj.set("duration", Json::number(double(c.duration)));
        clips.push(cj);
    }
    r.set("clips", clips);
    Json nodes = Json::array();
    for (const auto& n : m.nodes) nodes.push(Json::string(n.name));
    r.set("nodes", nodes);
    Json mats = Json::array();
    static const char* alpha[] = {"opaque", "mask", "blend"};
    static const char* team[] = {"none", "tint", "mask", "alpha", "vertex"};
    for (const auto& mt : m.materials) {
        Json mj = Json::object();
        mj.set("name", Json::string(mt.name));
        mj.set("alpha", Json::string(alpha[int(mt.alpha)]));
        mj.set("team", Json::string(team[int(mt.team)]));
        mj.set("textured", Json::fromBool(mt.baseTex >= 0));
        mats.push(mj);
    }
    r.set("materials", mats);
    Json vars = Json::array();
    for (const auto& v : m.variants) vars.push(Json::string(v));
    r.set("variants", vars);
    r.set("skins", Json::number(int64_t(m.skins.size())));
    r.set("bakedClips", Json::fromBool(m.baked.tex.valid()));
    Json b = Json::array();
    b.push(Json::number(double(m.boundsMax.x - m.boundsMin.x)));
    b.push(Json::number(double(m.boundsMax.y - m.boundsMin.y)));
    b.push(Json::number(double(m.boundsMax.z - m.boundsMin.z)));
    r.set("size", b);
    return r;
}
} // namespace

class ModelWindow : public EditorWindow {
public:
    const char* id() const override { return "models"; }
    const char* title() const override { return "Models"; }
    const char* dockArea() const override { return "right"; }
    int order() const override { return 3; }
    bool openByDefault() const override { return false; }

    ~ModelWindow() override { portrait_.shutdown(); }

    void draw(Editor& ed) override {
        const auto& types = ed.rules().types;
        if (types.empty()) { ImGui::TextDisabled("Нет типов"); return; }
        if (ed.selection.kind == Selection::Type)
            for (size_t i = 0; i < types.size(); ++i) if (types[i].id == ed.selection.key) type_ = int(i);
        type_ = std::clamp(type_, 0, int(types.size()) - 1);
        if (ImGui::BeginCombo("Тип", types[size_t(type_)].id.c_str())) {
            for (size_t i = 0; i < types.size(); ++i)
                if (ImGui::Selectable(types[i].id.c_str(), int(i) == type_)) type_ = int(i);
            ImGui::EndCombo();
        }
        ImGui::SliderInt("Игрок", &owner_, 0, 8);
        if (!portraitInit_) { portraitInit_ = true; portrait_.init(ed.device(), 256); }
        time_ += ImGui::GetIO().DeltaTime;
        portrait_.render(ed.view(), uint16_t(type_), uint8_t(owner_), time_);
        if (portrait_.texture().valid()) {
            const bool flip = ed.device().info().renderTargetFlipY;
            ImGui::Image(ImTextureRef(ImTextureID(portrait_.texture().id)), ImVec2(192, 192), ImVec2(0, flip ? 0.f : 1.f), ImVec2(1, flip ? 1.f : 0.f));
        }
        const std::string& tid = types[size_t(type_)].id;
        for (const auto& w : ed.view().warnings())
            if (w.find("type '" + tid + "'") == 0) ImGui::TextColored(ImVec4(1, 0.55f, 0.3f, 1), "%s", w.c_str());
        const render::Model* m = ed.view().modelOf(uint16_t(type_));
        if (!m) return;
        ImGui::Text("%s", m->path.c_str());
        ImGui::Text("Узлов %zu, мешей %zu, материалов %zu, текстур %zu, скинов %zu%s", m->nodes.size(), m->meshes.size(), m->materials.size(),
                    m->images.size(), m->skins.size(), m->baked.tex.valid() ? ", клипы запечены" : "");
        if (ImGui::CollapsingHeader("Клипы", ImGuiTreeNodeFlags_DefaultOpen))
            for (const auto& c : m->clips) ImGui::BulletText("%s  %.2f с", c.name.c_str(), double(c.duration));
        if (ImGui::CollapsingHeader("Материалы"))
            for (const auto& mt : m->materials)
                ImGui::BulletText("%s%s%s", mt.name.c_str(), mt.team != render::TeamMode::None ? "  [цвет игрока]" : "",
                                  mt.alpha == render::AlphaMode::Blend ? "  [прозрачный]" : mt.alpha == render::AlphaMode::Mask ? "  [маска]" : "");
        if (!m->variants.empty() && ImGui::CollapsingHeader("Варианты"))
            for (const auto& v : m->variants) ImGui::BulletText("%s", v.c_str());
        if (ImGui::CollapsingHeader("Узлы (для procedural, attach, fx, muzzle)"))
            for (const auto& n : m->nodes) ImGui::BulletText("%s", n.name.c_str());
    }

    void update(Editor& ed, double dt) override {
        if (!opRegistered_) {
            opRegistered_ = true;
            ed.ws().registerOp({"model_info", "Contents of the model of a unit type (clips, nodes, materials, variants) and its view warnings.",
                                Json::parse(R"({"type": "object", "properties": {"type": {"type": "string", "description": "type id from rules.json"}}, "required": ["type"]})"),
                                [&ed](const Json& p) {
                                    Json r = Json::object();
                                    const std::string id = p.getStr("type");
                                    const auto& types = ed.rules().types;
                                    for (size_t i = 0; i < types.size(); ++i) {
                                        if (types[i].id != id) continue;
                                        if (const render::Model* m = ed.view().modelOf(uint16_t(i))) r = modelInfo(*m);
                                        Json w = Json::array();
                                        for (const auto& s : ed.view().warnings()) if (s.find("type '" + id + "'") == 0) w.push(Json::string(s));
                                        r.set("warnings", w);
                                        return r;
                                    }
                                    r.set("error", Json::string("unknown type '" + id + "'"));
                                    return r;
                                }, true});
        }
        // hot reload: model files of the open game changed on disk
        sinceCheck_ += dt;
        if (sinceCheck_ < 1.0) return;
        sinceCheck_ = 0;
        const std::string& dir = ed.ws().project.dir;
        if (dir.empty()) return;
        bool changed = false;
        for (const auto& f : ed.view().modelFiles()) {
            std::error_code ec;
            const fs::path src = fs::path(dir) / f;
            const auto t = fs::last_write_time(src, ec);
            if (ec) continue;
            if (!render::isNativeModelFormat(f)) {   // converted format: keep its sidecar up to date
                const fs::path side = fs::path(src.string() + ".glb");
                std::error_code e2;
                if ((!fs::exists(side, e2) || fs::last_write_time(side, e2) < t) && convert(ed, src.string())) changed = true;
            }
            auto it = stamps_.find(f);
            if (it == stamps_.end()) stamps_[f] = t;
            else if (it->second != t) { it->second = t; changed = true; ed.ws().log("view", "модель изменена: " + f); }
        }
        if (changed) ed.reloadRules();
    }

private:
    // modelconv sits next to rtsbuild; one attempt per file and modification (no retry loops)
    bool convert(Editor& ed, const std::string& src) {
        std::error_code ec;
        const auto t = fs::last_write_time(src, ec);
        auto& tried = tried_[src];
        if (tried == t) return false;
        tried = t;
        fs::path tool = fs::path(ed.ws().buildTool).parent_path() /
#if defined(_WIN32)
                        "modelconv.exe";
#else
                        "modelconv";
#endif
        if (!fs::exists(tool, ec)) {
            ed.ws().log("view", "нужна конвертация " + src + ", но modelconv не найден рядом с rtsbuild");
            return false;
        }
        const std::string cmd = "\"" + tool.string() + "\" \"" + src + "\" 2>&1";
#if defined(_WIN32)
        FILE* p = _popen(cmd.c_str(), "r");
#else
        FILE* p = popen(cmd.c_str(), "r");
#endif
        if (!p) return false;
        std::string out;
        char buf[256];
        while (std::fgets(buf, sizeof buf, p)) out += buf;
#if defined(_WIN32)
        const int rc = _pclose(p);
#else
        const int rc = pclose(p);
#endif
        ed.ws().log("view", (rc == 0 ? "сконвертирована модель " : "не удалось сконвертировать ") + src + (rc == 0 ? std::string() : ": " + out));
        return rc == 0;
    }

    std::map<std::string, fs::file_time_type> tried_;
    int type_ = 0, owner_ = 1;
    double time_ = 0, sinceCheck_ = 0;
    bool portraitInit_ = false, opRegistered_ = false;
    game::Portrait portrait_;
    std::map<std::string, fs::file_time_type> stamps_;
};

RTS_EDITOR_WINDOW(ModelWindow);

} // namespace rts::editor
