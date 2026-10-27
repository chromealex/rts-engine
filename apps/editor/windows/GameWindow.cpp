// Game: the play-test as the player sees it. Can be framed as a device (phone simulator):
// resolution and aspect of the device, notch and safe area, orientation, touch-sized feedback.
// When the user plays a side, LMB selects (click / box), RMB gives smart orders, WASD/arrows pan.
#include <algorithm>
#include <cmath>
#include "../Devices.h"
#include "../Editor.h"

namespace rts::editor {

using namespace render;

class GameWindow : public EditorWindow {
public:
    const char* id() const override { return "game"; }
    const char* title() const override { return "Game"; }
    const char* dockArea() const override { return "center"; }
    bool zeroPadding() const override { return true; }
    ImGuiWindowFlags flags() const override { return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse; }

    void draw(Editor& ed) override {
        toolbar(ed);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(28, 30, 34, 255));
        if (!ed.playing()) {
            const char* msg = "Тест не запущен: «Боты» или «Играть за…» в панели выше (F5)";
            ImVec2 ts = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2(origin.x + (avail.x - ts.x) * 0.5f, origin.y + avail.y * 0.5f), IM_COL32(200, 200, 200, 255), msg);
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
        ed.renderGame(vp_, ed.gameCam);
        vp_.image(ed.device(), sw, sh);
        ed.gameViewport = &vp_;
        if (dev && dev->w > 0) deviceChrome(dl, *dev, ed.gameLandscape, sx, sy, sw, sh, k);
        hud(ed, dl, sx, sy, sw);
        input(ed);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + avail.y));
    }

private:
    void toolbar(Editor& ed) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 6, ImGui::GetCursorPosY() + 3));
        if (!ed.playing()) {
            if (ImGui::SmallButton("Боты")) ed.startPlay(0);
            for (size_t p = 0; p < ed.doc().data.players.size(); ++p) {
                if (ed.doc().data.players[p].waves) continue;
                ImGui::SameLine(0, 4);
                if (ImGui::SmallButton(("Играть за " + ed.doc().data.players[p].name).c_str())) ed.startPlay(int(p + 1));
            }
        } else {
            if (ImGui::SmallButton("Стоп")) ed.stopPlay();
            ImGui::SameLine(0, 4);
            if (ImGui::SmallButton(ed.playPaused ? "Продолжить" : "Пауза")) ed.playPaused = !ed.playPaused;
            ImGui::SameLine(0, 8);
            ImGui::SetNextItemWidth(110);
            ImGui::SliderFloat("##speed", &ed.playSpeed, 0.25f, 16.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
        }
        ImGui::SameLine(0, 16);
        const DeviceProfile* cur = findDevice(ed.gameDevice);
        ImGui::SetNextItemWidth(170);
        if (ImGui::BeginCombo("##device", cur ? cur->name : "Свободный размер")) {
            for (auto& d : kDevices) if (ImGui::Selectable(d.name, ed.gameDevice == d.id)) ed.gameDevice = d.id;
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 4);
        if (ImGui::SmallButton(ed.gameLandscape ? "Альбомная" : "Портретная")) ed.gameLandscape = !ed.gameLandscape;
        ImGui::PopStyleVar();
    }

    void deviceChrome(ImDrawList* dl, const DeviceProfile& d, bool landscape, float sx, float sy, float sw, float sh, float k) {
        // rounded screen corners
        float r = d.corner * k;
        if (r > 0) {
            dl->PathClear();
            dl->AddRect(ImVec2(sx - r * 0.4f, sy - r * 0.4f), ImVec2(sx + sw + r * 0.4f, sy + sh + r * 0.4f), IM_COL32(12, 12, 14, 255), r * 1.4f, 0, r * 0.8f);
        }
        // notch / dynamic island and safe-area guides
        float top = d.safeTop * k, bottom = d.safeBottom * k;
        ImU32 guide = IM_COL32(255, 200, 80, 120);
        if (!landscape) {
            if (d.notchW > 0) {
                float nw = d.notchW * k, nh = d.notchH * k;
                dl->AddRectFilled(ImVec2(sx + (sw - nw) * 0.5f, sy + 11 * k), ImVec2(sx + (sw + nw) * 0.5f, sy + 11 * k + nh), IM_COL32(0, 0, 0, 255), nh * 0.5f);
            }
            if (top > 0) dl->AddLine(ImVec2(sx, sy + top), ImVec2(sx + sw, sy + top), guide);
            if (bottom > 0) dl->AddLine(ImVec2(sx, sy + sh - bottom), ImVec2(sx + sw, sy + sh - bottom), guide);
        } else {
            if (d.notchW > 0) {
                float nw = d.notchW * k, nh = d.notchH * k;
                dl->AddRectFilled(ImVec2(sx + 11 * k, sy + (sh - nw) * 0.5f), ImVec2(sx + 11 * k + nh, sy + (sh + nw) * 0.5f), IM_COL32(0, 0, 0, 255), nh * 0.5f);
            }
            if (top > 0) dl->AddLine(ImVec2(sx + top, sy), ImVec2(sx + top, sy + sh), guide);
            if (top > 0) dl->AddLine(ImVec2(sx + sw - top, sy), ImVec2(sx + sw - top, sy + sh), guide);
            if (bottom > 0) dl->AddLine(ImVec2(sx, sy + sh - bottom * 0.6f), ImVec2(sx + sw, sy + sh - bottom * 0.6f), guide);
        }
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
            if (ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_DownArrow)) cam.target -= fwd * speed;
            if (ImGui::IsKeyDown(ImGuiKey_D) || ImGui::IsKeyDown(ImGuiKey_RightArrow)) cam.target += right * speed;
            if (ImGui::IsKeyDown(ImGuiKey_A) || ImGui::IsKeyDown(ImGuiKey_LeftArrow)) cam.target -= right * speed;
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
                if (io.KeyShift) c.flags = game::kCmdQueue;
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
