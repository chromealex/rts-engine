// Vision / fog of war (docs/terrain.md § Vision, docs/planets.md §3). Every 5 ticks: cells lit last pass fall back
// to "explored", then every unit lights the cells it sees for its owner and the allies sharing vision.
//
// The result is defined by the brute-force reference (visionFlatRef / visionSphereRef below: every unit marches a
// line of sight to every cell of its disk, every pass). The fast path computes exactly the same cells, in the same
// order (visibleCells is saved), with derived caches that never enter the hashed / saved state:
// - per unit: the list of cells it sees, kept while its inputs stay the same (flat: cell, sight, type; planets: exact
//   position, sight, type) and no cell it read changed (16 x 16 cell tiles marked dirty when a ground / occluder /
//   cliff level / dig cell changes). Replaying a list = the reference's see() calls in the reference's order. On planets
//   the reference skips cells the owner already sees this pass before testing them; replay applies the same skip,
//   so the outcome is identical (non-visible cells never call see()). Planet units build their list on the second
//   pass at the same position (a moving unit takes the reference path: the skip makes one-off passes cheaper).
// - flat ground / occluder heights (GameState::visionBase / visionOcc) are updated incrementally from the cells whose
//   corners, levels, dig state or blockers changed instead of being rebuilt over the whole map every pass.
// - flat line-of-sight rays: per sight radius a table of the sample cell offsets (no divisions in the march).
// - planets: cell centres cached per cell, distance tests on squared lengths (exact equivalent of the sqrt compare).
// The cache is tied to a GameState by a token: a snapshot restore, a loaded save or another simulation starts it
// over (full rebuild), so rollback / load can never see stale entries.
#include "RtsInternal.h"
#include "engine/core/FixedMath.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rts::game {
using namespace detail;

struct VisionCache {
    static constexpr int kTile = 16;
    uint64_t token = 0;
    int W = 0, H = 0, TW = 0, TH = 0;
    bool sphere = false;
    std::vector<uint8_t> dirty;          // per tile, this pass
    std::vector<int32_t> dirtyList;
    // inputs of the last pass (flat)
    std::vector<int16_t> corner;
    std::vector<int8_t> level;
    std::vector<uint8_t> dig;
    std::vector<std::pair<int32_t, int64_t>> raises, raisesNew;   // flat blockers: (cell, height in 1/64)
    std::vector<std::pair<int32_t, Fixed>> sraises, sraisesNew;   // planet blockers
    std::vector<int32_t> touch;                                    // cells to re-evaluate this pass
    // planets
    std::vector<Fixed> blockers, ground;
    std::vector<Vec3> center;
    std::vector<uint8_t> centerOk;
    // flat line-of-sight rays per radius: targets in the reference's order, sample offsets (sdy * W + sdx)
    struct Ray { int16_t dx, dy; int32_t k; uint32_t first; };
    struct Table { int W = 0; bool built = false; std::vector<Ray> rays; std::vector<int32_t> off; };
    std::vector<Table> tables;
    struct Entry {
        uint32_t gen = UINT32_MAX;
        uint8_t state = 0;               // 0 none, 1 key known (planets: seen once), 2 cells valid
        uint16_t type = 0;
        int32_t cx = 0, cy = 0, r = 0;   // flat key
        Vec3 pos{};                      // planet key
        Fixed sight{};
        int32_t tx0 = 0, ty0 = 0, tx1 = -1, ty1 = -1;   // flat: tiles read
        std::vector<int32_t> cells;      // cells seen, in the reference's order
        std::vector<int32_t> tiles;      // planets: tiles read (sorted, unique)
    };
    std::vector<Entry> entries;          // by entity index

    void reset(int w, int h, bool sph) {
        W = w; H = h; sphere = sph;
        TW = (w + kTile - 1) / kTile; TH = (h + kTile - 1) / kTile;
        dirty.assign(size_t(TW) * size_t(TH), 0); dirtyList.clear();
        entries.clear(); raises.clear(); sraises.clear();
        corner.clear(); level.clear(); dig.clear(); blockers.clear(); ground.clear(); center.clear(); centerOk.clear();
    }
    void clearDirty() { for (int32_t t : dirtyList) dirty[size_t(t)] = 0; dirtyList.clear(); }
    int tileOf(int c) const { return (c / W / kTile) * TW + (c % W) / kTile; }
    void markDirty(int c) { const int t = tileOf(c); if (!dirty[size_t(t)]) { dirty[size_t(t)] = 1; dirtyList.push_back(t); } }
    Entry& entry(Entity e) { if (entries.size() <= e.index) entries.resize(size_t(e.index) + 1); return entries[e.index]; }
    const Table& table(int r) {
        if (tables.size() <= size_t(r)) tables.resize(size_t(r) + 1);
        Table& t = tables[size_t(r)];
        if (t.built && t.W == W) return t;
        t = Table{}; t.W = W; t.built = true;
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (dx * dx + dy * dy > r * r) continue;
                const int k = std::max(std::abs(dx), std::abs(dy));
                t.rays.push_back({int16_t(dx), int16_t(dy), k, uint32_t(t.off.size())});
                for (int st = 1; st < k; ++st) {   // the reference's sample cells, relative to the eye
                    const int sx = (dx * st * 2 + (dx >= 0 ? k : -k)) / (2 * k), sy = (dy * st * 2 + (dy >= 0 ? k : -k)) / (2 * k);
                    t.off.push_back(sy * W + sx);
                }
            }
        return t;
    }
};

namespace {
std::atomic<uint64_t> gVisionToken{0};
int visionDebugMode() {   // RTS_VISION=ref: reference only, RTS_VISION=check: both, abort on a difference
    static const int m = [] { const char* v = std::getenv("RTS_VISION"); return !v ? 0 : std::strcmp(v, "ref") == 0 ? 1 : std::strcmp(v, "check") == 0 ? 2 : 0; }();
    return m;
}
// fxSqrt(L) > s without the square root (exact: fxSqrt = floor(sqrt(L.raw << 16)) below 2^47)
inline bool sqrtGreater(Fixed L, Fixed s) {
    if (L.raw <= 0) return 0 > s.raw;
    if (s.raw < 0) return true;
    if (L.raw >= (int64_t(1) << 47) || s.raw >= (int64_t(1) << 31) - 1) return fxSqrt(L) > s;
    const int64_t s1 = s.raw + 1;
    return (L.raw << Fixed::kFracBits) >= s1 * s1;
}
} // namespace

void RtsModule::setVisionReference(bool on) { visionRef_ = on; }

void RtsModule::sysVision(Simulation& sim) {
    if (sim.tick() % 5 != 0) return;
    GameState& gs = state(sim);
    if (gs.fogMode == uint8_t(FogMode::None)) return;   // everything stays visible
    const size_t np = gs.vision.size();
    // visible -> explored: only the cells lit last time (a whole-map sweep is too slow on 1024x1024 maps)
    gs.visibleCells.resize(np);
    for (size_t p = 1; p < np; ++p) {
        for (int32_t i : gs.visibleCells[p]) gs.vision[p][size_t(i)] = 1;
        gs.visibleCells[p].clear();
    }
    const int dbg = visionRef_ ? 1 : visionDebugMode();
    if (dbg == 1) { if (gs.surface) visionSphereRef(sim); else visionFlatRef(sim); return; }
    if (dbg == 2) {   // check: fast, then the reference from the same start; any difference aborts
        const auto vis0 = gs.vision; const auto cells0 = gs.visibleCells;
        const auto base0 = gs.visionBase, occ0 = gs.visionOcc;
        if (gs.surface) visionSphereFast(sim); else visionFlatFast(sim);
        const auto vis1 = gs.vision; const auto cells1 = gs.visibleCells;
        const auto base1 = gs.visionBase, occ1 = gs.visionOcc;
        const uint64_t tok = gs.visionToken;
        gs.vision = vis0; gs.visibleCells = cells0; gs.visionBase = base0; gs.visionOcc = occ0;
        if (gs.surface) visionSphereRef(sim); else visionFlatRef(sim);
        gs.visionToken = tok;
        if (gs.vision != vis1 || gs.visibleCells != cells1 || gs.visionBase != base1 || gs.visionOcc != occ1) {
            std::fprintf(stderr, "vision check: fast != reference at tick %u (vision %d, cells %d, base %d, occ %d)\n", sim.tick(),
                         int(gs.vision != vis1), int(gs.visibleCells != cells1), int(gs.visionBase != base1), int(gs.visionOcc != occ1));
            std::abort();
        }
        return;
    }
    if (gs.surface) visionSphereFast(sim); else visionFlatFast(sim);
}

// ----------------------------------------------------------------- flat maps

void RtsModule::visionFlatFast(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const MapGrid& g = gs.grid;
    const int W = g.w, H = g.h;
    const size_t n = size_t(W) * size_t(H);
    const size_t np = gs.vision.size();
    std::vector<std::vector<uint8_t>> sharers(np);
    for (size_t p = 1; p < np; ++p) for (size_t q = 1; q < np; ++q) if (q == p || (rules_.fog.shareAllies && sim.allied(uint8_t(p), uint8_t(q)))) sharers[p].push_back(uint8_t(q));
    const bool cliffs = rules_.heights.blockVision;
    const bool los = g.hf && cliffs;
    auto sumUnits = [](Fixed v) { return (v.raw * 64) >> Fixed::kFracBits; };
    const int64_t eyeUp = sumUnits(rules_.terrain.eyeHeight), targetUp = sumUnits(rules_.terrain.targetHeight);
    auto cellSum = [&](int x, int y) {
        return int64_t(g.corner[size_t(g.cornerIndex(x, y))]) + g.corner[size_t(g.cornerIndex(x + 1, y))] +
               g.corner[size_t(g.cornerIndex(x, y + 1))] + g.corner[size_t(g.cornerIndex(x + 1, y + 1))];
    };
    auto see = [&](uint8_t owner, size_t i) {
        for (uint8_t q : sharers[owner]) {
            uint8_t& v = gs.vision[q][i];
            if (v != 2) { v = 2; gs.visibleCells[q].push_back(int32_t(i)); }
        }
    };
    const bool losAll = los || rules_.fog.lineOfSight;
    if (!vcache_) vcache_ = std::make_shared<VisionCache>();
    VisionCache& vc = *vcache_;
    std::vector<int64_t>& base = gs.visionBase;
    std::vector<int64_t>& occ = gs.visionOcc;
    const int64_t rock = sumUnits(rules_.fog.rockHeight);
    const bool useDig = losAll && rock > 0 && g.dig.size() == n;
    const bool fresh = gs.visionToken == 0 || gs.visionToken != vc.token || vc.sphere || vc.W != W || vc.H != H ||
                       vc.level.size() != g.level.size() || (losAll && (base.size() != n || occ.size() != n || vc.dig.size() != (useDig ? n : 0) ||
                                                                         vc.corner.size() != (g.hf ? g.corner.size() : 0)));
    // dynamic blockers of this pass (types with vision.blocks, smoke areas): (cell, height), any order
    vc.raisesNew.clear();
    if (losAll) {
        auto raise = [&](int x, int y, int64_t h) { if (x >= 0 && y >= 0 && x < W && y < H) vc.raisesNew.push_back({y * W + x, h}); };
        w.each<Unit, Transform>([&](Entity, Unit& u, Transform& tr) {
            const UnitType& t = rules_.types[u.type];
            if (t.visionBlock.raw <= 0 || u.hp.raw <= 0) return;
            const int64_t h = sumUnits(t.visionBlock);
            if (t.structure) {
                const Fixed half = Fixed::fromRatio(1, 2);
                const int x0 = int((tr.pos.x - Fixed::fromInt(t.footprintW) / 2 + half).floorInt()), y0 = int((tr.pos.z - Fixed::fromInt(t.footprintH) / 2 + half).floorInt());
                for (int y = 0; y < t.footprintH; ++y) for (int x = 0; x < t.footprintW; ++x) raise(x0 + x, y0 + y, h);
            } else {
                raise(int(tr.pos.x.floorInt()), int(tr.pos.z.floorInt()), h);
            }
        });
        if (rules_.advancedEffects)
            w.each<Area, Transform>([&](Entity, Area& a, Transform& tr) {
                if (a.effList < 0 || sim.tick() < a.fireAt) return;
                const EffectDef& ef = rules_.payloads[size_t(a.effList)][size_t(a.effIndex)];
                if (ef.delivery.blocksVision.raw <= 0) return;
                const int64_t h = sumUnits(ef.delivery.blocksVision);
                const int rr = int(a.radius.floorInt()), cx = int(tr.pos.x.floorInt()), cy = int(tr.pos.z.floorInt());
                for (int y = cy - rr; y <= cy + rr; ++y)
                    for (int x = cx - rr; x <= cx + rr; ++x)
                        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= rr * rr) raise(x, y, h);
            });
        std::sort(vc.raisesNew.begin(), vc.raisesNew.end());
    }
    auto occOf = [&](int32_t i) {   // occluder height of a cell: ground, undug rock, the highest blocker on it
        int64_t o = base[size_t(i)];
        if (useDig && g.dig[size_t(i)]) o = std::max(o, base[size_t(i)] + rock);
        auto it = std::lower_bound(vc.raisesNew.begin(), vc.raisesNew.end(), std::pair<int32_t, int64_t>{i, INT64_MIN});
        for (; it != vc.raisesNew.end() && it->first == i; ++it) o = std::max(o, base[size_t(i)] + it->second);
        return o;
    };
    if (fresh) {
        vc.reset(W, H, false);
        vc.level = g.level;
        if (losAll) {
            if (g.hf) vc.corner = g.corner;
            if (useDig) vc.dig = g.dig;
            base.assign(n, 0);
            const int64_t lv = sumUnits(rules_.levelHeight);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    base[size_t(y * W + x)] = g.hf ? cellSum(x, y) : int64_t(g.level[size_t(y * W + x)]) * lv;
            occ = base;
            if (useDig) for (size_t i = 0; i < n; ++i) if (g.dig[i]) occ[i] = std::max(occ[i], base[i] + rock);
            for (auto& [c, h] : vc.raisesNew) occ[size_t(c)] = std::max(occ[size_t(c)], base[size_t(c)] + h);
        }
    } else {
        vc.clearDirty();
        vc.touch.clear();
        // cliff levels (the level branch reads them; LOS on cliff maps derives the ground from them)
        if (std::memcmp(vc.level.data(), g.level.data(), g.level.size()) != 0) {
            const int64_t lv = sumUnits(rules_.levelHeight);
            for (size_t i = 0; i < g.level.size(); ++i)
                if (vc.level[i] != g.level[i]) {
                    vc.level[i] = g.level[i];
                    vc.markDirty(int(i));
                    if (losAll && !g.hf) { base[i] = int64_t(g.level[i]) * lv; vc.touch.push_back(int32_t(i)); }
                }
        }
        if (losAll) {
            if (g.hf && std::memcmp(vc.corner.data(), g.corner.data(), g.corner.size() * sizeof(int16_t)) != 0) {
                const int CW = W + 1;
                for (size_t ci = 0; ci < g.corner.size(); ++ci) {
                    if (vc.corner[ci] == g.corner[ci]) continue;
                    vc.corner[ci] = g.corner[ci];
                    const int x = int(ci) % CW, y = int(ci) / CW;
                    for (int yy = y - 1; yy <= y; ++yy)
                        for (int xx = x - 1; xx <= x; ++xx)
                            if (xx >= 0 && yy >= 0 && xx < W && yy < H) vc.touch.push_back(yy * W + xx);
                }
                for (int32_t i : vc.touch) {
                    const int64_t b = cellSum(i % W, i / W);
                    if (b != base[size_t(i)]) { base[size_t(i)] = b; vc.markDirty(i); }
                }
            }
            if (useDig && std::memcmp(vc.dig.data(), g.dig.data(), n) != 0)
                for (size_t i = 0; i < n; ++i) if (vc.dig[i] != g.dig[i]) { vc.dig[i] = g.dig[i]; vc.touch.push_back(int32_t(i)); }
            if (vc.raises != vc.raisesNew) {
                for (auto& r : vc.raises) vc.touch.push_back(r.first);
                for (auto& r : vc.raisesNew) vc.touch.push_back(r.first);
            }
            for (int32_t i : vc.touch) {
                const int64_t o = occOf(i);
                if (o != occ[size_t(i)]) { occ[size_t(i)] = o; vc.markDirty(i); }
            }
        }
        if (!vc.dirtyList.empty())
            for (auto& en : vc.entries) {
                if (en.state != 2) continue;
                bool hit = false;
                for (int ty = en.ty0; ty <= en.ty1 && !hit; ++ty)
                    for (int tx = en.tx0; tx <= en.tx1 && !hit; ++tx) hit = vc.dirty[size_t(ty * vc.TW + tx)] != 0;
                if (hit) en.state = 1;
            }
    }
    std::swap(vc.raises, vc.raisesNew);
    vc.token = gs.visionToken = ++gVisionToken;

    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& tr) {
        if (o.player == 0 || o.player >= np || (u.flags & kUnitOffMap)) return;
        const UnitType& t = rules_.types[u.type];
        const int r = int(stat(sim, e, Stat::Sight).toInt());
        int cx, cy; g.cellXY(tr.pos, cx, cy);
        VisionCache::Entry& en = vc.entry(e);
        if (!(en.state == 2 && en.gen == e.gen && en.cx == cx && en.cy == cy && en.r == r && en.type == u.type)) {
            en.gen = e.gen; en.cx = cx; en.cy = cy; en.r = r; en.type = u.type; en.state = 2;
            en.cells.clear();
            const int rr = std::max(r, 0) + 1;   // tiles of every cell read (eye, targets, samples)
            en.tx0 = std::clamp(cx - rr, 0, W - 1) / VisionCache::kTile; en.tx1 = std::clamp(cx + rr, 0, W - 1) / VisionCache::kTile;
            en.ty0 = std::clamp(cy - rr, 0, H - 1) / VisionCache::kTile; en.ty1 = std::clamp(cy + rr, 0, H - 1) / VisionCache::kTile;
            const bool air = (t.layer & kLayerAir) != 0;
            if (losAll && !air && !t.seesOver && g.inside(cx, cy)) {
                const int64_t eye = base[size_t(cy * W + cx)] + eyeUp + sumUnits(t.eyeHeight);
                const int c0 = cy * W + cx;
                if (r >= 0 && r <= 96) {
                    const VisionCache::Table& tb = vc.table(r);
                    for (const VisionCache::Ray& ry : tb.rays) {
                        const int x = cx + ry.dx, y = cy + ry.dy;
                        if (x < 0 || y < 0 || x >= W || y >= H) continue;
                        const int k = ry.k;
                        const int64_t tgt = base[size_t(y * W + x)] + targetUp;
                        const int32_t* off = tb.off.data() + ry.first;
                        bool visible = true;
                        for (int st = 1; st < k; ++st)
                            if (occ[size_t(c0 + off[st - 1])] * k > eye * (k - st) + tgt * st) { visible = false; break; }
                        if (visible) en.cells.push_back(y * W + x);
                    }
                } else {
                    for (int y = std::max(0, cy - r); y <= std::min(H - 1, cy + r); ++y)
                        for (int x = std::max(0, cx - r); x <= std::min(W - 1, cx + r); ++x) {
                            int dx = x - cx, dy = y - cy;
                            if (dx * dx + dy * dy > r * r) continue;
                            const int k = std::max(std::abs(dx), std::abs(dy));
                            const int64_t tgt = base[size_t(y * W + x)] + targetUp;
                            bool visible = true;
                            for (int st = 1; st < k && visible; ++st) {
                                int sx = cx + (dx * st * 2 + (dx >= 0 ? k : -k)) / (2 * k), sy = cy + (dy * st * 2 + (dy >= 0 ? k : -k)) / (2 * k);
                                if (occ[size_t(sy * W + sx)] * k > eye * (k - st) + tgt * st) visible = false;
                            }
                            if (visible) en.cells.push_back(y * W + x);
                        }
                }
            } else {
                // eyes on a ramp count as the lower level; air units see over cliffs
                const int eye = air || !cliffs ? 127 : g.levelAt(cx, cy);
                for (int y = std::max(0, cy - r); y <= std::min(H - 1, cy + r); ++y)
                    for (int x = std::max(0, cx - r); x <= std::min(W - 1, cx + r); ++x) {
                        int dx = x - cx, dy = y - cy;
                        if (dx * dx + dy * dy > r * r) continue;
                        const size_t i = size_t(y * W + x);
                        if (g.level[i] > eye) continue;
                        en.cells.push_back(int32_t(i));
                    }
            }
        }
        for (int32_t i : en.cells) see(o.player, size_t(i));
    });
    if (rules_.advancedEffects)  // reveal areas (spy, scouting flares)
        w.each<Area, Owner, Transform>([&](Entity, Area& a, Owner& o, Transform& tr) {
            if (o.player == 0 || o.player >= np || a.effList < 0) return;
            const EffectDef& e = rules_.payloads[size_t(a.effList)][size_t(a.effIndex)];
            Fixed sight = e.delivery.sight.raw > 0 ? e.delivery.sight : e.kind == EffectKind::Reveal ? a.radius : Fixed{};
            if (sight.raw <= 0 || sim.tick() < a.fireAt) return;
            int r = int(sight.floorInt());
            int cx, cy; g.cellXY(tr.pos, cx, cy);
            for (int y = std::max(0, cy - r); y <= std::min(H - 1, cy + r); ++y)
                for (int x = std::max(0, cx - r); x <= std::min(W - 1, cx + r); ++x)
                    if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r)
                        see(o.player, size_t(y * W + x));
        });
}

// ----------------------------------------------------------------- planets

void RtsModule::visionSphereFast(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const MapGrid& g = gs.grid;
    const Surface& S = *gs.surface;
    const size_t np = gs.vision.size();
    const size_t n = g.flags.size();
    std::vector<std::vector<uint8_t>> sharers(np);
    for (size_t p = 1; p < np; ++p) for (size_t q = 1; q < np; ++q) if (q == p || (rules_.fog.shareAllies && sim.allied(uint8_t(p), uint8_t(q)))) sharers[p].push_back(uint8_t(q));
    auto see = [&](uint8_t owner, size_t i) {
        for (uint8_t q : sharers[owner]) {
            uint8_t& v = gs.vision[q][i];
            if (v != 2) { v = 2; gs.visibleCells[q].push_back(int32_t(i)); }
        }
    };
    const bool los = (g.hf && rules_.heights.blockVision) || rules_.fog.lineOfSight;
    if (!vcache_) vcache_ = std::make_shared<VisionCache>();
    VisionCache& vc = *vcache_;
    const Fixed rock = rules_.fog.rockHeight;
    const bool useDig = los && rock.raw > 0;
    const bool fresh = gs.visionToken == 0 || gs.visionToken != vc.token || !vc.sphere || vc.W != g.w || vc.H != g.h ||
                       vc.center.size() != n || (los && (vc.blockers.size() != n || vc.dig.size() != (useDig ? g.dig.size() : 0)));
    // vision blockers of this pass: structures with vision.blocks over their footprint (or their cell)
    vc.sraisesNew.clear();
    if (los) {
        w.each<Unit, Transform>([&](Entity e, Unit& u, Transform& tr) {
            const UnitType& t = rules_.types[u.type];
            if (t.visionBlock.raw <= 0 || u.hp.raw <= 0) return;
            if (auto* f = w.tryGet<Footprint>(e); f && f->x >= 0) {
                for (int y = 0; y < f->h; ++y) for (int x = 0; x < f->w; ++x) vc.sraisesNew.push_back({(f->y + y) * g.w + f->x + x, t.visionBlock});
            } else if (const int c = g.cellOf(tr.pos); c >= 0) vc.sraisesNew.push_back({c, t.visionBlock});
        });
        std::sort(vc.sraisesNew.begin(), vc.sraisesNew.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first < b.first : a.second.raw < b.second.raw; });
    }
    auto blockOf = [&](int32_t i) {   // undug rock, then the highest blocker on the cell
        Fixed b{};
        if (useDig && size_t(i) < g.dig.size() && g.dig[size_t(i)]) b = rock;
        auto it = std::lower_bound(vc.sraisesNew.begin(), vc.sraisesNew.end(), i, [](auto& a, int32_t c) { return a.first < c; });
        for (; it != vc.sraisesNew.end() && it->first == i; ++it) b = fxMax(b, it->second);
        return b;
    };
    if (fresh) {
        vc.reset(g.w, g.h, true);
        vc.center.assign(n, Vec3{}); vc.centerOk.assign(n, 0);
        if (los) {
            vc.blockers.assign(n, Fixed{}); vc.ground.assign(n, Fixed{});
            if (useDig) { vc.dig = g.dig; for (size_t i = 0; i < g.dig.size(); ++i) if (g.dig[i]) vc.blockers[i] = rock; }
            for (auto& [c, h] : vc.sraisesNew) vc.blockers[size_t(c)] = fxMax(vc.blockers[size_t(c)], h);
        }
    } else if (los) {
        vc.clearDirty();
        vc.touch.clear();
        if (useDig && std::memcmp(vc.dig.data(), g.dig.data(), g.dig.size()) != 0)
            for (size_t i = 0; i < g.dig.size(); ++i) if (vc.dig[i] != g.dig[i]) { vc.dig[i] = g.dig[i]; vc.touch.push_back(int32_t(i)); }
        if (vc.sraises != vc.sraisesNew) {
            for (auto& r : vc.sraises) vc.touch.push_back(r.first);
            for (auto& r : vc.sraisesNew) vc.touch.push_back(r.first);
        }
        for (int32_t i : vc.touch) {
            const Fixed b = blockOf(i);
            if (b != vc.blockers[size_t(i)]) { vc.blockers[size_t(i)] = b; vc.ground[size_t(i)] = Fixed{}; vc.markDirty(i); }
        }
        if (!vc.dirtyList.empty())
            for (auto& en : vc.entries) {
                if (en.state != 2) continue;
                for (int32_t t : en.tiles) if (vc.dirty[size_t(t)]) { en.state = 1; break; }
            }
    }
    std::swap(vc.sraises, vc.sraisesNew);
    vc.token = gs.visionToken = ++gVisionToken;

    std::vector<int32_t> cells;
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& tr) {
        if (o.player == 0 || o.player >= np || (u.flags & kUnitInHand)) return;
        const UnitType& t = rules_.types[u.type];
        const Fixed sight = stat(sim, e, Stat::Sight);
        const int body = S.bodyAt(tr.pos);
        if (!S.bodies[size_t(body)].hasSurface()) return;
        const std::vector<uint8_t>& mine = gs.vision[o.player];
        VisionCache::Entry& en = vc.entry(e);
        const bool same = en.state != 0 && en.gen == e.gen && en.type == u.type && en.sight == sight && en.pos.x == tr.pos.x && en.pos.y == tr.pos.y && en.pos.z == tr.pos.z;
        if (same && en.state == 2) {   // replay (with the reference's skip of cells already lit for the owner)
            for (int32_t c : en.cells) if (mine[size_t(c)] != 2) see(o.player, size_t(c));
            return;
        }
        // full = build the list of every visible cell (second pass at the same position), else the reference's pass
        const bool full = same;
        en.gen = e.gen; en.type = u.type; en.sight = sight; en.pos = tr.pos; en.state = full ? 2 : 1;
        en.cells.clear(); en.tiles.clear();
        const int c = S.cellAt(tr.pos);
        const Fixed cs = S.cellSize(body);
        const int k = int((sight / (cs * Fixed::fromRatio(8, 10))).floorInt()) + 1;
        const bool air = (t.layer & kLayerAir) != 0;
        const Vec3 o3 = Surface::anchor(body);
        const Vec3 eye = air || t.seesOver ? tr.pos : S.project(tr.pos, rules_.terrain.eyeHeight + t.eyeHeight);
        const Fixed R = S.bodies[size_t(body)].radius;
        const CubeSphere& cube = S.cubeOf(body);
        cells.clear();
        const int M = cube.faceSize();
        if (k >= M) { for (int i = 0; i < 6 * M * M; ++i) { const SphereCell sc = cube.cell(i); cells.push_back(S.gridCell(body, sc.face, sc.x, sc.y)); } }
        else
            for (int dy = -k; dy <= k; ++dy)
                for (int dx = -k; dx <= k; ++dx) {
                    int nn = S.offset(c, dx, dy);
                    if (nn < 0) { const int mid = S.offset(c, 0, dy); nn = mid < 0 ? -1 : S.offset(mid, dx, 0); }
                    if (nn >= 0) cells.push_back(nn);
                }
        const bool march = los && !air && !t.seesOver;
        for (int32_t nc : cells) {
            if (!full && mine[size_t(nc)] == 2) continue;   // already lit for this owner (and its sharers) this pass
            if (!vc.centerOk[size_t(nc)]) { vc.center[size_t(nc)] = S.cellCenter(nc); vc.centerOk[size_t(nc)] = 1; }
            const Vec3 cc = vc.center[size_t(nc)];
            const Fixed L = (cc - tr.pos).lengthSq();
            if (sqrtGreater(L, sight)) continue;
            if (march) {
                // march the 3D sight line in radial terms: at each sample the line's distance from the centre must
                // stay above the ground radius of the cell under it (hills and the horizon)
                const Vec3 tgt = cc + S.cellDir(nc) * rules_.terrain.targetHeight;
                const int steps = int((fxSqrt(L) / cs).floorInt()) + 1;   // about one sample per cell
                const Vec3 d = tgt - eye;
                bool visible = true;
                for (int st = 1; st < steps && visible; ++st) {
                    const Vec3 p = eye + d * Fixed::fromRatio(st, steps);
                    const Vec3 l = p - o3;
                    const SphereCell sc = cube.cell(cube.cellFromDir(l));
                    const int gc = S.gridCell(body, sc.face, sc.x, sc.y);
                    if (full) en.tiles.push_back(vc.tileOf(gc));
                    Fixed& gr = vc.ground[size_t(gc)];
                    if (gr.raw == 0) gr = R + S.cellHeight(gc) + vc.blockers[size_t(gc)] + Fixed::fromRaw(1);
                    if (l.lengthSq() < gr * gr) visible = false;
                }
                if (!visible) continue;
            }
            if (full) en.cells.push_back(nc); else see(o.player, size_t(nc));
        }
        if (full) {
            std::sort(en.tiles.begin(), en.tiles.end());
            en.tiles.erase(std::unique(en.tiles.begin(), en.tiles.end()), en.tiles.end());
            for (int32_t nc : en.cells) if (mine[size_t(nc)] != 2) see(o.player, size_t(nc));
        }
    });
}

// ----------------------------------------------------------------- reference (brute force)

// Reference (brute force, the pre-cache algorithm): every unit recomputes its whole disk every pass.
void RtsModule::visionFlatRef(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const MapGrid& g = gs.grid;
    const int W = g.w, H = g.h;
    const size_t np = gs.vision.size();
    std::vector<std::vector<uint8_t>> sharers(np);
    for (size_t p = 1; p < np; ++p) for (size_t q = 1; q < np; ++q) if (q == p || (rules_.fog.shareAllies && sim.allied(uint8_t(p), uint8_t(q)))) sharers[p].push_back(uint8_t(q));
    const bool cliffs = rules_.heights.blockVision;
    const bool los = g.hf && cliffs;   // heightfield: real line of sight over the surface
    // heights in corner-sum units (4 corners of 1/16 = 1/64 units) so the ray test stays integer
    auto sumUnits = [](Fixed v) { return (v.raw * 64) >> Fixed::kFracBits; };
    const int64_t eyeUp = sumUnits(rules_.terrain.eyeHeight), targetUp = sumUnits(rules_.terrain.targetHeight);
    auto cellSum = [&](int x, int y) {
        return int64_t(g.corner[size_t(g.cornerIndex(x, y))]) + g.corner[size_t(g.cornerIndex(x + 1, y))] +
               g.corner[size_t(g.cornerIndex(x, y + 1))] + g.corner[size_t(g.cornerIndex(x + 1, y + 1))];
    };
    auto see = [&](uint8_t owner, size_t i) {
        for (uint8_t q : sharers[owner]) {
            uint8_t& v = gs.vision[q][i];
            if (v != 2) { v = 2; gs.visibleCells[q].push_back(int32_t(i)); }
        }
    };
    // 3D line of sight: ground height per cell (heightfield corners or cliff levels) and an occlusion
    // height that adds dynamic blockers (types with vision.blocks, smoke areas, undug rock).
    // Integer units of 1/64 so the ray test is exact and deterministic.
    const bool losAll = los || rules_.fog.lineOfSight;
    std::vector<int64_t>& base = gs.visionBase;
    std::vector<int64_t>& occ = gs.visionOcc;
    if (losAll) {
        const size_t n = size_t(W) * size_t(H);
        {   // terrain can change (digging, sculpt ops): rebuilt on every vision pass, W*H additions
            base.assign(n, 0);
            const int64_t lv = sumUnits(rules_.levelHeight);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    base[size_t(y * W + x)] = g.hf ? cellSum(x, y) : int64_t(g.level[size_t(y * W + x)]) * lv;
        }
        occ = base;
        auto raise = [&](int x, int y, int64_t h) {
            if (x < 0 || y < 0 || x >= W || y >= H) return;
            int64_t& o = occ[size_t(y * W + x)];
            o = std::max(o, base[size_t(y * W + x)] + h);
        };
        const int64_t rock = sumUnits(rules_.fog.rockHeight);
        if (rock > 0 && g.dig.size() == n)
            for (size_t i = 0; i < n; ++i) if (g.dig[i]) occ[i] = std::max(occ[i], base[i] + rock);
        w.each<Unit, Transform>([&](Entity, Unit& u, Transform& tr) {
            const UnitType& t = rules_.types[u.type];
            if (t.visionBlock.raw <= 0 || u.hp.raw <= 0) return;
            const int64_t h = sumUnits(t.visionBlock);
            if (t.structure) {
                const Fixed half = Fixed::fromRatio(1, 2);
                const int x0 = int((tr.pos.x - Fixed::fromInt(t.footprintW) / 2 + half).floorInt()), y0 = int((tr.pos.z - Fixed::fromInt(t.footprintH) / 2 + half).floorInt());
                for (int y = 0; y < t.footprintH; ++y) for (int x = 0; x < t.footprintW; ++x) raise(x0 + x, y0 + y, h);
            } else {
                raise(int(tr.pos.x.floorInt()), int(tr.pos.z.floorInt()), h);
            }
        });
        if (rules_.advancedEffects)
            w.each<Area, Transform>([&](Entity, Area& a, Transform& tr) {
                if (a.effList < 0 || sim.tick() < a.fireAt) return;
                const EffectDef& ef = rules_.payloads[size_t(a.effList)][size_t(a.effIndex)];
                if (ef.delivery.blocksVision.raw <= 0) return;
                const int64_t h = sumUnits(ef.delivery.blocksVision);
                const int rr = int(a.radius.floorInt()), cx = int(tr.pos.x.floorInt()), cy = int(tr.pos.z.floorInt());
                for (int y = cy - rr; y <= cy + rr; ++y)
                    for (int x = cx - rr; x <= cx + rr; ++x)
                        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= rr * rr) raise(x, y, h);
            });
    }
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& tr) {
        if (o.player == 0 || o.player >= np || (u.flags & kUnitOffMap)) return;
        const UnitType& t = rules_.types[u.type];
        int r = int(stat(sim, e, Stat::Sight).toInt());
        int cx, cy; state(sim).grid.cellXY(tr.pos, cx, cy);
        const bool air = (t.layer & kLayerAir) != 0;
        if (losAll && !air && !t.seesOver && g.inside(cx, cy)) {
            const int64_t eye = base[size_t(cy * W + cx)] + eyeUp + sumUnits(t.eyeHeight);
            for (int y = std::max(0, cy - r); y <= std::min(H - 1, cy + r); ++y)
                for (int x = std::max(0, cx - r); x <= std::min(W - 1, cx + r); ++x) {
                    int dx = x - cx, dy = y - cy;
                    if (dx * dx + dy * dy > r * r) continue;
                    // march the cells between eye and target: hidden if ground or a blocker rises above the sight line
                    const int k = std::max(std::abs(dx), std::abs(dy));
                    const int64_t tgt = base[size_t(y * W + x)] + targetUp;
                    bool visible = true;
                    for (int st = 1; st < k && visible; ++st) {
                        int sx = cx + (dx * st * 2 + (dx >= 0 ? k : -k)) / (2 * k), sy = cy + (dy * st * 2 + (dy >= 0 ? k : -k)) / (2 * k);
                        if (occ[size_t(sy * W + sx)] * k > eye * (k - st) + tgt * st) visible = false;
                    }
                    if (visible) see(o.player, size_t(y * W + x));
                }
            return;
        }
        // eyes on a ramp count as the lower level; air units see over cliffs
        int eye = air || !cliffs ? 127 : g.levelAt(cx, cy);
        for (int y = std::max(0, cy - r); y <= std::min(H - 1, cy + r); ++y)
            for (int x = std::max(0, cx - r); x <= std::min(W - 1, cx + r); ++x) {
                int dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy > r * r) continue;
                size_t i = size_t(y * W + x);
                if (g.level[i] > eye) continue;
                see(o.player, i);
            }
    });
    if (rules_.advancedEffects)  // reveal areas (spy, scouting flares)
        w.each<Area, Owner, Transform>([&](Entity, Area& a, Owner& o, Transform& tr) {
            if (o.player == 0 || o.player >= np || a.effList < 0) return;
            const EffectDef& e = rules_.payloads[size_t(a.effList)][size_t(a.effIndex)];
            Fixed sight = e.delivery.sight.raw > 0 ? e.delivery.sight : e.kind == EffectKind::Reveal ? a.radius : Fixed{};
            if (sight.raw <= 0 || sim.tick() < a.fireAt) return;
            int r = int(sight.floorInt());
            int cx, cy; state(sim).grid.cellXY(tr.pos, cx, cy);
            for (int y = std::max(0, cy - r); y <= std::min(H - 1, cy + r); ++y)
                for (int x = std::max(0, cx - r); x <= std::min(W - 1, cx + r); ++x)
                    if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r)
                        see(o.player, size_t(y * W + x)); // tracked in visibleCells, so it fades back to explored
        });
}

// Reference on spheres (brute force, the pre-cache algorithm).
void RtsModule::visionSphereRef(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const MapGrid& g = gs.grid;
    const Surface& S = *gs.surface;
    const size_t np = gs.vision.size();
    std::vector<std::vector<uint8_t>> sharers(np);
    for (size_t p = 1; p < np; ++p) for (size_t q = 1; q < np; ++q) if (q == p || (rules_.fog.shareAllies && sim.allied(uint8_t(p), uint8_t(q)))) sharers[p].push_back(uint8_t(q));
    auto see = [&](uint8_t owner, size_t i) {
        for (uint8_t q : sharers[owner]) {
            uint8_t& v = gs.vision[q][i];
            if (v != 2) { v = 2; gs.visibleCells[q].push_back(int32_t(i)); }
        }
    };
    const bool los = (g.hf && rules_.heights.blockVision) || rules_.fog.lineOfSight;
    std::vector<int32_t> cells;
    // ground radius per cell (lazily filled; 0 = not yet): terrain + vision blockers (structures with
    // vision.blocks over their footprint, undug rock)
    std::vector<Fixed> ground(los ? g.flags.size() : 0);
    std::vector<Fixed> blockers(los ? g.flags.size() : 0);
    if (los) {
        const Fixed rock = rules_.fog.rockHeight;
        if (rock.raw > 0) for (size_t i = 0; i < g.dig.size(); ++i) if (g.dig[i]) blockers[i] = rock;
        w.each<Unit, Transform>([&](Entity e, Unit& u, Transform& tr) {
            const UnitType& t = rules_.types[u.type];
            if (t.visionBlock.raw <= 0 || u.hp.raw <= 0) return;
            if (auto* f = w.tryGet<Footprint>(e); f && f->x >= 0) {
                for (int y = 0; y < f->h; ++y) for (int x = 0; x < f->w; ++x) { Fixed& b = blockers[size_t((f->y + y) * g.w + f->x + x)]; b = fxMax(b, t.visionBlock); }
            } else if (const int c = g.cellOf(tr.pos); c >= 0) blockers[size_t(c)] = fxMax(blockers[size_t(c)], t.visionBlock);
        });
    }
    auto occHeight = [&](int c) { return S.cellHeight(c) + blockers[size_t(c)]; };
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& tr) {
        if (o.player == 0 || o.player >= np || (u.flags & kUnitInHand)) return;
        const UnitType& t = rules_.types[u.type];
        const Fixed sight = stat(sim, e, Stat::Sight);
        const int body = S.bodyAt(tr.pos);
        if (!S.bodies[size_t(body)].hasSurface()) return;
        const int c = S.cellAt(tr.pos);
        // cells are ~0.8 of the mean size near face corners: widen the square, then test the distance
        const Fixed cs = S.cellSize(body);
        const int k = int((sight / (cs * Fixed::fromRatio(8, 10))).floorInt()) + 1;
        const bool air = (t.layer & kLayerAir) != 0;
        const Vec3 o3 = Surface::anchor(body);
        const Vec3 eye = air || t.seesOver ? tr.pos : S.project(tr.pos, rules_.terrain.eyeHeight + t.eyeHeight);
        const Fixed R = S.bodies[size_t(body)].radius;
        cells.clear();
        const int M = S.cubeOf(body).faceSize();
        if (k >= M) { for (int i = 0; i < 6 * M * M; ++i) { const SphereCell sc = S.cubeOf(body).cell(i); cells.push_back(S.gridCell(body, sc.face, sc.x, sc.y)); } }
        else
            for (int dy = -k; dy <= k; ++dy)
                for (int dx = -k; dx <= k; ++dx) {
                    int n = S.offset(c, dx, dy);
                    if (n < 0) { const int mid = S.offset(c, 0, dy); n = mid < 0 ? -1 : S.offset(mid, dx, 0); }
                    if (n >= 0) cells.push_back(n);
                }
        const std::vector<uint8_t>& mine = gs.vision[o.player];
        for (int32_t n : cells) {
            if (mine[size_t(n)] == 2) continue;   // already lit for this owner (and its sharers) this pass
            const Vec3 cc = S.cellCenter(n);
            if (dist2D(cc, tr.pos) > sight) continue;
            if (los && !air && !t.seesOver) {
                // march the 3D sight line in radial terms: at sample k the line's distance from the centre
                // must stay above the ground radius of the cell under it (hills and the horizon). The cell
                // under a sample is found with cellFromDir (table lookup, no trigonometry); ground radii are
                // cached per cell for this pass.
                const Vec3 tgt = cc + S.cellDir(n) * rules_.terrain.targetHeight;
                const int steps = int((dist2D(cc, tr.pos) / cs).floorInt()) + 1;   // about one sample per cell
                bool visible = true;
                for (int st = 1; st < steps && visible; ++st) {
                    const Vec3 p = eye + (tgt - eye) * Fixed::fromRatio(st, steps);
                    const Vec3 l = p - o3;
                    const SphereCell sc = S.cubeOf(body).cell(S.cubeOf(body).cellFromDir(l));
                    const int gc = S.gridCell(body, sc.face, sc.x, sc.y);
                    Fixed& gr = ground[size_t(gc)];
                    if (gr.raw == 0) gr = R + occHeight(gc) + Fixed::fromRaw(1);
                    if (l.lengthSq() < gr * gr) visible = false;
                }
                if (!visible) continue;
            }
            see(o.player, size_t(n));
        }
    });
}

} // namespace rts::game
