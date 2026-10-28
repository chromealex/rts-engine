// Parameter UI of one particle emitter for the Effects window: Unity-style module sections, each with an
// enable checkbox. Every range can be switched to a curve (edited as keys). Included by FxWindow.cpp only.
#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "../EditorUtil.h"
#include "engine/fx/ParticleSystem.h"

namespace rts::editor::fxui {

using fx::EmitterDef;
using fx::FxRange;

inline bool combo(const char* label, int& v, const char* items) { return ImGui::Combo(label, &v, items); }
template <class E> bool comboE(const char* label, E& v, const char* items) {
    int i = int(v);
    if (!ImGui::Combo(label, &i, items)) return false;
    v = E(i);
    return true;
}

// Keys [t, value] with add / remove; returns true on change (kept sorted by t).
inline bool keys(const char* label, std::vector<fx::FxSizeKey>& k, float lo, float hi, const char* fmt = "%.2f", float def = 1) {
    bool ch = false;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    if (!k.empty()) {
        float vals[32];
        for (int i = 0; i < 32; ++i) vals[i] = fx::sampleKeys(k, float(i) / 31.0f, def);
        ImGui::SameLine();
        ImGui::PlotLines("##plot", vals, 32, 0, nullptr, lo, hi, ImVec2(ImGui::GetFontSize() * 8, ImGui::GetFontSize() * 1.4f));
    }
    int del = -1;
    for (size_t i = 0; i < k.size(); ++i) {
        ImGui::PushID(int(i));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
        ch |= ImGui::SliderFloat("##t", &k[i].t, 0, 1, "t %.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        ch |= ImGui::DragFloat("##v", &k[i].k, (hi - lo) / 200.0f, lo, hi, fmt);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) del = int(i);
        ImGui::PopID();
    }
    if (del >= 0) { k.erase(k.begin() + del); ch = true; }
    if (ImGui::SmallButton("+ ключ")) {
        fx::FxSizeKey nk = k.empty() ? fx::FxSizeKey{0, def} : k.back();
        nk.t = k.empty() ? 0 : std::min(1.0f, nk.t + 0.25f);
        k.push_back(nk);
        ch = true;
    }
    if (ch) std::stable_sort(k.begin(), k.end(), [](auto& a, auto& b) { return a.t < b.t; });
    ImGui::PopID();
    return ch;
}

inline bool colors(const char* label, std::vector<fx::FxColorKey>& k) {
    bool ch = false;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    int del = -1;
    for (size_t i = 0; i < k.size(); ++i) {
        ImGui::PushID(int(i));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
        ch |= ImGui::SliderFloat("##t", &k[i].t, 0, 1, "t %.2f");
        ImGui::SameLine();
        ch |= ImGui::ColorEdit4("##c", &k[i].rgba.x, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) del = int(i);
        ImGui::PopID();
    }
    if (del >= 0) { k.erase(k.begin() + del); ch = true; }
    if (ImGui::SmallButton("+ цвет")) {
        fx::FxColorKey nk = k.empty() ? fx::FxColorKey{} : k.back();
        nk.t = k.empty() ? 0 : std::min(1.0f, nk.t + 0.25f);
        k.push_back(nk);
        ch = true;
    }
    if (ch) std::stable_sort(k.begin(), k.end(), [](auto& a, auto& b) { return a.t < b.t; });
    ImGui::PopID();
    return ch;
}

// Range (min..max) or curve; the small button switches between them.
inline bool range(const char* label, FxRange& r, float speed, float lo, float hi, const char* fmt = "%.2f") {
    bool ch = false;
    ImGui::PushID(label);
    if (!r.curved()) {
        ch |= ImGui::DragFloatRange2(label, &r.min, &r.max, speed, lo, hi, fmt, nullptr, ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        if (ImGui::SmallButton("~")) { r.curve = {{0, r.max}, {1, r.max}}; ch = true; }
        if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted("Сделать кривой (по времени эмиттера или жизни частицы)"); ImGui::EndTooltip(); }
    } else {
        if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen, "%s: кривая", label)) {
            ch |= keys("макс.", r.curve, lo, hi, fmt, 0);
            bool two = !r.curveMin.empty();
            if (ImGui::Checkbox("случайно между двумя кривыми", &two)) { r.curveMin = two ? r.curve : std::vector<fx::FxSizeKey>{}; ch = true; }
            if (two) ch |= keys("мин.", r.curveMin, lo, hi, fmt, 0);
            if (ImGui::SmallButton("→ число")) { float a = r.average(); r = FxRange(a, a); ch = true; }
            ImGui::TreePop();
        }
        if (ch) {   // bounds for culling and the next switch back
            r.min = 1e30f; r.max = -1e30f;
            for (auto* ks : {&r.curve, &r.curveMin}) for (auto& k : *ks) { r.min = std::min(r.min, k.k); r.max = std::max(r.max, k.k); }
            if (r.curve.empty()) { r.min = r.max = 0; }
        }
    }
    ImGui::PopID();
    return ch;
}

inline bool vec3(const char* label, fx::float3& v, float speed, float lo, float hi, const char* fmt = "%.2f") {
    return ImGui::DragFloat3(label, &v.x, speed, lo, hi, fmt);
}

// Collapsing module header with an enable checkbox (nullptr: always on).
inline bool module(const char* title, bool* enabled, bool defaultOpen = false) {
    ImGui::PushID(title);
    bool changed = false;
    if (enabled) { changed = ImGui::Checkbox("##on", enabled); ImGui::SameLine(); }
    bool open = ImGui::TreeNodeEx(title, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    ImGui::PopID();
    if (changed) ImGui::GetStateStorage()->SetInt(ImGui::GetID(title), 1);
    return open;
}

// Returns true when anything changed.
inline bool emitterUi(EmitterDef& d) {
    bool ch = false;
    ImGui::PushItemWidth(ImGui::GetFontSize() * 15);

    if (module("Основное", nullptr, true)) {
        if (ImGui::BeginCombo("Текстура", d.texture.c_str())) {
            for (auto& b : fx::ParticleSystem::builtinTextures()) {
                std::string n = "builtin:" + b;
                if (ImGui::Selectable(n.c_str(), d.texture == n)) { d.texture = n; ch = true; }
            }
            ImGui::EndCombo();
        }
        ui::help("Встроенные: soft, spark, ring, smoke. Или путь к картинке игры — грузится той же функцией, что и спрайты.");
        std::string tex = d.texture;
        if (ui::inputString("Путь к текстуре", tex, ImGuiInputTextFlags_EnterReturnsTrue) && !tex.empty()) { d.texture = tex; ch = true; }
        ch |= comboE("Смешивание", d.blend, "alpha (дым, пыль)\0additive (огонь, искры, свет)\0premultiplied\0multiply (затемнение)\0subtract (вычитание)\0opaque (меши)\0");
        ch |= ImGui::SliderFloat("Длительность, с", &d.duration, 0, 20, "%.2f");
        ch |= ImGui::Checkbox("Цикл", &d.loop);
        ImGui::SameLine();
        ch |= ImGui::Checkbox("Прогрев (prewarm)", &d.prewarm);
        ch |= range("Задержка, с", d.delay, 0.01f, 0, 60);
        ch |= range("Жизнь, с", d.life, 0.01f, 0.01f, 60);
        ch |= range("Скорость", d.speed, 0.05f, -500, 500);
        ch |= range("Размер", d.size, 0.01f, 0.001f, 100);
        if (ImGui::Checkbox("Размер по осям (3D)", &d.size3D)) ch = true;
        if (d.size3D) { ch |= range("Размер Y", d.sizeY, 0.01f, 0.001f, 100); ch |= range("Размер Z", d.sizeZ, 0.01f, 0.001f, 100); }
        else ch |= range("Длина / ширина", d.aspect, 0.01f, 0.01f, 100);
        ch |= range("Поворот, °", d.rotation, 1, -3600, 3600, "%.0f");
        if (ImGui::Checkbox("Поворот по осям (меши)", &d.rotation3D)) ch = true;
        if (d.rotation3D) { ch |= range("Поворот X, °", d.rotationX, 1, -3600, 3600, "%.0f"); ch |= range("Поворот Y, °", d.rotationY, 1, -3600, 3600, "%.0f"); }
        ch |= colors("Стартовый цвет (1 ключ — постоянный)", d.startColor);
        if (d.startColor.size() > 1) {
            ch |= ImGui::Checkbox("Пара цветов (случайно между)", &d.startColorPair);
            ImGui::SameLine();
            ch |= ImGui::Checkbox("По времени эмиттера", &d.startColorOverTime);
        }
        ch |= vec3("Гравитация", d.gravity, 0.05f, -100, 100);
        ch |= range("Множитель гравитации", d.gravityScale, 0.01f, -100, 100);
        ch |= ImGui::Checkbox("Локальное пространство", &d.localSpace);
        ui::help("Живые частицы двигаются вместе с эффектом (ауры, огонь на юните).");
        ch |= ImGui::InputInt("Макс. частиц (0 — без лимита)", &d.maxParticles);
        d.maxParticles = std::clamp(d.maxParticles, 0, 100000);
        if (d.maxParticles > 0) { ImGui::SameLine(); ch |= ImGui::Checkbox("Кольцевой буфер", &d.ringBuffer); }
        int mq = std::max(1, int(d.minQuality)) - 1;
        if (ImGui::Combo("Начиная с качества", &mq, "низкого (всегда)\0среднего\0высокого\0")) { d.minQuality = fx::FxQuality(mq + 1); ch = true; }
        ui::help("Второстепенные эмиттеры (искры, ударные волны) лучше включать со «среднего»: на слабых телефонах их не будет.");
        ImGui::TreePop();
    }

    if (module("Рождение", nullptr, true)) {
        ch |= ImGui::SliderInt("Сразу (burst)", &d.burst, 0, 500);
        ch |= range("В секунду (rate)", d.rate, 0.1f, 0, 2000, "%.1f");
        ch |= range("На метр пути (perMeter)", d.perMeter, 0.05f, 0, 200, "%.2f");
        ui::help("Для шлейфов: частицы оставляются по пути, сколько бы кадров ни прошло.");
        int del = -1;
        for (size_t i = 0; i < d.bursts.size(); ++i) {
            auto& b = d.bursts[i];
            ImGui::PushID(int(i));
            ImGui::Text("Всплеск %d", int(i + 1));
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) del = int(i);
            ch |= ImGui::DragFloat("Время, с", &b.time, 0.01f, 0, 600, "%.2f");
            ch |= range("Количество", b.count, 0.5f, 0, 2000, "%.0f");
            ch |= ImGui::InputInt("Повторов (0 — бесконечно)", &b.cycles);
            b.cycles = std::clamp(b.cycles, 0, 1000);
            ch |= ImGui::DragFloat("Интервал, с", &b.interval, 0.01f, 0, 600, "%.2f");
            ch |= ImGui::SliderFloat("Вероятность", &b.chance, 0, 1, "%.2f");
            ImGui::PopID();
        }
        if (del >= 0) { d.bursts.erase(d.bursts.begin() + del); ch = true; }
        if (ImGui::SmallButton("+ всплеск")) { fx::FxBurst b; b.count = {10, 10}; d.bursts.push_back(b); ch = true; }
        ImGui::TreePop();
    }

    if (module("Форма", nullptr)) {
        ch |= comboE("Форма", d.shape, "точка\0шар\0диск\0кольцо\0полусфера\0конус\0коробка\0линия\0тор (donut)\0меш\0");
        using S = fx::FxShape;
        if (d.shape != S::Point && d.shape != S::Box) ch |= ImGui::DragFloat("Радиус", &d.radius, 0.01f, 0, 100, "%.2f");
        if (d.shape == S::Donut) ch |= ImGui::DragFloat("Радиус трубки", &d.donutRadius, 0.01f, 0, 100, "%.2f");
        if (d.shape == S::Cone) ch |= ImGui::DragFloat("Длина (объём конуса)", &d.length, 0.01f, 0, 1000, "%.2f");
        if (d.shape == S::Box || d.shape == S::Mesh) ch |= vec3(d.shape == S::Box ? "Размер коробки" : "Масштаб меша", d.box, 0.01f, 0, 1000);
        if (d.shape == S::Mesh) {
            if (ImGui::BeginCombo("Меш", d.mesh.c_str())) {
                for (auto& m : fx::ParticleSystem::builtinMeshes()) {
                    std::string n = "builtin:" + m;
                    if (ImGui::Selectable(n.c_str(), d.mesh == n)) { d.mesh = n; ch = true; }
                }
                ImGui::EndCombo();
            }
            std::string m = d.mesh;
            if (ui::inputString("Путь к модели", m, ImGuiInputTextFlags_EnterReturnsTrue)) { d.mesh = m; ch = true; }
        }
        if (d.shape != S::Point && d.shape != S::Line)
            ch |= comboE("Откуда", d.emitFrom, "объём\0поверхность\0рёбра\0вершины\0треугольники\0");
        if (d.shape == S::Sphere || d.shape == S::Hemisphere || d.shape == S::Disc || d.shape == S::Cone || d.shape == S::Donut) {
            ch |= ImGui::SliderFloat("Толщина (0 — поверхность)", &d.thickness, 0, 1, "%.2f");
            ch |= ImGui::SliderFloat("Дуга, °", &d.arc, 0, 360, "%.0f");
            ch |= comboE("Обход дуги", d.arcMode, "случайно\0по кругу\0туда-обратно\0равномерно во всплеске\0");
            if (d.arcMode == fx::FxArcMode::Loop || d.arcMode == fx::FxArcMode::PingPong) ch |= ImGui::DragFloat("Скорость обхода, об/с", &d.arcSpeed, 0.01f, -100, 100);
            ch |= ImGui::SliderFloat("Шаг дуги (0 — плавно)", &d.arcSpread, 0, 1, "%.2f");
        }
        ch |= vec3("Поворот формы, °", d.shapeRotation, 1, -360, 360, "%.0f");
        ch |= vec3("Смещение", d.offset, 0.01f, -1000, 1000);
        ch |= vec3("Направление", d.direction, 0.02f, -1, 1);
        ch |= ImGui::SliderFloat("Разброс, °", &d.spread, 0, 180, "%.0f");
        ch |= ImGui::SliderFloat("Наружу от центра (-1..1)", &d.radial, -1, 1, "%.2f");
        ch |= ImGui::SliderFloat("Случайное направление", &d.randomDirection, 0, 1, "%.2f");
        ch |= ImGui::SliderFloat("Сферическое направление", &d.sphericalDirection, 0, 1, "%.2f");
        ch |= ImGui::DragFloat("Случайная позиция", &d.randomPosition, 0.01f, 0, 100, "%.2f");
        ch |= ImGui::Checkbox("Повернуть по направлению", &d.alignToDirection);
        ImGui::TreePop();
    }

    {
        auto& v = d.velocityOverLife;
        if (module("Скорость за жизнь", &v.enabled)) {
            ch |= vec3("Постоянная скорость", d.velocity, 0.05f, -1000, 1000);
            ch |= range("X", v.x, 0.05f, -1000, 1000); ch |= range("Y", v.y, 0.05f, -1000, 1000); ch |= range("Z", v.z, 0.05f, -1000, 1000);
            ch |= ImGui::Checkbox("В мировых осях", &v.world);
            ch |= range("Орбита X, °/с", v.orbitalX, 1, -36000, 36000, "%.0f");
            ch |= range("Орбита Y, °/с", v.orbitalY, 1, -36000, 36000, "%.0f");
            ch |= range("Орбита Z, °/с", v.orbitalZ, 1, -36000, 36000, "%.0f");
            ch |= vec3("Центр орбиты", v.orbitalOffset, 0.01f, -1000, 1000);
            ch |= range("Радиально", v.radial, 0.05f, -1000, 1000);
            ch |= range("Множитель скорости", v.speedModifier, 0.01f, -100, 100);
            ImGui::TreePop();
        }
    }
    if (module("Ограничение скорости", nullptr)) {
        ch |= range("Предел", d.speedLimit, 0.05f, 0, 1000);
        ch |= ImGui::Checkbox("По осям", &d.speedLimitAxes.enabled);
        if (d.speedLimitAxes.enabled) {
            ch |= range("Предел X", d.speedLimitAxes.x, 0.05f, 0, 1e6f); ch |= range("Предел Y", d.speedLimitAxes.y, 0.05f, 0, 1e6f);
            ch |= range("Предел Z", d.speedLimitAxes.z, 0.05f, 0, 1e6f); ch |= ImGui::Checkbox("Оси мировые", &d.speedLimitAxes.world);
        }
        ch |= ImGui::SliderFloat("Гашение превышения", &d.limitDampen, 0, 1, "%.2f");
        ch |= range("Сопротивление", d.drag, 0.01f, 0, 50);
        ch |= ImGui::Checkbox("× размер", &d.dragBySize);
        ImGui::SameLine();
        ch |= ImGui::Checkbox("× скорость", &d.dragBySpeed);
        ImGui::TreePop();
    }
    if (module("Наследование скорости", &d.inheritVelocity.enabled)) {
        int m = d.inheritVelocity.current ? 1 : 0;
        if (combo("Режим", m, "при рождении\0всё время\0")) { d.inheritVelocity.current = m == 1; ch = true; }
        ch |= range("Доля", d.inheritVelocity.amount, 0.01f, -10, 10);
        ImGui::TreePop();
    }
    if (module("Жизнь по скорости эмиттера", &d.lifeBySpeed.enabled)) {
        ch |= range("Скорость (диапазон)", d.lifeBySpeed.speed, 0.1f, 0, 1e5f);
        ch |= keys("Множитель жизни", d.lifeBySpeed.keys, 0, 10);
        ImGui::TreePop();
    }
    if (module("Сила за жизнь", &d.force.enabled)) {
        ch |= range("X", d.force.x, 0.05f, -1000, 1000); ch |= range("Y", d.force.y, 0.05f, -1000, 1000); ch |= range("Z", d.force.z, 0.05f, -1000, 1000);
        ch |= ImGui::Checkbox("В мировых осях", &d.force.world);
        ImGui::SameLine();
        ch |= ImGui::Checkbox("Случайно каждый кадр", &d.force.randomize);
        ImGui::TreePop();
    }
    {
        bool ext = d.externalForces > 0;
        if (module("Внешние силы", &ext)) {
            ch |= ImGui::DragFloat("Множитель", &d.externalForces, 0.01f, 0, 100, "%.2f");
            ui::help("Ветер (ParticleSystem::setWind) и силовые поля эффекта (список внизу).");
            ImGui::TreePop();
        }
        if (ext && d.externalForces <= 0) { d.externalForces = 1; ch = true; }
        if (!ext && d.externalForces > 0) { d.externalForces = 0; ch = true; }
    }
    if (module("Цвет за жизнь", nullptr, true)) {
        ch |= colors("Цвет", d.color);
        bool two = !d.colorMin.empty();
        if (ImGui::Checkbox("Случайно между двумя градиентами", &two)) { d.colorMin = two ? d.color : std::vector<fx::FxColorKey>{}; ch = true; }
        if (two) ch |= colors("Второй градиент", d.colorMin);
        ImGui::TreePop();
    }
    {
        bool on = !d.colorBySpeed.empty();
        if (module("Цвет по скорости", &on)) {
            if (on && d.colorBySpeed.empty()) d.colorBySpeed = {{0, {1, 1, 1, 1}}, {1, {1, 1, 1, 1}}};
            ch |= range("Скорость (диапазон)", d.colorSpeed, 0.1f, 0, 1000);
            ch |= colors("Цвет", d.colorBySpeed);
            ImGui::TreePop();
        }
        if (!on && !d.colorBySpeed.empty()) { d.colorBySpeed.clear(); ch = true; }
        if (on && d.colorBySpeed.empty()) { d.colorBySpeed = {{0, {1, 1, 1, 1}}, {1, {1, 1, 1, 1}}}; ch = true; }
    }
    if (module("Размер за жизнь", nullptr, true)) {
        ch |= keys("Множитель", d.sizeOverLife, 0, 10);
        bool two = !d.sizeOverLifeMin.empty();
        if (ImGui::Checkbox("Случайно между двумя кривыми##size", &two)) { d.sizeOverLifeMin = two ? d.sizeOverLife : std::vector<fx::FxSizeKey>{}; ch = true; }
        if (two) ch |= keys("Вторая кривая", d.sizeOverLifeMin, 0, 10);
        bool axes = !d.sizeOverLifeY.empty();
        if (ImGui::Checkbox("Отдельно по осям", &axes)) { d.sizeOverLifeY = axes ? d.sizeOverLife : std::vector<fx::FxSizeKey>{}; d.sizeOverLifeZ = d.sizeOverLifeY; ch = true; }
        if (axes) { ch |= keys("Y", d.sizeOverLifeY, 0, 10); ch |= keys("Z (меши)", d.sizeOverLifeZ, 0, 10); }
        ImGui::TreePop();
    }
    {
        bool on = !d.sizeBySpeed.empty();
        if (module("Размер по скорости", &on)) {
            ch |= range("Скорость (диапазон)##s", d.sizeSpeed, 0.1f, 0, 1000);
            ch |= keys("Множитель##s", d.sizeBySpeed, 0, 10);
            ImGui::TreePop();
        }
        if (!on && !d.sizeBySpeed.empty()) { d.sizeBySpeed.clear(); ch = true; }
        if (on && d.sizeBySpeed.empty()) { d.sizeBySpeed = {{0, 1}, {1, 1}}; ch = true; }
    }
    if (module("Вращение", nullptr)) {
        ch |= range("Вращение, °/с", d.spin, 1, -36000, 36000, "%.0f");
        ch |= ImGui::Checkbox("По осям (меши)", &d.spin3D);
        if (d.spin3D) { ch |= range("X, °/с", d.spinX, 1, -36000, 36000, "%.0f"); ch |= range("Y, °/с", d.spinY, 1, -36000, 36000, "%.0f"); }
        ch |= ImGui::Checkbox("По скорости", &d.spinBySpeed.enabled);
        if (d.spinBySpeed.enabled) {
            ch |= range("Скорость (диапазон)##r", d.spinBySpeed.speed, 0.1f, 0, 1e5f);
            ch |= keys("Вращение, °/с##r", d.spinBySpeed.keys, -3600, 3600, "%.0f", 0);
        }
        ImGui::TreePop();
    }
    {
        auto& n = d.noise;
        bool on = n.enabled();
        if (module("Шум", &on)) {
            ch |= range("Сила", n.strength, 0.01f, 0, 100);
            ch |= vec3("По осям", n.axes, 0.01f, 0, 10);
            ch |= ImGui::DragFloat("Частота", &n.frequency, 0.01f, 0.001f, 100, "%.3f");
            ch |= ImGui::DragFloat("Прокрутка", &n.scroll, 0.01f, -100, 100, "%.2f");
            ch |= ImGui::SliderInt("Октавы", &n.octaves, 1, 4);
            ui::help("На слабых телефонах — 1 октава, на средних — до 2.");
            if (n.octaves > 1) { ch |= ImGui::SliderFloat("Множитель октавы", &n.octaveMultiplier, 0, 1); ch |= ImGui::SliderFloat("Масштаб октавы", &n.octaveScale, 1, 4); }
            ch |= ImGui::Checkbox("Демпфирование (сила ∝ частоте)", &n.damping);
            ch |= ImGui::SliderFloat("Влияние на позицию", &n.position, 0, 10);
            ch |= ImGui::DragFloat("Влияние на поворот, °/с", &n.rotation, 1, -3600, 3600, "%.0f");
            ch |= ImGui::SliderFloat("Влияние на размер", &n.size, 0, 10);
            ch |= keys("Переназначение (-1..1)", n.remap, -1, 1, "%.2f", 0);
            ImGui::TreePop();
        }
        if (on && !n.enabled()) { n.strength = {1, 1}; ch = true; }
        if (!on && n.enabled()) { n.strength = {0, 0}; ch = true; }
    }
    {
        auto& c = d.collision;
        if (module("Столкновения", &c.enabled)) {
            ch |= ImGui::Checkbox("С землёй", &c.ground);
            if (c.ground) ch |= ImGui::DragFloat("Высота над землёй", &c.height, 0.01f, -100, 100, "%.2f");
            int del = -1;
            for (size_t i = 0; i < c.planes.size(); ++i) {
                ImGui::PushID(int(i));
                ch |= vec3("Точка плоскости", c.planes[i].point, 0.01f, -1000, 1000);
                ch |= vec3("Нормаль", c.planes[i].normal, 0.01f, -1, 1);
                if (ImGui::SmallButton("убрать плоскость")) del = int(i);
                ImGui::PopID();
            }
            if (del >= 0) { c.planes.erase(c.planes.begin() + del); ch = true; }
            if (c.planes.size() < 6 && ImGui::SmallButton("+ плоскость")) { c.planes.push_back({}); ch = true; }
            ch |= ImGui::SliderFloat("Отскок", &c.bounce, 0, 2); ch |= ImGui::SliderFloat("Гашение", &c.dampen, 0, 1);
            ch |= ImGui::SliderFloat("Потеря жизни", &c.lifeLoss, 0, 1); ch |= ImGui::SliderFloat("Радиус (доля размера)", &c.radius, 0, 2);
            ch |= ImGui::DragFloat("Мин. скорость (убить)", &c.minKillSpeed, 0.05f, 0, 1000); ch |= ImGui::DragFloat("Макс. скорость (убить)", &c.maxKillSpeed, 0.1f, 0, 1e5f);
            ImGui::TreePop();
        }
    }
    {
        auto& tg = d.trigger;
        if (module("Триггер", &tg.enabled)) {
            int del = -1;
            for (size_t i = 0; i < tg.volumes.size(); ++i) {
                ImGui::PushID(int(i));
                ch |= vec3("Центр", tg.volumes[i].center, 0.01f, -1000, 1000);
                ch |= ImGui::DragFloat("Радиус##v", &tg.volumes[i].radius, 0.01f, 0, 1000);
                if (ImGui::SmallButton("убрать объём")) del = int(i);
                ImGui::PopID();
            }
            if (del >= 0) { tg.volumes.erase(tg.volumes.begin() + del); ch = true; }
            if (tg.volumes.size() < 8 && ImGui::SmallButton("+ объём")) { tg.volumes.push_back({}); ch = true; }
            const char* acts = "ничего\0убить\0под-эмиттеры «trigger»\0";
            ch |= comboE("Внутри", tg.inside, acts); ch |= comboE("Снаружи", tg.outside, acts);
            ch |= comboE("Вход", tg.enter, acts); ch |= comboE("Выход", tg.exit, acts);
            ch |= ImGui::SliderFloat("Радиус частицы (доля размера)", &tg.radius, 0, 2);
            ImGui::TreePop();
        }
        if (tg.enabled && tg.volumes.empty()) { tg.volumes.push_back({}); ch = true; }
    }
    if (module("Под-эмиттеры", nullptr)) {
        int del = -1;
        for (size_t i = 0; i < d.subEmitters.size(); ++i) {
            auto& se = d.subEmitters[i];
            ImGui::PushID(int(i));
            ch |= comboE("Когда", se.on, "рождение\0смерть\0столкновение\0триггер\0вручную\0");
            if (ui::inputString("Эффект", se.effect)) ch = true;
            ch |= ImGui::SliderFloat("Вероятность", &se.chance, 0, 1);
            ImGui::TextUnformatted("Наследовать:");
            ImGui::SameLine(); ch |= ImGui::Checkbox("цвет", &se.inheritColor);
            ImGui::SameLine(); ch |= ImGui::Checkbox("размер", &se.inheritSize);
            ImGui::SameLine(); ch |= ImGui::Checkbox("поворот", &se.inheritRotation);
            ImGui::SameLine(); ch |= ImGui::Checkbox("жизнь", &se.inheritLifetime);
            ImGui::SameLine(); ch |= ImGui::Checkbox("длительность", &se.inheritDuration);
            if (ImGui::SmallButton("убрать")) del = int(i);
            ImGui::PopID();
        }
        if (del >= 0) { d.subEmitters.erase(d.subEmitters.begin() + del); ch = true; }
        if (d.subEmitters.size() < size_t(fx::kMaxSubEmitters) && ImGui::SmallButton("+ под-эмиттер")) { fx::FxSubEmitter se; se.effect = "sparks"; d.subEmitters.push_back(se); ch = true; }
        ImGui::TreePop();
    }
    if (module("Флипбук (анимация текстуры)", nullptr)) {
        int frames[2] = {d.frameCols, d.frameRows};
        if (ImGui::InputInt2("Кадры (колонки, строки)", frames)) { d.frameCols = std::clamp(frames[0], 1, 16); d.frameRows = std::clamp(frames[1], 1, 16); ch = true; }
        ch |= ImGui::SliderFloat("Кадров в секунду", &d.fps, 0, 120, d.fps == 0 ? "за жизнь" : "%.0f");
        ch |= ImGui::DragFloat("Повторов за жизнь", &d.cycles, 0.05f, 0.01f, 1000);
        ch |= range("Первый кадр", d.startFrame, 0.2f, 0, 255, "%.0f");
        int rowMode = d.row == -1 ? 0 : d.row == -2 ? 1 : 2;
        if (combo("Ряды", rowMode, "весь лист\0случайный ряд\0один ряд\0")) { d.row = rowMode == 0 ? -1 : rowMode == 1 ? -2 : 0; ch = true; }
        if (d.row >= 0) { ch |= ImGui::SliderInt("Ряд", &d.row, 0, std::max(0, d.frameRows - 1)); }
        ch |= range("Кадр за жизнь (0..1)", d.frameOverTime, 0.01f, 0, 1);
        ch |= range("Кадр по скорости (диапазон)", d.framesBySpeed, 0.1f, 0, 1e5f);
        ch |= ImGui::SliderFloat2("Отражение U / V (вероятность)", &d.flip.x, 0, 1);
        std::string sprites;
        for (auto& s : d.sprites) sprites += (sprites.empty() ? "" : ", ") + s;
        if (ui::inputString("Спрайты (через запятую)", sprites, ImGuiInputTextFlags_EnterReturnsTrue)) {
            d.sprites.clear();
            size_t a = 0;
            while (a < sprites.size()) {
                size_t b = sprites.find(',', a);
                std::string s = sprites.substr(a, b == std::string::npos ? std::string::npos : b - a);
                s.erase(0, s.find_first_not_of(' '));
                if (!s.empty()) s.erase(s.find_last_not_of(' ') + 1);
                if (!s.empty()) d.sprites.push_back(s);
                if (b == std::string::npos) break;
                a = b + 1;
            }
            ch = true;
        }
        ImGui::TreePop();
    }
    {
        auto& l = d.light;
        if (module("Свет", &l.enabled)) {
            ch |= ImGui::ColorEdit3("Цвет света", &l.color.x);
            ch |= ImGui::DragFloat("Радиус##l", &l.radius, 0.05f, 0.01f, 100); ch |= ImGui::DragFloat("Яркость", &l.intensity, 0.01f, 0, 20);
            ch |= ImGui::SliderFloat("Доля частиц", &l.ratio, 0, 1); ch |= ImGui::SliderInt("Макс. источников", &l.max, 0, 64);
            ch |= ImGui::Checkbox("Цвет частицы", &l.useParticleColor); ImGui::SameLine();
            ch |= ImGui::Checkbox("Размер → радиус", &l.sizeAffectsRadius); ImGui::SameLine();
            ch |= ImGui::Checkbox("Прозрачность → яркость", &l.alphaAffectsIntensity);
            ch |= keys("Радиус за жизнь", l.radiusOverLife, 0, 10); ch |= keys("Яркость за жизнь", l.intensityOverLife, 0, 10);
            ImGui::TreePop();
        }
    }
    {
        auto& t = d.trail;
        if (module("Шлейф", &t.enabled)) {
            int m = t.ribbon ? 1 : 0;
            if (combo("Режим", m, "у каждой частицы\0лента через частицы\0")) { t.ribbon = m == 1; ch = true; }
            if (t.ribbon) ch |= ImGui::SliderInt("Лент", &t.ribbons, 1, 16);
            else ch |= ImGui::SliderFloat("Доля частиц", &t.ratio, 0, 1);
            ch |= ImGui::DragFloat("Время, с", &t.time, 0.01f, 0.01f, 10);
            ch |= ImGui::SliderFloat("Время = доля жизни (0 — выкл)", &t.lifeFraction, 0, 1);
            ch |= ImGui::DragFloat("Мин. расстояние", &t.minDistance, 0.01f, 0, 100);
            ch |= keys("Ширина вдоль шлейфа", t.width, 0, 10);
            ch |= colors("Цвет вдоль шлейфа", t.color);
            ch |= colors("Цвет за жизнь частицы", t.lifeColor);
            std::string tx = t.texture;
            if (ui::inputString("Текстура шлейфа", tx, ImGuiInputTextFlags_EnterReturnsTrue) && !tx.empty()) { t.texture = tx; ch = true; }
            ch |= comboE("Смешивание шлейфа", t.blend, "alpha\0additive\0premultiplied\0multiply\0subtract\0opaque\0");
            ch |= comboE("Текстура вдоль", t.textureMode, "растянуть\0повторять по длине\0по точкам\0");
            ch |= ImGui::Checkbox("Цвет частицы", &t.inheritColor); ImGui::SameLine();
            ch |= ImGui::Checkbox("Размер → ширина", &t.sizeAffectsWidth); ImGui::SameLine();
            ch |= ImGui::Checkbox("Размер → время", &t.sizeAffectsLifetime);
            ch |= ImGui::Checkbox("Исчезает с частицей", &t.dieWithParticles);
            int mq = std::max(1, int(t.minQuality)) - 1;
            if (ImGui::Combo("Начиная с качества##t", &mq, "низкого\0среднего\0высокого\0")) { t.minQuality = fx::FxQuality(mq + 1); ch = true; }
            ImGui::TreePop();
        }
    }
    if (module("Отрисовка", nullptr)) {
        ch |= comboE("Ориентация", d.align, "к камере\0по скорости\0по земле\0вертикально\0мировая плоскость XY\0плоскость эффекта\0");
        if (d.align == fx::FxAlign::Velocity) {
            ch |= ImGui::SliderFloat("Растяжение по скорости", &d.stretch, 0, 2, "%.3f");
            ch |= ImGui::SliderFloat("Растяжение по камере", &d.cameraStretch, -2, 2, "%.3f");
        }
        ch |= vec3("Опорная точка (в размерах)", d.pivot, 0.01f, -10, 10);
        ch |= comboE("Сортировка", d.sort, "авто\0нет\0по расстоянию\0старые спереди\0новые спереди\0");
        ch |= ImGui::SliderInt("Порядок (выше — поверх)", &d.sortOrder, -100, 100);
        ch |= ImGui::SliderFloat("Мин. размер на экране", &d.minScreenSize, 0, 1); ch |= ImGui::SliderFloat("Макс. размер на экране", &d.maxScreenSize, 0, 2);
        std::string meshes;
        for (auto& s : d.meshes) meshes += (meshes.empty() ? "" : ", ") + s;
        if (ui::inputString("Меши частиц (через запятую)", meshes, ImGuiInputTextFlags_EnterReturnsTrue)) {
            d.meshes.clear();
            size_t a = 0;
            while (a < meshes.size() && d.meshes.size() < 4) {
                size_t b = meshes.find(',', a);
                std::string s = meshes.substr(a, b == std::string::npos ? std::string::npos : b - a);
                s.erase(0, s.find_first_not_of(' '));
                if (!s.empty()) s.erase(s.find_last_not_of(' ') + 1);
                if (!s.empty()) d.meshes.push_back(s);
                if (b == std::string::npos) break;
                a = b + 1;
            }
            ch = true;
        }
        ui::help("builtin:cube | sphere | capsule | cylinder | plane | quad или путь к модели (FBX / OBJ / glTF). Меш-частицы дороже квадов: на слабых устройствах их число ограничено.");
        ch |= ImGui::Checkbox("Не рисовать (только шлейфы, свет, под-эмиттеры)", &d.hidden);
        ImGui::TreePop();
    }
    ImGui::PopItemWidth();
    return ch;
}

// Effect-level force fields.
inline bool forceFieldsUi(fx::EffectDef& def) {
    bool ch = false;
    if (!ImGui::CollapsingHeader("Силовые поля эффекта")) return false;
    ui::help("Действуют на эмиттеры с включёнными «Внешними силами». Координаты — в пространстве эффекта.");
    int del = -1;
    for (size_t i = 0; i < def.forceFields.size(); ++i) {
        auto& f = def.forceFields[i];
        ImGui::PushID(int(i));
        ImGui::Text("Поле %d", int(i + 1));
        int shape = f.cylinder ? 1 : 0;
        if (combo("Форма", shape, "шар\0цилиндр\0")) { f.cylinder = shape == 1; ch = true; }
        ch |= vec3("Позиция", f.position, 0.01f, -1000, 1000);
        ch |= ImGui::DragFloat("Радиус", &f.radius, 0.05f, 0.01f, 1000); ch |= ImGui::DragFloat("Внутр. радиус", &f.startRadius, 0.05f, 0, 1000);
        if (f.cylinder) ch |= ImGui::DragFloat("Длина", &f.length, 0.05f, 0, 1000);
        ch |= vec3("Постоянная сила", f.direction, 0.05f, -1000, 1000);
        ch |= ImGui::DragFloat("Притяжение", &f.gravity, 0.05f, -1000, 1000); ch |= ImGui::SliderFloat("Фокус", &f.gravityFocus, 0, 1);
        ch |= ImGui::DragFloat("Вихрь, °/с", &f.rotation, 1, -36000, 36000); ch |= ImGui::SliderFloat("Втягивание в вихрь", &f.rotationAttraction, 0, 1);
        ch |= ImGui::DragFloat("Сопротивление##f", &f.drag, 0.01f, 0, 100);
        if (ImGui::SmallButton("убрать поле")) del = int(i);
        ImGui::PopID();
    }
    if (del >= 0) { def.forceFields.erase(def.forceFields.begin() + del); ch = true; }
    if (def.forceFields.size() < 8 && ImGui::SmallButton("+ силовое поле")) { def.forceFields.push_back({}); ch = true; }
    return ch;
}

} // namespace rts::editor::fxui
