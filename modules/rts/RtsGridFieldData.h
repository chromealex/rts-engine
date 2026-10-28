// Grid fields (rules "fields", map "gridFields"): per-cell integer / fixed-point layers over the map that the
// simulation updates with deterministic C++ kernels (decay, diffuse, spread, flow, consume), fed by unit
// emitters and "field" effects, read by per-cell effects on units (damage, speed, statuses, yield) and by
// pathing / line of sight / fog. Fire, water, lava, poison gas, creep, influence, noise... are data.
// Implementation: RtsGridFields.cpp (kernels, store, effects), RtsGridFieldsApi.cpp (Lua `field` table),
// RtsGridFieldData.cpp (parsing). Reference: docs/fields.md.
//
// Values are int32 per field cell: "int" fields store integers, "fixed" fields store Q16.16 raw values
// (Fixed::raw clamped to 32 bits: |v| < 32768). Every parameter below is stored in the field's raw units.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../engine/core/Fixed.h"
#include "../../engine/data/Json.h"

namespace rts::game {

enum class FieldKernelKind : uint8_t { Decay, Diffuse, Spread, Flow, Consume };

// What makes a neighbour cell catch (spread kernel): any condition listed must hold.
struct FieldFuelDef {
    int16_t field = -1;                 // value of this field >= fieldMin
    int32_t fieldMin = 1;
    std::vector<uint8_t> materials;     // map terrainMaterial index of the cell is one of these
    std::string terrain;                // map terrain characters ('.', ',' ...): the cell's char is one of these
    uint64_t tags = 0;                  // a unit with one of these type tags stands in the cell (trees)
    bool any() const { return field >= 0 || !materials.empty() || !terrain.empty() || tags; }
};

struct FieldKernelDef {
    FieldKernelKind kind = FieldKernelKind::Decay;
    uint32_t period = 5, phase = 0;     // runs when (tick - phase) % period == 0
    // decay: toward the default by `amount` raw units per run and / or `percent` (Q16 share of the distance)
    int32_t amount = 0;
    int32_t percent = 0;
    // diffuse / flow: share (Q16, 0..1) of the difference exchanged per run; eight: 8 neighbours instead of 4
    int32_t rate = 0;
    bool eight = false;
    // spread: cells >= above ignite neighbours below `below` (default: above) to max(their value, set)
    int32_t above = 1, below = 1, set = 1;
    int32_t chance = 0;                 // Q16 probability per neighbour and run (0: deterministic threshold)
    int32_t fuelChance = 0;             // Q16 added per raw unit of the fuel field of the target cell
    FieldFuelDef fuel;
    // flow: field units per world unit of terrain height (water depth vs ground)
    int64_t unitsPerHeight = 65536;
    int32_t minDepth = 0;               // cells shallower than this don't flow
    // consume: cells >= above eat `amount` of `fuelField` per run; when it runs out the cell becomes `out`
    int16_t fuelField = -1;
    int32_t out = 0;
};

enum class FieldEffectKind : uint8_t { Damage, Speed, BlockMove, BlockVision, Status, Yield, Reveal };
enum class FieldWho : uint8_t { Any, Enemies, Own };   // per-player fields: whose layer counts for a unit / cell

struct FieldEffectDef {
    FieldEffectKind kind = FieldEffectKind::Damage;
    int32_t above = 1;                  // active where value >= above
    uint32_t period = 10;               // ticks between applications (damage, status, yield)
    Fixed perSecond{};                  // damage / yield per second
    uint16_t damageType = 0;
    Fixed mul = Fixed::fromInt(1);      // speed multiplier
    Fixed height = Fixed::fromInt(2);   // vision blocker height
    int16_t status = -1;
    Fixed seconds{};                    // status duration (0: two periods, refreshed while inside)
    int16_t resource = -1;
    bool scaled = false;                // damage / yield * value / above (stronger in hotter cells)
    FieldWho who = FieldWho::Any;
    uint64_t tags = 0, notTags = 0;     // units: only / except types with these tags
    bool units = true, structures = true;
    bool all = false;                   // reveal: to every player (else the layer's owner / everyone for plain fields)
};

struct FieldViewDef { Json look; };     // presentation keys ("view"), read by modules/rts/view/RtsFieldView

struct GridFieldDef {
    std::string id;
    bool fixed = false;                 // "type": "fixed"
    int resolution = 1;                 // map cells per field cell (1, 2, 4, 8)
    int32_t def = 0, min = INT32_MIN, max = INT32_MAX;   // raw units
    bool perPlayer = false;             // one layer per player slot (influence, creep)
    std::vector<FieldKernelDef> kernels;
    std::vector<FieldEffectDef> effects;
    Json view;
    Json initial;                       // map "gridFields".<id>: "values" RLE / "fill" (applied at setup)
    int32_t raw(Fixed v) const;         // Fixed -> raw units of this field (int: integer part, fixed: Q16)
    Fixed toFixed(int32_t v) const;     // raw -> Fixed
};

// Type block "field": {"<field>": {"emit": n, "radius": r, "mode": "add" | "max" | "set", "period": ticks}}
struct FieldEmitDef {
    int16_t field = -1;
    int32_t amount = 0;                 // raw units
    Fixed radius{};                     // world units (0: the cell under the unit)
    uint8_t mode = 0;                   // 0 add, 1 max, 2 set
    uint32_t period = 10;
    bool whileBuilding = false;         // structures under construction emit too
};

struct Rules;
// rules "fields" (and the "field" blocks of types); called by loadRules after types and effects are parsed.
void loadGridFieldRules(Rules& r, const Json& j);
// Applies map "gridFields" (parameter overrides, new fields, initial values) to a copy of the rules' fields.
void applyMapGridFields(std::vector<GridFieldDef>& defs, const Json& map, const Rules& r);
// One field definition from JSON (rules or map); `base` = defaults (an existing definition being overridden).
GridFieldDef parseGridField(const std::string& id, const Json& j, const Rules& r, const std::vector<GridFieldDef>& all, const GridFieldDef* base);

// Initial values of a field from its map entry (GridFieldDef::initial: "values" RLE / {"<layer>": RLE}, "fill"
// rects / circles): layers * cells values (the default elsewhere). Shared by the simulation setup and the editor.
bool buildInitialField(const GridFieldDef& d, int mapW, int mapH, int layers, std::vector<int32_t>& v, std::string* err);
inline int fieldCellsW(const GridFieldDef& d, int mapW) { return (mapW + d.resolution - 1) / d.resolution; }

// Run-length text encoding of a field layer (maps, editor): "v*n v v*n ..." over row-major cells.
std::string encodeFieldRle(const std::vector<int32_t>& v);
bool decodeFieldRle(const std::string& s, size_t n, std::vector<int32_t>& out);

} // namespace rts::game
