// Agent link: the editor serves MCP tool calls forwarded by rtsmcp over localhost, implements the
// editor-only tools (screenshots, camera, play-test, device simulator, windows), and launches the
// game client on a simulated device.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include "Devices.h"
#include "Editor.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x) ((void)0)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "third_party/stb_image_write/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;

namespace rts::editor {

namespace {
std::string base64(const std::vector<uint8_t>& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t v = uint32_t(in[i]) << 16 | (i + 1 < in.size() ? uint32_t(in[i + 1]) << 8 : 0) | (i + 2 < in.size() ? in[i + 2] : 0);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? t[v & 63] : '=';
    }
    return out;
}
void pngWrite(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}
Json reply(const Json& id, Json result) {
    Json r = Json::object();
    r.set("id", id);
    r.set("result", std::move(result));
    return r;
}
} // namespace

void Editor::pollAgentLink() {
    if (!server_.listening()) return;
    for (auto& m : server_.poll()) {
        Json msg;
        try { msg = Json::parse(m.line); } catch (...) { continue; }
        const Json& id = msg["id"];
        if (msg.getStr("method") != "call") { server_.send(m.client, reply(id, Json::parse(R"({"error": "unknown method"})")).dump(-1)); continue; }
        const std::string name = msg["params"].getStr("name");
        const Json& args = msg["params"]["arguments"];
        if (name == "view_screenshot") {
            if (args.has("device")) gameDevice = args.getStr("device");
            if (args.has("x") && args.has("z")) { // jump (not fly): the picture is taken this frame
                sceneCam.target = {float(args["x"].toDouble()), 0, float(args["z"].toDouble())};
                if (args.has("distance")) sceneCam.distance = std::clamp(float(args["distance"].toDouble()), sceneCam.minDistance, sceneCam.maxDistance);
                sceneFlying_ = false;
            }
            shots_.push_back({m.client, id.integer(), args.getStr("window", args.has("device") ? "game" : "all")});
            ws_.log("agent", "Скриншот редактора");
            continue;
        }
        Json result = ws_.call(name, args, "agent");
        server_.send(m.client, reply(id, result).dump(-1));
    }
}

void Editor::deliverShots(gfx::Device& dev) {
    if (shots_.empty() || captureArmed_) return;
    captureArmed_ = true;
    auto pending = shots_;
    shots_.clear();
    const float scale = ImGui::GetIO().DisplayFramebufferScale.x;
    // crop rectangles in pixels, computed now (the windows were laid out this frame)
    struct Crop { int x, y, w, h; };
    std::vector<Crop> crops;
    for (auto& s : pending) {
        Viewport* vp = s.device == "game" ? gameViewport : s.device == "scene" ? sceneViewport : nullptr;
        if (vp && vp->pointsW > 0) crops.push_back({int(vp->originX * scale), int(vp->originY * scale), int(vp->pointsW * scale), int(vp->pointsH * scale)});
        else crops.push_back({0, 0, 0, 0});
    }
    dev.captureNextFrame([this, pending, crops](int w, int h, std::vector<uint8_t> px) {
        captureArmed_ = false;
        for (size_t k = 0; k < pending.size(); ++k) {
            Json r = Json::object();
            if (w <= 0 || px.empty()) {
                r.set("error", Json::string("screenshots are not supported by this graphics backend"));
            } else {
                Crop c = crops[k];
                if (c.w <= 0 || c.h <= 0) c = {0, 0, w, h};
                c.x = std::clamp(c.x, 0, w - 1); c.y = std::clamp(c.y, 0, h - 1);
                c.w = std::min(c.w, w - c.x); c.h = std::min(c.h, h - c.y);
                // downscale large frames so the image stays a reasonable MCP payload
                int step = std::max(1, (std::max(c.w, c.h) + 1599) / 1600);
                int ow = c.w / step, oh = c.h / step;
                std::vector<uint8_t> out(size_t(ow) * size_t(oh) * 3);
                for (int y = 0; y < oh; ++y)
                    for (int x = 0; x < ow; ++x) {
                        const uint8_t* s = &px[(size_t(c.y + y * step) * size_t(w) + size_t(c.x + x * step)) * 4];
                        uint8_t* d = &out[(size_t(y) * size_t(ow) + size_t(x)) * 3];
                        d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                    }
                std::vector<uint8_t> png;
                stbi_write_png_to_func(pngWrite, &png, ow, oh, 3, out.data(), ow * 3);
                r.set("png_base64", Json::string(base64(png)));
                r.set("width", Json::number(int64_t(ow)));
                r.set("height", Json::number(int64_t(oh)));
                std::string where = pending[k].device == "game" ? "окно Game" : pending[k].device == "scene" ? "окно Scene" : "весь редактор";
                r.set("summary", Json::string("Скриншот: " + where));
            }
            server_.send(pending[k].client, reply(Json::number(pending[k].id), r).dump(-1));
        }
    });
}

void Editor::launchOnDevice(const std::string& device, bool landscape) {
    if (doc().path.empty() || doc().dirty) {
        std::string f = doc().path.empty() ? ws_.mapsDir() + "/_editor_test.json" : doc().path;
        saveMap(f);
    }
    std::string exe = opt_.gameExe;
    if (exe.empty()) { status = "Не найден исполняемый файл игры (RTSGame)"; return; }
    std::string cmd = "\"" + exe + "\" --map-file \"" + doc().path + "\" --device " + device + (landscape ? ":landscape" : ":portrait");
#if defined(_WIN32)
    cmd = "start \"\" " + cmd;
#else
    cmd = "RTS_CONTENT=\"" + ws_.project.dir + "\" " + cmd + " >/dev/null 2>&1 &";
#endif
    if (std::system(cmd.c_str()) != 0) status = "Не удалось запустить игру";
    else status = "Игра запущена: " + device;
}

void Editor::registerGuiOps() {
    auto add = [this](const char* name, const char* desc, const char* schema, Workspace::OpFn fn) {
        ws_.registerOp({name, desc, Json::parse(schema), std::move(fn), true});
    };
    auto okSummary = [](const std::string& s) { Json r = Json::object(); r.set("ok", Json::fromBool(true)); r.set("summary", Json::string(s)); return r; };
    add("view_screenshot", "Screenshot of the editor (window: 'all' | 'scene' | 'game'); with 'device' the Game window shows that device first. Returns a PNG image.",
        R"({"properties": {"window": {"type": "string", "enum": ["all", "scene", "game"]}, "device": {"type": "string"}, "x": {"type": "number"}, "z": {"type": "number"}, "distance": {"type": "number"}}})",
        [](const Json&) -> Json { throw std::runtime_error("screenshots are answered asynchronously by the editor link"); });
    add("view_focus", "Move the Scene camera to look at x,z (optional distance).",
        R"({"properties": {"x": {"type": "number"}, "z": {"type": "number"}, "distance": {"type": "number"}}, "required": ["x", "z"]})", [this, okSummary](const Json& p) {
            focusScene(float(p["x"].toDouble()), float(p["z"].toDouble()), float(p["distance"].toDouble(-1)));
            return okSummary("Камера: (" + std::to_string(int(p["x"].toDouble())) + ", " + std::to_string(int(p["z"].toDouble())) + ")");
        });
    add("play_start", "Play-test the open map in the Game window: bots on every side, or the user plays slot 'human'. The user watches live.",
        R"({"properties": {"speed": {"type": "number"}, "human": {"type": "integer"}, "device": {"type": "string"}}})", [this, okSummary](const Json& p) {
            if (p.has("device")) gameDevice = p.getStr("device");
            startPlay(int(p["human"].integer(0)));
            if (!playing()) throw std::runtime_error(status);
            playSpeed = float(p["speed"].toDouble(1));
            return okSummary("Тест запущен");
        });
    add("play_stop", "Stop the play-test.", "{}", [this, okSummary](const Json&) { stopPlay(); return okSummary("Тест остановлен"); });
    add("play_status", "State of the play-test: time, result, per-player units/structures/resources.", "{}", [this](const Json&) {
        Json r = Json::object();
        r.set("playing", Json::fromBool(playing()));
        if (!sim_) return r;
        auto& gs = simModule()->state(*sim_);
        r.set("seconds", Json::number(double(sim_->tick()) / sim_->tickRate(), 1));
        r.set("result", Json::number(int64_t(sim_->result())));
        Json ps = Json::array();
        for (auto& p : sim_->players()) {
            Json pj = Json::object();
            int units = 0, structs = 0;
            sim_->world().each<game::Unit, Owner>([&](rts::Entity, game::Unit& u, Owner& o) { if (o.player == p.slot) (rules().types[u.type].structure ? structs : units)++; });
            pj.set("name", Json::string(p.name)); pj.set("alive", Json::fromBool(p.alive));
            pj.set("units", Json::number(int64_t(units))); pj.set("structures", Json::number(int64_t(structs)));
            Json res = Json::object();
            for (size_t k = 0; k < rules().resources.size(); ++k) res.set(rules().resources[k].id, Json::number(gs.players[p.slot].stock[k].toInt()));
            pj.set("resources", res);
            ps.push(pj);
        }
        r.set("players", ps);
        return r;
    });
    add("device_list", "Device presets of the Game window simulator: id, name, group, size in points, scale, safe-area insets "
        "(portrait and landscape), cut-out, touch. Optional 'filter' matches name/id/group.",
        R"({"properties": {"filter": {"type": "string"}}})", [](const Json& p) {
        auto lower = [](std::string v) { for (auto& c : v) c = char(std::tolower((unsigned char)c)); return v; };
        const std::string f = lower(p.getStr("filter"));
        static const char* cutNames[] = {"none", "notch", "island", "punch-center", "punch-left", "waterdrop"};
        auto insets = [](const Insets4& i) {
            Json j = Json::object();
            j.set("left", Json::number(double(i.left), 0)); j.set("top", Json::number(double(i.top), 0));
            j.set("right", Json::number(double(i.right), 0)); j.set("bottom", Json::number(double(i.bottom), 0));
            return j;
        };
        Json arr = Json::array();
        for (auto& d : kDevices) {
            if (!f.empty() && lower(d.name).find(f) == std::string::npos && lower(d.id).find(f) == std::string::npos && lower(d.group).find(f) == std::string::npos) continue;
            Json j = Json::object();
            j.set("id", Json::string(d.id)); j.set("name", Json::string(d.name)); j.set("group", Json::string(d.group));
            j.set("points", Json::string(std::to_string(d.w) + "x" + std::to_string(d.h)));
            j.set("pixels", Json::string(std::to_string(int(std::lround(d.w * d.scale))) + "x" + std::to_string(int(std::lround(d.h * d.scale)))));
            j.set("scale", Json::number(double(d.scale), 3));
            if (d.w > 0) { j.set("safePortrait", insets(d.portrait)); j.set("safeLandscape", insets(d.landscape)); }
            j.set("cutout", Json::string(cutNames[int(d.cutout)]));
            j.set("touch", Json::fromBool(d.touch));
            j.set("gpuTier", Json::number(int64_t(d.gpuTier)));
            arr.push(j);
        }
        Json r = Json::object();
        r.set("devices", arr);
        return r;
    });
    add("device_set", "Frame the Game window as a device (phone simulator) or 'none'; 'safeArea' shows the safe area overlay.",
        R"({"properties": {"device": {"type": "string"}, "orientation": {"type": "string", "enum": ["portrait", "landscape"]},
            "safeArea": {"type": "boolean"}}})", [this, okSummary](const Json& p) {
            if (p.has("device")) {
                if (!findDevice(p.getStr("device"))) throw std::runtime_error("unknown device (see device_list)");
                gameDevice = p.getStr("device");
            }
            if (p.has("orientation")) gameLandscape = p.getStr("orientation") != "portrait";
            if (p.has("safeArea")) gameSafeArea = p.getBool("safeArea", false);
            if (auto* w = window("game")) w->open = true;
            return okSummary("Устройство: " + gameDevice + (gameSafeArea ? ", safe area видна" : ""));
        });
    add("editor_windows", "List editor windows (id, title, open).", "{}", [this](const Json&) {
        Json arr = Json::array();
        for (auto& w : windows_) {
            Json j = Json::object();
            j.set("id", Json::string(w->id())); j.set("title", Json::string(w->title())); j.set("open", Json::fromBool(w->open));
            arr.push(j);
        }
        Json r = Json::object();
        r.set("windows", arr);
        return r;
    });
    add("editor_window", "Open or close an editor window by id, or 'resetLayout': true for the default layout.",
        R"({"properties": {"id": {"type": "string"}, "open": {"type": "boolean"}, "resetLayout": {"type": "boolean"}}})", [this, okSummary](const Json& p) {
            if (p.getBool("resetLayout", false)) { for (auto& w : windows_) w->open = w->openByDefault(); resetLayout_ = true; return okSummary("Раскладка сброшена"); }
            EditorWindow* w = window(p.getStr("id"));
            if (!w) throw std::runtime_error("unknown window (see editor_windows)");
            w->open = p.getBool("open", true);
            return okSummary(std::string(w->open ? "Открыто окно " : "Закрыто окно ") + w->title());
        });
    add("editor_theme", "Editor look: 'mode' dark | light, 'accent' blue | violet | teal | orange (saved with the layout).",
        R"({"properties": {"mode": {"type": "string", "enum": ["dark", "light"]}, "accent": {"type": "string", "enum": ["blue", "violet", "teal", "orange"]}}})",
        [this, okSummary](const Json& p) {
            EditorTheme::Mode m = theme_.mode();
            if (p.has("mode")) m = p.getStr("mode") == "light" ? EditorTheme::Mode::Light : EditorTheme::Mode::Dark;
            int a = theme_.accentIndex();
            for (int i = 0; i < EditorTheme::kAccentCount; ++i) if (p.getStr("accent") == EditorTheme::accent(i).id) a = i;
            theme_.set(m, a);
            return okSummary(std::string("Тема: ") + (m == EditorTheme::Mode::Light ? "светлая" : "тёмная") + ", акцент " + EditorTheme::accent(a).name);
        });
    add("editor_select", "Select something in the editor (Inspector shows it): entity index, player (1-based), area/region/type/resource/tech/faction key.",
        R"({"properties": {"kind": {"type": "string", "enum": ["entity", "player", "area", "region", "type", "resource", "tech", "faction", "map", "none"]}, "index": {"type": "integer"}, "key": {"type": "string"}}, "required": ["kind"]})",
        [this, okSummary](const Json& p) {
            std::string k = p.getStr("kind");
            Selection s;
            s.index = int(p["index"].integer(-1));
            s.key = p.getStr("key");
            s.kind = k == "entity" ? Selection::Entity : k == "player" ? Selection::Player : k == "area" ? Selection::Area : k == "region" ? Selection::Region
                   : k == "type" ? Selection::Type : k == "resource" ? Selection::Resource : k == "tech" ? Selection::Tech : k == "faction" ? Selection::Faction
                   : k == "map" ? Selection::MapProps : Selection::None;
            if (s.kind == Selection::Player) s.index -= 1;
            select(s);
            if (s.kind == Selection::Entity && s.index >= 0 && s.index < int(doc().data.entities.size())) focusScene(doc().data.entities[size_t(s.index)].x, doc().data.entities[size_t(s.index)].z, 18);
            return okSummary("Выбрано: " + k);
        });
}

} // namespace rts::editor
