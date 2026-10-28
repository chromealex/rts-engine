#include "OverlayOps.h"
#include "modules/rts/view/RtsOverlayData.h"

#include <stdexcept>
#include <string>

namespace rts::editor {

namespace {

Json str(const std::string& s) { return Json::string(s); }

void needProject(const Workspace& ws) {
    if (ws.project.dir.empty()) throw std::runtime_error("no game is open: call open_game first");
}

const Json& listIn(const Json& doc) {
    static const Json empty = Json::array();
    const Json& l = doc["overlays"];
    return l.isArray() ? l : empty;
}

int findIn(const Json& list, const std::string& id) {
    for (size_t i = 0; i < list.size(); ++i) if (list[i].getStr("id") == id) return int(i);
    return -1;
}

bool isBuiltin(const std::string& id) { return findIn(game::builtinOverlays(), id) >= 0; }

// The effective list: built-ins, then rules, then game.json (merged by id), with where each came from.
Json effective(const Workspace& ws) {
    Json merged = game::builtinOverlays();
    game::mergeOverlayEntries(merged, listIn(ws.project.rulesJson));
    game::mergeOverlayEntries(merged, listIn(ws.project.manifest));
    return merged;
}

std::string originOf(const Workspace& ws, const std::string& id) {
    std::string o;
    auto add = [&](const char* s) { if (!o.empty()) o += "+"; o += s; };
    if (isBuiltin(id)) add("builtin");
    if (findIn(listIn(ws.project.rulesJson), id) >= 0) add("rules");
    if (findIn(listIn(ws.project.manifest), id) >= 0) add("game");
    return o;
}

// Validate the whole result (rules list + game list on top of the built-ins) before saving.
void validateAll(const Workspace& ws, const Json& rulesList, const Json& gameList) {
    const game::Rules& R = ws.project.rules;
    game::validateOverlays(R, rulesList);
    game::validateOverlays(R, gameList);
    Json merged = game::builtinOverlays();
    game::mergeOverlayEntries(merged, rulesList);
    game::mergeOverlayEntries(merged, gameList);
    for (auto& e : merged.items()) game::parseOverlay(R, e);
}

void store(Workspace& ws, bool toGame, const Json& list, const std::string& what) {
    Json& doc = toGame ? ws.project.manifest : ws.project.rulesJson;
    if (list.size() == 0) doc.erase("overlays");
    else doc.set("overlays", list);
    std::string err;
    if (toGame ? !ws.project.saveManifest(&err) : !ws.project.saveRules(&err)) throw std::runtime_error(err);
    // the views re-read rules.json / game.json "overlays" on a rules change (live in the play-test)
    ws.note({Change::Rules, 0, 0, -1, -1, -1, what});
}

bool targetGame(const Json& p) {
    const std::string t = p.getStr("target", "rules");
    if (t != "rules" && t != "game") throw std::runtime_error("target: rules or game");
    return t == "game";
}

} // namespace

void registerOverlayOps(Workspace& ws) {
    Workspace* w = &ws;
    auto add = [&ws](const char* name, const char* desc, const char* schema, Workspace::OpFn fn, bool gui = false) {
        ws.registerOp({name, desc, Json::parse(schema), std::move(fn), gui});
    };
    add("overlay_templates",
        "Ready overlay recipes (helper elements on screen / in the world): build grid while placing, attack range on select, "
        "aura radius, rally line, order path, damage numbers, resource popup, territory border on hover, build radius, ping "
        "marker, minimap ping, low hp warning, energy bar, target line, cursor cell, HUD label... Each has 'overlay' = the "
        "JSON to pass to overlay_set (edit it first: ids, colours, radius, conditions). Schema: docs/game-format.md 'Overlays'.",
        "{}", [](const Json&) {
            Json r = Json::object();
            r.set("templates", game::overlayTemplates());
            return r;
        });
    add("overlay_list",
        "Overlays of the open game in draw order: built-ins (selection, hover, move_markers, placement, garrison_count, "
        "health_bars, build_progress) merged with rules.json and game.json 'overlays' by id. origin = builtin / rules / game.",
        "{}", [w](const Json&) {
            needProject(*w);
            Json arr = Json::array();
            Json merged = effective(*w);
            Json warnings = Json::array();
            for (auto& e : merged.items()) {
                const std::string id = e.getStr("id");
                Json j = Json::object();
                j.set("id", str(id));
                j.set("origin", str(originOf(*w, id)));
                j.set("enabled", Json::fromBool(e.getBool("enabled", true)));
                const Json& sh = e["shape"];
                j.set("shape", str(sh.isString() ? sh.str() : sh.getStr("type")));
                if (e.has("event")) j.set("event", Json::fromBool(true));
                try { game::parseOverlay(w->project.rules, e); }
                catch (const std::exception& ex) { j.set("error", str(ex.what())); warnings.push(str(ex.what())); }
                arr.push(j);
            }
            Json r = Json::object();
            r.set("overlays", arr);
            if (warnings.size()) r.set("warnings", warnings);
            return r;
        });
    add("overlay_get",
        "One overlay by id: 'overlay' = the effective entry (built-in merged with overrides), 'rules' / 'game' = the entries "
        "written in rules.json / game.json (an override of a built-in may be partial).",
        R"({"properties": {"id": {"type": "string"}}, "required": ["id"]})", [w](const Json& p) {
            needProject(*w);
            const std::string id = p.getStr("id");
            Json merged = effective(*w);
            int i = findIn(merged, id);
            if (i < 0) throw std::runtime_error("no overlay '" + id + "' (overlay_list shows them)");
            Json r = Json::object();
            r.set("overlay", merged[size_t(i)]);
            r.set("origin", str(originOf(*w, id)));
            if (int k = findIn(listIn(w->project.rulesJson), id); k >= 0) r.set("rules", listIn(w->project.rulesJson)[size_t(k)]);
            if (int k = findIn(listIn(w->project.manifest), id); k >= 0) r.set("game", listIn(w->project.manifest)[size_t(k)]);
            return r;
        });
    add("overlay_set",
        "Add or replace an overlay in rules.json (target 'game': game.json) and save; validated against the game's types, "
        "tags and statuses, live in the editor play-test. Pass 'overlay' (full entry with id, when, anchor, shape, style, "
        "event) or 'template' (an overlay_templates id; optional 'id' to rename, 'patch' object deep-merged over it). For a "
        "built-in id the entry is an override merged over the default, e.g. {\"id\": \"health_bars\", \"enabled\": false} or "
        "{\"id\": \"selection\", \"style\": {\"color\": \"#40c0ffff\", \"thickness\": 0.06}}. merge=true merges into the "
        "existing entry instead of replacing it.",
        R"({"properties": {"overlay": {"type": "object"}, "template": {"type": "string"}, "id": {"type": "string"},
            "patch": {"type": "object"}, "merge": {"type": "boolean"}, "target": {"type": "string", "enum": ["rules", "game"]}}})",
        [w](const Json& p) {
            needProject(*w);
            const bool toGame = targetGame(p);
            Json entry;
            if (p.has("template")) {
                Json tpl = game::overlayTemplates();
                for (auto& t : tpl.items()) if (t.getStr("id") == p.getStr("template")) entry = t["overlay"];
                if (entry.isNull()) throw std::runtime_error("no template '" + p.getStr("template") + "' (overlay_templates lists them)");
            } else if (p["overlay"].isObject()) entry = p["overlay"];
            else throw std::runtime_error("pass 'overlay' (object) or 'template' (id)");
            if (p.has("id")) entry.set("id", str(p.getStr("id")));
            if (p["patch"].isObject()) { Json one = Json::array(); Json patch = p["patch"]; patch.set("id", str(entry.getStr("id"))); Json l = Json::array(); l.push(entry); one.push(patch); game::mergeOverlayEntries(l, one); entry = l[size_t(0)]; }
            const std::string id = entry.getStr("id");
            if (id.empty()) throw std::runtime_error("the overlay needs an \"id\"");
            Json rulesList = listIn(w->project.rulesJson), gameList = listIn(w->project.manifest);
            Json& list = toGame ? gameList : rulesList;
            const int at = findIn(list, id);
            if (at >= 0 && p.getBool("merge")) { Json one = Json::array(); one.push(entry); game::mergeOverlayEntries(list, one); }
            else if (at >= 0) list.at(size_t(at)) = entry;
            else list.push(entry);
            validateAll(*w, rulesList, gameList);
            store(*w, toGame, toGame ? gameList : rulesList, "Оверлей: " + id);
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("id", str(id));
            r.set("origin", str(originOf(*w, id)));
            r.set("summary", str(std::string(at >= 0 ? "Оверлей изменён: " : "Оверлей добавлен: ") + id));
            return r;
        });
    add("overlay_delete",
        "Remove an overlay entry from rules.json (target 'game': game.json) and save. Deleting the override of a built-in "
        "restores the default look; to hide a built-in use disable=true (writes {\"id\": ..., \"enabled\": false}).",
        R"({"properties": {"id": {"type": "string"}, "disable": {"type": "boolean"}, "target": {"type": "string", "enum": ["rules", "game"]}},
            "required": ["id"]})", [w](const Json& p) {
            needProject(*w);
            const bool toGame = targetGame(p);
            const std::string id = p.getStr("id");
            Json rulesList = listIn(w->project.rulesJson), gameList = listIn(w->project.manifest);
            Json& list = toGame ? gameList : rulesList;
            const int at = findIn(list, id);
            if (p.getBool("disable")) {
                Json e = Json::object();
                e.set("id", str(id));
                e.set("enabled", Json::fromBool(false));
                if (at >= 0) { Json one = Json::array(); one.push(e); game::mergeOverlayEntries(list, one); }
                else if (isBuiltin(id) || findIn(effective(*w), id) >= 0) list.push(e);
                else throw std::runtime_error("no overlay '" + id + "'");
            } else {
                if (at < 0) throw std::runtime_error("no overlay '" + id + "' in " + (toGame ? "game.json" : "rules.json") +
                                                     (isBuiltin(id) ? " (built-in: use disable=true to hide it)" : ""));
                list.eraseAt(size_t(at));
            }
            validateAll(*w, rulesList, gameList);
            store(*w, toGame, list, "Оверлей удалён: " + id);
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("summary", str((p.getBool("disable") ? "Оверлей выключен: " : "Оверлей удалён: ") + id));
            return r;
        });
    add("overlay_preview",
        "Editor preview: draw these overlays in the Scene and Game views regardless of their 'when' (selected-entity overlays use "
        "every object when nothing is selected, event overlays spawn sample popups). placeType = a structure type id to preview "
        "ghost / placement overlays at the cursor or the view centre. off=true ends the preview. Combine with view_screenshot.",
        R"({"properties": {"ids": {"type": "array", "items": {"type": "string"}}, "placeType": {"type": "string"},
            "x": {"type": "number"}, "z": {"type": "number"}, "off": {"type": "boolean"}}})",
        [](const Json&) -> Json { throw std::runtime_error("this tool needs the editor window (start rts_editor)"); }, true);
}

} // namespace rts::editor
