#include "MaterialOps.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#include "engine/render/Model.h"
#include "engine/render/TextureGen.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image_write/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;

namespace rts::editor {

namespace {

Json okJson() { Json r = Json::object(); r.set("ok", Json::fromBool(true)); return r; }
Json strs(const std::vector<std::string>& v) { Json a = Json::array(); for (const auto& s : v) a.push(Json::string(s)); return a; }

void needProject(const Workspace& ws) {
    if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game first)");
}

fs::path gameFile(const Workspace& ws, const std::string& rel) {
    const fs::path norm = fs::path(rel).lexically_normal();
    if (rel.empty() || norm.is_absolute() || norm.string().rfind("..", 0) == 0) throw std::runtime_error("path must be inside the game folder: " + rel);
    return fs::path(ws.project.dir) / norm;
}

bool readDisk(const std::string& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// rules.json edit with the same guarantees as rules_merge: validated by the loader, saved, shown live
void editRules(Workspace& ws, const std::string& what, const std::function<void(Json&)>& edit) {
    Json backup = ws.project.rulesJson;
    edit(ws.project.rulesJson);
    ws.project.rulesDirty = true;
    if (!ws.project.reloadRules()) {
        const std::string err = ws.project.rulesError;
        ws.project.rulesJson = backup;
        ws.project.reloadRules();
        throw std::runtime_error("rules rejected by the loader: " + err);
    }
    ws.note({Change::Rules, 0, 0, -1, -1, -1, what});
    std::string err;
    if (!ws.project.saveRules(&err)) throw std::runtime_error(err);
}

Json& member(Json& obj, const std::string& key) {
    if (!obj[key].isObject()) obj.set(key, Json::object());
    return *obj.find(key);
}

// model files of a type's view ("model": "a.glb" | [..., {"file": ...}])
std::vector<std::string> modelFiles(const Json& view) {
    std::vector<std::string> out;
    const Json& m = view["model"];
    if (m.isString()) out.push_back(m.str());
    for (const auto& e : m.items()) {
        if (e.isString()) out.push_back(e.str());
        else if (e.isObject() && !e.getStr("file").empty()) out.push_back(e.getStr("file"));
    }
    return out;
}

// material and node names of a model (CPU load; no textures needed, errors reported in place)
Json modelSummary(const Workspace& ws, const std::string& file) {
    Json r = Json::object();
    r.set("file", Json::string(file));
    try {
        const std::string base = ws.project.dir + "/";
        auto m = render::loadModel(file, [&](const std::string& p, std::string& out) { return readDisk(base + p, out); }, nullptr);
        Json mats = Json::array(), nodes = Json::array(), all = Json::array();
        for (const auto& mt : m->materials) mats.push(Json::string(mt.name));
        for (const auto& n : m->nodes) {
            all.push(Json::string(n.name));
            if (n.mesh >= 0) nodes.push(Json::string(n.name));
        }
        r.set("materials", mats);
        r.set("meshNodes", nodes);
        r.set("nodes", all);   // "node" in material_apply covers a node and everything under it
    } catch (const std::exception& e) {
        r.set("error", Json::string(e.what()));
    }
    return r;
}

bool usesMaterial(const Json& j, const std::string& name) {
    if (j.isString()) return j.str() == name;
    if (j.isObject()) {
        if (j["use"].isString() && j.getStr("use") == name) return true;
        if (j["base"].isString() && j.getStr("base") == name) return true;
    }
    return false;
}

Json patternSchema() {
    return Json::parse(R"J({
        "path": {"type": "string", "description": "PNG inside the game folder (default textures/<pattern>_<seed>.png)"},
        "pattern": {"type": "string", "description": "solid, noise, stripes, checker, brick, tiles, panels, metal, wood, camo, gradient, dots, hex, scales (texture_patterns explains each)"},
        "size": {"type": "integer", "description": "pixels per side, 8..2048 (default 256)"},
        "colors": {"type": "array", "items": {"type": "array", "items": {"type": "number"}}, "description": "sRGB [r,g,b] 0..1 per colour (pattern defaults otherwise)"},
        "scale": {"type": "number", "description": "repetitions / cells per side (default 8)"},
        "width": {"type": "number", "description": "line / mortar / stripe / dot share 0..1"},
        "variation": {"type": "number", "description": "per-cell brightness variation 0..1 (default 0.15)"},
        "angle": {"type": "number", "description": "stripes: 0, 45 or 90"},
        "octaves": {"type": "integer"}, "seed": {"type": "integer"},
        "output": {"type": "string", "enum": ["color", "normal", "mask"], "description": "normal = normal map of the same relief, mask = team mask (white where the second colour is)"},
        "strength": {"type": "number", "description": "normal map strength (default 2)"}})J");
}

const char* kMaterialDoc =
    "Material keys: color [r,g,b(,a)] linear 0..1, metallic 0..1, roughness 0..1, emissive [r,g,b], emissiveStrength, "
    "alpha opaque|mask|blend, cutoff, doubleSided, unlit, castShadow, receiveShadow, vertexColors, team tint|mask|alpha|vertex|none, "
    "teamStrength, texture / normalMap / emissiveMap / teamMask (PNG/JPEG/TGA/KTX2 paths in the game folder, e.g. made by texture_generate), "
    "uvScale [u,v] tiling, base (inherit another library material), replace (start from a blank material instead of the model's), description.";

} // namespace

const std::vector<std::string>& materialKeys() {
    static const std::vector<std::string> keys = {"color", "metallic", "roughness", "emissive", "emissiveStrength", "alpha", "cutoff",
                                                  "doubleSided", "unlit", "castShadow", "receiveShadow", "vertexColors", "team", "teamStrength",
                                                  "teamMask", "texture", "normalMap", "emissiveMap", "uvScale", "base", "use", "replace", "description"};
    return keys;
}

std::vector<std::string> checkMaterial(const Json& m) {
    std::vector<std::string> out;
    if (!m.isObject()) { out.push_back("a material is an object"); return out; }
    const auto& keys = materialKeys();
    for (const auto& [k, v] : m.members()) {
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) { out.push_back("unknown key '" + k + "'"); continue; }
        if ((k == "color" || k == "emissive") && (!v.isArray() || v.size() < 3)) out.push_back(k + " must be [r, g, b]");
        if ((k == "metallic" || k == "roughness" || k == "cutoff" || k == "teamStrength") && (!v.isNumber() || v.toDouble() < 0 || v.toDouble() > 1))
            out.push_back(k + " must be a number 0..1");
        if (k == "alpha" && v.str() != "opaque" && v.str() != "mask" && v.str() != "blend") out.push_back("alpha must be opaque, mask or blend");
        if (k == "team" && v.str() != "tint" && v.str() != "mask" && v.str() != "alpha" && v.str() != "vertex" && v.str() != "none")
            out.push_back("team must be tint, mask, alpha, vertex or none");
        if (k == "uvScale" && (!v.isArray() || v.size() != 2)) out.push_back("uvScale must be [u, v]");
    }
    return out;
}

Json loadMaterialLibrary(const Workspace& ws) {
    std::string text;
    if (ws.project.dir.empty() || !readDisk(ws.project.dir + "/materials.json", text)) return Json::object();
    try {
        Json j = Json::parse(text)["materials"];
        return j.isObject() ? j : Json::object();
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("materials.json: ") + e.what());
    }
}

void saveMaterialLibrary(Workspace& ws, const Json& lib, const std::string& what) {
    Json doc = Json::object();
    doc.set("materials", lib);
    const fs::path f = fs::path(ws.project.dir) / "materials.json";
    {
        std::ofstream o(f.string() + ".tmp", std::ios::binary);
        o << doc.dump(2) << "\n";
        if (!o) throw std::runtime_error("cannot write " + f.string());
    }
    std::error_code ec;
    fs::rename(f.string() + ".tmp", f, ec);
    if (ec) throw std::runtime_error("cannot write " + f.string() + ": " + ec.message());
    ws.note({Change::Rules, 0, 0, -1, -1, -1, what});   // views rebuild with the library
}

void registerMaterialOps(Workspace& ws) {
    Workspace* w = &ws;
    auto add = [&ws](const char* name, const std::string& desc, const std::string& schema, Workspace::OpFn fn) {
        ws.registerOp({name, desc, Json::parse(schema), std::move(fn), false});
    };

    add("material_list",
        "The game's material library (materials.json) and, per type with a model, its material overrides, node assignments, "
        "the model's own material names and mesh nodes (targets for material_apply).",
        R"({"properties": {"type": {"type": "string", "description": "only this type"}}})", [w](const Json& p) {
            needProject(*w);
            Json r = okJson();
            r.set("library", loadMaterialLibrary(*w));
            Json types = Json::array();
            const std::string only = p.getStr("type");
            for (const auto& [id, t] : w->project.rulesJson["types"].members()) {
                if (!only.empty() && id != only) continue;
                const Json& view = t["view"];
                const auto files = modelFiles(view);
                if (files.empty() && only.empty()) continue;
                Json tj = Json::object();
                tj.set("type", Json::string(id));
                if (view.has("materials")) tj.set("materials", view["materials"]);
                if (view.has("assign")) tj.set("assign", view["assign"]);
                Json models = Json::array();
                for (const auto& f : files) models.push(modelSummary(*w, f));
                tj.set("models", models);
                if (files.empty()) tj.set("note", Json::string("built-in shape: materials \"*\" applies to all its parts"));
                types.push(tj);
            }
            r.set("types", types);
            return r;
        });

    add("material_get", "One library material, flattened with its \"base\" chain.",
        R"({"properties": {"name": {"type": "string"}}, "required": ["name"]})", [w](const Json& p) {
            needProject(*w);
            const Json lib = loadMaterialLibrary(*w);
            const std::string name = p.getStr("name");
            if (!lib[name].isObject()) throw std::runtime_error("no library material '" + name + "'");
            Json flat = Json::object();
            std::vector<std::string> chain;
            std::string cur = name;
            for (int depth = 0; depth < 8 && lib[cur].isObject(); ++depth) {
                chain.push_back(cur);
                for (const auto& [k, v] : lib[cur].members()) if (k != "base" && !flat.has(k)) flat.set(k, v);
                cur = lib[cur].getStr("base");
                if (cur.empty()) break;
            }
            Json r = okJson();
            r.set("material", lib[name]);
            r.set("resolved", flat);
            r.set("chain", strs(chain));
            return r;
        });

    add("material_set",
        std::string("Create or replace a material in the game's library (materials.json), usable by name from any type: "
                    "material_apply, or a view's \"materials\" / \"assign\". ") + kMaterialDoc +
            " merge: true keeps the existing keys and changes only the given ones.",
        R"J({"properties": {"name": {"type": "string", "description": "e.g. steel, rusty_iron, team_cloth"},
            "material": {"type": "object", "description": "material keys (see description)"},
            "merge": {"type": "boolean"}}, "required": ["name", "material"]})J", [w](const Json& p) {
            needProject(*w);
            const std::string name = p.getStr("name");
            if (name.empty() || name.find_first_of(" \t/\\\"") != std::string::npos) throw std::runtime_error("material name must be one word (letters, digits, _ -)");
            Json mat = p["material"];
            Json lib = loadMaterialLibrary(*w);
            if (p.getBool("merge") && lib[name].isObject()) {
                Json cur = lib[name];
                for (const auto& [k, v] : mat.members()) cur.set(k, v);
                mat = cur;
            }
            const auto problems = checkMaterial(mat);
            if (!problems.empty()) throw std::runtime_error("material '" + name + "': " + problems[0]);
            if (mat["base"].isString() && !lib[mat.getStr("base")].isObject() && mat.getStr("base") != name)
                throw std::runtime_error("material '" + name + "': base '" + mat.getStr("base") + "' is not in the library");
            if (mat.getStr("base") == name) throw std::runtime_error("a material can't be its own base");
            std::vector<std::string> warnings;
            for (const char* k : {"texture", "normalMap", "emissiveMap", "teamMask"})
                if (mat[k].isString() && !fs::exists(gameFile(*w, mat.getStr(k)))) warnings.push_back(std::string(k) + " '" + mat.getStr(k) + "' does not exist yet");
            const bool existed = lib.has(name);
            lib.set(name, mat);
            saveMaterialLibrary(*w, lib, std::string(existed ? "Материал изменён: " : "Материал создан: ") + name);
            Json r = okJson();
            r.set("summary", Json::string(std::string(existed ? "Материал изменён: " : "Материал создан: ") + name));
            r.set("warnings", strs(warnings));
            return r;
        });

    add("material_delete", "Delete a library material. Refuses while types or other materials use it, unless force.",
        R"({"properties": {"name": {"type": "string"}, "force": {"type": "boolean"}}, "required": ["name"]})", [w](const Json& p) {
            needProject(*w);
            const std::string name = p.getStr("name");
            Json lib = loadMaterialLibrary(*w);
            if (!lib.has(name)) throw std::runtime_error("no library material '" + name + "'");
            std::vector<std::string> users;
            for (const auto& [id, t] : w->project.rulesJson["types"].members()) {
                for (const char* block : {"materials", "assign"})
                    for (const auto& [k, v] : t["view"][block].members()) if (usesMaterial(v, name)) users.push_back(id);
                for (const auto& e : t["view"]["model"].items())
                    for (const char* block : {"materials", "assign"})
                        for (const auto& [k, v] : e[block].members()) if (usesMaterial(v, name)) users.push_back(id);
            }
            for (const auto& [k, v] : lib.members()) if (k != name && usesMaterial(v, name)) users.push_back("material " + k);
            if (!users.empty() && !p.getBool("force")) throw std::runtime_error("material '" + name + "' is used by " + users[0] + (users.size() > 1 ? " and others" : "") + " (force: true deletes it anyway)");
            lib.erase(name);
            saveMaterialLibrary(*w, lib, "Материал удалён: " + name);
            Json r = okJson();
            r.set("usedBy", strs(users));
            return r;
        });

    add("material_apply",
        std::string("Give a type's model a material: a library name (\"steel\") or an inline material object. "
                    "target = a material name of the model (material_list shows them) or \"*\" for all: the material's keys "
                    "override that material (textures are kept unless given; replace: true starts blank). node = a mesh node "
                    "name or glob (\"Turret\", \"Wheel*\"): the meshes of that node and of everything under it get a fresh material. remove: true deletes the "
                    "override / assignment. ") + kMaterialDoc,
        R"J({"properties": {"type": {"type": "string"}, "material": {"description": "library name or material object"},
            "target": {"type": "string", "description": "model material name, or * (default)"},
            "node": {"type": "string", "description": "mesh node name or glob; instead of target"},
            "remove": {"type": "boolean"}}, "required": ["type"]})J", [w](const Json& p) {
            needProject(*w);
            const std::string type = p.getStr("type");
            if (!w->project.rulesJson["types"][type].isObject()) throw std::runtime_error("unknown type '" + type + "'");
            const bool byNode = p.has("node");
            const std::string key = byNode ? p.getStr("node") : p.getStr("target", "*");
            if (key.empty()) throw std::runtime_error(byNode ? "node is empty" : "target is empty");
            const std::string block = byNode ? "assign" : "materials";
            const bool remove = p.getBool("remove");
            if (!remove) {
                const Json& m = p["material"];
                if (m.isString()) {
                    if (!loadMaterialLibrary(*w)[m.str()].isObject()) throw std::runtime_error("no library material '" + m.str() + "' (material_set creates it)");
                } else if (m.isObject()) {
                    const auto problems = checkMaterial(m);
                    if (!problems.empty()) throw std::runtime_error(problems[0]);
                } else {
                    throw std::runtime_error("material must be a library name or an object");
                }
            }
            // warn when the target / node doesn't exist in any of the type's models
            std::vector<std::string> warnings;
            const Json& view = w->project.rulesJson["types"][type]["view"];
            const auto files = modelFiles(view);
            if (!remove && key != "*" && !files.empty()) {
                bool found = false;
                for (const auto& f : files) {
                    const Json s = modelSummary(*w, f);
                    for (const auto& n : s[byNode ? "nodes" : "materials"].items()) {
                        const std::string nm = n.str();
                        if (byNode ? (key.find_first_of("*?") != std::string::npos || nm == key) : nm == key) found = true;
                    }
                }
                if (!found) warnings.push_back(std::string(byNode ? "node '" : "material '") + key + "' not found in the type's model(s) (material_list lists them)");
            }
            editRules(*w, "Материал: " + type + " / " + key, [&](Json& rules) {
                Json& v = member(member(member(rules, "types"), type), "view");
                Json& b = member(v, block);
                if (remove) b.erase(key);
                else b.set(key, p["material"]);
                if (b.size() == 0) v.erase(block);
            });
            Json r = okJson();
            r.set("summary", Json::string(std::string(remove ? "Материал снят: " : "Материал назначен: ") + type + " / " + key));
            r.set("warnings", strs(warnings));
            return r;
        });

    add("texture_patterns", "Procedural texture patterns of texture_generate, with the keys each one reads.", "{}", [](const Json&) {
        Json r = okJson();
        r.set("patterns", strs(render::texturePatterns()));
        r.set("help", Json::string(render::texturePatternHelp()));
        return r;
    });

    add("texture_generate",
        "Make a tileable texture (PNG in the game folder) for a material: bricks, metal panels, wood, camouflage, scales, noise, "
        "stripes, ... Also its normal map (output: normal) or a team mask (output: mask). Use the path in material_set / "
        "material_apply (texture, normalMap, teamMask) and uvScale for tiling.",
        std::string(R"({"properties": )") + patternSchema().dump(-1) + R"(, "required": ["pattern"]})", [w](const Json& p) {
            needProject(*w);
            render::Image img;
            render::generateTexture(p, img);
            std::string rel = p.getStr("path");
            if (rel.empty()) rel = "textures/" + p.getStr("pattern") + "_" + std::to_string(p.getInt("seed", 1)) +
                                   (p.getStr("output", "color") == "color" ? "" : "_" + p.getStr("output")) + ".png";
            if (fs::path(rel).extension() != ".png") throw std::runtime_error("path must end with .png");
            const fs::path f = gameFile(*w, rel);
            std::error_code ec;
            fs::create_directories(f.parent_path(), ec);
            std::string png;
            stbi_write_png_to_func([](void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), size_t(size)); },
                                   &png, img.width, img.height, 4, img.rgba.data(), img.width * 4);
            std::ofstream o(f, std::ios::binary);
            o.write(png.data(), std::streamsize(png.size()));
            if (!o) throw std::runtime_error("cannot write " + f.string());
            o.close();
            w->note({Change::Rules, 0, 0, -1, -1, -1, "Текстура: " + rel});   // materials using it reload
            Json r = okJson();
            r.set("path", Json::string(rel));
            r.set("size", Json::number(int64_t(img.width)));
            r.set("bytes", Json::number(int64_t(png.size())));
            return r;
        });
}

} // namespace rts::editor
