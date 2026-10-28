// Game: the play-test as the player sees it. Can be framed as a device (phone simulator):
// resolution and aspect of the device, notch and safe area, orientation, touch-sized feedback.
// When the user plays a side, LMB selects (click / box), RMB gives smart orders, WASD/arrows pan.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include "../Devices.h"
#include "../Editor.h"
#include "../EditorUtil.h"

namespace rts::editor {

using namespace render;

class GameWindow : public EditorWindow {
public:
    const char* id() const override { return "game"; }
    const char* title() const override { return "Game"; }
    int order() const override { return 1; }
    const char* dockArea() const override { return "center"; }
    bool zeroPadding() const override { return true; }
    ImGuiWindowFlags flags() const override { return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse; }

    void draw(Editor& ed) override {
        toolbar(ed);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), ImGui::GetColorU32(ed.theme().palette().canvas)); // lighter than the device bezel
        if (!ed.playing()) {
            ui::emptyState(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), ICON_GAME, "Тест не запущен",
                           "Нажмите " ICON_PLAY " на панели выше или F5: боты против ботов, или выберите, за кого играть.");
            ImGui::Dummy(avail);
            ed.gameViewport = nullptr;
            return;
        }
        // screen rect: whole area, or the device screen fitted with a bezel
        const DeviceProfile* dev = findDevice(ed.gameDevice);
        float sw = avail.x, sh = avail.y, sx = origin.x, sy = origin.y, k = 1;
        float devW = 0, devH = 0;
        if (dev && dev->w > 0) {
            devW = float(dev->w); devH = float(dev->h);
            bool portraitNative = dev->w < dev->h;
            if (portraitNative == ed.gameLandscape) std::swap(devW, devH);
            const float bezel = dev->touch ? 14 : 6;
            k = std::min((avail.x - 2 * bezel - 8) / devW, (avail.y - 2 * bezel - 8) / devH);
            k = std::max(k, 0.05f);
            sw = devW * k; sh = devH * k;
            sx = origin.x + (avail.x - sw) * 0.5f; sy = origin.y + (avail.y - sh) * 0.5f;
            float r = dev->corner * k;
            dl->AddRectFilled(ImVec2(sx - bezel, sy - bezel), ImVec2(sx + sw + bezel, sy + sh + bezel), IM_COL32(12, 12, 14, 255), r + bezel);
        }
        const float dpi = ImGui::GetIO().DisplayFramebufferScale.x;
        vp_.ensure(ed.device(), int(sw * dpi), int(sh * dpi));
        ImGui::SetCursorScreenPos(ImVec2(sx, sy));
        vp_.originX = sx; vp_.originY = sy; vp_.pointsW = sw; vp_.pointsH = sh;
        // the image command goes first so the overlays drawn by renderGame (health bars, markers) end up on
        // top of it; the GPU pass itself runs now, before ImGui renders the frame
        vp_.image(ed.device(), sw, sh);
        dl->PushClipRect(ImVec2(sx, sy), ImVec2(sx + sw, sy + sh), true); // overlays of off-screen units stay inside
        ed.renderGame(vp_, ed.gameCam);
        dl->PopClipRect();
        ed.gameViewport = &vp_;
        if (dev && dev->w > 0) deviceChrome(dl, *dev, ed.gameLandscape, ed.gameSafeArea, sx, sy, sw, sh, k);
        // the HUD keeps to the safe area like a game should
        Insets4 safe = dev && dev->w > 0 ? deviceInsets(*dev, ed.gameLandscape) : Insets4{};
        hud(ed, dl, sx + safe.left * k, sy + safe.top * k, sw - (safe.left + safe.right) * k);
        input(ed);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + avail.y));
        ImGui::Dummy(ImVec2(0, 0));
    }

private:
    std::string filter_;
    void toolbar(Editor& ed) {
        const float bottom = ui::beginToolbar();
        ui::beginSegment();
        if (!ed.playing()) {
            if (ui::toolButton(ICON_BOT, "Боты против ботов")) ed.startPlay(0);
            for (size_t p = 0; p < ed.doc().data.players.size(); ++p) {
                if (ed.doc().data.players[p].waves) continue;
                ImGui::SameLine();
                ImGui::PushID(int(p));
                if (ui::toolButton(ICON_USER, ("Играть за " + ed.doc().data.players[p].name).c_str())) ed.startPlay(int(p + 1));
                ImGui::PopID();
            }
        } else {
            if (ui::toolButton(ICON_PLAY, "Идёт тест", !ed.playPaused)) ed.playPaused = false;
            ImGui::SameLine();
            if (ui::toolButton(ICON_PAUSE, ed.playPaused ? "Продолжить" : "Пауза", ed.playPaused)) ed.playPaused = !ed.playPaused;
            ImGui::SameLine();
            if (ui::toolButton(ICON_STOP, "Стоп")) ed.stopPlay();
        }
        ui::endSegment();
        if (ed.playing()) {
            ImGui::SameLine(0, 10);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled(ICON_SPEED);
            ImGui::SameLine(0, 4);
            ImGui::SetNextItemWidth(130);
            ImGui::SliderFloat("##speed", &ed.playSpeed, 0.25f, 16.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
            ImGui::SetItemTooltip("Скорость симуляции");
        }
        ui::toolbarDivider();
        const DeviceProfile* cur = findDevice(ed.gameDevice);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(cur && cur->w > 0 ? (cur->touch ? ICON_SMARTPHONE : ICON_MONITOR) : ICON_MONITOR);
        ImGui::SameLine(0, 4);
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("##device", cur ? cur->name : "Свободный размер", ImGuiComboFlags_HeightLarge)) {
            if (ImGui::IsWindowAppearing()) { filter_.clear(); ImGui::SetKeyboardFocusHere(); }
            ui::inputString("##devfilter", filter_);
            if (filter_.empty()) { ImGui::SameLine(14); ImGui::TextDisabled(ICON_SEARCH "  поиск устройства"); }
            auto lower = [](std::string v) { for (auto& c : v) c = char(std::tolower((unsigned char)c)); return v; };
            const std::string f = lower(filter_);
            const char* group = nullptr;
            for (auto& d : kDevices) {
                if (!f.empty() && lower(d.name).find(f) == std::string::npos && lower(d.id).find(f) == std::string::npos) continue;
                if (d.group[0] && (!group || std::string(group) != d.group)) { group = d.group; ImGui::SeparatorText(group); }
                char label[160];
                if (d.w > 0) std::snprintf(label, sizeof label, "%s##%s", d.name, d.id); else std::snprintf(label, sizeof label, "%s", d.name);
                if (ImGui::Selectable(label, ed.gameDevice == d.id)) ed.gameDevice = d.id;
                if (d.w > 0) {
                    ImGui::SameLine(250);
                    ImGui::TextDisabled("%dx%d @%.3gx", d.w, d.h, d.scale);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ui::beginSegment();
        if (ui::toolButton(ICON_LANDSCAPE, "Альбомная", ed.gameLandscape)) ed.gameLandscape = true;
        ImGui::SameLine();
        if (ui::toolButton(ICON_PORTRAIT, "Портретная", !ed.gameLandscape)) ed.gameLandscape = false;
        ui::endSegment();
        if (cur && cur->w > 0) {
            ImGui::SameLine(0, 8);
            if (ui::toolButton(ICON_SAFE_AREA, "Safe area: показать безопасную зону (вырез, статус-бар, полоска «домой»). HUD должен быть внутри рамки.", ed.gameSafeArea))
                ed.gameSafeArea = !ed.gameSafeArea;
        }
        ui::endToolbar(bottom);
    }

    // Device frame: rounded corners, the camera cut-out and (on demand) the safe area. Cut-outs are
    // described in the natural orientation; rotated devices map natural (x, y) to screen (y, W - x)
    // so the cut-out ends up on the left in landscape.
    void deviceChrome(ImDrawList* dl, const DeviceProfile& d, bool landscape, bool showSafe, float sx, float sy, float sw, float sh, float k) {
        const float r = d.corner * k;
        if (r > 0) { // mask the four screen corners outside the rounded display
            const float pi = 3.14159265f;
            struct C { float cx, cy, x, y, a0; } cs[] = {{sx + r, sy + r, sx, sy, pi}, {sx + sw - r, sy + r, sx + sw, sy, 1.5f * pi},
                                                       {sx + sw - r, sy + sh - r, sx + sw, sy + sh, 0}, {sx + r, sy + sh - r, sx, sy + sh, 0.5f * pi}};
            for (auto& c : cs) { // a ring from radius r outwards covers the corner (at most 0.42 r away)
                (void)c.x; (void)c.y;
                dl->PathArcTo(ImVec2(c.cx, c.cy), r * 1.22f, c.a0, c.a0 + 0.5f * pi, 16);
                dl->PathStroke(IM_COL32(12, 12, 14, 255), 0, r * 0.44f + 1.0f);
            }
        }
        const bool naturalPortrait = d.w < d.h;
        const bool rotated = naturalPortrait == landscape;
        const float W = float(d.w);
        auto box = [&](float x0, float y0, float x1, float y1, float round) {
            ImVec2 a, b;
            if (!rotated) { a = ImVec2(sx + x0 * k, sy + y0 * k); b = ImVec2(sx + x1 * k, sy + y1 * k); }
            else { a = ImVec2(sx + y0 * k, sy + (W - x1) * k); b = ImVec2(sx + y1 * k, sy + (W - x0) * k); }
            dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 255), round * k);
        };
        const float cx = W * 0.5f;
        switch (d.cutout) {
        case Cutout::None: break;
        case Cutout::Notch: box(cx - d.cutW * 0.5f, 0, cx + d.cutW * 0.5f, d.cutH, d.cutH * 0.45f); break;
        case Cutout::Island: box(cx - d.cutW * 0.5f, d.cutY, cx + d.cutW * 0.5f, d.cutY + d.cutH, d.cutH * 0.5f); break;
        case Cutout::PunchCenter: box(cx - d.cutW * 0.5f, d.cutY, cx + d.cutW * 0.5f, d.cutY + d.cutH, d.cutW * 0.5f); break;
        case Cutout::PunchLeft: box(d.cutY + 6, d.cutY, d.cutY + 6 + d.cutW, d.cutY + d.cutH, d.cutW * 0.5f); break;
        case Cutout::Waterdrop: box(cx - d.cutW * 0.5f, 0, cx + d.cutW * 0.5f, d.cutH, d.cutW * 0.5f); break;
        }
        if (!showSafe) return;
        const Insets4 in = deviceInsets(d, landscape);
        const float l = in.left * k, t = in.top * k, rr = in.right * k, bb = in.bottom * k;
        const ImU32 shade = IM_COL32(255, 70, 60, 70), edge = IM_COL32(255, 210, 80, 230);
        if (t > 0) dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + sw, sy + t), shade);
        if (bb > 0) dl->AddRectFilled(ImVec2(sx, sy + sh - bb), ImVec2(sx + sw, sy + sh), shade);
        if (l > 0) dl->AddRectFilled(ImVec2(sx, sy + t), ImVec2(sx + l, sy + sh - bb), shade);
        if (rr > 0) dl->AddRectFilled(ImVec2(sx + sw - rr, sy + t), ImVec2(sx + sw, sy + sh - bb), shade);
        dl->AddRect(ImVec2(sx + l, sy + t), ImVec2(sx + sw - rr, sy + sh - bb), edge, 0, 0, 1.5f);
        char lbl[96];
        std::snprintf(lbl, sizeof lbl, "safe area  L%.0f T%.0f R%.0f B%.0f pt", in.left, in.top, in.right, in.bottom);
        ImVec2 ts = ImGui::CalcTextSize(lbl);
        dl->AddRectFilled(ImVec2(sx + l + 4, sy + sh - bb - ts.y - 8), ImVec2(sx + l + ts.x + 12, sy + sh - bb - 4), IM_COL32(0, 0, 0, 170), 4);
        dl->AddText(ImVec2(sx + l + 8, sy + sh - bb - ts.y - 6), edge, lbl);
    }

    void hud(Editor& ed, ImDrawList* dl, float sx, float sy, float sw) {
        Simulation* sim = ed.sim();
        auto* m = ed.simModule();
        if (!sim || !m) return;
        auto& gs = m->state(*sim);
        std::string line;
        uint8_t p = uint8_t(std::max(1, ed.humanSlot()));
        for (size_t r = 0; r < ed.rules().resources.size(); ++r)
            line += ed.rules().resources[r].name + " " + std::to_string(gs.players[p].stock[r].toInt()) + "   ";
        if (gs.players[p].supplyCap.raw > 0) line += "Лимит " + std::to_string(gs.players[p].supplyUsed.toInt()) + "/" + std::to_string(gs.players[p].supplyCap.toInt()) + "   ";
        char t[32];
        std::snprintf(t, sizeof t, "%u:%02u", sim->tick() / sim->tickRate() / 60, sim->tick() / sim->tickRate() % 60);
        line += t;
        ImVec2 ts = ImGui::CalcTextSize(line.c_str());
        float x = sx + (sw - ts.x) * 0.5f, y = sy + 8;
        dl->AddRectFilled(ImVec2(x - 8, y - 3), ImVec2(x + ts.x + 8, y + ts.y + 3), IM_COL32(0, 0, 0, 140), 5);
        dl->AddText(ImVec2(x, y), IM_COL32(255, 255, 255, 240), line.c_str());
        if (int res = sim->result(); res != 0) {
            std::string w = res < 0 ? "Ничья" : "Победила команда " + std::to_string(res);
            ImVec2 ws = ImGui::CalcTextSize(w.c_str());
            dl->AddText(ImVec2(sx + (sw - ws.x) * 0.5f, y + 30), IM_COL32(255, 230, 120, 255), w.c_str());
        }
        if (boxing_) dl->AddRect(boxA_, ImGui::GetIO().MousePos, IM_COL32(120, 255, 120, 220));
    }

    void input(Editor& ed) {
        ImGuiIO& io = ImGui::GetIO();
        RTSCamera& cam = ed.gameCam;
        if (vp_.hovered && io.MouseWheel != 0) cam.distance = std::clamp(cam.distance * std::pow(0.88f, io.MouseWheel), cam.minDistance, cam.maxDistance);
        if (vp_.hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            float x0, z0, x1, z1;
            if (ed.groundAt(vp_, cam, io.MousePos.x - io.MouseDelta.x, io.MousePos.y - io.MouseDelta.y, x0, z0) && ed.groundAt(vp_, cam, io.MousePos.x, io.MousePos.y, x1, z1))
                cam.target += float3{x0 - x1, 0, z0 - z1};
        }
        if (ImGui::IsWindowFocused() && !io.WantTextInput) {
            float speed = cam.distance * io.DeltaTime;
            float3 fwd{-std::sin(cam.yaw), 0, -std::cos(cam.yaw)}, right{std::cos(cam.yaw), 0, -std::sin(cam.yaw)};
            if (ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_UpArrow)) cam.target += fwd * speed;
            if (ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_DownArrow)) cam.target += fwd * -speed;
            if (ImGui::IsKeyDown(ImGuiKey_D) || ImGui::IsKeyDown(ImGuiKey_RightArrow)) cam.target += right * speed;
            if (ImGui::IsKeyDown(ImGuiKey_A) || ImGui::IsKeyDown(ImGuiKey_LeftArrow)) cam.target += right * -speed;
        }
        if (ed.humanSlot() <= 0) return;
        World& w = ed.sim()->world();
        const uint8_t me = uint8_t(ed.humanSlot());
        auto pickSim = [&](float mx, float my) {
            float x, z;
            Entity best;
            float bestD = 1e9f;
            if (!ed.groundAt(vp_, cam, mx, my, x, z)) return best;
            w.each<game::Unit, Transform>([&](Entity e, game::Unit& u, Transform& t) {
                float dx = float(t.pos.x.toDouble()) - x, dz = float(t.pos.z.toDouble()) - z;
                float r = ed.view().selectionRadius(u.type) + 0.2f, dd = std::sqrt(dx * dx + dz * dz);
                if (dd < r && dd < bestD) { bestD = dd; best = e; }
            });
            return best;
        };
        if (vp_.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { boxing_ = true; boxA_ = io.MousePos; }
        if (boxing_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            boxing_ = false;
            ImVec2 b = io.MousePos;
            if (!io.KeyShift) ed.gameSelection.clear();
            if (std::hypot(b.x - boxA_.x, b.y - boxA_.y) < 6) {
                Entity e = pickSim(b.x, b.y);
                if (e.valid() && w.get<Owner>(e).player == me) ed.gameSelection.push_back(e);
            } else {
                float x0 = std::min(boxA_.x, b.x) - vp_.originX, x1 = std::max(boxA_.x, b.x) - vp_.originX;
                float y0 = std::min(boxA_.y, b.y) - vp_.originY, y1 = std::max(boxA_.y, b.y) - vp_.originY;
                w.each<game::Unit, Owner, Transform>([&](Entity e, game::Unit& u, Owner& o, Transform& t) {
                    if (o.player != me || ed.rules().types[u.type].structure) return;
                    float2 s;
                    if (!cam.worldToScreen({float(t.pos.x.toDouble()), 0.5f, float(t.pos.z.toDouble())}, vp_.pointsW, vp_.pointsH, s)) return;
                    if (s.x >= x0 && s.x <= x1 && s.y >= y0 && s.y <= y1) ed.gameSelection.push_back(e);
                });
            }
        }
        if (vp_.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ed.gameSelection.empty()) {
            float x, z;
            if (ed.groundAt(vp_, cam, io.MousePos.x, io.MousePos.y, x, z)) {
                Command c;
                c.type = game::kCmdSmart;
                c.units = ed.gameSelection;
                c.point = {Fixed::fromRatio(int64_t(x * 256), 256), Fixed{}, Fixed::fromRatio(int64_t(z * 256), 256)};
                c.target = pickSim(io.MousePos.x, io.MousePos.y);
                if (io.KeyShift) c.flags = rts::kCmdQueue;
                ed.issueCommand(c);
            }
        }
        ed.gameSelection.erase(std::remove_if(ed.gameSelection.begin(), ed.gameSelection.end(), [&](Entity e) { return !w.alive(e); }), ed.gameSelection.end());
    }

    Viewport vp_;
    bool boxing_ = false;
    ImVec2 boxA_{};
};

RTS_EDITOR_WINDOW(GameWindow);

} // namespace rts::editor
