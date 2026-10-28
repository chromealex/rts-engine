// Client controls for player-level mechanics: player abilities (skills bar), rect tools (dig marks,
// painted rooms, roads), the dungeon hand (pick up / drop / slap) and possession (direct control).
// Like the rest of the client this only reads the sim and turns input into Commands.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "GameClient.h"

namespace rts::client {

using i18n::Args;
using platform::Event;
using platform::EventType;
using platform::Key;

namespace {

std::string fmt(const char* f, auto... args) {
    char b[256];
    std::snprintf(b, sizeof b, f, args...);
    return b;
}

void pushSection(UiState& s, const UiSection& sec) {   // same layout as ClientUi.cpp
    UiAction h;
    h.header = true; h.label = sec.title; h.kind = sec.kind; h.id = -1;
    s.card.push_back(h);
    for (UiAction a : sec.actions) { a.kind = sec.kind; s.card.push_back(std::move(a)); }
}

std::string costText(const Rules& r, const std::vector<Amount>& cost) {
    std::string c;
    for (auto& x : cost) {
        if (x.res >= r.resources.size()) continue;
        const Fixed v = x.value;
        char b[48];
        // per-cell prices are often fractional (road: 0.2 stone)
        if (v.raw % Fixed::fromInt(1).raw == 0) std::snprintf(b, sizeof b, "%lld", (long long)v.toInt());
        else std::snprintf(b, sizeof b, "%.1f", v.toDouble());
        c += (c.empty() ? "" : "  ") + std::string(b) + " " + r.resources[x.res].name;
    }
    return c;
}

Fixed fx(float v) { return Fixed::fromRatio(int64_t(std::lround(v * 256.0f)), 256); }

bool instantTarget(TargetKind k) { return k == TargetKind::None || k == TargetKind::Self || k == TargetKind::Player; }

} // namespace

bool GameClient::rectMode() const {
    return mode_ == Mode::Dig || mode_ == Mode::Undig || mode_ == Mode::Room || mode_ == Mode::Road || mode_ == Mode::Unroad;
}

Entity GameClient::possessedUnit() const {
    if (!sim_ || spectator_) return kNullEntity;
    auto* gs = sim_->world().resource<GameState>();
    if (!gs || human_ >= gs->possessed.size()) return kNullEntity;
    Entity e = gs->possessed[human_];
    return e.valid() && sim_->world().alive(e) ? e : kNullEntity;
}

bool GameClient::cellAt(float sx, float sy, int& x, int& y) const {
    // dig marks go on the tops of rock blocks (drawn ~1.2 above the ground): aim at that plane
    const float h = (mode_ == Mode::Dig || mode_ == Mode::Undig) ? 1.2f : 0.0f;
    float3 g;
    if (!cam_.screenToGround(sx, sy, platform::windowWidth(), platform::windowHeight(), g, h)) return false;
    x = int(std::floor(g.x)); y = int(std::floor(g.z));
    return true;
}

// ------------------------------------------------------------------ HUD: skills bar + tools

void GameClient::buildTools(UiState& s) {
    if (spectator_ || !sim_) return;
    auto& gs = mod().state(*sim_);
    const uint32_t tick = sim_->tick(), rate = std::max<uint32_t>(1, sim_->tickRate());

    if (human_ < gs.playerAbilities.size()) {
        const auto& mine = gs.playerAbilities[human_];
        for (size_t i = 0; i < rules_.playerAbilities.size() && i < mine.size(); ++i) {
            const PlayerAbilityState& st = mine[i];
            const PlayerAbilityDef& a = rules_.advancedEffects ? mod().playerAbilityFor(*sim_, human_, int(i)) : rules_.playerAbilities[i];
            if (!st.owned || a.passive || a.autoEvery.raw > 0) continue;
            UiAction b;
            b.label = rules_.playerAbilities[i].name;   // display copy: localized name (levels share it)
            b.cost = costText(rules_, a.cost);
            if (a.chargesMax > 0) b.cost += (b.cost.empty() ? "" : "  ") + fmt("%d/%d", st.charges, a.chargesMax);
            b.enabled = mod().canUsePlayerAbility(*sim_, human_, int(i));
            if (st.readyAt > tick) {
                const uint32_t left = st.readyAt - tick;
                b.tip = tr("card.cooldown", Args().num("seconds", int64_t((left + rate - 1) / rate)));
                const double total = std::max(1.0, a.cooldown.toDouble() * rate);
                b.progress = float(std::clamp(1.0 - left / total, 0.0, 1.0));
            }
            b.active = mode_ == Mode::Skill && abilityIndex_ == int(i);
            const int idx = int(i);
            if (instantTarget(a.target)) b.id = addAction([this, idx] { Command c; c.type = kCmdPlayerAbility; c.param = idx; issue(c); }, b.enabled);
            else b.id = addAction([this, idx] { mode_ = Mode::Skill; abilityIndex_ = idx; pairFirst_ = false; }, b.enabled);
            s.tools.push_back(b);
        }
    }
    if (mode_ == Mode::Skill && (abilityIndex_ < 0 || !mod().canUsePlayerAbility(*sim_, human_, abilityIndex_))) mode_ = Mode::Normal;

    auto tool = [&](const std::string& label, const std::string& cost, bool active, std::function<void()> fn, bool enabled = true) {
        UiAction b; b.label = label; b.cost = cost; b.active = active; b.enabled = enabled;
        b.id = addAction(std::move(fn), enabled);
        s.tools.push_back(b);
    };
    if (!rules_.diggables.empty()) {
        tool(tr("hud.dig"), "", mode_ == Mode::Dig, [this] { mode_ = Mode::Dig; });
        tool(tr("hud.undig"), "", mode_ == Mode::Undig, [this] { mode_ = Mode::Undig; });
    }
    for (size_t r = 0; r < rules_.rooms.size(); ++r) {
        const int ri = int(r);
        tool(rules_.rooms[r].name, costText(rules_, rules_.rooms[r].cost), mode_ == Mode::Room && roomIndex_ == ri, [this, ri] { mode_ = Mode::Room; roomIndex_ = ri; });
    }
    if (!rules_.rooms.empty()) tool(tr("hud.erase_room"), "", mode_ == Mode::Room && roomIndex_ < 0, [this] { mode_ = Mode::Room; roomIndex_ = -1; });
    if (rules_.roadNetwork.enabled) {
        tool(tr("hud.road"), costText(rules_, rules_.roadNetwork.cost), mode_ == Mode::Road, [this] { mode_ = Mode::Road; });
        tool(tr("hud.remove_road"), "", mode_ == Mode::Unroad, [this] { mode_ = Mode::Unroad; });
    }
    if (rules_.hand.max > 0 && human_ < gs.hand.size()) {
        int n = 0;
        for (Entity e : gs.hand[human_]) if (sim_->world().alive(e)) ++n;
        if (n == 0 && mode_ == Mode::Drop) mode_ = Mode::Normal;
        tool(tr("hud.drop", Args().num("count", n).num("max", rules_.hand.max)), "", mode_ == Mode::Drop, [this] { mode_ = Mode::Drop; }, n > 0);
    }
    if (possessedUnit().valid())
        tool(tr("hud.release"), "", false, [this] { Command c; c.type = kCmdPossess; issue(c); });
}

// Command card additions for the own selection: hand, possession, roads.
void GameClient::buildToolCard(UiState& s) {
    World& w = sim_->world();
    std::vector<Entity> mobile, structures;
    for (Entity e : selection_) {
        if (!w.alive(e) || !w.has<Unit>(e) || w.get<Owner>(e).player != human_) continue;
        const UnitType& t = rules_.types[w.get<Unit>(e).type];
        if (t.structure) structures.push_back(e);
        else if (w.has<Mobile>(e)) mobile.push_back(e);
    }
    UiSection sec{"", "row", {}};
    auto add = [&](const std::string& label, const std::string& cost, std::function<void()> fn) {
        UiAction a; a.label = label; a.cost = cost; a.id = addAction(std::move(fn)); sec.actions.push_back(a);
    };
    if (rules_.hand.max > 0 && !mobile.empty()) {
        add(tr("card.pick_up"), "", [this, mobile] { Command c; c.type = kCmdPickUp; c.units = mobile; issue(c); });
        add(tr("card.slap"), "", [this, mobile] { Command c; c.type = kCmdSlap; c.units = mobile; issue(c); });
    }
    if (rules_.possession.enabled && mobile.size() == 1 && mobile[0] != possessedUnit())
        add(tr("card.possess"), costText(rules_, rules_.possession.cost), [this, e = mobile[0]] { Command c; c.type = kCmdPossess; c.units = {e}; issue(c); });
    if (rules_.roadNetwork.enabled && !structures.empty())
        add(tr("card.connect_road"), "", [this, structures] { Command c; c.type = kCmdConnectRoad; c.units = structures; issue(c); });
    if (!sec.actions.empty()) pushSection(s, sec);
}

std::string GameClient::toolHint() const {
    if (mode_ == Mode::Skill && abilityIndex_ >= 0 && size_t(abilityIndex_) < rules_.playerAbilities.size())
        return tr("mode.skill", Args().str("name", rules_.playerAbilities[size_t(abilityIndex_)].name));
    if (mode_ == Mode::Dig) return tr("mode.dig");
    if (mode_ == Mode::Undig) return tr("mode.undig");
    if (mode_ == Mode::Room)
        return roomIndex_ >= 0 && size_t(roomIndex_) < rules_.rooms.size() ? tr("mode.room", Args().str("name", rules_.rooms[size_t(roomIndex_)].name)) : tr("mode.erase_room");
    if (mode_ == Mode::Road) return tr("mode.road");
    if (mode_ == Mode::Unroad) return tr("mode.remove_road");
    if (mode_ == Mode::Drop) return tr("mode.drop");
    return {};
}

// ------------------------------------------------------------------ input

bool GameClient::toolMouseDown(const Event& e) {
    if (possessedUnit().valid()) {
        if (e.button == 0) fireHeld_ = true;
        if (e.button == 1) walkHeld_ = true;
        return e.button != 2;   // middle button still pans
    }
    if (e.button != 0) return false;
    if (mode_ == Mode::Skill) {
        float3 g;
        const bool pair = abilityIndex_ >= 0 && size_t(abilityIndex_) < rules_.playerAbilities.size() &&
                          rules_.playerAbilities[size_t(abilityIndex_)].target == TargetKind::PointPair;
        if (pair && !pairFirst_) {   // first click: where from; the second click: where to
            if (groundAt(e.x, e.y, g)) { pairFirst_ = true; pairX_ = g.x; pairZ_ = g.z; }
            return true;
        }
        if (pair && groundAt(e.x, e.y, g)) {
            Command c; c.type = kCmdPlayerAbility;
            c.param = abilityIndex_ | RtsModule::packPoint2({fx(g.x), Fixed{}, fx(g.z)});
            c.point = {fx(pairX_), Fixed{}, fx(pairZ_)};
            issue(c);
            pairFirst_ = false;
            mode_ = Mode::Normal;
            return true;
        }
        if (!pair && groundAt(e.x, e.y, g)) {
            Command c; c.type = kCmdPlayerAbility; c.param = abilityIndex_;
            c.point = {fx(g.x), Fixed{}, fx(g.z)};
            if (planet()) c.point = simPoint(g);
            c.target = pick(e.x, e.y);
            issue(c);
        }
        mode_ = Mode::Normal;
        return true;
    }
    if (mode_ == Mode::Drop) {
        float3 g;
        if (groundAt(e.x, e.y, g)) {
            Command c; c.type = kCmdDrop; c.point = {fx(g.x), Fixed{}, fx(g.z)};
            if (planet()) c.point = simPoint(g);
            c.param = (e.mods & platform::kModShift) ? rules_.hand.max : 1;   // shift: the whole hand
            issue(c);
        }
        return true;   // stays in drop mode while the hand holds someone
    }
    if (rectMode()) {
        rectDrag_ = true;
        rectX_ = e.x; rectY_ = e.y;
        return true;
    }
    return false;
}

bool GameClient::toolMouseUp(const Event& e) {
    if (possessedUnit().valid() || fireHeld_ || walkHeld_) {
        if (e.button == 0) fireHeld_ = false;
        if (e.button == 1) walkHeld_ = false;
        if (possessedUnit().valid() && e.button != 2) return true;
    }
    if (e.button != 0 || !rectDrag_) return false;
    rectDrag_ = false;
    int ax, ay, bx, by;
    if (!rectMode() || !cellAt(rectX_, rectY_, ax, ay) || !cellAt(e.x, e.y, bx, by)) return true;
    const int x0 = std::min(ax, bx), y0 = std::min(ay, by);
    const int w = std::min(64, std::abs(bx - ax) + 1), h = std::min(64, std::abs(by - ay) + 1);
    Command c;
    c.point = {Fixed::fromInt(x0) + Fixed::fromRatio(1, 2), Fixed{}, Fixed::fromInt(y0) + Fixed::fromRatio(1, 2)};
    switch (mode_) {
    case Mode::Dig: c.type = kCmdDig; c.param = rectParam(w, h, 0); break;
    case Mode::Undig: c.type = kCmdDig; c.param = rectParam(w, h, 1); break;
    case Mode::Room: c.type = kCmdPaintRoom; c.param = rectParam(w, h, roomIndex_); break;
    case Mode::Road: c.type = kCmdBuildRoad; c.param = rectParam(w, h, 0); break;
    default: c.type = kCmdBuildRoad; c.param = rectParam(w, h, 1); break;
    }
    issue(c);
    return true;   // the tool stays active: paint several rects, right click / Esc to stop
}

// Possession: WASD / arrows (camera-relative) or the held right button walk, the held left button
// fires (at the hovered enemy if any). The sim drops steering it hasn't heard for 10 ticks, so the
// state is re-sent every few ticks while the player holds it.
void GameClient::steerPossessed() {
    Entity p = possessedUnit();
    if (!p.valid()) { fireHeld_ = walkHeld_ = false; return; }
    World& w = sim_->world();
    const Vec3 pos = w.get<Transform>(p).pos;
    const float px = float(pos.x.toDouble()), pz = float(pos.z.toDouble());
    cam_.target.x += (px - cam_.target.x) * 0.15f;     // the camera rides along
    cam_.target.z += (pz - cam_.target.z) * 0.15f;

    float dx = 0, dz = 0;
    if (!ui_.textInputFocused()) {
        const float sx = std::sin(cam_.yaw), cz = std::cos(cam_.yaw);
        const float fwdX = -sx, fwdZ = -cz, rightX = cz, rightZ = -sx;
        auto key = [&](Key k) { return keys_[int(k)]; };
        if (key(Key::W) || key(Key::Up)) { dx += fwdX; dz += fwdZ; }
        if (key(Key::S) || key(Key::Down)) { dx -= fwdX; dz -= fwdZ; }
        if (key(Key::D) || key(Key::Right)) { dx += rightX; dz += rightZ; }
        if (key(Key::A) || key(Key::Left)) { dx -= rightX; dz -= rightZ; }
    }
    float3 g;
    if (walkHeld_ && dx == 0 && dz == 0 && groundAt(mouseX_, mouseY_, g)) {
        dx = g.x - px; dz = g.z - pz;
        if (std::hypot(dx, dz) < 0.3f) dx = dz = 0;
    }
    const float len = std::hypot(dx, dz);
    if (len > 0) { dx /= len; dz /= len; }
    Entity target = kNullEntity;
    if (fireHeld_ && hover_.valid() && w.alive(hover_) && w.has<Owner>(hover_)) {
        const uint8_t o = w.get<Owner>(hover_).player;
        if (o != human_ && o != 0) target = hover_;
    }
    const bool fire = fireHeld_;
    const bool changed = std::abs(dx - steerX_) > 0.05f || std::abs(dz - steerZ_) > 0.05f || fire != steerFire_ || target != steerTarget_;
    const bool idle = len == 0 && !fire;
    if (!changed && (idle || sim_->tick() < lastSteerTick_ + 4)) return;
    steerX_ = dx; steerZ_ = dz; steerFire_ = fire; steerTarget_ = target;
    lastSteerTick_ = sim_->tick();
    Command c; c.type = kCmdDirect; c.point = {fx(dx), Fixed{}, fx(dz)}; c.param = fire ? 1 : 0; c.target = target;
    issue(c);
}

// ------------------------------------------------------------------ world preview

void GameClient::drawToolPreview() {
    if (!rectMode()) return;
    int ax, ay, bx, by;
    if (!cellAt(mouseX_, mouseY_, bx, by)) return;
    if (!rectDrag_ || !cellAt(rectX_, rectY_, ax, ay)) { ax = bx; ay = by; }
    const int x0 = std::min(ax, bx), y0 = std::min(ay, by);
    const int x1 = std::min(x0 + 63, std::max(ax, bx)), y1 = std::min(y0 + 63, std::max(ay, by));
    uint32_t col = rgba(255, 210, 90, 90);                                  // dig
    if (mode_ == Mode::Undig || mode_ == Mode::Unroad || (mode_ == Mode::Room && roomIndex_ < 0)) col = rgba(255, 80, 70, 90);
    else if (mode_ == Mode::Room) col = rgba(110, 220, 140, 90);
    else if (mode_ == Mode::Road) col = rgba(205, 170, 120, 110);
    const auto& grid = mod().state(*sim_).grid;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            if (!grid.inside(x, y)) continue;
            float h = view_.heightAt(grid, x + 0.5f, y + 0.5f) + 0.07f;
            const size_t ci = size_t(grid.index(x, y));
            if (ci < grid.dig.size() && grid.dig[ci]) h += 1.2f;   // on top of undug rock (ground fx blocks <= 1.25, fog at 1.3)
            const float a = x + 0.06f, b = x + 0.94f, c = y + 0.06f, d = y + 0.94f;
            r3d_.triangle({a, h, c}, {a, h, d}, {b, h, d}, col);
            r3d_.triangle({a, h, c}, {b, h, d}, {b, h, c}, col);
        }
}

// ------------------------------------------------------------------ "How to play"

void GameClient::buildHowto(UiState& s) {
    s.hasHowto = loc_.has("howto.goal");
    if (!s.hasHowto) { howtoOpen_ = false; return; }
    s.howtoOpen = howtoOpen_;
    if (!howtoOpen_) return;
    s.howtoLike = tr("howto.like");
    s.howtoGoal = tr("howto.goal");
    s.howtoTip = loc_.has("howto.tip") ? tr("howto.tip") : "";
    for (int i = 1; i < 16; ++i) {
        const std::string k = "howto.step" + std::to_string(i);
        if (!loc_.has(k)) break;
        s.howtoSteps.push_back(tr(k));
    }
}

} // namespace rts::client
