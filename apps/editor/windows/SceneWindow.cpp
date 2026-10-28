// Scene: the map in 3D with a free camera; select/drag objects and apply the current tool.
//   RMB drag: look around, RMB + WASD/QE: fly, MMB drag: pan, wheel: zoom, F: focus selection.
#include <algorithm>
#include <cmath>
#include "../Editor.h"
#include "../EditorUtil.h"

namespace rts::editor {

using namespace render;

class SceneWindow : public EditorWindow {
public:
    const char* id() const override { return "scene"; }
    const char* title() const override { return "Scene"; }
    int order() const override { return 0; }
    const char* dockArea() const override { return "center"; }
    bool zeroPadding() const override { return true; }
    ImGuiWindowFlags flags() const override { return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse; }

    void draw(Editor& ed) override {
        toolbar(ed);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const float dpi = ImGui::GetIO().DisplayFramebufferScale.x;
        vp_.ensure(ed.device(), int(avail.x * dpi), int(avail.y * dpi));
        ImVec2 p = ImGui::GetCursorScreenPos();
        vp_.originX = p.x; vp_.originY = p.y; vp_.pointsW = avail.x; vp_.pointsH = avail.y;
        // image first (like the Game window): the screen overlays renderScene adds (bars, labels) stay on top of it
        vp_.image(ed.device(), avail.x, avail.y);
        ed.renderScene(vp_, ed.sceneCam, !ed.playing());
        ed.sceneViewport = &vp_;
        if (ImGui::IsWindowFocused() || vp_.hovered) input(ed);
        hud(ed);
    }

private:
    void toolbar(Editor& ed) {
        static const char* names[] = {"Выбор", "Объекты", "Клетки", "Уровни", "Высота", "Поля", "Регионы", "Зоны", "Старты", "Дороги", "Ластик"};
        static const char* icons[] = {ICON_SELECT, ICON_OBJECTS, ICON_CELLS, ICON_LEVELS, ICON_HEIGHT, ICON_FIELDS, ICON_REGIONS, ICON_ZONES,
                                      ICON_STARTS, ICON_ROADS, ICON_ERASER};
        static_assert(sizeof names / sizeof *names == size_t(Tool::Count) && sizeof icons / sizeof *icons == size_t(Tool::Count));
        const float bottom = ui::beginToolbar();
        ui::beginSegment();
        for (int i = 0; i < int(Tool::Count); ++i) {
            if (i == 1 || i == 10) { ui::endSegment(); ImGui::SameLine(0, 8); ui::beginSegment(); } // select | edit tools | eraser
            else if (i) ImGui::SameLine();
            if (ui::toolButton(icons[i], names[i], ed.tool == Tool(i))) { ed.tool = Tool(i); ed.roadStart = -1; }
        }
        ui::endSegment();
        ui::toolbarDivider();
        if (ui::toolButton(ICON_FIT, "Вся карта (F)")) ed.fitScene();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", names[int(ed.tool)]);
        ui::endToolbar(bottom);
    }

    // planet maps: wheel zoom (surface -> planet -> system), RMB drag turn + pitch, MMB drag / WASD move over
    // the surface, Tab next body, F back to the start body
    void planetInput(Editor& ed) {
        ImGuiIO& io = ImGui::GetIO();
        game::PlanetCamera& pc = ed.view().planetCamera();
        if (vp_.hovered && io.MouseWheel != 0) pc.zoom(std::pow(0.88f, io.MouseWheel));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && vp_.hovered) looking_ = true;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && vp_.hovered) panning_ = true;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) looking_ = false;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) panning_ = false;
        if (looking_) {
            pc.rotate(-io.MouseDelta.x * 0.006f);
            pc.pitch = std::clamp(pc.pitch + io.MouseDelta.y * 0.005f, 0.15f, 1.55f);
        }
        if (panning_) pc.pan(io.MouseDelta.x, io.MouseDelta.y, vp_.pointsH);
        if (!io.WantTextInput && (ImGui::IsWindowFocused() || vp_.hovered)) {
            const float speed = std::max(pc.distance, 6.0f) * 0.9f * io.DeltaTime * (io.KeyShift ? 3.0f : 1.0f);
            if (ImGui::IsKeyDown(ImGuiKey_W)) pc.move(0, speed);
            if (ImGui::IsKeyDown(ImGuiKey_S)) pc.move(0, -speed);
            if (ImGui::IsKeyDown(ImGuiKey_D)) pc.move(speed, 0);
            if (ImGui::IsKeyDown(ImGuiKey_A)) pc.move(-speed, 0);
            if (ImGui::IsKeyDown(ImGuiKey_Q)) pc.rotate(io.DeltaTime * 1.6f);
            if (ImGui::IsKeyDown(ImGuiKey_E)) pc.rotate(-io.DeltaTime * 1.6f);
            if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
                const auto& W = ed.view().planets().world();
                for (size_t k = 1; k <= W.bodies.size(); ++k) {
                    const int b = int((size_t(pc.body()) + k) % W.bodies.size());
                    if (W.bodies[size_t(b)].surface()) { pc.focus(b); break; }
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F, false)) ed.fitScene();
        }
    }

    void input(Editor& ed) {
        ImGuiIO& io = ImGui::GetIO();
        RTSCamera& cam = ed.sceneCam;
        const float mx = io.MousePos.x, my = io.MousePos.y;
        if (ed.planetMap()) { // camera from the planet controls; tools below still work on the start body
            planetInput(ed);
            if (ed.playing()) return;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && vp_.hovered) down(ed, mx, my, io);
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && (dragging_ || painting_) && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) drag(ed, mx, my, io);
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) up(ed, mx, my);
            hoverX_ = mx; hoverY_ = my;
            return;
        }
        // camera
        if (vp_.hovered && io.MouseWheel != 0) cam.distance = std::clamp(cam.distance * std::pow(0.88f, io.MouseWheel), cam.minDistance, cam.maxDistance);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && vp_.hovered) looking_ = true;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && vp_.hovered) panning_ = true;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) looking_ = false;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) panning_ = false;
        if (looking_) {
            cam.yaw -= io.MouseDelta.x * 0.006f;
            cam.pitch = std::clamp(cam.pitch + io.MouseDelta.y * 0.005f, 0.15f, 1.53f);
            float speed = cam.distance * 0.9f * io.DeltaTime * (io.KeyShift ? 3.0f : 1.0f);
            float3 fwd{-std::sin(cam.yaw), 0, -std::cos(cam.yaw)}, right{std::cos(cam.yaw), 0, -std::sin(cam.yaw)};
            if (ImGui::IsKeyDown(ImGuiKey_W)) cam.target += fwd * speed;
            if (ImGui::IsKeyDown(ImGuiKey_S)) cam.target += fwd * -speed;
            if (ImGui::IsKeyDown(ImGuiKey_D)) cam.target += right * speed;
            if (ImGui::IsKeyDown(ImGuiKey_A)) cam.target += right * -speed;
            if (ImGui::IsKeyDown(ImGuiKey_E)) cam.distance = std::min(cam.maxDistance, cam.distance * (1 + io.DeltaTime));
            if (ImGui::IsKeyDown(ImGuiKey_Q)) cam.distance = std::max(cam.minDistance, cam.distance * (1 - io.DeltaTime));
        }
        if (panning_) {
            float x0, z0, x1, z1;
            if (ed.groundAt(vp_, cam, mx - io.MouseDelta.x, my - io.MouseDelta.y, x0, z0) && ed.groundAt(vp_, cam, mx, my, x1, z1))
                cam.target += float3{x0 - x1, 0, z0 - z1};
        }
        if (!io.WantTextInput && ImGui::IsWindowFocused() && !looking_) {
            if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
                int s = ed.selectedEntity();
                if (s >= 0 && s < int(ed.doc().data.entities.size())) ed.focusScene(ed.doc().data.entities[size_t(s)].x, ed.doc().data.entities[size_t(s)].z, 18);
                else ed.fitScene();
            }
            if ((ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) && ed.selectedEntity() >= 0) {
                ed.doc().removeEntity(ed.selectedEntity());
                ed.select({});
            }
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) ed.brush = std::max(1, ed.brush - 1);
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) ed.brush = std::min(16, ed.brush + 1);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { ed.select({}); ed.roadStart = -1; }
        }
        if (ed.playing()) return;
        // tools
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && vp_.hovered) down(ed, mx, my, io);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && (dragging_ || painting_) && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) drag(ed, mx, my, io);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) up(ed, mx, my);
        hoverX_ = mx; hoverY_ = my;
    }

    void down(Editor& ed, float sx, float sy, ImGuiIO& io) {
        int n = ed.pickEntity(vp_, ed.sceneCam, sx, sy);
        float x = 0, z = 0;
        bool ground = ed.groundAt(vp_, ed.sceneCam, sx, sy, x, z);
        auto& doc = ed.doc();
        auto& d = doc.data;
        const auto& rules = ed.rules();
        switch (ed.tool) {
        case Tool::Select:
            ed.selectEntity(n);
            if (n >= 0) {
                doc.beginEdit();
                dragging_ = true;
                dragOffX_ = d.entities[size_t(n)].x - x;
                dragOffZ_ = d.entities[size_t(n)].z - z;
                if (io.KeyAlt) { // Alt+drag copies
                    DocEntity copy = d.entities[size_t(n)];
                    copy.id.clear();
                    d.entities.push_back(copy);
                    ed.selectEntity(int(d.entities.size() - 1));
                }
            }
            break;
        case Tool::Place: {
            if (n >= 0 && !io.KeyShift) { ed.selectEntity(n); break; }
            if (!ground || rules.types.empty()) break;
            DocEntity e;
            e.type = rules.types[size_t(ed.placeType)].id;
            e.owner = ed.placeOwner;
            e.x = ed.snapCoord(x, ed.placeType); e.z = ed.snapCoord(z, ed.placeType);
            ed.selectEntity(doc.addEntity(e));
            ed.status = "Добавлено: " + rules.types[size_t(ed.placeType)].name;
            break;
        }
        case Tool::Terrain: case Tool::Level: case Tool::Height: case Tool::Field: case Tool::Region:
            if (!ground) break;
            doc.beginEdit();
            painting_ = true;
            drag(ed, sx, sy, io);
            break;
        case Tool::Area:
            if (!ground) break;
            areaDrag_ = true;
            areaX0_ = std::floor(x); areaZ0_ = std::floor(z);
            break;
        case Tool::Start:
            if (!ground || ed.startPlayer < 1 || ed.startPlayer > int(d.players.size())) break;
            doc.beginEdit();
            d.players[size_t(ed.startPlayer - 1)].hasStart = true;
            d.players[size_t(ed.startPlayer - 1)].startX = std::round(x * 2) / 2;
            d.players[size_t(ed.startPlayer - 1)].startZ = std::round(z * 2) / 2;
            break;
        case Tool::Road:
            if (n < 0) { ed.roadStart = -1; break; }
            if (ed.roadStart < 0) { ed.roadStart = n; break; }
            if (n != ed.roadStart) {
                bool added = doc.toggleRoad(ed.roadStart, n);
                ed.status = added ? "Дорога добавлена" : "Дорога удалена";
                ed.roadStart = io.KeyShift ? n : -1;
            }
            break;
        case Tool::Erase:
            if (n >= 0) { doc.removeEntity(n); ed.select({}); }
            else if (int r = ed.pickRoad(vp_, ed.sceneCam, sx, sy); r >= 0) doc.removeRoad(size_t(r));
            break;
        default: break;
        }
    }

    void drag(Editor& ed, float sx, float sy, ImGuiIO& io) {
        float x, z;
        if (!ed.groundAt(vp_, ed.sceneCam, sx, sy, x, z)) return;
        auto& doc = ed.doc();
        auto& d = doc.data;
        int sel = ed.selectedEntity();
        if (dragging_ && sel >= 0 && sel < int(d.entities.size())) {
            auto& e = d.entities[size_t(sel)];
            int t = ed.rules().findType(e.type);
            e.x = ed.snapCoord(x + dragOffX_, t); e.z = ed.snapCoord(z + dragOffZ_, t);
            doc.touched();
        }
        if (!painting_) return;
        int cx = int(std::floor(x)), cz = int(std::floor(z));
        const bool erase = io.KeyShift;
        const int r = ed.brush - 1;
        switch (ed.tool) {
            case Tool::Terrain: doc.paint(cx, cz, r, erase ? '.' : ed.paintCell); break;
            case Tool::Level: doc.setLevel(cx, cz, r, erase ? 0 : ed.paintLevel); break;
            case Tool::Height: doc.raise(cx, cz, ed.brush, erase ? -2 : 2); break;
            case Tool::Field:
                if (!ed.rules().resources.empty())
                    doc.paintField(cx, cz, r, ed.rules().resources[size_t(std::clamp(ed.fieldRes, 0, int(ed.rules().resources.size()) - 1))].id, erase ? 0 : ed.fieldAmount);
                break;
            case Tool::Region: doc.paintRegion(cx, cz, r, erase ? '.' : ed.regionLetter); break;
            default: break;
        }
    }

    void up(Editor& ed, float sx, float sy) {
        if (areaDrag_) {
            areaDrag_ = false;
            float x, z;
            if (ed.groundAt(vp_, ed.sceneCam, sx, sy, x, z)) {
                float x0 = std::min(areaX0_, std::floor(x)), z0 = std::min(areaZ0_, std::floor(z));
                float x1 = std::max(areaX0_, std::floor(x)) + 1, z1 = std::max(areaZ0_, std::floor(z)) + 1;
                ed.doc().beginEdit();
                Json& areas = ed.doc().data.extra.member("areas");
                std::string name;
                for (int i = 1;; ++i) { name = "area" + std::to_string(i); if (!areas.has(name)) break; }
                Json a = Json::array();
                for (float v : {x0, z0, x1 - x0, z1 - z0}) a.push(Json::number(double(v), 1));
                areas.set(name, a);
                ed.select({Selection::Area, -1, name});
                ed.status = "Зона «" + name + "» — переименуйте в Инспекторе";
            }
        }
        dragging_ = painting_ = false;
    }

    // brush circle, placement ghost and help text over the viewport
    void hud(Editor& ed) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float x, z;
        if (ed.planetMap()) { // body / face / cell under the cursor
            int body; float3 local;
            if (vp_.hovered && ed.planetAt(vp_, hoverX_, hoverY_, body, local)) {
                const game::PlanetSurface* s = ed.view().planets().surface(body);
                int f = 0; float a = 0, b = 0;
                if (s) s->faceFromDir(normalize(local), f, a, b);
                char buf[128];
                std::snprintf(buf, sizeof buf, "%s  грань %d  x %.1f  y %.1f  высота %.2f", ed.planetBodyId(body).c_str(), f, a, b, s ? s->heightAt(normalize(local)) : 0.0f);
                dl->AddText(ImVec2(vp_.originX + 8, vp_.originY + vp_.pointsH - 22), IM_COL32(255, 255, 255, 200), buf);
            }
            dl->AddText(ImVec2(vp_.originX + 8, vp_.originY + 8), IM_COL32(220, 230, 255, 200), "Планеты: колесо — масштаб, ПКМ — поворот, СКМ / WASD — движение, Tab — следующее тело");
            return;
        }
        if (!ed.playing() && vp_.hovered && ed.groundAt(vp_, ed.sceneCam, hoverX_, hoverY_, x, z)) {
            bool brushTool = ed.tool == Tool::Terrain || ed.tool == Tool::Level || ed.tool == Tool::Height || ed.tool == Tool::Field || ed.tool == Tool::Region;
            const RTSCamera& cam = ed.sceneCam;
            auto toScreen = [&](float wx, float wz, ImVec2& out) {
                float2 s;
                if (!cam.worldToScreen({wx, ed.view().heightAt(ed.editGrid(), wx, wz) + 0.1f, wz}, vp_.pointsW, vp_.pointsH, s)) return false;
                out = ImVec2(vp_.originX + s.x, vp_.originY + s.y);
                return true;
            };
            if (brushTool) {
                float r = float(ed.tool == Tool::Height ? ed.brush : ed.brush - 1) + 0.5f, cx = std::floor(x) + 0.5f, cz = std::floor(z) + 0.5f;
                ImVec2 prev;
                bool havePrev = false;
                for (int i = 0; i <= 40; ++i) {
                    float a = 6.2831853f * float(i) / 40.0f;
                    ImVec2 p;
                    if (toScreen(cx + std::cos(a) * r, cz + std::sin(a) * r, p)) {
                        if (havePrev) dl->AddLine(prev, p, IM_COL32(255, 255, 255, 200), 1.5f);
                        prev = p; havePrev = true;
                    } else havePrev = false;
                }
            }
            if (ed.tool == Tool::Place && !ed.rules().types.empty()) {
                const auto& t = ed.rules().types[size_t(ed.placeType)];
                float cx = ed.snapCoord(x, ed.placeType), cz = ed.snapCoord(z, ed.placeType);
                float hw = t.structure ? t.footprintW * 0.5f : float(t.radius.toDouble()), hh = t.structure ? t.footprintH * 0.5f : hw;
                ImVec2 a, b, c, d;
                if (toScreen(cx - hw, cz - hh, a) && toScreen(cx + hw, cz - hh, b) && toScreen(cx + hw, cz + hh, c) && toScreen(cx - hw, cz + hh, d))
                    dl->AddQuadFilled(a, b, c, d, IM_COL32(255, 255, 255, 60)), dl->AddQuad(a, b, c, d, IM_COL32(255, 255, 255, 200));
            }
            if (areaDrag_) {
                float x0 = std::min(areaX0_, std::floor(x)), z0 = std::min(areaZ0_, std::floor(z));
                float x1 = std::max(areaX0_, std::floor(x)) + 1, z1 = std::max(areaZ0_, std::floor(z)) + 1;
                ImVec2 a, b, c, d;
                if (toScreen(x0, z0, a) && toScreen(x1, z0, b) && toScreen(x1, z1, c) && toScreen(x0, z1, d)) dl->AddQuadFilled(a, b, c, d, IM_COL32(255, 220, 80, 60));
            }
            char buf[96];
            std::snprintf(buf, sizeof buf, "x %.1f  z %.1f  уровень %d", x, z, ed.doc().data.level(int(std::floor(x)), int(std::floor(z))));
            dl->AddText(ImVec2(vp_.originX + 8, vp_.originY + vp_.pointsH - 22), IM_COL32(255, 255, 255, 200), buf);
        }
        if (ed.playing())
            dl->AddText(ImVec2(vp_.originX + 8, vp_.originY + 8), IM_COL32(255, 230, 120, 230), "Идёт тест — правка отключена (F5 — стоп)");
    }

    Viewport vp_;
    bool looking_ = false, panning_ = false, dragging_ = false, painting_ = false, areaDrag_ = false;
    float dragOffX_ = 0, dragOffZ_ = 0, areaX0_ = 0, areaZ0_ = 0, hoverX_ = 0, hoverY_ = 0;
};

RTS_EDITOR_WINDOW(SceneWindow);

} // namespace rts::editor
