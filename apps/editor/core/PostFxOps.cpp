#include "PostFxOps.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include "engine/render/PostFxData.h"

namespace rts::editor {

using namespace rts::render;
namespace fs = std::filesystem;

namespace {
Json okJson() { Json r = Json::object(); r.set("ok", Json::fromBool(true)); return r; }
std::string scopeOf(const Json& p) {
    const std::string s = p.getStr("scope", "game");
    if (s != "game" && s != "map") throw std::runtime_error("scope must be 'game' (game.json, every map) or 'map' (the open map only)");
    return s;
}
const char* kScope = R"J("scope": {"type": "string", "enum": ["game", "map"], "description": "game = game.json (all maps, default); map = the open map only (overrides game.json per effect type)"})J";

Json effectsArray(const Json& block) { return block.isArray() ? block : block["effects"].isArray() ? block["effects"] : Json::array(); }
Json withEffects(const Json& block, Json effects) {
    Json b = block.isObject() ? block : Json::object();
    b.set("effects", std::move(effects));
    return b;
}
int indexOfType(const Json& effects, const std::string& type) {
    for (size_t i = 0; i < effects.size(); ++i) if (effects[i].getStr("type") == type) return int(i);
    return -1;
}

// Resolved chain (game + open map) with its cost and the parser's notes.
Json chainReport(Workspace& ws) {
    std::vector<std::string> issues;
    const PostFxSettings s = parsePostFx(ws.project.manifest["postfx"], ws.map.data.extra["postfx"], &issues);
    Json r = postFxCostReport(s);
    if (!issues.empty()) {
        Json a = Json::array();
        for (auto& i : issues) a.push(Json::string(i));
        r.set("issues", a);
    }
    r.set("enabled", Json::fromBool(s.enabled));
    return r;
}

// Effect params from the call: either "params": {...} or the keys at the top level.
Json effectFromParams(const Json& p, const Json* base) {
    Json e = base ? *base : Json::object();
    e.set("type", Json::string(p.getStr("type")));
    auto take = [&e](const Json& src) {
        for (auto& [k, v] : src.members())
            if (k != "type" && k != "scope" && k != "params") e.set(k, v);
    };
    take(p);
    if (p["params"].isObject()) take(p["params"]);
    return e;
}

std::string ruCost(PostFxCostClass c) { return c == PostFxCostClass::Cheap ? "лёгкий" : c == PostFxCostClass::Medium ? "средний" : "ТЯЖЁЛЫЙ"; }

// Answer of add / set / preset: the effect as stored, warnings for the touched effects (heavy ones) and for
// the whole chain (over the frame budget), the chain cost, a summary for the activity feed.
Json changeAnswer(Workspace& ws, const std::vector<PostFxEffect>& touched, const std::string& what) {
    Json r = okJson();
    Json warnings = Json::array();
    bool heavy = false;
    if (touched.size() == 1) {
        r.set("effect", postFxEffectJson(touched[0]));
        r.set("class", Json::string(postFxCostName(postFxType(touched[0].type).cost)));
    }
    for (auto& e : touched) {
        if (postFxType(e.type).cost == PostFxCostClass::Heavy && e.enabled) heavy = true;
        if (e.enabled) for (auto& w : postFxEffectWarnings(e)) warnings.push(Json::string(w));
    }
    Json cost = chainReport(ws);
    for (auto& w : cost["warnings"].items())
        if (w.str().rfind("HEAVY", 0) != 0) warnings.push(w);   // budget of the whole chain
    r.set("heavy", Json::fromBool(heavy));
    r.set("warnings", warnings);
    r.set("cost", cost["total"]);
    std::string summary = what;
    if (touched.size() == 1) summary += std::string(" (") + ruCost(postFxType(touched[0].type).cost) + ")";
    if (warnings.size() > 0) {
        summary += " — предупреждений: " + std::to_string(warnings.size());
        for (auto& w : warnings.items()) ws.log("postfx", "⚠ " + w.str());
    }
    r.set("summary", Json::string(summary));
    return r;
}

// PNG: zlib with stored blocks, CRC32, Adler-32.
uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t k = i;
            for (int j = 0; j < 8; ++j) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            table[i] = k;
        }
        init = true;
    }
    c = ~c;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xff] ^ (c >> 8);
    return ~c;
}
void be32(std::string& s, uint32_t v) { for (int i = 3; i >= 0; --i) s.push_back(char((v >> (i * 8)) & 0xff)); }
void chunk(std::string& out, const char* type, const std::string& data) {
    be32(out, uint32_t(data.size()));
    std::string td = std::string(type, 4) + data;
    out += td;
    be32(out, crc32(reinterpret_cast<const uint8_t*>(td.data()), td.size()));
}
} // namespace

std::string encodePngRgba(int w, int h, const std::vector<uint8_t>& rgba) {
    std::string raw;
    raw.reserve(size_t(h) * (size_t(w) * 4 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.append(reinterpret_cast<const char*>(&rgba[size_t(y) * size_t(w) * 4]), size_t(w) * 4);
    }
    std::string z = "\x78\x01";
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(char(n & 0xff)); z.push_back(char(n >> 8));
        z.push_back(char(~n & 0xff)); z.push_back(char((~n >> 8) & 0xff));
        z.append(raw, pos, n);
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    be32(z, (b << 16) | a);
    std::string png = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    be32(ihdr, uint32_t(w)); be32(ihdr, uint32_t(h));
    ihdr += std::string("\x08\x06\x00\x00\x00", 5);
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", "");
    return png;
}

std::vector<uint8_t> neutralLut(int n) {
    std::vector<uint8_t> px(size_t(n) * size_t(n) * size_t(n) * 4);
    const int w = n * n;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < w; ++x) {
            const int r = x % n, b = x / n, g = y;
            uint8_t* p = &px[(size_t(y) * size_t(w) + size_t(x)) * 4];
            p[0] = uint8_t(r * 255 / (n - 1)); p[1] = uint8_t(g * 255 / (n - 1)); p[2] = uint8_t(b * 255 / (n - 1)); p[3] = 255;
        }
    return px;
}

Json postFxBlock(Workspace& ws, const std::string& scope) {
    return scope == "map" ? ws.map.data.extra["postfx"] : ws.project.manifest["postfx"];
}

void setPostFxBlock(Workspace& ws, const std::string& scope, const Json& block) {
    const bool empty = !block.isObject() || (effectsArray(block).size() == 0 && block.size() == (block.has("effects") ? 1u : 0u));
    if (scope == "map") {
        ws.map.beginEdit();
        if (empty) ws.map.data.extra.erase("postfx"); else ws.map.data.extra.set("postfx", block);
        ws.map.touched();
        return;
    }
    if (ws.project.dir.empty()) throw std::runtime_error("no game is open: call open_game first");
    if (empty) ws.project.manifest.erase("postfx"); else ws.project.manifest.set("postfx", block);
    std::string err;
    if (!ws.project.saveManifest(&err)) throw std::runtime_error(err);
    ws.note({Change::Project, 0, 0, -1, -1, -1, "game.json: postfx"});
}

void registerPostFxOps(Workspace& ws) {
    auto add = [&ws](const char* name, const std::string& desc, const std::string& schema, Workspace::OpFn fn, bool gui = false) {
        ws.registerOp({name, desc, Json::parse(schema), std::move(fn), gui});
    };
    Workspace* w = &ws;
    std::string types;
    for (auto& t : postFxCatalog()) types += std::string(types.empty() ? "" : ", ") + t.id + " (" + postFxCostName(t.cost) + ")";
    std::string presets;
    for (auto& p : postFxPresets()) presets += std::string(presets.empty() ? "" : ", ") + p.id;

    add("postfx_catalog", "Post-processing effects the engine has: types with parameters (range, default, meaning), cost class "
        "(cheap / medium / heavy) and estimated milliseconds per frame on a weak phone, a mid phone and a desktop, plus presets. Read this before postfx_add.",
        "{}", [](const Json&) {
            Json r = Json::object(), list = Json::array(), pres = Json::array();
            for (auto& t : postFxCatalog()) {
                Json j = Json::object();
                j.set("type", Json::string(t.id));
                j.set("help", Json::string(t.help));
                j.set("class", Json::string(postFxCostName(t.cost)));
                j.set("defaultMinQuality", Json::string(postFxQualityName(t.defaultMinQuality)));
                Json params = Json::array();
                for (auto& p : t.params) {
                    Json pj = Json::object();
                    pj.set("name", Json::string(p.name));
                    pj.set("kind", Json::string(p.kind == PostFxParamKind::Number ? "number" : p.kind == PostFxParamKind::Color ? "color [r,g,b]"
                                                 : p.kind == PostFxParamKind::Bool ? "bool" : "string"));
                    if (p.kind == PostFxParamKind::Number || p.kind == PostFxParamKind::Color) {
                        Json range = Json::array(); range.push(Json::number(double(p.lo), 3)); range.push(Json::number(double(p.hi), 3));
                        pj.set("range", range);
                    }
                    if (p.kind == PostFxParamKind::Color) {
                        Json d = Json::array(); for (int i = 0; i < 3; ++i) d.push(Json::number(double(p.def[i]), 3));
                        pj.set("default", d);
                    } else if (p.kind == PostFxParamKind::Bool) pj.set("default", Json::fromBool(p.def[0] > 0.5f));
                    else if (p.kind == PostFxParamKind::Number) pj.set("default", Json::number(double(p.def[0]), 3));
                    pj.set("help", Json::string(p.help));
                    params.push(pj);
                }
                j.set("params", params);
                const PostFxEffect e = defaultPostFxEffect(t.type);
                Json ms = Json::object();
                for (auto& prof : postFxProfiles()) {
                    Json per = Json::object();
                    for (PostFxQuality q : {PostFxQuality::Low, PostFxQuality::Medium, PostFxQuality::High})
                        per.set(postFxQualityName(q), Json::number(double(postFxMs(postFxEffectWork(e, q, true), prof)), 2));
                    ms.set(prof.id, per);
                }
                j.set("msPerFrame", ms);
                list.push(j);
            }
            for (auto& p : postFxPresets()) {
                Json j = Json::object();
                j.set("id", Json::string(p.id));
                j.set("help", Json::string(p.help));
                j.set("effects", Json::parse(p.effectsJson));
                pres.push(j);
            }
            r.set("effects", list);
            r.set("presets", pres);
            Json prof = Json::array();
            for (auto& p : postFxProfiles()) {
                Json j = Json::object();
                j.set("id", Json::string(p.id)); j.set("device", Json::string(p.label)); j.set("defaultTier", Json::string(postFxQualityName(p.tier)));
                prof.push(j);
            }
            r.set("devices", prof);
            r.set("notes", Json::string("One effect per type; fixed order (fxaa, pixelate, chromaticAberration, sharpen, tiltShift, bloom, sunShafts, "
                                        "colorGrade, lut, posterize, vignette, grain, fade, letterbox). minQuality skips an effect on lower tiers. "
                                        "Cheap effects share one composite pass; heavy ones add passes. Milliseconds are estimates from a fill-rate model."));
            return r;
        });

    add("postfx_get", "Post-processing of the game: the 'postfx' blocks of game.json and of the open map, the resolved chain "
        "with cost per device class (ms per frame, passes, fps at 60) and warnings.",
        "{}", [w](const Json&) {
            Json r = okJson();
            r.set("game", w->project.manifest["postfx"]);
            r.set("map", w->map.data.extra["postfx"]);
            r.set("chain", chainReport(*w));
            return r;
        });

    add("postfx_add", "Add a post-processing effect (or replace the one of that type) — types: " + types + ". Parameters at the top level "
        "or in \"params\" (see postfx_catalog), plus enabled and minQuality (low | medium | high: lower tiers skip it; weak phones run 'low'). "
        "The answer has 'warnings' — HEAVY effects (bloom, fxaa, sunShafts) and chains over the frame budget are reported with milliseconds "
        "per device class; tell the user about them. scope 'map' overrides game.json for the open map only.",
        std::string(R"({"properties": {"type": {"type": "string"}, "params": {"type": "object"}, "minQuality": {"type": "string", "enum": ["low", "medium", "high"]},
            "enabled": {"type": "boolean"}, )") + kScope + R"(}, "required": ["type"], "additionalProperties": true})",
        [w](const Json& p) {
            const std::string scope = scopeOf(p);
            Json block = postFxBlock(*w, scope);
            Json effects = effectsArray(block);
            Json e = effectFromParams(p, nullptr);
            const PostFxEffect parsed = parsePostFxEffect(e, true);   // throws on unknown type / parameter
            Json stored = postFxEffectJson(parsed);
            if (e.has("minQuality")) stored.set("minQuality", e["minQuality"]);
            const int i = indexOfType(effects, postFxType(parsed.type).id);
            if (i >= 0) effects.at(size_t(i)) = stored; else effects.push(stored);
            setPostFxBlock(*w, scope, withEffects(block, effects));
            return changeAnswer(*w, {parsed}, std::string("Постэффект ") + postFxType(parsed.type).label + (scope == "map" ? " (карта)" : ""));
        });

    add("postfx_set", "Change parameters of a post-processing effect already in the block (omitted parameters are kept); "
        "same warnings as postfx_add. On scope 'map' with no map entry yet, the map gets an override of that effect.",
        std::string(R"({"properties": {"type": {"type": "string"}, "params": {"type": "object"}, "minQuality": {"type": "string", "enum": ["low", "medium", "high"]},
            "enabled": {"type": "boolean"}, )") + kScope + R"(}, "required": ["type"], "additionalProperties": true})",
        [w](const Json& p) {
            const std::string scope = scopeOf(p);
            const std::string type = p.getStr("type");
            if (!postFxType(type)) throw std::runtime_error("postfx_set: unknown effect type '" + type + "'");
            Json block = postFxBlock(*w, scope);
            Json effects = effectsArray(block);
            const int i = indexOfType(effects, type);
            if (i < 0 && scope == "game") throw std::runtime_error("postfx_set: game.json has no '" + type + "' effect (use postfx_add)");
            Json base = i >= 0 ? effects[size_t(i)] : Json::object();
            Json e = effectFromParams(p, &base);
            parsePostFxEffect(e, true);
            if (i >= 0) effects.at(size_t(i)) = e; else effects.push(e);
            setPostFxBlock(*w, scope, withEffects(block, effects));
            // warnings and cost on the merged (game + map) result
            const PostFxSettings s = parsePostFx(w->project.manifest["postfx"], w->map.data.extra["postfx"]);
            std::vector<PostFxEffect> touched;
            if (const PostFxEffect* m = s.find(postFxType(type)->type)) touched.push_back(*m);
            return changeAnswer(*w, touched, std::string("Постэффект ") + postFxType(type)->label + ": параметры");
        });

    add("postfx_remove", "Remove a post-processing effect by type, or all of them (all=true) from game.json or the open map.",
        std::string(R"({"properties": {"type": {"type": "string"}, "all": {"type": "boolean"}, )") + kScope + "}}",
        [w](const Json& p) {
            const std::string scope = scopeOf(p);
            Json block = postFxBlock(*w, scope);
            Json effects = effectsArray(block);
            if (p.getBool("all")) effects = Json::array();
            else {
                const int i = indexOfType(effects, p.getStr("type"));
                if (i < 0) throw std::runtime_error("postfx_remove: no '" + p.getStr("type") + "' effect in " + (scope == "map" ? "the map" : "game.json"));
                effects.eraseAt(size_t(i));
            }
            setPostFxBlock(*w, scope, withEffects(block, effects));
            return changeAnswer(*w, {}, p.getBool("all") ? "Постэффекты удалены" : "Постэффект удалён: " + p.getStr("type"));
        });

    add("postfx_preset", "Apply a ready look: " + presets + ". mode 'replace' (default) swaps the whole effect list, 'merge' "
        "adds / replaces only the preset's effect types. Answers with warnings and cost like postfx_add.",
        std::string(R"({"properties": {"preset": {"type": "string"}, "mode": {"type": "string", "enum": ["replace", "merge"]}, )") + kScope +
            R"(}, "required": ["preset"]})",
        [w](const Json& p) {
            const PostFxPreset* pr = postFxPreset(p.getStr("preset"));
            if (!pr) throw std::runtime_error("postfx_preset: unknown preset '" + p.getStr("preset") + "'");
            const std::string scope = scopeOf(p);
            Json block = postFxBlock(*w, scope);
            Json effects = p.getStr("mode", "replace") == "merge" ? effectsArray(block) : Json::array();
            const Json presetEffects = Json::parse(pr->effectsJson);
            for (auto& e : presetEffects.items()) {
                const int i = indexOfType(effects, e.getStr("type"));
                if (i >= 0) effects.at(size_t(i)) = e; else effects.push(e);
            }
            setPostFxBlock(*w, scope, withEffects(block, effects));
            std::vector<PostFxEffect> touched;
            for (auto& e : presetEffects.items()) touched.push_back(parsePostFxEffect(e, false));
            Json r = changeAnswer(*w, touched, std::string("Набор постэффектов «") + pr->label + "»");
            r.set("effects", effects);
            return r;
        });

    add("postfx_enable", "Switch the whole post-processing chain on or off (game.json, or per map) without deleting the effects.",
        std::string(R"({"properties": {"enabled": {"type": "boolean"}, )") + kScope + R"(}, "required": ["enabled"]})",
        [w](const Json& p) {
            const std::string scope = scopeOf(p);
            Json block = postFxBlock(*w, scope);
            Json b = block.isObject() ? block : withEffects(block, effectsArray(block));
            b.set("enabled", Json::fromBool(p.getBool("enabled", true)));
            setPostFxBlock(*w, scope, b);
            return changeAnswer(*w, {}, p.getBool("enabled", true) ? "Постэффекты включены" : "Постэффекты выключены");
        });

    add("postfx_estimate", "Frame cost of a post-processing chain without changing anything: pass \"effects\" (array as in game.json) "
        "to try a combination, or nothing for the current game + map chain. Per device class: tier, ms, passes, fps at 60, warnings.",
        R"({"properties": {"effects": {"type": "array", "items": {"type": "object"}}}})", [w](const Json& p) {
            if (!p["effects"].isArray()) return chainReport(*w);
            Json game = Json::object();
            game.set("effects", p["effects"]);
            for (auto& e : p["effects"].items()) parsePostFxEffect(e, true);
            std::vector<std::string> issues;
            return postFxCostReport(parsePostFx(game, Json(), &issues));
        });

    add("postfx_lut_neutral", "Write a neutral colour lookup table (PNG strip, Unreal layout) into the game folder: grade it in any image editor "
        "together with a screenshot, then postfx_add {type: 'lut', texture: <path>}. size 16 (256x16) or 32 (1024x32, default).",
        R"({"properties": {"path": {"type": "string", "description": "default postfx/neutral_lut.png"}, "size": {"type": "integer", "enum": [16, 32]}}})",
        [w](const Json& p) {
            if (w->project.dir.empty()) throw std::runtime_error("no game is open: call open_game first");
            const int n = p.getInt("size", 32) == 16 ? 16 : 32;
            const std::string rel = p.getStr("path", "postfx/neutral_lut.png");
            if (rel.find("..") != std::string::npos || rel.empty() || rel[0] == '/') throw std::runtime_error("path must stay inside the game folder");
            const fs::path out = fs::path(w->project.dir) / rel;
            std::error_code ec;
            fs::create_directories(out.parent_path(), ec);
            const std::string png = encodePngRgba(n * n, n, neutralLut(n));
            std::ofstream f(out, std::ios::binary);
            if (!f || !f.write(png.data(), std::streamsize(png.size()))) throw std::runtime_error("can't write " + out.string());
            Json r = okJson();
            r.set("path", Json::string(rel));
            r.set("size", Json::string(std::to_string(n * n) + "x" + std::to_string(n)));
            r.set("summary", Json::string("Нейтральная LUT: " + rel));
            return r;
        });

    add("view_postfx", "Editor preview of post-processing (does not change the game): quality off | low | medium | high shows what "
        "weak phones (low) or strong devices (high) get; scene=false hides effects in the Scene view (Game view keeps them). "
        "Combine with view_screenshot to compare tiers.",
        R"({"properties": {"quality": {"type": "string", "enum": ["off", "low", "medium", "high"]}, "scene": {"type": "boolean"}}})",
        [](const Json&) -> Json { throw std::runtime_error("this tool needs the editor window (start rts_editor)"); }, true);
}

} // namespace rts::editor
