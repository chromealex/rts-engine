#include "Gallery.h"
#include "../../modules/rts/RtsModule.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace rts::modelview {

using namespace render;
using namespace game;

namespace {
constexpr float kCycle = 9.0f;      // idle 0-2, move 2-4, attack 4-6.5, dead 6.5-9
bool readFile(const std::string& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}
} // namespace

bool Gallery::init(gfx::Device& dev, const std::string& dir, const std::vector<std::string>& types, std::string& err) {
    dir_ = dir.empty() || dir.back() == '/' ? dir : dir + "/";
    std::string text;
    if (!readFile(dir_ + "rules.json", text)) { err = dir_ + "rules.json not found"; return false; }
    try { rules_ = loadRules(Json::parse(text)); }
    catch (const std::exception& e) { err = e.what(); return false; }
    dev_ = &dev;
    view_.init(dev, rules_, [this](const std::string& p, std::string& out) { return readFile(dir_ + p, out); });
    // structures in the back row, units in front; widths from footprints
    float xs = 2, xu = 2;
    for (size_t i = 0; i < rules_.types.size(); ++i) {
        const UnitType& t = rules_.types[i];
        if (!types.empty() && std::find(types.begin(), types.end(), t.id) == types.end()) continue;
        Slot s;
        s.type = uint16_t(i);
        if (t.structure) { s.base = {xs + t.footprintW * 0.5f, 0, 4}; xs += float(t.footprintW) + 1.5f; }
        else { s.base = {xu + 0.75f, 0, 9}; xu += 2.2f; }
        slots_.push_back(s);
    }
    const float w = std::max(xs, xu) + 2;
    grid_.init(int(std::ceil(w)), 14);
    for (auto& f : grid_.flags) f = kCellGround;
    view_.buildTerrain(dev, grid_);
    float3 mn{1e9f, 0, 1e9f}, mx{-1e9f, 0, -1e9f};
    for (auto& sl : slots_) { mn = min3(mn, sl.base); mx = max3(mx, sl.base); }
    if (slots_.empty()) mn = mx = {w * 0.5f, 0, 6.5f};
    center_ = {(mn.x + mx.x) * 0.5f, 0.5f, (mn.z + mx.z) * 0.5f};
    extent_ = std::max(6.0f, std::max(mx.x - mn.x, mx.z - mn.z) + 4);
    return true;
}

void Gallery::shutdown(gfx::Device& dev) { portrait_.shutdown(); view_.shutdown(dev); }

gfx::TextureHandle Gallery::portrait(double time) {
    if (slots_.empty() || !dev_) return {};
    if (!portraitInit_) { portraitInit_ = true; portrait_.init(*dev_, 192); }
    portrait_.render(view_, slots_[0].type, 1, time);
    return portrait_.texture();
}

void Gallery::update(double time) {
    scene_.objects.clear();
    std::vector<SimEvent> events;
    for (size_t i = 0; i < slots_.size(); ++i) {
        Slot& s = slots_[i];
        const UnitType& t = rules_.types[s.type];
        const float tt = float(time) + float(i) * 0.37f;
        const float ph = std::fmod(tt, kCycle);
        const uint32_t cycle = uint32_t(tt / kCycle);
        auto crossed = [&](float at) { return s.lastPhase >= 0 && s.lastPhase < at && ph >= at; };
        Entity e{uint32_t(i), cycle};
        if (s.lastPhase >= 0 && cycle != s.gen) events.push_back({kEvSpawned, 1, e, kNullEntity, s.type, {}});
        s.gen = cycle;
        if (crossed(6.5f)) events.push_back({kEvDied, 1, Entity{uint32_t(i), cycle}, kNullEntity, s.type, {}});
        const bool armed = !t.weapons.empty();
        for (float at : {4.3f, 5.5f}) if (armed && crossed(at)) events.push_back({kEvAttack, 1, e, kNullEntity, 0, {}});
        if (crossed(5.0f)) events.push_back({kEvHit, 2, e, kNullEntity, 10, {}});
        s.lastPhase = ph;
        if (ph >= 6.5f) continue; // dead: the view keeps the corpse
        game::SceneObject o;
        o.entity = e;
        o.type = s.type;
        o.owner = uint8_t(1 + i % 2);
        o.pos = s.base;
        o.facing = {0, 0, 1};       // towards the camera
        if (t.structure) {
            o.build = std::clamp(ph / 2.0f, 0.0f, 1.0f);
            o.producing = ph >= 2 && ph < 4 && !t.produces.empty();
        } else if (ph >= 2 && ph < 4) { // back and forth along x
            const float u = (ph - 2) / 2;
            o.pos.x += 0.9f * std::sin(u * 6.2831853f);
            const float dir = std::cos(u * 6.2831853f);
            o.facing = {dir >= 0 ? 1.0f : -1.0f, 0, 0};
            o.moving = true;
            o.carry = 10; // carried cargo (socket attachments with "carry")
            o.carryRes = int16_t((s.gen + i) % std::max<size_t>(1, rules_.resources.size()));
        }
        if (ph >= 4) o.hp = 0.35f;  // damaged: damage stages, effect anchors
        if (armed && ph >= 4 && ph < 6.5f) {
            const float a = (ph - 4) * 1.2f;
            o.hasTarget = true;
            o.target = s.base + float3{3 * std::cos(a), 0, 3 + 2 * std::sin(a)};
            o.order = uint8_t(OrderType::Attack) + 1;
            if (!t.structure) { float3 d = o.target - o.pos; o.facing = normalize(float3{d.x, 0, d.z}); }
        }
        o.selected = i % 4 == 1; // selection outlines
        o.hovered = i % 4 == 2;
        scene_.objects.push_back(o);
    }
    if (!events.empty()) { scene_.events = std::move(events); scene_.tick = ++tick_; }
}

void Gallery::draw(Renderer3D& r, double time) {
    view_.draw(r, scene_, time, nullptr, grid_);
    // placement ghost of the first structure type, alternating allowed / blocked
    for (const auto& sl : slots_)
        if (rules_.types[sl.type].structure) {
            view_.drawGhost(r, sl.type, {center_.x, 0, 12.0f}, std::fmod(time, 2.0) < 1.0, grid_, time);
            break;
        }
    // effect anchors and view events made visible (particles / sounds would attach here)
    for (const auto& e : view_.effects()) {
        r.circleXZ(e.pos, 0.12f, rgba(255, 220, 60, 230), 12, e.pos.y);
        r.line(e.pos, e.pos + float3{0, 0.3f, 0}, rgba(255, 220, 60, 230));
    }
    for (const auto& e : view_.events()) {
        r.circleXZ(e.pos, 0.35f, rgba(255, 90, 200, 255), 16, e.pos.y + 0.05f);
        recent_.push_back(e.name + " (" + rules_.types[e.type].id + ")");
    }
    if (recent_.size() > 8) recent_.erase(recent_.begin(), recent_.end() - 8);
}

} // namespace rts::modelview
