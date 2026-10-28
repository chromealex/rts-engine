// Inspector: properties of the current selection — map object, player, area, region, wave,
// trigger, map, or a rules item (type / resource / tech / faction) with block templates.
#include <algorithm>
#include "../Editor.h"
#include "../EditorUtil.h"

namespace rts::editor {

namespace {
struct BlockTemplate { const char* key; const char* json; const char* hint; };
const BlockTemplate kTypeBlocks[] = {
    {"health", R"({"max": 100, "armor": 0, "armorType": "none"})", "Здоровье и броня"},
    {"move", R"({"speed": 2.5, "radius": 0.4, "layer": "ground"})", "Движение: ground / air / naval / sand"},
    {"weapons", R"([{"range": 1, "damage": 10, "cooldown": 1.0, "targets": ["ground"]}])", "Оружие"},
    {"cost", R"({"gold": 100, "time": 20, "supply": 1})", "Стоимость, время, лимит"},
    {"structure", R"({"footprint": [2, 2]})", "Здание: размер; on / takesOverNode / terrain"},
    {"produces", R"([])", "Что тренирует (здания здесь = стройка как в C&C)"},
    {"builds", R"([])", "Что строит рабочий"},
    {"researches", R"([])", "Какие техи исследует"},
    {"upgrades", R"([])", "Во что улучшается на месте (уровни)"},
    {"requires", R"([])", "Требования: типы или техи"},
    {"gather", R"({"gold": {"carry": 10, "time": 2}})", "Что добывает"},
    {"resourceNode", R"({"resource": "gold", "amount": 10000})", "Источник ресурса (amount < 0 — бесконечный)"},
    {"dropoff", R"(["gold"])", "Куда сдавать ресурсы"},
    {"supply", "10", "Даёт лимит"},
    {"income", R"({"gold": 1})", "Доход в секунду"},
    {"storage", R"({"gold": 500})", "Увеличивает хранилище"},
    {"provides", R"({"power": 100})", "Даёт мощность (энергия)"},
    {"consumes", R"({"power": 20})", "Потребляет мощность"},
    {"energy", R"({"max": 100, "regen": 0.5, "start": 50})", "Мана для способностей"},
    {"abilities", R"([{"id": "heal", "name": "Лечение", "target": "unit", "range": 6, "cooldown": 8, "energy": 25, "effects": [{"type": "heal", "amount": 50}]}])", "Способности"},
    {"hero", R"({"levels": [200, 500, 900], "perLevel": [{"stat": "maxHp", "add": 50}]})", "Герой: уровни и прирост"},
    {"squad", R"({"members": 4, "reinforceCost": {"gold": 25}, "reinforceTime": 4})", "Отряд (CoH)"},
    {"charges", R"({"max": 3, "interval": 11})", "Личинки: параллельное производство"},
    {"buildMode", R"("walk")", "walk / warp / morph"},
    {"creep", "6", "Распространяет слизь"},
    {"fieldSpawner", R"({"resource": "ore", "rate": 10, "radius": 3})", "Выращивает поле ресурса"},
    {"deploysTo", R"("yard")", "Разворачивается в здание (MCV)"},
    {"wreck", R"({"metal": 100})", "Оставляет обломки"},
    {"garrison", R"({"max": 20, "unit": "unit_id", "interval": 1.0})", "Гарнизон (Mushroom Wars)"},
    {"capture", R"({"time": 10, "radius": 3, "victoryPoint": false, "region": false})", "Точка захвата / флаг"},
    {"neutral", R"({"leash": 10, "wander": false})", "Поведение нейтралов"},
    {"waveSpawn", R"({"unit": "unit_id", "count": 2})", "Выпускает волну (lane battler)"},
    {"bounty", R"({"gold": 10})", "Награда за убийство"},
    {"tags", R"(["infantry"])", "Теги для техов и триггеров"},
    {"key", "true", "Ключевой объект (потеря = поражение)"},
    {"view", R"({"shape": "soldier", "scale": 1.0})", "Внешний вид"},
};
} // namespace

class InspectorWindow : public EditorWindow {
public:
    const char* id() const override { return "inspector"; }
    const char* title() const override { return "Inspector"; }
    int order() const override { return 0; }
    const char* dockArea() const override { return "right"; }

    void draw(Editor& ed) override {
        Selection& s = ed.selection;
        switch (s.kind) {
            case Selection::Entity: entity(ed); break;
            case Selection::Player: player(ed); break;
            case Selection::Area: area(ed); break;
            case Selection::Region: region(ed); break;
            case Selection::Wave: mapJson(ed, "waves[" + std::to_string(s.index) + "]", "Волна " + std::to_string(s.index + 1)); break;
            case Selection::Trigger: mapJson(ed, "triggers[" + std::to_string(s.index) + "]", "Триггер карты"); break;
            case Selection::Type: rulesItem(ed, "types." + s.key, true); break;
            case Selection::Resource: rulesItem(ed, "resources." + s.key, false); break;
            case Selection::Tech: rulesItem(ed, "techs." + s.key, false); break;
            case Selection::Faction: rulesItem(ed, "factions." + s.key, false); break;
            case Selection::MapProps: mapProps(ed); break;
            default:
                ui::emptyStateHere(ICON_INSPECTOR, "Ничего не выбрано", "Выберите объект в Scene или Hierarchy, элемент правил в Project.",
                                   ImGui::GetFontSize() * 9);
                const float bw = ImGui::CalcTextSize(ICON_REGIONS "  Свойства карты").x + ImGui::GetStyle().FramePadding.x * 2;
                ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - bw) * 0.5f));
                if (ui::button(ICON_REGIONS, "Свойства карты")) ed.select({Selection::MapProps, -1, {}});
        }
    }

private:
    // ---------------- map object
    void entity(Editor& ed) {
        auto& doc = ed.doc();
        auto& d = doc.data;
        int i = ed.selectedEntity();
        if (i < 0 || i >= int(d.entities.size())) { ed.select({}); return; }
        auto& e = d.entities[size_t(i)];
        const auto& rules = ed.rules();
        int ti = rules.findType(e.type);
        ImGui::SeparatorText("Объект на карте");
        if (ImGui::BeginCombo("Тип", ti >= 0 ? rules.types[size_t(ti)].name.c_str() : e.type.c_str())) {
            for (auto& t : rules.types) if (ImGui::Selectable((t.name + "##" + t.id).c_str(), t.id == e.type)) { doc.beginEdit(); d.entities[size_t(i)].type = t.id; }
            ImGui::EndCombo();
        }
        ownerCombo(ed, "Владелец", d.entities[size_t(i)].owner);
        float pos[2] = {e.x, e.z};
        bool ch = ImGui::DragFloat2("Позиция", pos, 0.1f);
        if (ImGui::IsItemActivated()) doc.beginEdit();
        if (ch) { d.entities[size_t(i)].x = pos[0]; d.entities[size_t(i)].z = pos[1]; doc.touched(); }
        if (ti >= 0 && rules.types[size_t(ti)].garrisonMax > 0) {
            int g = std::max(0, d.entities[size_t(i)].garrison);
            bool c2 = ImGui::SliderInt("Гарнизон", &g, 0, rules.types[size_t(ti)].garrisonMax * 2);
            if (ImGui::IsItemActivated()) doc.beginEdit();
            if (c2) { d.entities[size_t(i)].garrison = g; doc.touched(); }
        }
        if (ti >= 0 && rules.types[size_t(ti)].resourceNode >= 0) {
            float a = d.entities[size_t(i)].amount >= 0 ? d.entities[size_t(i)].amount : float(rules.types[size_t(ti)].resourceAmount.toDouble());
            bool c3 = ImGui::DragFloat("Запас", &a, 10.0f, -1.0f, 1e7f, "%.0f");
            if (ImGui::IsItemActivated()) doc.beginEdit();
            if (c3) { d.entities[size_t(i)].amount = a; doc.touched(); }
        }
        if (ui::inputString("id", d.entities[size_t(i)].id)) doc.touched();
        ui::help("Нужен для дорог и для ссылок из триггеров");
        if (ImGui::Button("Правила этого типа", ImVec2(-1, 0))) ed.select({Selection::Type, -1, e.type});
        if (ImGui::Button("Удалить объект", ImVec2(-1, 0))) { doc.removeEntity(i); ed.select({}); }
    }

    void ownerCombo(Editor& ed, const char* label, int& owner) {
        auto& d = ed.doc().data;
        std::string cur = owner == 0 ? "Нейтральный" : owner < 0 ? "Враждебные монстры" : owner <= int(d.players.size()) ? d.players[size_t(owner - 1)].name : "?";
        if (ImGui::BeginCombo(label, cur.c_str())) {
            if (ImGui::Selectable("Нейтральный", owner == 0)) { ed.doc().beginEdit(); owner = 0; }
            if (ImGui::Selectable("Враждебные монстры", owner == -1)) { ed.doc().beginEdit(); owner = -1; }
            for (size_t p = 0; p < d.players.size(); ++p)
                if (ImGui::Selectable(d.players[p].name.c_str(), owner == int(p + 1))) { ed.doc().beginEdit(); owner = int(p + 1); }
            ImGui::EndCombo();
        }
    }

    // ---------------- player
    void player(Editor& ed) {
        auto& doc = ed.doc();
        auto& d = doc.data;
        int p = ed.selection.index;
        if (p < 0 || p >= int(d.players.size())) { ed.select({}); return; }
        auto& pl = d.players[size_t(p)];
        ImGui::SeparatorText(("Игрок " + std::to_string(p + 1)).c_str());
        if (ui::inputString("Имя", pl.name)) doc.touched();
        if (ImGui::InputInt("Команда", &pl.team)) doc.touched();
        ui::help("Игроки с одинаковой командой — союзники");
        if (!ed.rules().factions.empty() && ImGui::BeginCombo("Фракция", pl.faction.empty() ? "(по умолчанию)" : pl.faction.c_str())) {
            if (ImGui::Selectable("(по умолчанию)", pl.faction.empty())) { pl.faction.clear(); doc.touched(); }
            for (auto& f : ed.rules().factions) if (ImGui::Selectable(f.name.c_str(), pl.faction == f.id)) { pl.faction = f.id; doc.touched(); }
            ImGui::EndCombo();
        }
        if (ImGui::Checkbox("Сценарная сторона (волны)", &pl.waves)) doc.touched();
        if (pl.hasStart) {
            float st[2] = {pl.startX, pl.startZ};
            if (ImGui::DragFloat2("Старт", st, 0.25f)) { pl.startX = st[0]; pl.startZ = st[1]; doc.touched(); }
            if (ImGui::SmallButton("Убрать старт")) { pl.hasStart = false; doc.touched(); }
        } else if (ImGui::SmallButton("Задать старт на карте")) { ed.tool = Tool::Start; ed.startPlayer = p + 1; }
        ImGui::SeparatorText("Стартовые ресурсы");
        for (auto& rd : ed.rules().resources) {
            if (rd.kind == game::ResourceKind::Capacity) continue;
            float v = float(rd.start.toDouble());
            for (auto& [rid, val] : pl.resources) if (rid == rd.id) v = val;
            if (ImGui::DragFloat(rd.name.c_str(), &v, 5.0f, 0.0f, 1e6f, "%.0f")) {
                bool found = false;
                for (auto& [rid, val] : pl.resources) if (rid == rd.id) { val = v; found = true; }
                if (!found) pl.resources.push_back({rd.id, v});
                doc.touched();
            }
        }
        ImGui::Spacing();
        if (d.players.size() > 2 && ImGui::Button("Удалить игрока", ImVec2(-1, 0))) {
            Json prm = Json::object(); prm.set("player", Json::number(int64_t(p + 1)));
            ed.ws().call("player_remove", prm);
            ed.select({});
        }
        if (d.players.size() < 16 && ImGui::Button("+ Добавить игрока", ImVec2(-1, 0))) {
            doc.beginEdit();
            static const char* names[] = {"Blue", "Red", "Green", "Yellow", "Purple", "Teal", "Orange", "Pink"};
            d.players.push_back({names[d.players.size() % 8], int(d.players.size() + 1)});
            ed.select({Selection::Player, int(d.players.size() - 1), {}});
        }
    }

    // ---------------- area / region
    void area(Editor& ed) {
        auto& doc = ed.doc();
        Json* areas = doc.data.extra.find("areas");
        Json* a = areas ? areas->find(ed.selection.key) : nullptr;
        if (!a) { ed.select({}); return; }
        ImGui::SeparatorText("Зона для триггеров");
        std::string name = ed.selection.key;
        if (ui::inputString("Имя", name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() && !areas->has(name)) {
            doc.beginEdit();
            for (auto& [k, v] : areas->membersMut()) if (k == ed.selection.key) k = name;
            ed.selection.key = name;
            return;
        }
        ui::help("Enter — переименовать. Триггеры ссылаются на зону по имени.");
        float r[4] = {float(a->at(0).toDouble()), float(a->at(1).toDouble()), float(a->at(2).toDouble()), float(a->at(3).toDouble())};
        if (ImGui::DragFloat4("x, z, w, h", r, 0.25f)) {
            for (int k = 0; k < 4; ++k) a->at(size_t(k)) = Json::number(double(r[k]), 2);
            doc.touched();
        }
        if (ImGui::Button("Удалить зону", ImVec2(-1, 0))) { doc.beginEdit(); areas->erase(ed.selection.key); ed.select({}); }
    }

    void region(Editor& ed) {
        auto& d = ed.doc().data;
        char c = ed.selection.key.empty() ? '.' : ed.selection.key[0];
        ImGui::SeparatorText((std::string("Регион ") + c).c_str());
        std::string name = d.regionNames.count(c) ? d.regionNames[c] : "";
        if (ui::inputString("Название", name)) { d.regionNames[c] = name; ed.doc().touched(); }
        int cells = int(std::count(d.regions.begin(), d.regions.end(), c));
        ImGui::Text("Клеток: %d", cells);
        if (ImGui::Button("Рисовать этот регион", ImVec2(-1, 0))) { ed.tool = Tool::Region; ed.regionLetter = c; }
    }

    // ---------------- JSON editing (map blocks and rules items)
    bool jsonEditor(const std::string& key, const Json& current, Json& out, float minH = 220) {
        std::string canon = current.dump();
        if (textFor_ != key + "\n" + canon) { text_ = canon; textFor_ = key + "\n" + canon; }
        float h = std::max(minH, ImGui::GetContentRegionAvail().y - 70);
        ui::inputMultiline("##json", text_, ImVec2(-1, h));
        bool apply = ImGui::Button("Применить");
        ImGui::SameLine();
        if (ImGui::Button("Сбросить")) textFor_.clear();
        if (!apply) return false;
        try { out = Json::parse(text_); return true; }
        catch (const std::exception& e) { message_ = std::string("JSON: ") + e.what(); return false; }
    }

    void mapJson(Editor& ed, const std::string& path, const std::string& title) {
        ImGui::SeparatorText(title.c_str());
        const Json* cur = jsonPath(ed.doc().data.extra, path);
        if (!cur) { ed.select({}); return; }
        Json v;
        if (jsonEditor("map:" + path, *cur, v)) {
            Json prm = Json::object();
            prm.set("path", Json::string(path));
            prm.set("value", v);
            Json r = ed.ws().call("map_set", prm);
            message_ = r.has("error") ? r.getStr("error") : "Применено";
            if (!r.has("error"))
                for (auto& is : ed.doc().validate(ed.rules(), ed.ws().project.rulesJson)) if (is.error) { message_ = is.text; break; }
        }
        ImGui::SameLine();
        if (ImGui::Button("Удалить")) {
            ed.doc().beginEdit();
            std::string arr = path.substr(0, path.find('['));
            if (Json* a = ed.doc().data.extra.find(arr)) a->eraseAt(size_t(ed.selection.index));
            ed.select({});
        }
        if (!message_.empty()) ImGui::TextWrapped("%s", message_.c_str());
        ImGui::TextDisabled("Формат: docs/game-format.md (Maps, Triggers)");
    }

    void rulesItem(Editor& ed, const std::string& path, bool isType) {
        Json& R = ed.ws().project.rulesJson;
        const Json* cur = jsonPath(R, path);
        if (!cur) { ImGui::TextDisabled("Нет в правилах: %s", path.c_str()); return; }
        ImGui::SeparatorText((cur->getStr("name", ed.selection.key) + "  (" + path + ")").c_str());
        if (isType && cur->has("move")) bodyFields(ed, path, (*cur)["move"]);
        if (isType && ImGui::BeginCombo("##addblock", "+ Добавить блок…")) {
            for (auto& b : kTypeBlocks) {
                if (cur->has(b.key)) continue;
                if (ImGui::Selectable((std::string(b.key) + " — " + b.hint).c_str())) {
                    try { Json edited = Json::parse(text_); edited.set(b.key, Json::parse(b.json)); text_ = edited.dump(); }
                    catch (...) { message_ = "Сначала исправьте JSON"; }
                }
            }
            ImGui::EndCombo();
        }
        Json v;
        if (jsonEditor("rules:" + path, *cur, v)) {
            Json prm = Json::object();
            prm.set("path", Json::string(path));
            prm.set("value", v);
            Json r = ed.ws().call("rules_set", prm);
            message_ = r.has("error") ? r.getStr("error") : "Сохранено в rules.json";
            if (!r.has("error")) ed.reloadRules();
        }
        if (!message_.empty()) ImGui::TextWrapped("%s", message_.c_str());
    }

    // Body of a mobile unit: radius (separation, ranges edge-to-edge, path clearance) and mass
    // (push priority, default 4·r²). Edited through rules_set like an agent would.
    void bodyFields(Editor& ed, const std::string& path, const Json& move) {
        float r = float(move.has("radius") ? move["radius"].toDouble() : 0.5);
        bool explicitMass = move.has("mass");
        float m = explicitMass ? float(move["mass"].toDouble()) : 4 * r * r;
        auto set = [&](const char* key, double v) {
            Json prm = Json::object();
            prm.set("path", Json::string(path + ".move." + key));
            prm.set("value", Json::number(v, 3));
            Json res = ed.ws().call("rules_set", prm);
            message_ = res.has("error") ? res.getStr("error") : "Сохранено в rules.json";
            if (!res.has("error")) ed.reloadRules();
        };
        if (!bodyActive_) { radiusEdit_ = r; massEdit_ = m; } // follow the data unless being dragged
        bool active = false;
        ImGui::SetNextItemWidth(160);
        ImGui::DragFloat("Радиус тела", &radiusEdit_, 0.01f, 0.05f, 8.0f, "%.2f");
        active |= ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) set("radius", radiusEdit_);
        ui::help("move.radius: размер юнита — расталкивание (r1+r2), дальность атаки от края до края, проходы для больших.");
        ImGui::SetNextItemWidth(160);
        ImGui::DragFloat(explicitMass ? "Масса" : "Масса (авто 4·r²)", &massEdit_, 0.05f, 0.01f, 1000.0f, "%.2f");
        active |= ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) set("mass", massEdit_);
        bodyActive_ = active;
        ui::help("move.mass: кто кого толкает. Атакующие и стоящие на позиции почти не сдвигаются.");
    }

    void mapProps(Editor& ed) {
        auto& doc = ed.doc();
        auto& d = doc.data;
        ImGui::SeparatorText("Карта");
        if (ui::inputString("Название", d.name)) doc.touched();
        ImGui::Text("Размер %d × %d, объектов %zu, игроков %zu", d.w, d.h, d.entities.size(), d.players.size());
        static int sz[2] = {0, 0};
        if (sz[0] == 0) { sz[0] = d.w; sz[1] = d.h; }
        ImGui::SetNextItemWidth(140);
        ImGui::InputInt2("##size", sz);
        ImGui::SameLine();
        if (ImGui::Button("Изменить размер")) doc.resize(sz[0], sz[1]);
        bool ro = d.roadsOnly;
        if (ImGui::Checkbox("Только по дорогам (Mushroom Wars)", &ro)) { doc.beginEdit(); d.roadsOnly = ro; }
        ImGui::SeparatorText("Победа (поверх правил)");
        Json v;
        if (jsonEditor("map:victory", d.extra["victory"], v, 60)) {
            doc.beginEdit();
            if (v.isNull()) d.extra.erase("victory"); else d.extra.set("victory", v);
        }
        ImGui::SeparatorText("Проверка");
        if (ImGui::Button("Проверить карту", ImVec2(-1, 0))) issues_ = doc.validate(ed.rules(), ed.ws().project.rulesJson);
        for (size_t i = 0; i < issues_.size(); ++i) {
            const auto& is = issues_[i];
            ImGui::PushID(int(i));
            ImGui::PushStyleColor(ImGuiCol_Text, is.error ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(1, 0.85f, 0.4f, 1));
            if (ImGui::Selectable(is.text.c_str()) && is.entity >= 0 && is.entity < int(d.entities.size())) {
                ed.selectEntity(is.entity);
                ed.focusScene(d.entities[size_t(is.entity)].x, d.entities[size_t(is.entity)].z, 18);
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        if (!issues_.empty() && std::none_of(issues_.begin(), issues_.end(), [](const Issue& i) { return i.error; }))
            ImGui::TextColored(ImVec4(0.5f, 1, 0.5f, 1), "Ошибок нет");
    }

    std::string text_, textFor_, message_;
    float radiusEdit_ = 0, massEdit_ = 0;
    bool bodyActive_ = false;
    std::vector<Issue> issues_;
};

RTS_EDITOR_WINDOW(InspectorWindow);

} // namespace rts::editor
