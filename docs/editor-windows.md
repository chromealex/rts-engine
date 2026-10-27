# Adding a window to the editor

The editor (`apps/editor`) is a set of dockable Dear ImGui windows (docking branch). Every window
is a small class; the editor creates one instance of each registered class, lists it in the
**Окна** menu, docks it into the default layout, saves the user's layout
(`<userDataDir>/editor_layout.ini`) and lets agents open/close it over MCP (`editor_windows`,
`editor_window`).

## 1. Create the file

Put a `.cpp` file in `apps/editor/windows/`. Every file there is compiled into the editor
automatically (CMake `GLOB ... CONFIGURE_DEPENDS`), nothing else to register.

```cpp
// apps/editor/windows/BalanceWindow.cpp
#include "../Editor.h"
#include "../EditorUtil.h"     // ui::inputString, ui::inputMultiline, ui::help

namespace rts::editor {

class BalanceWindow : public EditorWindow {
public:
    const char* id() const override { return "balance"; }        // stable latin id (MCP, layout)
    const char* title() const override { return "Balance"; }     // tab title (must be unique)
    const char* dockArea() const override { return "bottom"; }   // left | left-bottom | right | center | bottom
    int order() const override { return 5; }                     // tab order in that area (0 = shown first)
    bool openByDefault() const override { return false; }        // hidden until opened from the menu

    void draw(Editor& ed) override {                             // called between ImGui::Begin/End
        for (auto& t : ed.rules().types) {
            if (t.cost.empty()) continue;
            ImGui::Text("%s: %lld hp", t.name.c_str(), (long long)t.maxHp.toInt());
        }
    }
    void update(Editor& ed, double dt) override {}              // optional: every frame, even when closed
};

RTS_EDITOR_WINDOW(BalanceWindow);

} // namespace rts::editor
```

Rebuild the editor; the window appears in **Окна**. New windows are docked into their `dockArea`
only when the layout is reset (**Окна → Сбросить раскладку**); otherwise they open floating and the
user can dock them anywhere.

## 2. What a window can use (`Editor` public API, `apps/editor/Editor.h`)

| API | What for |
|---|---|
| `ed.ws()` | the `Workspace`: project (manifest, `rulesJson`, parsed `rules`), map document, activity log, and **`ws().call(op, params)`** — run any MCP operation (`rules_set`, `entity_place`, `match_run`, ...) exactly like an agent; prefer this over editing data directly, it validates and records history |
| `ed.doc()` | the open map (`MapDoc`): `data` (terrain layers, players, entities, roads, `extra` JSON blocks), `beginEdit()` before a change (undo snapshot), `touched()` after a change (views rebuild) |
| `ed.rules()` | parsed rules of the open game (read-only; change rules through `rules_set`) |
| `ed.selection`, `ed.select(...)`, `ed.selectEntity(i)` | what the Inspector shows |
| `ed.tool`, `ed.brush`, `ed.placeType`, ... | Scene tool state |
| `ed.sceneCam`, `ed.focusScene(x, z)`, `ed.flash(x0, z0, x1, z1)` | camera and highlight in Scene |
| `ed.playing()`, `ed.sim()`, `ed.simModule()`, `ed.startPlay(slot)`, `ed.stopPlay()` | the play-test simulation (read-only from windows; issue player commands with `ed.issueCommand`) |
| `ed.playHistory`, `ed.frameHistory` | sampled statistics |
| `Viewport` + `ed.renderScene(vp, cam, overlays)` / `ed.renderGame(vp, cam)` | a 3D view of the map inside your window (see `SceneWindow.cpp`: `vp.ensure()`, render, `vp.image()`) |
| `ed.status` | text in the status bar |

Rules for windows:
- Never write into the simulation (`ed.sim()`): it is deterministic and replayable; use commands.
- Change rules and maps through `ws().call(...)` or `doc().beginEdit()` + edit + `doc().touched()`.
- Keep per-window state in members (one instance per window).
- Russian UI text, English code and comments (see AGENTS.md).

## 3. Exposing it to agents

If the window adds an action an agent should be able to do, register an operation (in the
window's constructor is too early; do it in `update()` once, or in `EditorAgent.cpp`):

```cpp
ed.ws().registerOp({"balance_report", "Damage per cost of every unit type.", Json::parse("{}"),
                    [&ed](const Json&) { Json r = Json::object(); /* ... */ return r; }, true});
```

Operations appear in MCP `tools/list` for agents connected through `rtsmcp` (the tool list is the
`Workspace` registry; GUI-only operations have `guiOnly = true` and fail politely without the
editor window).
