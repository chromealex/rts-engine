// Grid fields at run time (data: RtsGridFieldData.h, reference: docs/fields.md).
//
// State: the GridFieldStore world resource (present only in games with fields, so other games keep their
// checksums and saves): per field one int32 per cell and layer, a per-chunk count of cells that differ from the
// default ("live" chunks: kernels only visit live chunks and their neighbours, so an idle field costs nothing),
// the cells whose movement blocking is applied to MapGrid::blocked, and per-chunk hashes that are recomputed
// only for chunks written since the last checksum. Rollback copies the resource like any other.
//
// Config: GridFields (owned by RtsModule, read-only after setup except scratch buffers): definitions with
// resolved indices, static fuel masks (terrain / materials), ground heights for flow, emitters per type,
// Lua threshold hooks. Every kernel is integer-only, visits chunks in index order and cells in row-major order,
// reads the previous values and writes a scratch copy (double buffer); random spread uses sim.rng() in that
// fixed order.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "RtsGridFieldData.h"
#include "../../engine/core/ECS.h"
#include "../../engine/core/Fixed.h"

namespace rts { class Simulation; class Hasher; class Archive; }

namespace rts::game {

class RtsModule;
struct MapGrid;
struct EffectDef;
struct EffectCtx;

constexpr int kFieldChunk = 16;   // field cells per chunk side

struct GridFieldStore {
    struct Field {
        int w = 0, h = 0, layers = 1, cw = 0, ch = 0;
        std::vector<int32_t> v;          // layers * w * h, row-major per layer
        std::vector<uint16_t> live;      // per chunk: cells (all layers) != default
        std::vector<uint8_t> blockOn;    // per cell: movement blocking applied (fields with a blockMove effect)
        std::vector<uint64_t> chunkHash; // derived: hash of the chunk's values (valid unless hashDirty)
        std::vector<uint8_t> hashDirty;
        uint32_t version = 0;            // presentation: bumps on every change
        size_t cells() const { return size_t(w) * size_t(h); }
        int chunkOf(int x, int y) const { return (y / kFieldChunk) * cw + x / kFieldChunk; }
    };
    std::vector<Field> fields;
    struct Crossing { uint16_t hook = 0; uint8_t layer = 0; int32_t cell = 0, value = 0; };
    std::vector<Crossing> crossings;     // threshold crossings of this tick (consumed by the scripts phase)
    uint32_t crossTick = 0;
};

class GridFields {
public:
    struct Hook { int16_t field = -1; int32_t value = 0; bool fall = false; };   // Lua on_field_threshold

    bool active() const { return !defs_.empty(); }
    const std::vector<GridFieldDef>& defs() const { return defs_; }
    int find(const std::string& id) const;
    // setup: after setupMap (grid, players) - defs from rules + map, initial values, emitters
    void setup(Simulation& sim, RtsModule& m, const Json& map);
    void tick(Simulation& sim);                                   // the "gridfields" system
    void hash(Simulation& sim, Hasher& h) const;
    void io(Simulation& sim, Archive& a);                         // save states (after RtsSave's own data)
    int addHook(int field, int32_t value, bool fall);             // Lua registration (while scripts load)
    const std::vector<Hook>& hooks() const { return hooks_; }

    // ---- cell access (field cells; layer 0 for plain fields, the player slot for per-player fields)
    GridFieldStore* store(Simulation& sim) const;
    bool cellOf(int field, Fixed x, Fixed z, int& cx, int& cz) const;   // world -> field cell (false: outside)
    Vec3 centerOf(int field, int cx, int cz) const;
    int32_t get(Simulation& sim, int field, int cx, int cz, int layer = 0) const;
    // set / add / max one cell (clamped); returns true when the value changed
    bool write(Simulation& sim, int field, int cx, int cz, int layer, int32_t v, int mode);   // mode 0 set, 1 add, 2 max
    // disc of world radius around (x, z) (radius 0: the one cell); returns cells changed
    int fillCircle(Simulation& sim, int field, Fixed x, Fixed z, Fixed radius, int layer, int32_t v, int mode);
    int fillRect(Simulation& sim, int field, int cx0, int cz0, int cx1, int cz1, int layer, int32_t v, int mode);
    int layerFor(int field, uint8_t owner) const;                 // -1: no layer for this owner
    int layers(int field) const;

    // ---- hooks into other systems
    bool blocksVision() const { return blocksVision_; }
    bool hasSpeed() const { return hasSpeed_; }
    Fixed speedMul(Simulation& sim, Vec3 pos, uint8_t owner) const;   // multiplier of speed effects at pos
    // vision: raise occluder heights (1/64 units, same grid as visionOcc) / reveal cells to players
    void occlude(Simulation& sim, std::vector<int64_t>& occ, const std::vector<int64_t>& base) const;
    void reveal(Simulation& sim, const std::function<void(uint8_t, size_t)>& see) const;
    void applyEffect(Simulation& sim, const EffectDef& e, const EffectCtx& ctx);   // EffectKind::Field

    // perf diagnostics (tests / bench): kernel cells visited since setup
    uint64_t cellsVisited = 0;

private:
    struct Emitter { FieldEmitDef def; };
    void initStore(Simulation& sim);
    void runKernel(Simulation& sim, int fi, const FieldKernelDef& k);
    void decay(GridFieldStore::Field& f, const GridFieldDef& d, const FieldKernelDef& k, int fi, Simulation& sim);
    void diffuse(GridFieldStore::Field& f, const GridFieldDef& d, const FieldKernelDef& k, int fi, Simulation& sim);
    void spread(GridFieldStore::Field& f, const GridFieldDef& d, const FieldKernelDef& k, int fi, Simulation& sim);
    void flow(GridFieldStore::Field& f, const GridFieldDef& d, const FieldKernelDef& k, int fi, Simulation& sim);
    void consume(GridFieldStore::Field& f, const GridFieldDef& d, const FieldKernelDef& k, int fi, Simulation& sim);
    void emit(Simulation& sim);
    void effects(Simulation& sim);
    // writes one cell (index into f.v) through the bookkeeping: live counts, hash, blocking, thresholds
    void put(Simulation& sim, int fi, GridFieldStore::Field& f, size_t i, int32_t nv);
    int32_t clampTo(const GridFieldDef& d, int64_t v) const;
    void workChunks(const GridFieldStore::Field& f, bool ring);   // fills work_ (sorted)
    void updateBlock(Simulation& sim, int fi, GridFieldStore::Field& f, size_t cell);
    int32_t cellMax(const GridFieldStore::Field& f, size_t cell) const;   // over layers
    bool fuelOk(Simulation& sim, int fi, const FieldKernelDef& k, size_t kIndex, size_t cell, int32_t& fuelValue);
    int32_t valueFor(const GridFieldStore::Field& f, const GridFieldDef& d, const FieldEffectDef& e, size_t cell, uint8_t owner, Simulation& sim) const;

    RtsModule* m_ = nullptr;
    std::vector<GridFieldDef> defs_;
    std::vector<std::vector<Emitter>> emitters_;        // per unit type
    std::vector<uint32_t> emitPeriods_;                  // distinct periods (sorted)
    std::vector<std::vector<std::vector<uint8_t>>> fuelMask_;   // [field][kernel]: static fuel per cell (empty: none)
    std::vector<std::vector<int64_t>> ground_;           // [field]: terrain height in field units (flow kernels only)
    std::vector<int> layerCount_;                        // [field]: layers (1, or player slots + 1)
    std::vector<int> blockEffect_;                       // [field]: index of the blockMove effect, -1 none
    std::vector<Hook> hooks_;
    std::vector<std::vector<int>> hooksOf_;              // [field] -> hook indices
    bool blocksVision_ = false, hasSpeed_ = false;
    int mapW_ = 0, mapH_ = 0;
    // scratch (not state)
    std::vector<int32_t> tmp_;
    std::vector<int64_t> flowDelta_;
    std::vector<int32_t> work_;
    std::vector<uint8_t> mark_, tagMask_;
};

} // namespace rts::game
