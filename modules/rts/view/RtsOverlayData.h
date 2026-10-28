// Data model of the overlay system (RtsOverlays): on-screen and in-world helper elements described by
// rules.json "overlays" (game.json "overlays" adds entries or overrides them by id). Built-in overlays
// (selection rings, hover ring, move markers, placement footprint, health / build bars, garrison
// counts) are data too (builtinOverlays()), so a game restyles or disables them by id.
//
//   { "id": "build_grid", "when": "placing", "anchor": "ghost",
//     "shape": {"type": "grid", "radius": 4, "predicate": "buildable"},
//     "style": {"colors": {"true": "#40ff6060", "false": "#ff404060"}, "lineColor": "#ffffff30"} }
//
// The full schema and recipes are in docs/game-format.md ("Overlays"). This file has no GPU or ImGui
// code: the editor core / MCP server validate overlays with it, the view renders them.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../../engine/data/Json.h"
#include "../../../engine/render/Math3D.h"
#include "../RtsData.h"

namespace rts::game {

// owner of an entity relative to the local player
enum OvRelation : uint8_t { kRelOwn = 1, kRelAlly = 2, kRelEnemy = 4, kRelNeutral = 8 };

enum OvEntityFlag : uint32_t {
    kOvStructure = 1u << 0, kOvUnit = 1u << 1, kOvConstructing = 1u << 2, kOvDamaged = 1u << 3,
    kOvGarrison = 1u << 4, kOvMoving = 1u << 5, kOvProducing = 1u << 6, kOvIdle = 1u << 7,
    kOvHero = 1u << 8, kOvWorker = 1u << 9, kOvInvulnerable = 1u << 10, kOvAir = 1u << 11,
    kOvCarrying = 1u << 12, kOvAttacking = 1u << 13, kOvResource = 1u << 14, kOvEnergy = 1u << 15,
    kOvRanged = 1u << 16, kOvAura = 1u << 17,
};

// Which entities: every listed key must match (empty filter = every entity).
struct OvFilter {
    std::vector<uint16_t> types;    // any of these types (or a type with any of `tags`)
    uint64_t tags = 0;
    uint8_t relations = 0;          // OvRelation mask, 0 = any owner
    uint32_t flagsOn = 0, flagsOff = 0;
    std::vector<int16_t> statuses;  // any of these statuses active (needs the live match)
    float hpMin = -1, hpMax = 2;    // hp fraction in [min, max)
};

// Pseudo key codes for "either side" modifiers (above platform::Key::Count).
constexpr uint16_t kOvKeyShift = 1000, kOvKeyCtrl = 1001, kOvKeyAlt = 1002, kOvKeySuper = 1003;
// Pseudo event kind: a ping from the client / editor context (OverlayContext::pings).
constexpr uint16_t kOvEventPing = 1000;

struct OvCondition {
    enum Kind : uint8_t { All, Any, Not, Always, Never, Placing, Selected, Hovered, Entity, Key, State, Var, Zoom, Cursor, Mode };
    Kind kind = Always;
    bool want = true;                 // Selected / Hovered: false = must NOT be selected / hovered
    OvFilter filter;                  // Placing (types / tags), Selected, Hovered, Entity
    std::vector<OvCondition> kids;    // All / Any / Not
    std::vector<uint16_t> keys;       // Key: platform::Key values or kOvKey* (all held)
    uint8_t states = 0;               // State: 1 playing, 2 paused, 4 over
    std::vector<std::string> names;   // Var: variable name; Mode: client input modes
    uint8_t cmp = 0;                  // Var: 0 != 0, 1 ==, 2 !=, 3 <, 4 <=, 5 >, 6 >=
    float value = 0, lo = 0, hi = 1e9f;
    bool entityScoped() const;        // uses the anchored entity (entity anchors evaluate it per entity)
};

enum class OvAnchor : uint8_t { Cursor, Selected, Hovered, Entities, Ghost, Area, Point, Screen, Orders, Route, Rally, Markers, Map, Event };
enum class OvShape : uint8_t { Grid, Circle, Ring, Rect, Footprint, Line, Arrow, Path, Cells, Text, Icon, Bar };
enum class OvPred : uint8_t {
    None, Ok, Buildable, Blocked, BuildRadius, Walkable, Water, Resource, Creep, Road, Region,
    Territory, EnemyTerritory, AnyTerritory, TerritoryEdge, Visible, Explored, Fogged, Unexplored,
};
enum class OvRadius : uint8_t { Fixed, Range, MinRange, Sight, Aura, BuildRadius, Selection, Capture, Claim, Footprint, Ability, Leash };
enum class OvValue : uint8_t { Hp, Energy, Build, Garrison, Production, Carry, Capture };
enum class OvSpace : uint8_t { Ground, Billboard, Screen, Minimap };
enum class OvGround : uint8_t { Follow, Flat, Zero };
enum class OvTo : uint8_t { None, Target, Rally, Cursor, Point, Order };
enum OvColorKey : uint8_t { kColTrue, kColFalse, kColOwn, kColAlly, kColEnemy, kColNeutral, kColCount };

struct OvStyle {
    render::float4 color{1, 1, 1, 1};
    bool playerColor = false;                     // "color": "player": the entity owner's colour
    render::float4 colors[kColCount];             // per predicate result / relation ("colors")
    uint8_t hasColor = 0;                         // bit per OvColorKey
    std::vector<std::pair<float, render::float4>> stops;   // value thresholds (bars): first stop >= value
    render::float4 fillColor, lineColor, background;
    bool hasFill = false, hasLine = false, hasBackground = false, backgroundPlayer = false;
    float backgroundShade = 1, backgroundAlpha = 1;
    float alpha = 1;
    float thickness = 0;                          // world units (ground / billboard) or points (screen); 0 = hairline
    float dashOn = 0, dashOff = 0, scroll = 0;    // dash pattern (world units / points), scroll speed per second
    float pulse = 0, pulseMin = 0.35f;            // alpha pulse (Hz) down to pulseMin
    float rotate = 0;                             // radians per second (rings, icons, dashes)
    float fadeIn = 0, fadeOut = 0;                // seconds
    float lift = 0.05f;                           // above the ground
    float rounding = 0, padX = 0, padY = 0, border = 1;
    float offsetX = 0, offsetY = 0;               // screen points
    float outerAlpha = 1;                         // grid: alpha of cells outside the ghost footprint
    int segments = 40;
    bool fill = false, outline = true, onTop = false, additive = false, shadow = true, falloff = true;
    OvSpace space = OvSpace::Ground;
    OvGround ground = OvGround::Follow;
};

struct OvEvent {
    std::vector<uint16_t> kinds;     // GameEvent values, kOvEventPing
    float lifetime = 1.2f;           // seconds
    float rise = 0;                  // world units per second upwards (floating text)
    float grow = 0;                  // radius growth per second (expanding rings)
    float spread = 0;                // random-looking sideways offset (world units), stable per event
    uint8_t by = 0, on = 0;          // relation masks: event player / owner of the entity hit (0 = any)
    OvFilter types;                  // type filter of the event entity (types / tags only)
    int max = 32;                    // live popups of this overlay
};

struct OverlayDef {
    std::string id;
    bool enabled = true, builtin = false;
    int order = 0;
    OvCondition when;
    // anchor
    OvAnchor anchor = OvAnchor::Selected;
    OvFilter anchorFilter;           // Entities (and an optional filter for Selected / Hovered)
    std::string areaName;            // Area: a named map area ("areas" in the map)
    float rect[4] = {0, 0, 0, 0};    // Area: x, z, w, h (when no name)
    float point[2] = {0, 0};         // Point: x, z; Screen: x, y points (negative: from the right / bottom)
    bool snap = false;               // Cursor: snap to the cell centre
    // shape
    OvShape shape = OvShape::Ring;
    OvPred pred = OvPred::None;
    OvRadius radiusFrom = OvRadius::Fixed;
    float radius = 1, radiusAdd = 0, radiusScale = 1;
    int index = 0;                   // weapon / ability index for range radii
    float size[2] = {1, 1};          // rect / icon size (world units, or points in screen space)
    float margin = 0;                // footprint / grid around the ghost: extra cells
    bool square = false;             // grid: square instead of round
    bool highlightFootprint = true;  // grid around a footprint: cells under it at full alpha
    std::string text, image;
    OvValue value = OvValue::Hp;
    float barW = 26, barWStructure = 26, barH = 4;
    OvTo to = OvTo::None;
    float toPoint[2] = {0, 0};
    float head = 0.6f;               // arrow head length
    OvStyle style;
    bool hasEvent = false;
    OvEvent event;
    Json source;                     // merged JSON entry (editor round trip)
};

struct OverlaySet {
    std::vector<OverlayDef> defs;        // draw order (order, then position in the list)
    std::vector<std::string> warnings;   // entries that failed to parse (skipped)
};

// Built-in overlays as data (the hard-coded look of earlier versions).
Json builtinOverlays();
// One overlay entry -> definition. Throws std::runtime_error("overlay 'id': ...") on invalid data.
OverlayDef parseOverlay(const Rules& rules, const Json& entry);
// Merge `more` (array of entries) into `list` by id: same id = keys of the new entry win (objects are
// deep-merged; "replace": true replaces the whole entry); new ids are appended.
void mergeOverlayEntries(Json& list, const Json& more);
// builtins + rules "overlays" + game.json "overlays"; invalid entries are skipped with a warning.
OverlaySet loadOverlaySet(const Rules& rules, const Json& rulesOverlays, const Json& gameOverlays);
// Strict check of a whole "overlays" list (array): throws with the first error (unique ids too).
void validateOverlays(const Rules& rules, const Json& list);
// Ready recipes for agents and the editor: [{"id", "name", "description", "overlay"}].
Json overlayTemplates();

// Player slot colours (0 = neutral); RtsView::playerColor returns these.
render::float4 overlayPlayerColor(int player);
render::float4 parseOverlayColor(const Json& j, const std::string& where, bool* player = nullptr);
uint16_t overlayEventKind(const std::string& name);   // 0 = unknown
const char* overlayEventName(uint16_t kind);

} // namespace rts::game
