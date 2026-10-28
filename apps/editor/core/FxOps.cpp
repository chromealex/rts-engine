// Particle effect operations for agents (see FxOps.h). Paths are confined to the open game's fx/
// folder; the Unity importer only reads the given source and writes into the game.
#include "FxOps.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <stdexcept>
#include "engine/fx/ParticleSystem.h"
#include "modules/rts/fx/RtsFx.h"

namespace fs = std::filesystem;

namespace rts::editor {

namespace {

Json str(const std::string& s) { return Json::string(s); }
Json strings(const std::vector<std::string>& v) { Json a = Json::array(); for (auto& s : v) a.push(str(s)); return a; }

void needGame(const Workspace& ws) { if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)"); }
fs::path fxDir(const Workspace& ws) { return fs::path(ws.project.dir) / "fx"; }
fs::path fxFile(const Workspace& ws, const std::string& n) { return fxDir(ws) / (n + ".json"); }

bool readText(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
void writeText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!(f << text)) throw std::runtime_error("cannot write " + p.filename().string());
}

std::string needName(const Json& p, const char* key = "name") {
    const std::string n = p.getStr(key);
    if (!validFxName(n)) throw std::runtime_error(std::string("'") + key + "' must be an effect name: a-z, 0-9, _ (got '" + n + "')");
    return n;
}

std::vector<std::string> effectNames(const Workspace& ws) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(fxDir(ws), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".json" && it->path().stem() != "fx") out.push_back(it->path().stem().string());
    std::sort(out.begin(), out.end());
    return out;
}

Json loadBank(const Workspace& ws, std::string* text = nullptr) {
    std::string t;
    if (!readText(fxFile(ws, "fx"), t)) { if (text) text->clear(); return Json::object(); }
    if (text) *text = t;
    Json j = Json::parse(t);
    return j.isObject() ? j : Json::object();
}

// Writes fx.json keeping the comment lines above the first '{' (the file's explanation for humans).
void saveBank(const Workspace& ws, const Json& bank, const std::string& oldText) {
    std::string head;
    size_t brace = oldText.find('{');
    if (brace != std::string::npos) {
        std::string pre = oldText.substr(0, brace);
        if (pre.find("//") != std::string::npos) head = pre;
    }
    writeText(fxFile(ws, "fx"), head + bank.dump(2) + "\n");
}

// Headless particle system reading the game's files: what the game would load.
struct Loader {
    fx::ParticleSystem ps;
    explicit Loader(const Workspace& ws) {
        ps.init(nullptr);
        const std::string dir = ws.project.dir;
        ps.setFileReader([dir](const std::string& path, std::string& out) { return readText(fs::path(dir) / path, out); });
    }
};

std::vector<std::string> bindingWarnings(const Workspace& ws, const Json& bank) {
    Loader l(ws);
    game::RtsFx bridge;
    std::string err;
    std::vector<std::string> w;
    if (!bridge.init(l.ps, ws.project.rules, bank, &err)) w.push_back(err);
    for (auto& x : bridge.warnings()) w.push_back(x);
    return w;
}

// Every place fx.json uses an effect: "events.died.unit", "types.tank.weapons.cannon.hit", ...
void bindingUses(const Json& bank, std::vector<std::pair<std::string, std::string>>& out) {
    auto eff = [](const Json& v) { return v.isString() ? v.str() : v.isObject() ? v.getStr("effect") : std::string(); };
    auto add = [&](const std::string& where, const Json& v) { std::string e = eff(v); if (!e.empty()) out.push_back({where, e}); };
    for (auto& [k, v] : bank["events"].members()) add("events." + k, v);
    for (auto& [t, tj] : bank["types"].members())
        for (auto& [k, v] : tj.members()) {
            if (k == "weapons") { for (auto& [w, wj] : v.members()) for (auto& [c, x] : wj.members()) add("types." + t + ".weapons." + w + "." + c, x); }
            else if (k == "abilities") { for (auto& [a, x] : v.members()) add("types." + t + ".abilities." + a, x); }
            else add("types." + t + "." + k, v);
        }
    for (auto& [w, wj] : bank["weapons"].members()) for (auto& [c, x] : wj.members()) add("weapons." + w + "." + c, x);
    for (auto& [a, x] : bank["abilities"].members()) add("abilities." + a, x);
}

void noteFx(Workspace& ws, const std::string& text) { ws.note({Change::Fx, 0, 0, -1, -1, -1, text}); }

Json result(const FxCheck& c, const std::string& summary) {
    Json r = Json::object();
    r.set("ok", Json::fromBool(c.errors.empty()));
    if (!c.errors.empty()) r.set("errors", strings(c.errors));
    r.set("warnings", strings(c.warnings));
    if (!summary.empty()) r.set("summary", str(summary));
    return r;
}

// "#rrggbb" multiply + uniform scale of everything with a length in it (templates).
void tintAndScale(fx::EffectDef& d, fx::float4 tint, float k) {
    auto mulc = [&](std::vector<fx::FxColorKey>& keys) { for (auto& c : keys) { c.rgba.x *= tint.x; c.rgba.y *= tint.y; c.rgba.z *= tint.z; } };
    for (auto& e : d.emitters) {
        mulc(e.color);
        mulc(e.trail.color);
        if (e.light.enabled) { e.light.color.x *= tint.x; e.light.color.y *= tint.y; e.light.color.z *= tint.z; e.light.radius *= k; }
        e.size = {e.size.min * k, e.size.max * k};
        e.speed = {e.speed.min * k, e.speed.max * k};
        e.radius *= k; e.gravity = e.gravity * k; e.offset = e.offset * k; e.box = e.box * k; e.velocity = e.velocity * k;
        e.noise.strength *= k; e.noise.frequency /= k; e.speedLimit *= k; e.collision.height *= k;
        if (e.perMeter > 0) e.perMeter /= k;
        e.colorSpeed = {e.colorSpeed.min * k, e.colorSpeed.max * k};
        e.sizeSpeed = {e.sizeSpeed.min * k, e.sizeSpeed.max * k};
    }
}

std::string shellQuote(const std::string& s) {
#if defined(_WIN32)
    return "\"" + s + "\"";
#else
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
#endif
}

std::string runCommand(const std::string& cmd, int& rc) {
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) { rc = -1; return {}; }
    std::string out;
    char buf[4096];
    while (size_t n = std::fread(buf, 1, sizeof buf, pipe)) out.append(buf, n);
#if defined(_WIN32)
    rc = _pclose(pipe);
#else
    rc = pclose(pipe);
#endif
    return out;
}

std::string pythonExe() {
    if (const char* p = std::getenv("RTS_PYTHON")) return p;
#if defined(_WIN32)
    return "python";
#else
    return "python3";
#endif
}

const char* kCueAliases[][2] = {{"spawn", "spawned"}, {"death", "died"}, {"die", "died"}, {"level_up", "levelUp"}, {"levelup", "levelUp"},
                                {"build_done", "buildDone"}, {"build_placed", "buildPlaced"}, {"region_claimed", "regionClaimed"}};

std::string cueName(std::string c) {
    for (auto& a : kCueAliases) if (c == a[0]) c = a[1];
    for (size_t i = 0; i < size_t(game::FxCue::Count); ++i) if (c == game::fxCueName(game::FxCue(i))) return c;
    std::string all;
    for (size_t i = 0; i < size_t(game::FxCue::Count); ++i) all += std::string(i ? ", " : "") + game::fxCueName(game::FxCue(i));
    throw std::runtime_error("unknown cue '" + c + "' (" + all + ")");
}

} // namespace

bool validFxName(const std::string& n) {
    if (n.empty() || n.size() > 48) return false;
    for (char c : n) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

FxCheck checkFx(const Workspace& ws, const std::string& name, const Json& j) {
    FxCheck c;
    if (name == "fx") {
        if (!j.isObject()) { c.errors.push_back("fx/fx.json: expected an object { \"events\": ..., \"types\": ..., \"weapons\": ..., \"abilities\": ... }"); return c; }
        c.warnings = bindingWarnings(ws, j);
        return c;
    }
    fx::EffectDef d;
    fx::parseEffect(j, "fx/" + name + ".json", d, c.errors);
    c.emitters = int(d.emitters.size());
    std::set<std::string> seenTex;
    int burst = 0;
    for (size_t i = 0; i < d.emitters.size(); ++i) {
        const auto& e = d.emitters[i];
        const std::string where = "emitters[" + std::to_string(i) + "]";
        for (const std::string* t : {&e.texture, e.trail.enabled ? &e.trail.texture : nullptr}) {
            if (!t || t->rfind("builtin:", 0) == 0 || !seenTex.insert(*t).second) continue;
            std::error_code ec;
            if (!fs::exists(fs::path(ws.project.dir) / *t, ec)) c.warnings.push_back(where + ": texture '" + *t + "' is not in the game folder (fine only if an atlas provides it)");
        }
        for (auto& s : e.subEmitters)
            if (s.effect != name && !fs::exists(fxFile(ws, s.effect))) c.warnings.push_back(where + ": sub-emitter effect '" + s.effect + "' does not exist (fx/" + s.effect + ".json)");
        burst += e.burst;
        for (auto& b : e.bursts) burst += int(b.count.max) * b.cycles;
        if (e.size.max * (e.sizeOverLife.empty() ? 1.0f : std::max_element(e.sizeOverLife.begin(), e.sizeOverLife.end(), [](auto& a, auto& b) { return a.k < b.k; })->k) > 12)
            c.warnings.push_back(where + ": particles grow beyond 12 m: heavy fill rate on phones");
        if (e.trail.enabled && e.trail.minQuality == fx::FxQuality::Low) c.warnings.push_back(where + ": trail on the low tier: ribbons cost fill rate on weak GPUs");
    }
    if (burst > 600) c.warnings.push_back("bursts total " + std::to_string(burst) + " particles: the low tier keeps 600 for the whole screen");
    return c;
}

void registerFxOps(Workspace& ws) {
    auto add = [&ws](const char* name, const std::string& desc, const char* schema, Workspace::OpFn fn) {
        ws.registerOp({name, desc, Json::parse(schema), std::move(fn)});
    };

    add("fx_list", "Particle effects of the open game (fx/*.json): emitters, looping, lifetime, textures, load errors; where each is "
        "bound in fx/fx.json, and the bindings' warnings.", R"({"properties": {}})", [&ws](const Json&) {
        needGame(ws);
        std::string bankText;
        Json bank;
        std::vector<std::string> warn;
        try { bank = loadBank(ws, &bankText); } catch (const std::exception& e) { warn.push_back(std::string("fx/fx.json: ") + e.what()); bank = Json::object(); }
        std::vector<std::pair<std::string, std::string>> uses;
        bindingUses(bank, uses);
        Json list = Json::array();
        Loader l(ws);
        for (auto& n : effectNames(ws)) {
            Json e = Json::object();
            e.set("name", str(n));
            l.ps.clearErrors();
            int id = l.ps.effect(n);
            if (const fx::EffectDef* d = l.ps.def(id)) {
                e.set("emitters", Json::number(int64_t(d->emitters.size())));
                e.set("looping", Json::fromBool(d->looping()));
                if (!d->looping()) e.set("lifetime", Json::number(double(d->lifetime()), 2));
                std::set<std::string> tex;
                for (auto& em : d->emitters) { tex.insert(em.texture); if (em.trail.enabled) tex.insert(em.trail.texture); }
                e.set("textures", strings(std::vector<std::string>(tex.begin(), tex.end())));
            } else e.set("errors", strings(l.ps.errors()));
            std::vector<std::string> where;
            for (auto& [w, eff] : uses) if (eff == n) where.push_back(w);
            e.set("boundAt", strings(where));
            list.push(e);
        }
        for (auto& w : bindingWarnings(ws, bank)) warn.push_back(w);
        Json r = Json::object();
        r.set("effects", list);
        r.set("warnings", strings(warn));
        r.set("summary", str("Эффектов: " + std::to_string(list.size())));
        return r;
    });

    add("fx_get", "Read an effect (name without .json; 'fx' = the event bindings): raw text and the parsed JSON. Format: engine/fx/FxEffect.h "
        "(top comment lists every key incl. trails, sub-emitters, noise, lights), bindings: modules/rts/fx/RtsFx.h.",
        R"({"properties": {"name": {"type": "string"}}, "required": ["name"]})", [&ws](const Json& p) {
        needGame(ws);
        const std::string n = needName(p);
        std::string text;
        if (!readText(fxFile(ws, n), text)) throw std::runtime_error("fx/" + n + ".json not found (fx_list shows the effects, fx_templates gives starting points)");
        Json r = Json::object();
        r.set("name", str(n));
        r.set("text", str(text));
        try { r.set("json", Json::parse(text)); } catch (const std::exception& e) { r.set("parseError", str(e.what())); }
        return r;
    });

    add("fx_set", "Create or replace an effect (name without .json; 'fx' = the event bindings) from 'json' (object) or 'text'. Validated "
        "with the engine's loader first: unknown keys, out-of-range values and missing particle sources are errors with the exact path "
        "(emitters[1].trail.time: must be in 0.01..10); missing textures / sub-effects are warnings. Then fx_preview to look at it.",
        R"({"properties": {"name": {"type": "string"}, "json": {"type": "object"}, "text": {"type": "string"}}, "required": ["name"]})",
        [&ws](const Json& p) {
        needGame(ws);
        const std::string n = needName(p);
        std::string text;
        Json j;
        if (p.has("json")) { j = p["json"]; text = j.dump(2) + "\n"; }
        else if (p.has("text")) {
            text = p.getStr("text");
            try { j = Json::parse(text); }
            catch (const std::exception& e) { throw std::runtime_error(std::string("invalid JSON, not saved: ") + e.what()); }
        } else throw std::runtime_error("give 'json' (an object) or 'text'");
        FxCheck c = checkFx(ws, n, j);
        if (!c.errors.empty()) { Json r = result(c, ""); r.set("summary", str("Не сохранено: " + std::to_string(c.errors.size()) + " ошибок")); return r; }
        const bool existed = fs::exists(fxFile(ws, n));
        writeText(fxFile(ws, n), text);
        noteFx(ws, (existed ? "Изменён эффект " : "Создан эффект ") + n);
        return result(c, (existed ? "Эффект изменён: fx/" : "Эффект создан: fx/") + n + ".json");
    });

    add("fx_delete", "Delete an effect file. Refuses while fx.json or another effect's subEmitters use it, unless 'unbind': true (removes those "
        "bindings; sub-emitter references are only reported).",
        R"({"properties": {"name": {"type": "string"}, "unbind": {"type": "boolean"}}, "required": ["name"]})", [&ws](const Json& p) {
        needGame(ws);
        const std::string n = needName(p);
        if (n == "fx") throw std::runtime_error("fx.json holds the bindings: edit it with fx_bind / fx_set instead");
        if (!fs::exists(fxFile(ws, n))) throw std::runtime_error("fx/" + n + ".json not found");
        std::string bankText;
        Json bank = loadBank(ws, &bankText);
        std::vector<std::pair<std::string, std::string>> uses;
        bindingUses(bank, uses);
        std::vector<std::string> where, subs;
        for (auto& [w, e] : uses) if (e == n) where.push_back(w);
        for (auto& other : effectNames(ws)) {
            if (other == n) continue;
            std::string t;
            if (readText(fxFile(ws, other), t) && t.find("\"" + n + "\"") != std::string::npos) subs.push_back(other);
        }
        if (!where.empty() && !p.getBool("unbind")) {
            std::string all;
            for (auto& w : where) all += (all.empty() ? "" : ", ") + w;
            throw std::runtime_error("'" + n + "' is bound at " + all + ": pass unbind: true to remove those bindings too");
        }
        int removed = 0;
        if (!where.empty()) {   // drop every binding whose effect is n
            std::function<void(Json&)> strip = [&](Json& o) {
                auto& m = o.membersMut();
                for (size_t i = 0; i < m.size();) {
                    Json& v = m[i].second;
                    std::string e = v.isString() ? v.str() : v.isObject() ? v.getStr("effect") : "";
                    if (e == n) { m.erase(m.begin() + long(i)); ++removed; continue; }
                    if (v.isObject() && !v.has("effect")) strip(v);
                    ++i;
                }
            };
            strip(bank);
            saveBank(ws, bank, bankText);
        }
        std::error_code ec;
        fs::remove(fxFile(ws, n), ec);
        if (ec) throw std::runtime_error("cannot delete fx/" + n + ".json: " + ec.message());
        noteFx(ws, "Удалён эффект " + n);
        Json r = Json::object();
        r.set("ok", Json::fromBool(true));
        r.set("unbound", Json::number(int64_t(removed)));
        if (!subs.empty()) r.set("warnings", strings({"still referenced as a sub-emitter by: " + [&] { std::string s; for (auto& x : subs) s += (s.empty() ? "" : ", ") + x; return s; }()}));
        r.set("summary", str("Эффект удалён: " + n));
        return r;
    });

    {
        size_t count = 0;
        const FxTemplate* t = fxTemplates(count);
        std::string ids;
        for (size_t i = 0; i < count; ++i) ids += (i ? ", " : "") + std::string(t[i].id);
        add("fx_templates", "Ready effect recipes (built-in textures only, quality tiers set): " + ids + ". Without 'template': the list with "
            "descriptions. With 'template' and 'name': writes fx/<name>.json; optional 'color' (\"#rrggbb\", multiplies the colours), "
            "'scale' (sizes, speeds, distances), 'overwrite'. Then tune with fx_set and look with fx_preview.",
            R"({"properties": {"template": {"type": "string"}, "name": {"type": "string"}, "color": {"type": "string"},
                "scale": {"type": "number"}, "overwrite": {"type": "boolean"}}})", [&ws](const Json& p) {
            size_t count = 0;
            const FxTemplate* t = fxTemplates(count);
            if (!p.has("template")) {
                Json list = Json::array();
                for (size_t i = 0; i < count; ++i) {
                    Json e = Json::object();
                    e.set("template", str(t[i].id));
                    e.set("description", str(t[i].description));
                    list.push(e);
                }
                Json r = Json::object();
                r.set("templates", list);
                return r;
            }
            needGame(ws);
            const std::string id = p.getStr("template");
            const FxTemplate* tp = nullptr;
            for (size_t i = 0; i < count; ++i) if (id == t[i].id) tp = &t[i];
            if (!tp) throw std::runtime_error("unknown template '" + id + "' (fx_templates without arguments lists them)");
            const std::string n = p.has("name") ? needName(p) : id;
            if (fs::exists(fxFile(ws, n)) && !p.getBool("overwrite")) throw std::runtime_error("fx/" + n + ".json exists: pass overwrite: true or another name");
            std::string text = tp->json;
            const float k = float(p["scale"].toDouble(1));
            if (!(k > 0.01f && k < 100)) throw std::runtime_error("scale must be in 0.01..100");
            fx::float4 tint{1, 1, 1, 1};
            if (p.has("color") && !fx::parseColor(p["color"], tint)) throw std::runtime_error("color must be \"#rrggbb\"");
            if (p.has("color") || k != 1) {
                fx::EffectDef d;
                std::vector<std::string> errs;
                fx::parseEffect(Json::parse(text), "template", d, errs);
                tintAndScale(d, tint, k);
                Json out = fx::effectToJson(d);
                out.set("comment", str(std::string("From template '") + tp->id + "': " + tp->description));
                text = out.dump(2) + "\n";
            } else text = "// From template '" + std::string(tp->id) + "': " + tp->description + "\n" + text + "\n";
            FxCheck c = checkFx(ws, n, Json::parse(text));
            if (!c.errors.empty()) return result(c, "");
            writeText(fxFile(ws, n), text);
            noteFx(ws, "Эффект из шаблона " + id + ": " + n);
            Json r = result(c, "Эффект " + n + " создан из шаблона " + id);
            r.set("name", str(n));
            r.set("emitters", Json::number(int64_t(c.emitters)));
            return r;
        });
    }

    add("fx_import_unity", "Make the same effect as a Unity one: 'source' = a .prefab, a folder (Unity package folder / Assets subfolder with "
        "prefabs, .mat, textures and .meta files) or a .unitypackage. Converts every ParticleSystem (child systems = layers, sub-emitters = "
        "extra effects) into fx/<name>.json and copies textures to fx/textures. Returns the importer's report: per system what was "
        "mapped, approximated or dropped (VFX Graph, custom shaders, meshes, scripts). Optional: 'prefab' (filter), 'scale' (Unity metres -> "
        "game units), 'ignoreRootRotation', 'dryRun'. Then fx_preview and fx_bind.",
        R"({"properties": {"source": {"type": "string"}, "name": {"type": "string"}, "prefab": {"type": "string"}, "scale": {"type": "number"},
            "ignoreRootRotation": {"type": "boolean"}, "dryRun": {"type": "boolean"}}, "required": ["source"]})", [&ws](const Json& p) {
        needGame(ws);
        std::string src = p.getStr("source");
        if (src.empty()) throw std::runtime_error("'source' is required: path to a .prefab, a folder or a .unitypackage");
        if (src.rfind("~/", 0) == 0) if (const char* h = std::getenv("HOME")) src = std::string(h) + src.substr(1);
        std::error_code ec;
        if (!fs::exists(src, ec)) throw std::runtime_error("source not found: " + src);
        const fs::path script = fs::path(ws.engineDir) / "tools" / "importers" / "unity_vfx" / "import.py";
        if (!fs::exists(script, ec)) throw std::runtime_error("importer not found: " + script.string());
        std::string cmd = shellQuote(pythonExe()) + " " + shellQuote(script.string()) + " --source " + shellQuote(src) + " --game " +
                          shellQuote(ws.project.dir) + " --json";
        if (p.has("name")) cmd += " --name " + shellQuote(needName(p));
        if (p.has("prefab")) cmd += " --prefab " + shellQuote(p.getStr("prefab"));
        if (p.has("scale")) {
            double s = p["scale"].toDouble(1);
            if (!(s > 0)) throw std::runtime_error("scale must be > 0");
            char b[32]; std::snprintf(b, sizeof b, "%g", s);
            cmd += std::string(" --scale ") + b;
        }
        if (p.getBool("ignoreRootRotation")) cmd += " --ignore-root-rotation";
        if (p.getBool("dryRun")) cmd += " --dry-run";
        int rc = 0;
        std::string out = runCommand(cmd + " 2>&1", rc);
        size_t brace = out.rfind('{', 0) == 0 ? 0 : out.find("\n{");
        if (brace != std::string::npos && brace > 0) ++brace;
        if (brace == std::string::npos) throw std::runtime_error("the importer failed (is Python 3 installed? set RTS_PYTHON):\n" + out.substr(0, 3000));
        Json rep;
        try { rep = Json::parse(out.substr(brace)); }
        catch (const std::exception&) { throw std::runtime_error("the importer failed:\n" + out.substr(0, 3000)); }
        // the engine's view of what was written: same loader as the game
        Json checks = Json::array();
        int bad = 0;
        if (!p.getBool("dryRun"))
            for (auto& e : rep["effects"].items()) {
                const std::string n = e.getStr("name");
                std::string text;
                Json c = Json::object();
                c.set("name", str(n));
                if (!readText(fxFile(ws, n), text)) { c.set("errors", strings({"not written"})); ++bad; checks.push(c); continue; }
                FxCheck fc = checkFx(ws, n, Json::parse(text));
                if (!fc.errors.empty()) { c.set("errors", strings(fc.errors)); ++bad; }
                if (!fc.warnings.empty()) c.set("warnings", strings(fc.warnings));
                checks.push(c);
            }
        rep.set("engineCheck", checks);
        const size_t made = rep["effects"].size();
        if (made && !p.getBool("dryRun")) noteFx(ws, "Импорт из Unity: " + std::to_string(made) + " эффект(ов)");
        rep.set("summary", str(made ? "Импортировано эффектов: " + std::to_string(made) + (bad ? " (с ошибками: " + std::to_string(bad) + ")" : "")
                                    : "Ничего не импортировано"));
        if (!made && rep["warnings"].size()) throw std::runtime_error(rep["warnings"][size_t(0)].str());
        return rep;
    });

    add("fx_bind", "Attach an effect to game events in fx/fx.json (or detach with effect: null; remove: true deletes the entry so fallbacks "
        "apply). cue: attack, hit, miss, died, spawned, ready, buildPlaced, buildDone, gathered, captured, levelUp, ability, upgraded, sold, "
        "regionClaimed, wave, trail (projectile), damaged (loop while hp < below). Scope (most specific wins in the game): type + weapon / "
        "ability, type, weapon (any type), ability (any type), category (structure | air | unit), or all. Options: height, scale, "
        "teamColor, below. Returns the binding warnings of the whole file.",
        R"({"properties": {"effect": {"type": ["string", "null"]}, "cue": {"type": "string"}, "type": {"type": "string"},
            "weapon": {"type": "string"}, "ability": {"type": "string"}, "category": {"type": "string", "enum": ["structure", "air", "unit"]},
            "height": {"type": "number"}, "scale": {"type": "number"}, "teamColor": {"type": "boolean"}, "below": {"type": "number"},
            "remove": {"type": "boolean"}}})", [&ws](const Json& p) {
        needGame(ws);
        const auto& R = ws.project.rules;
        const bool remove = p.getBool("remove");
        const bool none = !remove && (!p.has("effect") || p["effect"].isNull());
        std::string eff;
        if (!remove && !none) {
            eff = needName(p, "effect");
            if (!fs::exists(fxFile(ws, eff))) throw std::runtime_error("fx/" + eff + ".json does not exist (fx_set / fx_templates / fx_import_unity first)");
        }
        const std::string ability = p.getStr("ability"), weapon = p.getStr("weapon"), type = p.getStr("type"), cat = p.getStr("category");
        std::string cue = p.has("cue") ? cueName(p.getStr("cue")) : ability.empty() ? "" : "ability";
        if (cue.empty()) throw std::runtime_error("'cue' is required (died, hit, attack, ability, spawned, ...)");
        if (!weapon.empty() && cue != "attack" && cue != "hit" && cue != "trail") throw std::runtime_error("weapon bindings take cue attack, hit or trail");
        if (!ability.empty() && cue != "ability") throw std::runtime_error("ability bindings take cue 'ability'");
        int t = -1;
        if (!type.empty() && (t = R.findType(type)) < 0) throw std::runtime_error("unknown type '" + type + "' (rules_list)");
        auto hasWeapon = [&](const game::UnitType& ut) { return std::any_of(ut.weapons.begin(), ut.weapons.end(), [&](auto& w) { return w.id == weapon; }); };
        auto hasAbility = [&](const game::UnitType& ut) { return std::any_of(ut.abilities.begin(), ut.abilities.end(), [&](auto& a) { return a.id == ability; }); };
        if (!weapon.empty() && (t >= 0 ? !hasWeapon(R.types[size_t(t)]) : std::none_of(R.types.begin(), R.types.end(), hasWeapon)))
            throw std::runtime_error("no " + std::string(t >= 0 ? "weapon '" + weapon + "' on type '" + type + "'" : "type has a weapon '" + weapon + "'"));
        if (!ability.empty() && (t >= 0 ? !hasAbility(R.types[size_t(t)]) : std::none_of(R.types.begin(), R.types.end(), hasAbility)))
            throw std::runtime_error("no " + std::string(t >= 0 ? "ability '" + ability + "' on type '" + type + "'" : "type has an ability '" + ability + "'"));
        if (!cat.empty() && (t >= 0 || !weapon.empty() || !ability.empty())) throw std::runtime_error("'category' only applies to events for all types");

        Json value;
        if (none) value = Json();
        else if (!remove) {
            const bool obj = p.has("height") || p.has("scale") || p.has("teamColor") || p.has("below");
            if (!obj) value = str(eff);
            else {
                value = Json::object();
                value.set("effect", str(eff));
                if (p.has("height")) value.set("height", Json::number(p["height"].toDouble(), 3));
                if (p.has("scale")) value.set("scale", Json::number(p["scale"].toDouble(1), 3));
                if (p.has("teamColor")) value.set("teamColor", Json::fromBool(p.getBool("teamColor")));
                if (p.has("below")) value.set("below", Json::number(p["below"].toDouble(0.5), 3));
            }
        }
        std::string bankText;
        Json bank = loadBank(ws, &bankText);
        std::vector<std::string> path;
        if (t >= 0) {
            path = {"types", type};
            if (!weapon.empty()) path.insert(path.end(), {"weapons", weapon, cue});
            else if (!ability.empty()) path.insert(path.end(), {"abilities", ability});
            else path.push_back(cue);
        } else if (!weapon.empty()) path = {"weapons", weapon, cue};
        else if (!ability.empty()) path = {"abilities", ability};
        else path = {"events", cat.empty() ? cue : cue + "." + cat};
        Json* node = &bank;
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            Json* next = node->find(path[i]);
            if (!next || !next->isObject()) { node->set(path[i], Json::object()); next = node->find(path[i]); }
            node = next;
        }
        std::string where;
        for (auto& s : path) where += (where.empty() ? "" : ".") + s;
        if (remove) { if (!node->erase(path.back())) throw std::runtime_error(where + " is not bound"); }
        else node->set(path.back(), value);
        std::vector<std::string> warn = bindingWarnings(ws, bank);
        saveBank(ws, bank, bankText);
        noteFx(ws, remove ? "Привязка снята: " + where : none ? "Без эффекта: " + where : "Привязка: " + where + " → " + eff);
        Json r = Json::object();
        r.set("ok", Json::fromBool(true));
        r.set("binding", str(where));
        r.set("warnings", strings(warn));
        r.set("summary", str(remove ? "Привязка снята: " + where : where + " → " + (none ? std::string("нет эффекта") : eff)));
        return r;
    });
}

} // namespace rts::editor
