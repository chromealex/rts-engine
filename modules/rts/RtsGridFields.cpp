// Grid fields: setup, kernels, emitters, per-cell effects, hooks for vision / pathing / speed, hashing and saves.
// See RtsGridFields.h for the state / config split and the determinism contract.
#include "RtsGridFields.h"
#include "RtsInternal.h"
#include "../../engine/core/Archive.h"
#include <algorithm>
#include <stdexcept>

namespace rts::game {

using namespace detail;

namespace {
constexpr int kDx[8] = {1, -1, 0, 0, 1, -1, 1, -1};
constexpr int kDy[8] = {0, 0, 1, -1, 1, 1, -1, -1};
constexpr size_t kMaxCrossings = 4096;

// rows of characters from a map key that is either an array of strings or {"rows": [...]}
const Json& rowsOf(const Json& j) { return j.isObject() ? j["rows"] : j; }
char rowChar(const Json& rows, int x, int y, char def) {
    if (!rows.isArray() || y < 0 || size_t(y) >= rows.size()) return def;
    const std::string s = rows[size_t(y)].str();
    return x >= 0 && size_t(x) < s.size() ? s[size_t(x)] : def;
}
} // namespace

int GridFields::find(const std::string& id) const {
    for (size_t i = 0; i < defs_.size(); ++i) if (defs_[i].id == id) return int(i);
    return -1;
}

GridFieldStore* GridFields::store(Simulation& sim) const { return sim.world().resource<GridFieldStore>(); }

int GridFields::layers(int field) const {
    return field >= 0 && size_t(field) < layerCount_.size() ? layerCount_[size_t(field)] : 0;
}

int GridFields::layerFor(int field, uint8_t owner) const {
    if (field < 0 || size_t(field) >= defs_.size()) return -1;
    if (!defs_[size_t(field)].perPlayer) return 0;
    return owner >= 1 && int(owner) < layers(field) ? int(owner) : -1;
}

int32_t GridFields::clampTo(const GridFieldDef& d, int64_t v) const { return int32_t(std::clamp<int64_t>(v, d.min, d.max)); }

// ================================================================= setup

void GridFields::setup(Simulation& sim, RtsModule& m, const Json& map) {
    m_ = &m;
    defs_ = m.rules().gridFields;
    applyMapGridFields(defs_, map, m.rules());
    if (defs_.empty()) return;
    GameState& gs = m.state(sim);
    const MapGrid& g = gs.grid;
    mapW_ = g.w; mapH_ = g.h;
    const size_t nf = defs_.size();
    layerCount_.assign(nf, 1);
    for (size_t i = 0; i < nf; ++i) if (defs_[i].perPlayer) layerCount_[i] = int(gs.players.size());
    blockEffect_.assign(nf, -1);
    hooksOf_.assign(nf, {});
    for (size_t i = 0; i < nf; ++i)
        for (size_t e = 0; e < defs_[i].effects.size(); ++e) {
            const FieldEffectKind k = defs_[i].effects[e].kind;
            if (k == FieldEffectKind::BlockMove && blockEffect_[i] < 0) blockEffect_[i] = int(e);
            if (k == FieldEffectKind::BlockVision) blocksVision_ = true;
            if (k == FieldEffectKind::Speed) hasSpeed_ = true;
        }
    // emitters per type (rules fields only: their indices don't move when a map adds fields)
    const Rules& R = m.rules();
    emitters_.assign(R.types.size(), {});
    for (size_t t = 0; t < R.types.size(); ++t)
        for (auto& e : R.types[t].fieldEmit) {
            emitters_[t].push_back({e});
            if (std::find(emitPeriods_.begin(), emitPeriods_.end(), e.period) == emitPeriods_.end()) emitPeriods_.push_back(e.period);
        }
    std::sort(emitPeriods_.begin(), emitPeriods_.end());
    // static fuel masks (terrain characters / material layers) and ground heights for flow
    const Json& terrRows = map["terrain"]["rows"];
    const Json& matRows = rowsOf(map["terrainMaterial"]);
    fuelMask_.assign(nf, {});
    ground_.assign(nf, {});
    for (size_t fi = 0; fi < nf; ++fi) {
        const GridFieldDef& d = defs_[fi];
        const int res = d.resolution, fw = (mapW_ + res - 1) / res, fh = (mapH_ + res - 1) / res;
        fuelMask_[fi].assign(d.kernels.size(), {});
        for (size_t k = 0; k < d.kernels.size(); ++k) {
            const FieldFuelDef& fu = d.kernels[k].fuel;
            if (fu.materials.empty() && fu.terrain.empty()) continue;
            auto& mask = fuelMask_[fi][k];
            mask.assign(size_t(fw) * size_t(fh), 0);
            for (int y = 0; y < fh; ++y)
                for (int x = 0; x < fw; ++x) {
                    const int mx = std::min(mapW_ - 1, x * res + res / 2), my = std::min(mapH_ - 1, y * res + res / 2);
                    bool ok = false;
                    if (!fu.terrain.empty()) ok = fu.terrain.find(rowChar(terrRows, mx, my, '.')) != std::string::npos;
                    if (!ok && !fu.materials.empty()) {
                        const char c = rowChar(matRows, mx, my, ' ');
                        ok = c >= '0' && c <= '9' && std::find(fu.materials.begin(), fu.materials.end(), uint8_t(c - '0')) != fu.materials.end();
                    }
                    mask[size_t(y * fw + x)] = ok;
                }
        }
        for (auto& k : d.kernels) {
            if (k.kind != FieldKernelKind::Flow || !ground_[fi].empty()) continue;
            ground_[fi].assign(size_t(fw) * size_t(fh), 0);
            for (int y = 0; y < fh; ++y)
                for (int x = 0; x < fw; ++x) {
                    const Fixed wx = Fixed::fromInt(x * res) + Fixed::fromRatio(res, 2), wz = Fixed::fromInt(y * res) + Fixed::fromRatio(res, 2);
                    const int mx = std::min(mapW_ - 1, x * res), my = std::min(mapH_ - 1, y * res);
                    const Fixed hgt = g.hf ? g.surfaceHeight(wx, wz) : R.levelHeight * g.levelAt(mx, my);
                    ground_[fi][size_t(y * fw + x)] = (hgt.raw * k.unitsPerHeight) >> Fixed::kFracBits;
                }
        }
    }
    initStore(sim);
    GridFieldStore& st = *store(sim);
    // initial values (map "gridFields".<id>: "values" RLE, "fill" rects / circles), written through put()
    for (size_t fi = 0; fi < nf; ++fi) {
        if (!defs_[fi].initial.isObject()) continue;
        GridFieldStore::Field& f = st.fields[fi];
        std::vector<int32_t> vals;
        std::string err;
        if (!buildInitialField(defs_[fi], mapW_, mapH_, f.layers, vals, &err)) throw std::runtime_error(err);
        for (size_t i = 0; i < vals.size(); ++i) if (vals[i] != f.v[i]) put(sim, int(fi), f, i, vals[i]);
    }
    st.crossings.clear();
}

void GridFields::initStore(Simulation& sim) {
    GridFieldStore& st = sim.world().setResource<GridFieldStore>();
    st.fields.assign(defs_.size(), {});
    for (size_t fi = 0; fi < defs_.size(); ++fi) {
        const GridFieldDef& d = defs_[fi];
        GridFieldStore::Field& f = st.fields[fi];
        f.w = (mapW_ + d.resolution - 1) / d.resolution;
        f.h = (mapH_ + d.resolution - 1) / d.resolution;
        f.layers = layerCount_[fi];
        f.cw = (f.w + kFieldChunk - 1) / kFieldChunk;
        f.ch = (f.h + kFieldChunk - 1) / kFieldChunk;
        f.v.assign(size_t(f.layers) * f.cells(), d.def);
        f.live.assign(size_t(f.cw * f.ch), 0);
        f.chunkHash.assign(size_t(f.cw * f.ch), 0);
        f.hashDirty.assign(size_t(f.cw * f.ch), 1);
        if (blockEffect_[fi] >= 0) f.blockOn.assign(f.cells(), 0);
    }
}

int GridFields::addHook(int field, int32_t value, bool fall) {
    if (field < 0 || size_t(field) >= defs_.size()) return -1;
    hooks_.push_back({int16_t(field), value, fall});
    hooksOf_[size_t(field)].push_back(int(hooks_.size() - 1));
    return int(hooks_.size() - 1);
}

// ================================================================= cell writes

int32_t GridFields::cellMax(const GridFieldStore::Field& f, size_t cell) const {
    int32_t best = f.v[cell];
    for (int l = 1; l < f.layers; ++l) best = std::max(best, f.v[size_t(l) * f.cells() + cell]);
    return best;
}

void GridFields::updateBlock(Simulation& sim, int fi, GridFieldStore::Field& f, size_t cell) {
    const int32_t above = defs_[size_t(fi)].effects[size_t(blockEffect_[size_t(fi)])].above;
    const uint8_t on = cellMax(f, cell) >= above;
    if (on == f.blockOn[cell]) return;
    f.blockOn[cell] = on;
    const int res = defs_[size_t(fi)].resolution, x = int(cell % size_t(f.w)) * res, y = int(cell / size_t(f.w)) * res;
    m_->state(sim).grid.block(x, y, std::min(res, mapW_ - x), std::min(res, mapH_ - y), on ? 1 : -1);
}

void GridFields::put(Simulation& sim, int fi, GridFieldStore::Field& f, size_t i, int32_t nv) {
    const int32_t old = f.v[i];
    if (old == nv) return;
    const GridFieldDef& d = defs_[size_t(fi)];
    const size_t cell = i % f.cells();
    const int x = int(cell % size_t(f.w)), y = int(cell / size_t(f.w));
    const size_t ch = size_t(f.chunkOf(x, y));
    if ((old != d.def) != (nv != d.def)) f.live[ch] = uint16_t(f.live[ch] + (nv != d.def ? 1 : -1));
    f.v[i] = nv;
    f.hashDirty[ch] = 1;
    ++f.version;
    if (blockEffect_[size_t(fi)] >= 0) updateBlock(sim, fi, f, cell);
    for (int hk : hooksOf_[size_t(fi)]) {
        const Hook& h = hooks_[size_t(hk)];
        const bool hit = h.fall ? (old >= h.value && nv < h.value) : (old < h.value && nv >= h.value);
        if (!hit) continue;
        GridFieldStore& st = *store(sim);
        if (st.crossings.size() < kMaxCrossings) st.crossings.push_back({uint16_t(hk), uint8_t(i / f.cells()), int32_t(cell), nv});
    }
}

bool GridFields::cellOf(int field, Fixed x, Fixed z, int& cx, int& cz) const {
    if (field < 0 || size_t(field) >= defs_.size()) return false;
    const int res = defs_[size_t(field)].resolution;
    const int64_t mx = x.floorInt(), mz = z.floorInt();
    cx = int(mx >= 0 ? mx / res : -1); cz = int(mz >= 0 ? mz / res : -1);
    const int fw = (mapW_ + res - 1) / res, fh = (mapH_ + res - 1) / res;
    return mx >= 0 && mz >= 0 && cx < fw && cz < fh;
}

Vec3 GridFields::centerOf(int field, int cx, int cz) const {
    const int res = field >= 0 && size_t(field) < defs_.size() ? defs_[size_t(field)].resolution : 1;
    return {Fixed::fromInt(cx * res) + Fixed::fromRatio(res, 2), Fixed{}, Fixed::fromInt(cz * res) + Fixed::fromRatio(res, 2)};
}

int32_t GridFields::get(Simulation& sim, int field, int cx, int cz, int layer) const {
    const GridFieldStore* st = store(sim);
    if (!st || field < 0 || size_t(field) >= st->fields.size()) return 0;
    const auto& f = st->fields[size_t(field)];
    if (cx < 0 || cz < 0 || cx >= f.w || cz >= f.h || layer < 0 || layer >= f.layers) return defs_[size_t(field)].def;
    return f.v[size_t(layer) * f.cells() + size_t(cz * f.w + cx)];
}

bool GridFields::write(Simulation& sim, int field, int cx, int cz, int layer, int32_t v, int mode) {
    GridFieldStore* st = store(sim);
    if (!st || field < 0 || size_t(field) >= st->fields.size()) return false;
    auto& f = st->fields[size_t(field)];
    if (cx < 0 || cz < 0 || cx >= f.w || cz >= f.h || layer < 0 || layer >= f.layers) return false;
    const size_t i = size_t(layer) * f.cells() + size_t(cz * f.w + cx);
    const int32_t old = f.v[i];
    const int64_t nv = mode == 1 ? int64_t(old) + v : mode == 2 ? std::max<int64_t>(old, v) : v;
    put(sim, field, f, i, clampTo(defs_[size_t(field)], nv));
    return f.v[i] != old;
}

int GridFields::fillRect(Simulation& sim, int field, int cx0, int cz0, int cx1, int cz1, int layer, int32_t v, int mode) {
    GridFieldStore* st = store(sim);
    if (!st || field < 0 || size_t(field) >= st->fields.size()) return 0;
    const auto& f = st->fields[size_t(field)];
    if (cx0 > cx1) std::swap(cx0, cx1);
    if (cz0 > cz1) std::swap(cz0, cz1);
    cx0 = std::max(cx0, 0); cz0 = std::max(cz0, 0); cx1 = std::min(cx1, f.w - 1); cz1 = std::min(cz1, f.h - 1);
    int n = 0;
    for (int z = cz0; z <= cz1; ++z)
        for (int x = cx0; x <= cx1; ++x) n += write(sim, field, x, z, layer, v, mode);
    return n;
}

int GridFields::fillCircle(Simulation& sim, int field, Fixed x, Fixed z, Fixed radius, int layer, int32_t v, int mode) {
    int cx, cz;
    if (field < 0 || size_t(field) >= defs_.size()) return 0;
    if (radius.raw <= 0) return cellOf(field, x, z, cx, cz) ? int(write(sim, field, cx, cz, layer, v, mode)) : 0;
    const int res = defs_[size_t(field)].resolution;
    const int x0 = int((x - radius).floorInt()) / res - 1, x1 = int((x + radius).floorInt()) / res + 1;
    const int z0 = int((z - radius).floorInt()) / res - 1, z1 = int((z + radius).floorInt()) / res + 1;
    const Fixed r2 = radius * radius;
    int n = 0;
    for (int iz = std::max(0, z0); iz <= z1; ++iz)
        for (int ix = std::max(0, x0); ix <= x1; ++ix) {
            const Vec3 c = centerOf(field, ix, iz);
            const Fixed dx = c.x - x, dz = c.z - z;
            if (dx * dx + dz * dz <= r2) n += write(sim, field, ix, iz, layer, v, mode);
        }
    return n;
}

// ================================================================= kernels

void GridFields::workChunks(const GridFieldStore::Field& f, bool ring) {
    const size_t nc = size_t(f.cw * f.ch);
    if (mark_.size() < nc) mark_.assign(nc, 0);
    work_.clear();
    bool any = false;
    for (size_t c = 0; c < nc; ++c) {
        if (!f.live[c]) continue;
        any = true;
        if (!ring) { mark_[c] = 1; continue; }
        const int x = int(c) % f.cw, y = int(c) / f.cw;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if (nx >= 0 && ny >= 0 && nx < f.cw && ny < f.ch) mark_[size_t(ny * f.cw + nx)] = 1;
            }
    }
    if (!any) return;
    for (size_t c = 0; c < nc; ++c) if (mark_[c]) { work_.push_back(int32_t(c)); mark_[c] = 0; }
}

// Calls fn(cell) for every cell of the work chunks in chunk order, row-major inside a chunk.
template <class Fn> static void forWork(const GridFieldStore::Field& f, const std::vector<int32_t>& work, Fn&& fn) {
    for (int32_t c : work) {
        const int x0 = (c % f.cw) * kFieldChunk, y0 = (c / f.cw) * kFieldChunk;
        const int x1 = std::min(f.w, x0 + kFieldChunk), y1 = std::min(f.h, y0 + kFieldChunk);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) fn(size_t(y * f.w + x), x, y);
    }
}

void GridFields::decay(GridFieldStore::Field& f, const GridFieldDef& d, const FieldKernelDef& k, int fi, Simulation& sim) {
    workChunks(f, false);
    for (int l = 0; l < f.layers; ++l) {
        const size_t base = size_t(l) * f.cells();
        forWork(f, work_, [&](size_t c, int, int) {
            const int64_t v = f.v[base + c];
            if (v == d.def) return;
            int64_t nv = v;
            if (k.percent) nv -= (nv - d.def) * k.percent / 65536;
            if (k.amount) nv = nv > d.def ? std::max<int64_t>(d.def, nv - k.amount) : std::min<int64_t>(d.def, nv + k.amount);
            put(sim, fi, f, base + c, clampTo(d, nv));
        });
        cellsVisited += uint64_t(work_.size()) * kFieldChunk * kFieldChunk;
    }
}

void GridFields::diffuse(GridFieldStore::Field& f, const GridFieldDef& d, const FieldKernelDef& k, int fi, Simulation& sim) {
    workChunks(f, true);
    if (work_.empty()) return;
    if (tmp_.size() < f.cells()) tmp_.resize(f.cells());
    const int nb = k.eight ? 8 : 4;
    for (int l = 0; l < f.layers; ++l) {
        const size_t base = size_t(l) * f.cells();
        const int32_t* v = f.v.data() + base;
        forWork(f, work_, [&](size_t c, int x, int y) {
            int64_t sum = 0; int n = 0;
            for (int q = 0; q < nb; ++q) {
                const int nx = x + kDx[q], ny = y + kDy[q];
                if (nx < 0 || ny < 0 || nx >= f.w || ny >= f.h) continue;   // edges: no flux out of the map
                sum += v[ny * f.w + nx]; ++n;
            }
            const int64_t self = v[c];
            tmp_[c] = n ? clampTo(d, self + (sum / n - self) * k.rate / 65536) : int32_t(self);
        });
        forWork(f, work_, [&](size_t c, int, int) { put(sim, fi, f, base + c, tmp_[c]); });
        cellsVisited += uint64_t(work_.size()) * kFieldChunk * kFieldChunk;
    }
}

bool GridFields::fuelOk(Simulation& sim, int fi, const FieldKernelDef& k, size_t kIndex, size_t cell, int32_t& fuelValue) {
    fuelValue = 0;
    const FieldFuelDef& fu = k.fuel;
    if (!fu.any()) return true;
    bool ok = false;
    if (fu.field >= 0) {
        const auto& ff = store(sim)->fields[size_t(fu.field)];
        fuelValue = ff.v[cell];
        ok = fuelValue >= fu.fieldMin;
    }
    if (!ok && !fuelMask_[size_t(fi)][kIndex].empty()) ok = fuelMask_[size_t(fi)][kIndex][cell] != 0;
    if (!ok && fu.tags) ok = tagMask_[cell] != 0;
    return ok;
}

void GridFields::spread(GridFieldStore::Field& f, const GridFieldDef& d, const FieldKernelDef& k, int fi, Simulation& sim) {
    workChunks(f, false);
    if (work_.empty()) return;
    const size_t kIndex = size_t(&k - d.kernels.data());
    if (k.fuel.tags) {   // cells with a unit of the fuel tags (trees), this run
        tagMask_.assign(f.cells(), 0);
        const Rules& R = m_->rules();
        sim.world().each<Unit, Transform>([&](Entity, Unit& u, Transform& t) {
            if (u.hp.raw <= 0 || !(R.types[u.type].tags & k.fuel.tags)) return;
            int cx, cz;
            if (cellOf(fi, t.pos.x, t.pos.z, cx, cz)) tagMask_[size_t(cz * f.w + cx)] = 1;
        });
    }
    const int nb = k.eight ? 8 : 4;
    const bool random = k.chance > 0 || k.fuelChance > 0;
    std::vector<std::pair<size_t, int32_t>> ignite;
    for (int l = 0; l < f.layers; ++l) {
        const size_t base = size_t(l) * f.cells();
        ignite.clear();
        forWork(f, work_, [&](size_t c, int x, int y) {
            if (f.v[base + c] < k.above) return;
            for (int q = 0; q < nb; ++q) {
                const int nx = x + kDx[q], ny = y + kDy[q];
                if (nx < 0 || ny < 0 || nx >= f.w || ny >= f.h) continue;
                const size_t n = size_t(ny * f.w + nx);
                if (f.v[base + n] >= k.below) continue;
                int32_t fuel = 0;
                if (!fuelOk(sim, fi, k, kIndex, n, fuel)) continue;
                if (random) {
                    const int64_t p = std::min<int64_t>(65536, k.chance + int64_t(k.fuelChance) * fuel);
                    if (int64_t(sim.rng().below(65536)) >= p) continue;
                }
                ignite.push_back({n, k.set});
            }
        });
        for (auto& [n, v] : ignite) put(sim, fi, f, base + n, clampTo(d, std::max(f.v[base + n], v)));
        cellsVisited += uint64_t(work_.size()) * kFieldChunk * kFieldChunk;
    }
}

void GridFields::flow(GridFieldStore::Field& f, const GridFieldDef& d, const FieldKernelDef& k, int fi, Simulation& sim) {
    workChunks(f, true);
    if (work_.empty()) return;
    if (tmp_.size() < f.cells()) tmp_.resize(f.cells());
    const std::vector<int64_t>& gr = ground_[size_t(fi)];
    const int nb = k.eight ? 8 : 4;
    if (flowDelta_.size() < f.cells()) flowDelta_.assign(f.cells(), 0);
    const int32_t* v = f.v.data();
    forWork(f, work_, [&](size_t c, int, int) { flowDelta_[c] = 0; });
    forWork(f, work_, [&](size_t c, int x, int y) {
        const int64_t depth = v[c];
        if (depth <= k.minDepth) return;
        const int64_t H = gr[c] + depth;
        int64_t dk[8] = {}, D = 0, maxD = 0;
        for (int q = 0; q < nb; ++q) {
            const int nx = x + kDx[q], ny = y + kDy[q];
            if (nx < 0 || ny < 0 || nx >= f.w || ny >= f.h) continue;
            const size_t n = size_t(ny * f.w + nx);
            const int64_t diff = H - (gr[n] + v[n]);
            if (diff > 0) { dk[q] = diff; D += diff; maxD = std::max(maxD, diff); }
        }
        if (D <= 0) return;
        const int64_t out = std::min(depth - k.minDepth, maxD / 2) * k.rate / 65536;
        if (out <= 0) return;
        int64_t sent = 0;
        for (int q = 0; q < nb; ++q) {
            if (!dk[q]) continue;
            const int64_t part = out * dk[q] / D;
            if (part <= 0) continue;
            flowDelta_[size_t((y + kDy[q]) * f.w + x + kDx[q])] += part;
            sent += part;
        }
        flowDelta_[c] -= sent;
    });
    forWork(f, work_, [&](size_t c, int, int) { if (flowDelta_[c]) put(sim, fi, f, c, clampTo(d, int64_t(v[c]) + flowDelta_[c])); });
    cellsVisited += uint64_t(work_.size()) * kFieldChunk * kFieldChunk;
}

void GridFields::consume(GridFieldStore::Field& f, const GridFieldDef& d, const FieldKernelDef& k, int fi, Simulation& sim) {
    (void)d;
    workChunks(f, false);
    if (work_.empty() || k.fuelField < 0) return;
    GridFieldStore::Field& ff = store(sim)->fields[size_t(k.fuelField)];
    const GridFieldDef& fd = defs_[size_t(k.fuelField)];
    forWork(f, work_, [&](size_t c, int, int) {
        if (f.v[c] < k.above) return;
        const int64_t nfv = int64_t(ff.v[c]) - k.amount;
        put(sim, k.fuelField, ff, c, clampTo(fd, nfv));
        if (nfv <= 0) put(sim, fi, f, c, k.out);
    });
    cellsVisited += uint64_t(work_.size()) * kFieldChunk * kFieldChunk;
}

void GridFields::runKernel(Simulation& sim, int fi, const FieldKernelDef& k) {
    GridFieldStore::Field& f = store(sim)->fields[size_t(fi)];
    const GridFieldDef& d = defs_[size_t(fi)];
    switch (k.kind) {
    case FieldKernelKind::Decay: decay(f, d, k, fi, sim); break;
    case FieldKernelKind::Diffuse: diffuse(f, d, k, fi, sim); break;
    case FieldKernelKind::Spread: spread(f, d, k, fi, sim); break;
    case FieldKernelKind::Flow: flow(f, d, k, fi, sim); break;
    case FieldKernelKind::Consume: consume(f, d, k, fi, sim); break;
    }
}

// ================================================================= emitters and effects

void GridFields::emit(Simulation& sim) {
    const uint32_t tick = sim.tick();
    for (uint32_t p : emitPeriods_) {
        if (tick % p != 0) continue;
        sim.world().each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& t) {
            if (u.type >= emitters_.size() || emitters_[u.type].empty() || u.hp.raw <= 0 || (u.flags & kUnitOffMap)) return;
            for (const Emitter& em : emitters_[u.type]) {
                if (em.def.period != p || ((u.flags & kUnitUnderConstruction) && !em.def.whileBuilding)) continue;
                const int layer = layerFor(em.def.field, o.player);
                if (layer < 0) continue;
                const int mode = em.def.mode == 0 ? 1 : em.def.mode == 1 ? 2 : 0;
                fillCircle(sim, em.def.field, t.pos.x, t.pos.z, em.def.radius, layer, em.def.amount, mode);
            }
        });
    }
}

int32_t GridFields::valueFor(const GridFieldStore::Field& f, const GridFieldDef& d, const FieldEffectDef& e, size_t cell, uint8_t owner, Simulation& sim) const {
    if (!d.perPlayer) return f.v[cell];
    if (e.who == FieldWho::Own) return owner >= 1 && int(owner) < f.layers ? f.v[size_t(owner) * f.cells() + cell] : d.def;
    int32_t best = INT32_MIN;
    for (int l = 1; l < f.layers; ++l) {
        if (e.who == FieldWho::Enemies && !m_->hostile(sim, uint8_t(l), owner)) continue;
        best = std::max(best, f.v[size_t(l) * f.cells() + cell]);
    }
    return best == INT32_MIN ? d.def : best;
}

namespace {
bool anyLive(const GridFieldStore::Field& f) { for (auto c : f.live) if (c) return true; return false; }
bool unitMatches(const FieldEffectDef& e, const UnitType& t) {
    if (t.structure ? !e.structures : !e.units) return false;
    if (e.tags && !(t.tags & e.tags)) return false;
    return !(t.tags & e.notTags);
}
} // namespace

void GridFields::effects(Simulation& sim) {
    const uint32_t tick = sim.tick();
    GridFieldStore& st = *store(sim);
    World& w = sim.world();
    const Rules& R = m_->rules();
    struct Hit { Entity e; uint8_t owner; size_t eff; int32_t value; };
    std::vector<Hit> hits;
    for (size_t fi = 0; fi < defs_.size(); ++fi) {
        const GridFieldDef& d = defs_[fi];
        const GridFieldStore::Field& f = st.fields[fi];
        for (size_t ei = 0; ei < d.effects.size(); ++ei) {
            const FieldEffectDef& e = d.effects[ei];
            if (e.kind != FieldEffectKind::Damage && e.kind != FieldEffectKind::Status && e.kind != FieldEffectKind::Yield) continue;
            if (tick % e.period != 0) continue;
            const bool idle = !anyLive(f) && d.def < e.above;
            if (idle) continue;
            const Fixed perRun = e.perSecond * int64_t(e.period) / int64_t(sim.tickRate());
            if (e.kind == FieldEffectKind::Yield && d.perPlayer) {   // per owned cell
                for (int l = 1; l < f.layers; ++l) {
                    int64_t sum = 0;
                    const size_t base = size_t(l) * f.cells();
                    for (size_t c = 0; c < f.cells(); ++c)
                        if (f.v[base + c] >= e.above) sum += e.scaled && e.above > 0 ? f.v[base + c] : std::max<int32_t>(1, e.above);
                    if (sum && e.above > 0) m_->addResource(sim, uint8_t(l), uint16_t(e.resource), perRun * sum / e.above);
                    else if (sum) m_->addResource(sim, uint8_t(l), uint16_t(e.resource), perRun * sum);
                }
                continue;
            }
            hits.clear();
            w.each<Unit, Owner, Transform>([&](Entity ent, Unit& u, Owner& o, Transform& t) {
                if (u.hp.raw <= 0 || (u.flags & kUnitOffMap) || !unitMatches(e, R.types[u.type])) return;
                int cx, cz;
                if (!cellOf(int(fi), t.pos.x, t.pos.z, cx, cz)) return;
                const int32_t v = valueFor(f, d, e, size_t(cz * f.w + cx), o.player, sim);
                if (v >= e.above) hits.push_back({ent, o.player, ei, v});
            });
            for (const Hit& h : hits) {
                if (!w.alive(h.e) || !w.has<Unit>(h.e)) continue;
                const Fixed scale = e.scaled && e.above > 0 ? Fixed::fromInt(h.value) / e.above : Fixed::fromInt(1);
                switch (e.kind) {
                case FieldEffectKind::Damage:
                    if (w.get<Unit>(h.e).hp.raw > 0) m_->dealDamage(sim, h.e, perRun * scale, e.damageType, kNullEntity, 0);
                    break;
                case FieldEffectKind::Status: {
                    EffectCtx ctx;
                    ctx.point = w.get<Transform>(h.e).pos;
                    const Fixed dur = e.seconds.raw > 0 ? e.seconds : Fixed::fromInt(int64_t(e.period) * 2) / int64_t(sim.tickRate());
                    m_->addStatus(sim, h.e, e.status, dur, ctx);
                    break;
                }
                case FieldEffectKind::Yield:
                    if (h.owner >= 1 && h.owner < m_->state(sim).players.size()) m_->addResource(sim, h.owner, uint16_t(e.resource), perRun * scale);
                    break;
                default: break;
                }
            }
        }
    }
}

void GridFields::tick(Simulation& sim) {
    if (defs_.empty()) return;
    emit(sim);
    const uint32_t tick = sim.tick();
    for (size_t fi = 0; fi < defs_.size(); ++fi)
        for (const FieldKernelDef& k : defs_[fi].kernels)
            if (tick >= k.phase && (tick - k.phase) % k.period == 0) runKernel(sim, int(fi), k);
    effects(sim);
}

Fixed GridFields::speedMul(Simulation& sim, Vec3 pos, uint8_t owner) const {
    Fixed mul = Fixed::fromInt(1);
    const GridFieldStore* st = store(sim);
    if (!st) return mul;
    for (size_t fi = 0; fi < defs_.size(); ++fi)
        for (const FieldEffectDef& e : defs_[fi].effects) {
            if (e.kind != FieldEffectKind::Speed) continue;
            int cx, cz;
            if (!cellOf(int(fi), pos.x, pos.z, cx, cz)) continue;
            const auto& f = st->fields[fi];
            if (valueFor(f, defs_[fi], e, size_t(cz * f.w + cx), owner, sim) >= e.above) mul = mul * e.mul;
        }
    return mul;
}

void GridFields::visionBlockers(Simulation& sim, const std::function<void(int, int, int64_t)>& raise) const {
    const GridFieldStore* st = store(sim);
    if (!st) return;
    for (size_t fi = 0; fi < defs_.size(); ++fi)
        for (const FieldEffectDef& e : defs_[fi].effects) {
            if (e.kind != FieldEffectKind::BlockVision) continue;
            const auto& f = st->fields[fi];
            const int res = defs_[fi].resolution;
            const int64_t hgt = (e.height.raw * 64) >> Fixed::kFracBits;
            const bool all = defs_[fi].def >= e.above;
            for (int c = 0; c < f.cw * f.ch; ++c) {
                if (!all && !f.live[size_t(c)]) continue;
                const int x0 = (c % f.cw) * kFieldChunk, y0 = (c / f.cw) * kFieldChunk;
                for (int y = y0; y < std::min(f.h, y0 + kFieldChunk); ++y)
                    for (int x = x0; x < std::min(f.w, x0 + kFieldChunk); ++x) {
                        if (cellMax(f, size_t(y * f.w + x)) < e.above) continue;
                        for (int my = y * res; my < std::min(mapH_, y * res + res); ++my)
                            for (int mx = x * res; mx < std::min(mapW_, x * res + res); ++mx) raise(mx, my, hgt);
                    }
            }
        }
}

void GridFields::reveal(Simulation& sim, const std::function<void(uint8_t, size_t)>& see) const {
    const GridFieldStore* st = store(sim);
    if (!st) return;
    const int np = int(m_->state(sim).players.size());
    for (size_t fi = 0; fi < defs_.size(); ++fi)
        for (const FieldEffectDef& e : defs_[fi].effects) {
            if (e.kind != FieldEffectKind::Reveal) continue;
            const auto& f = st->fields[fi];
            const int res = defs_[fi].resolution;
            for (int c = 0; c < f.cw * f.ch; ++c) {
                if (!f.live[size_t(c)] && defs_[fi].def < e.above) continue;
                const int x0 = (c % f.cw) * kFieldChunk, y0 = (c / f.cw) * kFieldChunk;
                for (int y = y0; y < std::min(f.h, y0 + kFieldChunk); ++y)
                    for (int x = x0; x < std::min(f.w, x0 + kFieldChunk); ++x) {
                        const size_t cell = size_t(y * f.w + x);
                        for (int l = 0; l < f.layers; ++l) {
                            if (f.v[size_t(l) * f.cells() + cell] < e.above) continue;
                            const bool toAll = e.all || !defs_[fi].perPlayer;
                            for (int my = y * res; my < std::min(mapH_, y * res + res); ++my)
                                for (int mx = x * res; mx < std::min(mapW_, x * res + res); ++mx) {
                                    const size_t i = size_t(my * mapW_ + mx);
                                    if (toAll) for (int p = 1; p < np; ++p) see(uint8_t(p), i);
                                    else if (l >= 1) see(uint8_t(l), i);
                                }
                            if (toAll) break;
                        }
                    }
            }
        }
}

void GridFields::applyEffect(Simulation& sim, const EffectDef& e, const EffectCtx& ctx) {
    if (e.field < 0 || size_t(e.field) >= defs_.size()) return;
    World& w = sim.world();
    Vec3 at = ctx.point;
    if (w.alive(ctx.target) && w.has<Transform>(ctx.target)) at = w.get<Transform>(ctx.target).pos;
    const int layer = layerFor(e.field, ctx.owner);
    if (layer < 0) return;
    const int32_t v = defs_[size_t(e.field)].raw(e.fieldAmount * ctx.scale);
    const int mode = e.fieldOp == 0 ? 1 : e.fieldOp == 1 ? 0 : 2;
    fillCircle(sim, e.field, at.x, at.z, e.radius, layer, v, mode);
}

// ================================================================= hashing and saves

void GridFields::hash(Simulation& sim, Hasher& h) const {
    GridFieldStore* st = store(sim);
    if (!st) return;
    h.u32(uint32_t(st->fields.size()));
    for (auto& f : st->fields) {
        for (int c = 0; c < f.cw * f.ch; ++c) {
            if (f.hashDirty[size_t(c)]) {
                Hasher ch;
                const int x0 = (c % f.cw) * kFieldChunk, y0 = (c / f.cw) * kFieldChunk;
                for (int l = 0; l < f.layers; ++l)
                    for (int y = y0; y < std::min(f.h, y0 + kFieldChunk); ++y)
                        ch.bytes(&f.v[size_t(l) * f.cells() + size_t(y * f.w + x0)], size_t(std::min(kFieldChunk, f.w - x0)) * sizeof(int32_t));
                const_cast<GridFieldStore::Field&>(f).chunkHash[size_t(c)] = ch.value();
                const_cast<GridFieldStore::Field&>(f).hashDirty[size_t(c)] = 0;
            }
            h.u64(f.chunkHash[size_t(c)]);
        }
    }
    h.u32(uint32_t(st->crossings.size()));
    for (auto& c : st->crossings) { h.u32(c.hook); h.u32(c.layer); h.i32(c.cell); h.i32(c.value); }
}

namespace {
// run-length encoding of int32 cells (most cells repeat their neighbour)
void ioRun(Archive& a, std::vector<int32_t>& v, size_t expect) {
    uint32_t n = uint32_t(v.size());
    io(a, n);
    if (a.writing()) {
        size_t i = 0;
        while (i < v.size()) {
            size_t j = i + 1;
            while (j < v.size() && v[j] == v[i]) ++j;
            uint32_t run = uint32_t(j - i);
            int32_t val = v[i];
            io(a, run); io(a, val);
            i = j;
        }
        return;
    }
    if (n != expect) { a.fail(); return; }
    size_t i = 0;
    while (i < n && a.ok()) {
        uint32_t run = 0; int32_t val = 0;
        io(a, run); io(a, val);
        if (run == 0 || i + run > n) { a.fail(); return; }
        std::fill(v.begin() + ptrdiff_t(i), v.begin() + ptrdiff_t(i + run), val);
        i += run;
    }
}
} // namespace

void GridFields::io(Simulation& sim, Archive& a) {
    GridFieldStore* st = store(sim);
    if (!st) return;
    uint32_t nf = uint32_t(st->fields.size());
    rts::io(a, nf);
    if (!a.writing() && nf != st->fields.size()) { a.fail(); return; }
    for (size_t fi = 0; fi < st->fields.size() && a.ok(); ++fi) {
        auto& f = st->fields[fi];
        ioRun(a, f.v, f.v.size());
        std::vector<int32_t> blk(f.blockOn.begin(), f.blockOn.end());
        ioRun(a, blk, f.blockOn.size());
        if (!a.writing()) {
            for (size_t i = 0; i < f.blockOn.size(); ++i) f.blockOn[i] = uint8_t(blk[i] != 0);
            std::fill(f.live.begin(), f.live.end(), uint16_t(0));
            for (size_t i = 0; i < f.v.size(); ++i)
                if (f.v[i] != defs_[fi].def) { const size_t c = i % f.cells(); ++f.live[size_t(f.chunkOf(int(c % size_t(f.w)), int(c / size_t(f.w))))]; }
            std::fill(f.hashDirty.begin(), f.hashDirty.end(), uint8_t(1));
            ++f.version;
        }
    }
    uint32_t nc = a.count(st->crossings.size(), 14);
    if (!a.writing()) st->crossings.assign(nc, {});
    for (auto& c : st->crossings) { rts::io(a, c.hook); rts::io(a, c.layer); rts::io(a, c.cell); rts::io(a, c.value); }
}

} // namespace rts::game
