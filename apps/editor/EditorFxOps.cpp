// fx_preview: spawn a particle effect in the editor's Scene (or, during a play-test, Game) view at a
// point, so an agent can look at it with view_screenshot. Also the per-frame particle tick shared by
// both views (previews, the play-test's event effects, particle lights). Core fx ops: core/FxOps.cpp.
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "Editor.h"
#include "core/FxOps.h"

namespace rts::editor {

void Editor::tickFx(const render::RTSCamera& cam, int w, int h, const std::vector<game::SceneObject>* objects) {
    const bool active = playing() || !fxPreview_.name.empty() || gameFx_.stats().particles > 0 || gameFx_.stats().instances > 0;
    if (!active) { fxTime_ = time_; return; }
    float dt = fxTime_ < 0 ? 0.0f : float(time_ - fxTime_);
    if (playing()) dt = playPaused ? 0.0f : dt * playSpeed;
    fxTime_ = time_;
    gameFx_.setView(cam, r3d_.viewProj(), w, h);
    if (playing() && dt > 0) gameFxBridge_.update(*sim_, objects);
    if (fxFrozen_) dt = 0;
    if (fxPreview_.loop && !fxPreview_.name.empty() && !gameFx_.playing(fxPreview_.h) && time_ >= fxPreview_.next && dt > 0) {
        fxPreview_.h = gameFx_.play(fxPreview_.name, fxPreview_.pos, {0, 1, 0}, fxPreview_.scale, fxPreview_.tint);
        int id = gameFx_.effect(fxPreview_.name);
        const fx::EffectDef* d = gameFx_.def(id);
        fxPreview_.next = time_ + (d && !d->looping() ? std::min(8.0, double(d->lifetime()) + 0.5) : 0.5);
    }
    gameFx_.update(dt);
    std::vector<fx::FxPointLight> lights;
    gameFx_.collectLights(lights);
    for (auto& l : lights) r3d_.lights.add(l.pos, l.radius, l.color, l.intensity);
}

void Editor::registerFxPreviewOps() {
    ws_.registerOp({"fx_preview",
        "Play a particle effect in the editor so you can look at it: spawns fx/<effect>.json at x, z (default: the view's centre; y = "
        "height above the ground) in the Scene view, or in the Game view during a play-test, and moves that view's camera there "
        "('distance' optional). 'advance' pre-runs it (seconds) and 'freeze' holds it still: then view_screenshot {window: 'scene'} "
        "shows that moment. 'loop' replays one-shots; 'scale', 'color' (\"#rrggbbaa\" tint), 'quality' (low | medium | high: how weak "
        "GPUs see it, also affects the play-test). {stop: true} removes the preview.",
        Json::parse(R"({"properties": {"effect": {"type": "string"}, "x": {"type": "number"}, "z": {"type": "number"}, "y": {"type": "number"},
            "scale": {"type": "number"}, "color": {"type": "string"}, "advance": {"type": "number"}, "freeze": {"type": "boolean"},
            "loop": {"type": "boolean"}, "distance": {"type": "number"}, "quality": {"type": "string", "enum": ["low", "medium", "high"]},
            "stop": {"type": "boolean"}}})"),
        [this](const Json& p) -> Json {
            Json r = Json::object();
            if (p.getBool("stop")) {
                gameFx_.kill(fxPreview_.h);
                fxPreview_ = {};
                fxFrozen_ = false;
                r.set("ok", Json::fromBool(true));
                r.set("summary", Json::string("Превью эффекта убрано"));
                return r;
            }
            if (ws_.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
            const std::string name = p.getStr("effect");
            if (!validFxName(name) || name == "fx") throw std::runtime_error("'effect' must be an effect name (fx_list)");
            // an fx_set / import this frame has not been applied yet: load the new files now
            if (std::any_of(ws_.changes.begin(), ws_.changes.end(), [](const Change& c) { return c.kind == Change::Fx; })) reloadFx();
            if (p.has("quality")) {
                fx::FxQuality q;
                if (!fx::qualityFromName(p.getStr("quality"), q)) throw std::runtime_error("quality: low, medium or high");
                gameFx_.setQuality(q);
            }
            gameFx_.clearErrors();
            const int id = gameFx_.effect(name);
            if (id < 0) {
                std::string why;
                for (auto& e : gameFx_.errors()) why += "\n" + e;
                throw std::runtime_error("effect '" + name + "' cannot be played:" + (why.empty() ? std::string(" not found") : why));
            }
            fx::float4 tint{1, 1, 1, 1};
            if (p.has("color") && !fx::parseColor(p["color"], tint)) throw std::runtime_error("color must be \"#rrggbb\" or \"#rrggbbaa\"");
            render::RTSCamera& cam = playing() ? gameCam : sceneCam;
            const float x = p.has("x") ? float(p["x"].toDouble()) : cam.target.x, z = p.has("z") ? float(p["z"].toDouble()) : cam.target.z;
            const float ground = sim_ ? view_.heightAt(simModule()->state(*sim_).grid, x, z) : view_.heightAt(grid_, x, z);
            const fx::float3 pos{x, ground + float(p["y"].toDouble(0)), z};
            cam.target = {x, ground, z};
            if (p.has("distance")) cam.distance = std::clamp(float(p["distance"].toDouble()), cam.minDistance, cam.maxDistance);
            else if (cam.distance > 22) cam.distance = std::max(cam.minDistance, 14.0f);
            if (!playing()) sceneFlying_ = false;
            // culling uses the camera of the last frame: give it the new one before play()
            Viewport* vp = playing() ? gameViewport : sceneViewport;
            const int w = vp && vp->w > 0 ? vp->w : 1280, h = vp && vp->h > 0 ? vp->h : 720;
            gameFx_.setView(cam, cam.proj(float(w) / float(h), dev_ && dev_->info().clipDepthZeroToOne) * cam.view(), w, h);
            gameFx_.kill(fxPreview_.h);
            const float scale = std::clamp(float(p["scale"].toDouble(1)), 0.05f, 20.0f);
            fx::FxHandle hnd = gameFx_.play(id, pos, {0, 1, 0}, scale, tint);
            if (!hnd.valid())
                throw std::runtime_error(gameFx_.quality() == fx::FxQuality::Off ? "particles are switched off (quality off)" : "the effect was not started (instance budget full)");
            const fx::EffectDef* d = gameFx_.def(id);
            const float advance = std::clamp(float(p["advance"].toDouble(0)), 0.0f, 10.0f);
            for (int i = 0, n = int(std::lround(advance * 60)); i < n; ++i) gameFx_.update(1.0f / 60.0f);
            fxFrozen_ = p.getBool("freeze");
            fxPreview_ = {name, pos, scale, tint, p.getBool("loop"), hnd, time_ + (d && !d->looping() ? std::min(8.0, double(d->lifetime()) + 0.5) : 0.5)};
            fxTime_ = time_;
            r.set("ok", Json::fromBool(true));
            r.set("effect", Json::string(name));
            r.set("x", Json::number(double(pos.x), 2));
            r.set("y", Json::number(double(pos.y), 2));
            r.set("z", Json::number(double(pos.z), 2));
            r.set("view", Json::string(playing() ? "game" : "scene"));
            r.set("particles", Json::number(int64_t(gameFx_.stats().particles)));
            r.set("quality", Json::string(fx::qualityName(gameFx_.quality())));
            if (d) {
                r.set("emitters", Json::number(int64_t(d->emitters.size())));
                r.set("looping", Json::fromBool(d->looping()));
                if (!d->looping()) r.set("lifetime", Json::number(double(d->lifetime()), 2));
            }
            r.set("next", Json::string(std::string("view_screenshot {\"window\": \"") + (playing() ? "game" : "scene") + "\"}"));
            r.set("summary", Json::string("Превью эффекта " + name));
            return r;
        },
        true});
}

} // namespace rts::editor
