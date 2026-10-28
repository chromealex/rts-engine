#include "LightOps.h"
#include <stdexcept>
#include <string>

namespace rts::editor {

namespace {
Json n3(double v) { return Json::number(v, 3); }
Json okJson() { Json r = Json::object(); r.set("ok", Json::fromBool(true)); return r; }
const char* kLightProps = R"J("x": {"type": "number"}, "z": {"type": "number"}, "height": {"type": "number", "description": "above the ground"},
    "color": {"type": "array", "items": {"type": "number"}, "description": "[r, g, b] 0..1"}, "radius": {"type": "number", "description": "cells"},
    "intensity": {"type": "number"}, "flicker": {"type": "number", "description": "0..1, fire"}, "halo": {"type": "number", "description": "glow sprite size, 0 = none"},
    "name": {"type": "string"}, "type": {"type": "string", "enum": ["point", "spot"], "description": "spot = searchlight / lamp cone"},
    "angle": {"type": "number", "description": "spot cone, degrees (default 40)"}, "softness": {"type": "number", "description": "spot edge 0..1"},
    "pitch": {"type": "number", "description": "spot: degrees below the horizon (default 35)"}, "yaw": {"type": "number", "description": "spot: direction, degrees (0 = +x, 90 = +z)"},
    "sweep": {"type": "number", "description": "spot: swing amplitude, degrees (searchlight)"}, "sweepSpeed": {"type": "number", "description": "degrees per second"},
    "pattern": {"type": "string", "enum": ["steady", "pulse", "strobe", "beacon"], "description": "beacon: spots rotate, points blink"}, "rate": {"type": "number", "description": "pattern frequency, Hz"},
    "nightOnly": {"type": "boolean", "description": "on at dusk, off at dawn"}, "electric": {"type": "boolean", "description": "affected by power / damage (map lights: always powered)"})J";
} // namespace

Json& mapLightsMut(MapDoc& doc) {
    Json& extra = doc.data.extra;
    if (!extra["lights"].isArray()) extra.set("lights", Json::array());
    return *extra.find("lights");
}

void applyLightParams(Json& l, const Json& p) {
    for (const char* k : {"x", "z", "height", "radius", "intensity", "flicker", "angle", "softness", "pitch", "yaw", "sweep", "sweepSpeed", "beam"})
        if (p[k].isNumber()) l.set(k, n3(p[k].toDouble()));
    if (p["halo"].isNumber()) l.set("halo", n3(p["halo"].toDouble()));
    else if (p.has("halo")) l.set("halo", Json::fromBool(p["halo"].boolean(true)));
    if (const Json& c = p["color"]; c.size() >= 3) {
        Json a = Json::array();
        for (size_t i = 0; i < 3; ++i) a.push(n3(c[i].toDouble()));
        l.set("color", a);
    }
    if (p.has("name")) l.set("name", Json::string(p.getStr("name")));
    if (p.has("pattern")) {
        const std::string pat = p.getStr("pattern");
        if (pat == "steady" || pat.empty()) l.erase("pattern"); else l.set("pattern", Json::string(pat));
    }
    if (p["rate"].isNumber()) l.set("rate", n3(p["rate"].toDouble()));
    for (const char* k : {"electric", "nightOnly"}) if (p.has(k)) l.set(k, Json::fromBool(p[k].boolean(false)));
    if (p.has("type")) {
        if (p.getStr("type") == "spot") l.set("type", Json::string("spot"));
        else { l.erase("type"); for (const char* k : {"angle", "softness", "pitch", "yaw", "sweep", "sweepSpeed"}) l.erase(k); }
    }
    if (p.has("x") || p.has("z")) l.erase("pos");
}

void registerLightOps(Workspace& ws) {
    auto add = [&ws](const char* name, const char* desc, const std::string& schema, Workspace::OpFn fn, bool gui = false) {
        ws.registerOp({name, desc, Json::parse(schema), std::move(fn), gui});
    };
    Workspace* w = &ws;
    add("light_list", "Decorative lights of the open map ('lights') and its 'lighting' block (time of day, dayLength, clouds, storm).",
        "{}", [w](const Json&) {
            Json r = okJson();
            const Json& extra = w->map.data.extra;
            r.set("lights", extra["lights"].isArray() ? extra["lights"] : Json::array());
            r.set("lighting", extra["lighting"]);
            return r;
        });
    add("light_add", "Add a decorative point light to the map (lantern, campfire, lava glow): x, z in cells; height above "
        "ground (default 1); color [r,g,b] 0..1; radius in cells (default 4); intensity (default 1.5); flicker 0..1 for fire; "
        "halo = glow sprite size. type 'spot' makes a searchlight / lamp cone: angle, pitch, yaw (0 = +x, 90 = +z), sweep for a swinging "
        "searchlight. Shown once the cell is explored. Cheap on every quality tier, including old Android.",
        std::string(R"({"properties": {)") + kLightProps + R"(}, "required": ["x", "z"]})", [w](const Json& p) {
            w->map.beginEdit();
            Json l = Json::object();
            l.set("x", n3(0)); l.set("z", n3(0)); l.set("height", n3(1)); l.set("radius", n3(4)); l.set("intensity", n3(1.5));
            applyLightParams(l, p);
            Json& arr = mapLightsMut(w->map);
            arr.push(l);
            w->map.touched();
            Json r = okJson();
            r.set("index", Json::number(int64_t(arr.size() - 1)));
            return r;
        });
    add("light_update", "Change a decorative map light by index (fields as in light_add; omitted fields are kept).",
        std::string(R"({"properties": {"index": {"type": "integer"}, )") + kLightProps + R"(}, "required": ["index"]})", [w](const Json& p) {
            const Json& cur = w->map.data.extra["lights"];
            const int64_t i = p.getInt("index", -1);
            if (i < 0 || size_t(i) >= cur.size()) throw std::runtime_error("light_update: no light with index " + std::to_string(i));
            w->map.beginEdit();
            applyLightParams(mapLightsMut(w->map).at(size_t(i)), p);
            w->map.touched();
            return okJson();
        });
    add("light_delete", "Delete a decorative map light by index, or all of them with all=true.",
        R"({"properties": {"index": {"type": "integer"}, "all": {"type": "boolean"}}})", [w](const Json& p) {
            const Json& cur = w->map.data.extra["lights"];
            const int64_t i = p.getInt("index", -1);
            if (!p.getBool("all") && (i < 0 || size_t(i) >= cur.size())) throw std::runtime_error("light_delete: no light with index " + std::to_string(i));
            w->map.beginEdit();
            if (p.getBool("all")) w->map.data.extra.erase("lights");
            else mapLightsMut(w->map).eraseAt(size_t(i));
            w->map.touched();
            return okJson();
        });
    add("lighting_set", "Set the map's 'lighting' block (overrides game.json 'lighting', which set_manifest edits): time 0..1 "
        "(0 midnight, 0.25 dawn, 0.5 noon, 0.75 dusk), dayLength = game seconds per day (0 = time stays), nightLevel 0..1, "
        "clouds 0..1, cloudStrength, cloudScale, wind [x,z] cells/s, storm 0..1 (overcast + lightning), preset desert | snow | volcanic | "
        "overcast | cave (cave = underground: permanent night, only lights). Omitted fields are kept; remove=true deletes the block.",
        R"({"properties": {"time": {"type": "number"}, "dayLength": {"type": "number"}, "nightLevel": {"type": "number"}, "clouds": {"type": "number"},
            "cloudStrength": {"type": "number"}, "cloudScale": {"type": "number"}, "wind": {"type": "array", "items": {"type": "number"}},
            "storm": {"type": "number"}, "preset": {"type": "string", "enum": ["desert", "snow", "volcanic", "overcast", "cave"]},
            "remove": {"type": "boolean"}}})", [w](const Json& p) {
            Json& extra = w->map.data.extra;
            w->map.beginEdit();
            if (p.getBool("remove")) extra.erase("lighting");
            else {
                Json b = extra["lighting"].isObject() ? extra["lighting"] : Json::object();
                for (auto& [k, v] : p.members()) if (k != "remove") b.set(k, v);
                extra.set("lighting", b);
            }
            w->map.touched();
            Json r = okJson();
            r.set("lighting", extra["lighting"]);
            return r;
        });
    add("view_lighting", "Editor preview of time of day / clouds / storm (does not change the map): time 0..1, clouds 0..1, "
        "storm 0..1; off=true returns to the map's own lighting. Combine with view_screenshot to check night scenes.",
        R"({"properties": {"time": {"type": "number"}, "clouds": {"type": "number"}, "storm": {"type": "number"}, "off": {"type": "boolean"}}})",
        [](const Json&) -> Json { throw std::runtime_error("this tool needs the editor window (start rts_editor)"); }, true);
}

} // namespace rts::editor
