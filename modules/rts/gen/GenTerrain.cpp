// Terrain stage of the generator: heights (noise + shape + erosion + rivers), base flattening, water,
// cliff levels / heightfield corners, biomes, caves and lanes, connectivity repair. Integer only.
#include "GenInternal.h"
#include <queue>

namespace rts::gen {

const char* biomeName(int b) {
    static const char* n[] = {"ocean", "coast", "grass", "forest", "desert", "steppe", "rock", "snow", "swamp"};
    return b >= 0 && b < BCount ? n[b] : "?";
}

namespace {

constexpr int32_t Q(int num, int den) { return int32_t(int64_t(num) * kOne / den); }

// distance (cells, 8-connected chessboard-ish BFS) from every cell to the nearest cell where src != 0, capped
std::vector<int32_t> distanceTo(const Ctx& c, const std::vector<uint8_t>& src, int cap) {
    std::vector<int32_t> d(size_t(c.w * c.h), cap);
    std::queue<int> q;
    for (int i = 0; i < c.w * c.h; ++i) if (src[size_t(i)]) { d[size_t(i)] = 0; q.push(i); }
    while (!q.empty()) {
        int i = q.front(); q.pop();
        if (d[size_t(i)] >= cap) continue;
        int x = i % c.w, y = i / c.w;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                int nx = x + dx, ny = y + dy;
                if (!c.inside(nx, ny)) continue;
                int j = c.ci(nx, ny);
                if (d[size_t(j)] > d[size_t(i)] + 1) { d[size_t(j)] = d[size_t(i)] + 1; q.push(j); }
            }
    }
    return d;
}

// shape mask in Q16 (-1..1 added to the noise) at corner (x, y)
int32_t shapeMask(const Ctx& c, int x, int y) {
    const int64_t cx = int64_t(c.w) * kOne / 2, cy = int64_t(c.h) * kOne / 2;
    const int64_t dx = (int64_t(x) << 16) - cx, dy = (int64_t(y) << 16) - cy;
    const int64_t half = int64_t(std::min(c.w, c.h)) * kOne / 2;
    const int64_t r = isqrt(((dx * dx) >> 16) + ((dy * dy) >> 16)) << 8;     // Q16 cells
    const int64_t rn = std::min<int64_t>(r * kOne / std::max<int64_t>(half, 1), 2 * kOne);  // 0 centre, 1 inscribed edge
    // edge distance (0 at the border, 1 at the centre line)
    const int64_t ex = std::min<int64_t>(x, c.w - x) * 2 * kOne / std::max(1, c.w), ey = std::min<int64_t>(y, c.h - y) * 2 * kOne / std::max(1, c.h);
    const int64_t edge = std::min(ex, ey);
    if (c.shape == "continents") return int32_t(smoothstepQ(0, Q(1, 4), int32_t(edge)) - Q(3, 5));          // ocean rim
    if (c.shape == "islands") return -int32_t(Q(1, 2));                                                     // islands come from bumps
    if (c.shape == "island") return int32_t(Q(1, 2) - (rn * rn >> 16));                                     // one big island
    if (c.shape == "canyons" || c.shape == "highlands") return 0;
    return int32_t(smoothstepQ(0, Q(1, 8), int32_t(edge)) / 4 - Q(1, 8));                                    // classic
}

int32_t bump(int64_t dx, int64_t dy, int64_t radius) {   // smooth bump 1 at the centre -> 0 at radius (Q16 cells)
    const int64_t d2 = ((dx * dx) >> 16) + ((dy * dy) >> 16), r2 = (radius * radius) >> 16;
    if (d2 >= r2 || r2 <= 0) return 0;
    const int64_t t = kOne - d2 * kOne / r2;
    return int32_t((t * t) >> 16);
}

} // namespace

void genHeights(Ctx& c) {
    c.hg.init(c.w, c.h);
    Fbm f;
    f.octaves = std::clamp(c.pint("octaves", 5), 1, 8);
    f.gain = std::clamp(c.pq("roughness", Q(1, 2)), Q(1, 10), Q(9, 10));
    f.ridged = std::clamp(c.pq("ridged", c.shape == "highlands" ? Q(7, 10) : c.shape == "canyons" ? kOne : 0), 0, kOne);
    const int64_t scale = std::max<int64_t>(c.pq("scale", c.shape == "islands" ? Q(14, 1) : Q(24, 1)), Q(2, 1));  // feature size, cells
    f.warp = std::clamp(c.pq("warp", Q(1, 2)), 0, 4 * kOne);
    const uint32_t s = c.sub(1);
    std::vector<int32_t> n(c.hg.v.size());
    for (int y = 0; y <= c.h; ++y)
        for (int x = 0; x <= c.w; ++x) {
            int fx = x, fy = y;
            if (c.sym.kind != Sym::None) c.sym.foldCorner(x, y, fx, fy);   // sample the domain: symmetric by construction
            const int64_t px = (int64_t(fx) << 32) / scale, py = (int64_t(fy) << 32) / scale;
            int32_t v = fbm2(px + (int64_t(c.seed & 0xffff) << 16), py, s, f);
            if (c.shape == "canyons") v = -v;               // ridged noise inverted: networks of deep canyons
            v += shapeMask(c, fx, fy);
            n[size_t(c.hg.idx(x, y))] = v;
        }
    if (c.shape == "islands") {   // one island per player + a few neutral ones, all from the domain
        const int count = std::clamp(c.pint("islands", 3), 0, 12);
        const int64_t rad = std::max<int64_t>(c.pq("islandSize", Q(std::min(c.w, c.h), 6)), Q(4, 1));
        std::vector<std::pair<P2, int64_t>> isl;
        for (int k = 0; k < count; ++k) {
            uint32_t hv = hashCoords(k, 7, 1, c.seed);
            P2 p{int64_t(hv % uint32_t(c.w)) << 16, int64_t((hv >> 12) % uint32_t(c.h)) << 16};
            if (c.sym.kind != Sym::None) c.sym.fold(p.x, p.y, p.x, p.y);
            isl.push_back({p, rad * (kOne * 3 / 4 + int64_t(hv >> 24) * 256) >> 16});
        }
        for (int y = 0; y <= c.h; ++y)
            for (int x = 0; x <= c.w; ++x) {
                int fx = x, fy = y;
                if (c.sym.kind != Sym::None) c.sym.foldCorner(x, y, fx, fy);
                int32_t b = 0;
                for (auto& [p, r] : isl) b = std::max(b, bump((int64_t(fx) << 16) - p.x, (int64_t(fy) << 16) - p.y, r));
                n[size_t(c.hg.idx(x, y))] += b * 6 / 5;
            }
    }
    c.relief = std::clamp(c.pq("relief", c.model == game::TerrainModel::Heightfield ? Q(6, 1) : Q(8, 1)), Q(1, 2), Q(60, 1)) >> 4; // world units -> kHU
    for (size_t i = 0; i < n.size(); ++i) c.hg.v[i] = int32_t((int64_t(n[i]) * c.relief) >> 16);

    // sea level: quantile of corner heights
    const int32_t water = std::clamp(c.pq("water", c.shape == "islands" ? Q(62, 100) : c.shape == "continents" ? Q(38, 100) : Q(10, 100)), 0, Q(9, 10));
    c.sea = water > 0 ? quantile(c.hg.v, water) : INT32_MIN / 2;

    // erosion
    const int thermal = std::clamp(c.pint("thermal", 6), 0, 50);
    const int hydro = std::clamp(c.pint("erosion", 3), 0, 20);
    if (thermal) thermalErosion(c.hg, thermal, c.pq("talus", Q(1, 2)) >> 4, Q(1, 2));
    if (hydro) hydraulicErosion(c.hg, c.sea, hydro, c.pq("erosionRate", Q(1, 2)));
    // rivers: corners with large flow accumulation are carved below the sea level
    if (c.sea > INT32_MIN / 2) {
        const int32_t riv = c.pq("rivers", c.shape == "classic" || c.shape == "continents" ? Q(1, 2) : 0);
        if (riv > 0) {
            Drainage d;
            drainage(c.hg, c.sea, d);
            const int32_t thr = std::max(40, int32_t(int64_t(c.w * c.h) * 2 / 3 * (kOne - riv / 2) >> 16) / 8);
            std::vector<uint8_t> riverMark(c.hg.v.size(), 0);
            for (size_t i = 0; i < d.flux.size(); ++i)
                if (d.flux[i] >= thr && c.hg.v[i] > c.sea) riverMark[i] = 1;
            for (size_t i = 0; i < riverMark.size(); ++i)
                if (riverMark[i]) c.hg.v[i] = std::min(c.hg.v[i], c.sea - kHU / 4);
            // lakes: pits deeper than lakeDepth stay as lakes (filled surface above the ground)
            const int32_t lakeDepth = c.pq("lakes", Q(1, 2)) >> 4;
            for (size_t i = 0; i < d.filled.size(); ++i)
                if (lakeDepth > 0 && d.filled[i] - c.hg.v[i] > lakeDepth && c.hg.v[i] > c.sea) c.hg.v[i] = std::min(c.hg.v[i], c.sea - kHU / 4);
        }
    }
    symmetrizeCorners(c.hg, c.sym);   // erosion order breaks symmetry slightly: copy the domain again
}

void flattenBases(Ctx& c) {
    const int64_t r = int64_t(c.baseRadius) << 16;
    for (auto& s : c.starts) {
        const int sx = std::clamp(int(s.x >> 16), 0, c.w), sy = std::clamp(int(s.y >> 16), 0, c.h);
        int32_t target = std::max(c.hg.at(sx, sy), c.sea + kHU);
        if (c.model == game::TerrainModel::Heightfield) target = std::max(target, c.sea + kHU * 3 / 2);
        const int R = c.baseRadius + 4;
        for (int y = sy - R; y <= sy + R; ++y)
            for (int x = sx - R; x <= sx + R; ++x) {
                if (x < 0 || y < 0 || x > c.w || y > c.h) continue;
                const int64_t dx = (int64_t(x) << 16) - s.x, dy = (int64_t(y) << 16) - s.y;
                const int64_t d = isqrt(((dx * dx) >> 16) + ((dy * dy) >> 16)) << 8;
                const int32_t t = smoothstepQ(int32_t(r + (4 << 16)), int32_t(r), int32_t(std::min<int64_t>(d, INT32_MAX)));
                int32_t& v = c.hg.at(x, y);
                v = int32_t(v + ((int64_t(target - v) * t) >> 16));
            }
    }
    if (c.sym.exact()) symmetrizeCorners(c.hg, c.sym);
}

void classifyCells(Ctx& c) {
    const int W = c.w, H = c.h, N = W * H;
    c.cell.assign(size_t(N), '.');
    c.lvl.assign(size_t(N), 0);
    c.biome.assign(size_t(N), BGrass);
    c.moist.assign(size_t(N), 0);
    std::vector<int32_t> ch(static_cast<size_t>(N));
    std::vector<uint8_t> wet(static_cast<size_t>(N), 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            ch[size_t(c.ci(x, y))] = c.hg.cell(x, y);
            wet[size_t(c.ci(x, y))] = c.hg.cell(x, y) < c.sea ? 1 : 0;
        }
    // land statistics
    std::vector<int32_t> land;
    for (int i = 0; i < N; ++i) if (!wet[size_t(i)]) land.push_back(ch[size_t(i)]);
    if (land.empty()) land.push_back(c.sea + kHU);
    const int32_t lmin = quantile(land, 0), lmax = std::max(quantile(land, kOne), lmin + 1);
    auto hnorm = [&](int32_t v) { return int32_t(std::clamp<int64_t>((int64_t(v) - lmin) * kOne / (lmax - lmin), 0, kOne)); };
    const std::vector<int32_t> dWater = distanceTo(c, wet, 16);

    // moisture, temperature
    Fbm mf; mf.octaves = 4;
    const int32_t temp0 = c.pq("temperature", Q(1, 2)), moist0 = c.pq("moisture", Q(1, 2));
    const int32_t lat = c.pq("latitude", 0);            // flat maps: >0 = the top edge is polar
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int fx = x, fy = y;
            if (c.sym.kind != Sym::None) c.sym.foldCell(x, y, fx, fy);
            const int i = c.ci(x, y);
            int64_t m = fbm2((int64_t(fx) << 16) / 20, (int64_t(fy) << 16) / 20, c.sub(2), mf) / 2 + kOne / 2;  // 0..1
            m = m * 3 / 5 + int64_t(16 - std::min(16, dWater[size_t(c.ci(fx, fy))])) * kOne / 40 + (moist0 - kOne / 2);
            c.moist[size_t(i)] = int32_t(std::clamp<int64_t>(m, 0, kOne));
        }
    const int32_t blockLine = c.pq("peaks", c.shape == "highlands" ? Q(88, 100) : Q(96, 100));
    const int32_t peakH = quantile(land, blockLine);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int i = c.ci(x, y);
            const int32_t v = ch[size_t(i)], hn = hnorm(v), m = c.moist[size_t(i)];
            int64_t t = temp0 - int64_t(hn) * 2 / 5 - (lat > 0 ? (int64_t(H - 1 - y) * 0 + int64_t(H - y) * lat / std::max(1, H)) / 2 : 0);
            uint8_t b;
            if (wet[size_t(i)]) b = BOcean;
            else if (dWater[size_t(i)] <= 1 && hn < Q(1, 10)) b = BCoast;
            else if (hn > Q(80, 100) && t < Q(35, 100)) b = BSnow;
            else if (hn > Q(70, 100)) b = BRock;
            else if (t > Q(60, 100) && m < Q(40, 100)) b = BDesert;
            else if (m < Q(38, 100)) b = BSteppe;
            else if (m > Q(80, 100) && hn < Q(20, 100)) b = BSwamp;
            else if (m > Q(56, 100)) b = BForest;
            else b = BGrass;
            c.biome[size_t(i)] = b;
            char ch2 = '.';
            if (wet[size_t(i)]) ch2 = '~';
            else if (c.model != game::TerrainModel::Heightfield && v >= peakH && blockLine < kOne) ch2 = '#';
            else if (b == BSwamp) ch2 = ',';
            else if (c.rockBases && (b == BRock || b == BSnow)) ch2 = 'R';
            c.cell[size_t(i)] = ch2;
        }
    // shallows: land cells next to water that are barely above it (fords) and water next to the shore
    const int32_t shallow = c.pq("shallows", 0);
    if (shallow > 0)
        for (int i = 0; i < N; ++i)
            if (wet[size_t(i)] && dWater[size_t(i)] == 0) {
                int x = i % W, y = i / W;
                bool shore = false;
                for (int dy = -1; dy <= 1 && !shore; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) if (c.inside(x + dx, y + dy) && !wet[size_t(c.ci(x + dx, y + dy))]) { shore = true; break; }
                if (shore && c.sea - ch[size_t(i)] < (shallow >> 4)) c.cell[size_t(i)] = '=';
            }

    // cliff levels (levels model): land quantiles, small islands of a level merged into the neighbours
    if (c.model == game::TerrainModel::Levels && c.levels > 0) {
        std::vector<int32_t> thr;
        const int32_t plateau = std::clamp(c.pq("plateau", Q(45, 100)), Q(5, 100), Q(95, 100));
        for (int k = 1; k <= c.levels; ++k) thr.push_back(quantile(land, kOne - plateau / k));
        for (int i = 0; i < N; ++i) {
            if (wet[size_t(i)]) continue;
            int l = 0;
            for (int32_t t : thr) if (ch[size_t(i)] >= t) ++l;
            c.lvl[size_t(i)] = int8_t(l);
        }
        for (int pass = 0; pass < 2; ++pass) {      // majority filter (deterministic, from a copy)
            std::vector<int8_t> src = c.lvl;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    int cnt[10] = {};
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) if (c.inside(x + dx, y + dy)) cnt[src[size_t(c.ci(x + dx, y + dy))]]++;
                    int best = src[size_t(c.ci(x, y))];
                    for (int l = 0; l < 10; ++l) if (cnt[l] > cnt[best] || (cnt[l] == cnt[best] && l < best && cnt[l] >= 5)) best = l;
                    if (cnt[best] >= 5) c.lvl[size_t(c.ci(x, y))] = int8_t(best);
                }
        }
        for (int i = 0; i < N; ++i) if (wet[size_t(i)]) c.lvl[size_t(i)] = 0;
        c.levelStep = std::max<int32_t>(1, (lmax - lmin) / (c.levels + 1));
    }
    // base areas: plain ground, one level, never water
    for (auto& s : c.starts) {
        const int sx = int(s.x >> 16), sy = int(s.y >> 16), r = c.baseRadius + 3;   // + room for the mines next to the base
        int8_t lv = c.inside(sx, sy) ? c.lvl[size_t(c.ci(sx, sy))] : 0;
        for (int y = sy - r; y <= sy + r; ++y)
            for (int x = sx - r; x <= sx + r; ++x) {
                if (!c.inside(x, y) || (x - sx) * (x - sx) + (y - sy) * (y - sy) > r * r) continue;
                const int i = c.ci(x, y);
                const bool core = (x - sx) * (x - sx) + (y - sy) * (y - sy) <= c.baseRadius * c.baseRadius;
                if (!core && (c.cell[size_t(i)] == '~' || c.cell[size_t(i)] == '#')) continue;
                c.cell[size_t(i)] = c.rockBases ? 'R' : '.';
                c.lvl[size_t(i)] = lv;
                if (c.biome[size_t(i)] == BOcean || c.biome[size_t(i)] == BSwamp || c.biome[size_t(i)] == BForest) c.biome[size_t(i)] = BGrass;
            }
    }
    if (c.pbool("border", false))
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) if (x == 0 || y == 0 || x == W - 1 || y == H - 1) c.cell[size_t(c.ci(x, y))] = '#';
    if (c.sym.exact()) { symmetrizeCells(c.cell, W, H, c.sym); symmetrizeCells(c.lvl, W, H, c.sym); symmetrizeCells(c.biome, W, H, c.sym); }
}

// --------------------------------------------------------------- caves (Dungeon Keeper)

void genCavesTerrain(Ctx& c) {
    const int W = c.w, H = c.h, N = W * H;
    c.cell.assign(size_t(N), c.digPlain ? c.digPlain : '#');
    c.lvl.assign(size_t(N), 0);
    c.biome.assign(size_t(N), BRock);
    c.moist.assign(size_t(N), kOne / 2);
    Fbm f; f.octaves = 4; f.warp = kOne;
    const int32_t goldQ = c.pq("gold", Q(14, 100)), gemQ = c.pq("gems", Q(2, 100)), bedQ = c.pq("bedrock", Q(10, 100));
    std::vector<int32_t> nv(static_cast<size_t>(N)), gv(static_cast<size_t>(N));
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int fx = x, fy = y;
            if (c.sym.kind != Sym::None) c.sym.foldCell(x, y, fx, fy);
            nv[size_t(c.ci(x, y))] = fbm2((int64_t(fx) << 16) / 9, (int64_t(fy) << 16) / 9, c.sub(10), f);
            gv[size_t(c.ci(x, y))] = fbm2((int64_t(fx) << 16) / 5, (int64_t(fy) << 16) / 5, c.sub(11), f);
        }
    const int32_t tBed = bedQ > 0 ? quantile(nv, kOne - bedQ) : INT32_MAX, tCave = quantile(nv, c.pq("caverns", Q(8, 100)));
    const int32_t tGold = goldQ > 0 ? quantile(gv, kOne - goldQ) : INT32_MAX, tGem = gemQ > 0 ? quantile(gv, kOne - gemQ) : INT32_MAX;
    for (int i = 0; i < N; ++i) {
        const int x = i % W, y = i / W;
        char ch = c.digPlain ? c.digPlain : '#';
        if (nv[size_t(i)] >= tBed) ch = '#';
        else if (nv[size_t(i)] <= tCave) ch = '.';
        else if (gv[size_t(i)] >= tGem && c.digGem) ch = c.digGem;
        else if (gv[size_t(i)] >= tGold && c.digGold) ch = c.digGold;
        if (x == 0 || y == 0 || x == W - 1 || y == H - 1) ch = '#';
        c.cell[size_t(i)] = ch;
        c.biome[size_t(i)] = ch == '.' ? BSteppe : BRock;
    }
    // start chambers
    for (auto& s : c.starts) {
        const int sx = int(s.x >> 16), sy = int(s.y >> 16), r = c.baseRadius;
        for (int y = sy - r - 2; y <= sy + r + 2; ++y)
            for (int x = sx - r - 2; x <= sx + r + 2; ++x) {
                if (x <= 0 || y <= 0 || x >= W - 1 || y >= H - 1) continue;
                const int d2 = (x - sx) * (x - sx) + (y - sy) * (y - sy);
                const int i = c.ci(x, y);
                if (d2 <= r * r) { c.cell[size_t(i)] = '.'; c.biome[size_t(i)] = BGrass; }
                else if (d2 <= (r + 2) * (r + 2) && c.cell[size_t(i)] == '#') c.cell[size_t(i)] = c.digPlain ? c.digPlain : '.';
            }
    }
    if (c.sym.exact()) { symmetrizeCells(c.cell, W, H, c.sym); symmetrizeCells(c.biome, W, H, c.sym); }
    c.hg.init(W, H);
    c.sea = INT32_MIN / 2;
}

// --------------------------------------------------------------- lanes (MW2 / lane battlers)

void genLanesTerrain(Ctx& c) {
    const int W = c.w, H = c.h, N = W * H;
    c.cell.assign(size_t(N), '#');
    c.lvl.assign(size_t(N), 0);
    c.biome.assign(size_t(N), BRock);
    c.moist.assign(size_t(N), kOne / 2);
    c.hg.init(W, H);
    c.sea = INT32_MIN / 2;
    const bool open = c.pbool("open", false);   // MW2: open field with obstacles instead of corridors
    const int lanes = std::clamp(c.pint("lanes", 3), 1, 5);
    const int width = std::clamp(c.pint("laneWidth", 3), 1, 9);
    const int64_t bow = c.pq("bend", Q(32, 100));
    if (open) {
        Fbm f; f.octaves = 3;
        const int32_t obst = c.pq("obstacles", Q(14, 100));
        std::vector<int32_t> nv(static_cast<size_t>(N));
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int fx = x, fy = y;
                if (c.sym.kind != Sym::None) c.sym.foldCell(x, y, fx, fy);
                nv[size_t(c.ci(x, y))] = fbm2((int64_t(fx) << 16) / 6, (int64_t(fy) << 16) / 6, c.sub(20), f);
            }
        const int32_t t = obst > 0 ? quantile(nv, kOne - obst) : INT32_MAX;
        for (int i = 0; i < N; ++i) {
            c.cell[size_t(i)] = nv[size_t(i)] >= t ? '#' : '.';
            c.biome[size_t(i)] = nv[size_t(i)] >= t ? BRock : BGrass;
        }
    }
    // corridors between consecutive starts (2 players: a, b); lanes bow outwards on both sides
    auto carve = [&](int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t off, int wdt) {
        const int64_t mx = (ax + bx) / 2, my = (ay + by) / 2;
        const int64_t dx = bx - ax, dy = by - ay;
        const int64_t px = -dy, py = dx;                                  // perpendicular (length = |d|)
        const int64_t cx = mx + ((px * off) >> 16), cy = my + ((py * off) >> 16);
        const int steps = std::max(8, int((std::abs(dx) + std::abs(dy)) >> 15));
        for (int k = 0; k <= steps; ++k) {
            const int64_t t = int64_t(k) * kOne / steps, u = kOne - t;
            const int64_t x = (((u * u) >> 16) * ax + ((2 * u * t) >> 16) * cx + ((t * t) >> 16) * bx) >> 16;
            const int64_t y = (((u * u) >> 16) * ay + ((2 * u * t) >> 16) * cy + ((t * t) >> 16) * by) >> 16;
            const int ix = int(x >> 16), iy = int(y >> 16);
            for (int yy = iy - wdt / 2 - 1; yy <= iy + wdt / 2 + 1; ++yy)
                for (int xx = ix - wdt / 2 - 1; xx <= ix + wdt / 2 + 1; ++xx) {
                    if (!c.inside(xx, yy)) continue;
                    const int64_t ddx = (int64_t(xx) << 16) + 32768 - x, ddy = (int64_t(yy) << 16) + 32768 - y;
                    const int64_t r = (int64_t(wdt) << 15) + 16384;
                    if (((ddx * ddx) >> 16) + ((ddy * ddy) >> 16) <= ((r * r) >> 16)) {
                        c.cell[size_t(c.ci(xx, yy))] = '.';
                        c.biome[size_t(c.ci(xx, yy))] = BSteppe;
                    }
                }
        }
    };
    if (c.starts.size() >= 2) {
        for (size_t a = 0; a < c.starts.size(); ++a) {
            const size_t b = (a + 1) % c.starts.size();
            if (c.starts.size() == 2 && a == 1) break;
            for (int l = 0; l < lanes; ++l) {
                const int64_t off = lanes == 1 ? 0 : (int64_t(l) * 2 - (lanes - 1)) * bow / (lanes - 1);
                carve(c.starts[a].x, c.starts[a].y, c.starts[b].x, c.starts[b].y, off, width);
            }
        }
        if (c.starts.size() > 2 && c.pbool("centre", true))      // spokes to the middle
            for (auto& s : c.starts) carve(s.x, s.y, int64_t(W) << 15, int64_t(H) << 15, 0, width);
    }
    for (auto& s : c.starts) {
        const int sx = int(s.x >> 16), sy = int(s.y >> 16), r = c.baseRadius;
        for (int y = sy - r; y <= sy + r; ++y)
            for (int x = sx - r; x <= sx + r; ++x)
                if (c.inside(x, y) && (x - sx) * (x - sx) + (y - sy) * (y - sy) <= r * r) { c.cell[size_t(c.ci(x, y))] = '.'; c.biome[size_t(c.ci(x, y))] = BGrass; }
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) if ((x == 0 || y == 0 || x == W - 1 || y == H - 1) && !open) c.cell[size_t(c.ci(x, y))] = '#';
    if (c.sym.exact()) { symmetrizeCells(c.cell, W, H, c.sym); symmetrizeCells(c.biome, W, H, c.sym); }
}

// --------------------------------------------------------------- grid + connectivity

std::vector<int16_t> cornersOut(const Ctx& c) {
    std::vector<int16_t> out(c.hg.v.size());
    for (size_t i = 0; i < out.size(); ++i) out[i] = int16_t(std::clamp<int32_t>(c.hg.v[i] / (kHU / 16), -32000, 32000));
    return out;
}

void buildGrid(const Ctx& c, game::MapGrid& g, bool withObjects) {
    g.init(c.w, c.h);
    for (int i = 0; i < c.w * c.h; ++i) {
        const int dk = c.R.findDiggable(c.cell[size_t(i)]);
        if (dk >= 0) { g.flags[size_t(i)] = 0; g.dig[size_t(i)] = uint8_t(dk + 1); }
        else g.flags[size_t(i)] = game::cellFlagsFromChar(c.cell[size_t(i)]);
        g.level[size_t(i)] = c.model == game::TerrainModel::Levels ? c.lvl[size_t(i)] : 0;
    }
    if (c.model == game::TerrainModel::Heightfield) {
        g.corner = cornersOut(c);
        const game::TerrainRules& tr = c.R.terrain;
        g.slopeLimit[0] = game::MapGrid::slopeUnits(tr.maxSlopeGround);
        g.sandLimit = game::MapGrid::slopeUnits(tr.maxSlopeSand.raw < 0 ? tr.maxSlopeGround : tr.maxSlopeSand);
        g.buildLimit = game::MapGrid::slopeUnits(tr.buildSlope);
        for (size_t k = 0; k < tr.slopeClasses.size() && k < 15; ++k) g.slopeLimit[k + 1] = game::MapGrid::slopeUnits(tr.slopeClasses[k]);
        g.enableHeightfield(c.R.levelHeight);
    }
    if (withObjects) for (auto& r : c.blockRects) g.block(r[0], r[1], r[2], r[3], 1);
}

std::vector<uint8_t> reachable(const Ctx& c, const game::MapGrid& g, int sx, int sy, bool dig) {
    std::vector<uint8_t> seen(size_t(c.w * c.h), 0);
    if (!c.inside(sx, sy)) return seen;
    auto ok = [&](int x0, int y0, int x1, int y1) {
        if (dig && g.dig[size_t(c.ci(x1, y1))]) return true;
        return g.stepAllowed(x0, y0, x1, y1, game::kLayerGround);
    };
    std::queue<int> q;
    q.push(c.ci(sx, sy)); seen[size_t(c.ci(sx, sy))] = 1;
    while (!q.empty()) {
        int i = q.front(); q.pop();
        int x = i % c.w, y = i / c.w;
        for (auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
            int nx = x + dx, ny = y + dy;
            if (!c.inside(nx, ny) || seen[size_t(c.ci(nx, ny))]) continue;
            if (dig && g.dig[size_t(i)] == 0 && !g.passable(x, y, game::kLayerGround) && !(x == sx && y == sy)) continue;
            if (!ok(x, y, nx, ny)) continue;
            seen[size_t(c.ci(nx, ny))] = 1; q.push(c.ci(nx, ny));
        }
    }
    return seen;
}

namespace {
// cheapest cell path from a to b where every non-walkable step costs `penalty`
std::vector<int> repairPath(const Ctx& c, const game::MapGrid& g, int ax, int ay, int bx, int by, bool dig) {
    const int N = c.w * c.h;
    std::vector<int32_t> dist(static_cast<size_t>(N), INT32_MAX), par(static_cast<size_t>(N), -1);
    using It = std::pair<int32_t, int32_t>;
    std::priority_queue<It, std::vector<It>, std::greater<It>> pq;
    const int s = c.ci(ax, ay), t = c.ci(bx, by);
    dist[size_t(s)] = 0; pq.push({0, s});
    while (!pq.empty()) {
        auto [d, i] = pq.top(); pq.pop();
        if (d != dist[size_t(i)]) continue;
        if (i == t) break;
        const int x = i % c.w, y = i / c.w;
        for (auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
            const int nx = x + dx, ny = y + dy;
            if (!c.inside(nx, ny) || nx == 0 || ny == 0 || nx == c.w - 1 || ny == c.h - 1) continue;
            const int j = c.ci(nx, ny);
            int cost = 10;
            const bool walk = (dig && g.dig[size_t(j)]) || g.stepAllowed(x, y, nx, ny, game::kLayerGround);
            if (!walk) cost = c.cell[size_t(j)] == '~' ? 60 : 90;
            if (dist[size_t(i)] + cost < dist[size_t(j)]) { dist[size_t(j)] = dist[size_t(i)] + cost; par[size_t(j)] = i; pq.push({dist[size_t(j)], j}); }
        }
    }
    std::vector<int> path;
    if (dist[size_t(t)] == INT32_MAX) return path;
    for (int i = t; i >= 0; i = par[size_t(i)]) path.push_back(i);
    std::reverse(path.begin(), path.end());
    return path;
}
} // namespace

void ensureConnectivity(Ctx& c) {
    if (c.connect == "none" || c.starts.size() < 2) return;
    const bool dig = c.digPlain != 0;
    const int32_t maxStep = c.model == game::TerrainModel::Heightfield
        ? int32_t((int64_t(c.R.terrain.maxSlopeGround.raw) * kHU >> 16) * 7 / 10) : 0;   // margin under the limit
    for (int round = 0; round < 12; ++round) {
        game::MapGrid g;
        buildGrid(c, g, false);
        const int sx = int(c.starts[0].x >> 16), sy = int(c.starts[0].y >> 16);
        std::vector<uint8_t> seen = reachable(c, g, sx, sy, dig);
        int bad = -1;
        for (size_t k = 1; k < c.starts.size(); ++k)
            if (!seen[size_t(c.ci(int(c.starts[k].x >> 16), int(c.starts[k].y >> 16)))]) { bad = int(k); break; }
        if (bad < 0) {
            c.info.set("repairs", Json::number(int64_t(round)));
            // reserve the walkable route between consecutive starts so objects never wall it off
            for (size_t k = 1; k < c.starts.size(); ++k) {
                std::vector<int> p = repairPath(c, g, int(c.starts[k - 1].x >> 16), int(c.starts[k - 1].y >> 16), int(c.starts[k].x >> 16), int(c.starts[k].y >> 16), dig);
                if (!p.empty()) c.corridors.push_back(std::move(p));
            }
            return;
        }
        std::vector<int> path = repairPath(c, g, sx, sy, int(c.starts[size_t(bad)].x >> 16), int(c.starts[size_t(bad)].y >> 16), dig);
        if (path.empty()) break;
        c.corridors.push_back(path);
        if (c.model == game::TerrainModel::Heightfield) {
            // a road along the path: smoothed heights, climb per cell limited to a quarter of the slope limit,
            // written to the corners of the path cells and their neighbours (all symmetric images)
            std::vector<int32_t> t(path.size());
            for (size_t k = 0; k < path.size(); ++k) {
                int64_t sum = 0; int cnt = 0;
                for (int d = -4; d <= 4; ++d) {
                    const int64_t j = int64_t(k) + d;
                    if (j < 0 || j >= int64_t(path.size())) continue;
                    sum += std::max(c.hg.cell(path[size_t(j)] % c.w, path[size_t(j)] / c.w), c.sea + kHU / 4); ++cnt;
                }
                t[k] = int32_t(sum / cnt);
            }
            const int32_t step = std::max<int32_t>(1, maxStep * 10 / 28);
            for (size_t k = 1; k < t.size(); ++k) t[k] = std::clamp(t[k], t[k - 1] - step, t[k - 1] + step);
            for (size_t k = t.size() - 1; k-- > 0;) t[k] = std::clamp(t[k], t[k + 1] - step, t[k + 1] + step);
            for (size_t k = 0; k < path.size(); ++k) {
                const int x = path[k] % c.w, y = path[k] / c.w;
                for (int cy = y - 1; cy <= y + 2; ++cy)
                    for (int cx = x - 1; cx <= x + 2; ++cx) {
                        if (cx < 0 || cy < 0 || cx > c.w || cy > c.h) continue;
                        c.forCornerImages(cx, cy, [&](int ix, int iy) { c.hg.at(ix, iy) = t[k]; });
                    }
                c.forImages(x, y, [&](int ix, int iy, int) { char& ch = c.cell[size_t(c.ci(ix, iy))]; if (ch == '~' || ch == '#' || ch == '=') ch = '.'; });
            }
            continue;
        }
        // fix every blocking step on the path, on all symmetric images
        int8_t curLv = c.lvl[size_t(path[0])];
        for (size_t k = 1; k < path.size(); ++k) {
            const int i = path[k], pi = path[k - 1];
            const int x = i % c.w, y = i / c.w, px = pi % c.w, py = pi / c.w;
            const bool walk = (dig && g.dig[size_t(i)]) || g.stepAllowed(px, py, x, y, game::kLayerGround);
            if (walk) { curLv = c.lvl[size_t(i)]; continue; }
            c.forImages(x, y, [&](int ix, int iy, int) {
                char& ch = c.cell[size_t(c.ci(ix, iy))];
                if (ch == '~') ch = c.connect == "shallows" || c.shape == "islands" ? '=' : '.';
                else if (ch == '#') ch = c.rockBases ? 'R' : '.';
                else if (dig && c.R.findDiggable(ch) < 0 && !(game::cellFlagsFromChar(ch) & game::kCellGround)) ch = '.';
            });
            if (c.model == game::TerrainModel::Levels) {
                const int8_t l = c.lvl[size_t(i)];
                if (l != curLv) {   // ramp: step the level by one toward the target and mark both cells
                    const int8_t nl = int8_t(curLv + (l > curLv ? 1 : -1));
                    c.forImages(x, y, [&](int ix, int iy, int) { c.lvl[size_t(c.ci(ix, iy))] = nl; c.cell[size_t(c.ci(ix, iy))] = '/'; });
                    c.forImages(px, py, [&](int ix, int iy, int) { if (c.cell[size_t(c.ci(ix, iy))] != '~') c.cell[size_t(c.ci(ix, iy))] = '/'; });
                    curLv = nl;
                } else curLv = l;
            }
        }
    }
    c.warn("start areas could not be fully connected (check terrain params)");
}

Json materialRows(const Ctx& c) {
    if (c.layerIds.empty()) return Json();
    // preferred layer ids per biome, first match wins
    static const std::vector<std::vector<const char*>> pref = {
        {"lakebed", "earth", "sand", "dirt"},               // ocean (bed)
        {"sand", "beach", "path", "earth"},                 // coast
        {"grass", "snow", "earth"},                         // grass
        {"forest", "moss", "grass", "earth", "snow"},       // forest
        {"sand", "desert", "dune", "path", "earth"},        // desert
        {"dirt", "earth", "steppe", "path", "grass"},       // steppe
        {"rock", "stone", "cliff"},                          // rock
        {"snow", "ice", "rock"},                             // snow
        {"mud", "swamp", "earth", "dirt"},                   // swamp
    };
    std::vector<int> layerOf(BCount, -1);
    for (int b = 0; b < BCount; ++b)
        for (const char* id : pref[size_t(b)]) {
            auto it = std::find(c.layerIds.begin(), c.layerIds.end(), id);
            if (it != c.layerIds.end()) { layerOf[size_t(b)] = int(it - c.layerIds.begin()); break; }
        }
    Json rows = Json::array();
    bool any = false;
    for (int y = 0; y < c.h; ++y) {
        std::string r(size_t(c.w), '.');
        for (int x = 0; x < c.w; ++x) {
            const int l = layerOf[c.biome[size_t(c.ci(x, y))]];
            if (l >= 0 && l < 4) { r[size_t(x)] = char('0' + l); any = true; }
        }
        rows.push(Json::string(r));
    }
    return any ? rows : Json();
}

Json heightsOut(const Ctx& c) {
    if (c.model != game::TerrainModel::Levels || c.pbool("fineHeights", false) == false) return Json();
    Json a = Json::array();
    bool any = false;
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x) {
            // gentle undulation inside a level (visual only): +-0.25 units of the noise height
            const int32_t v = c.hg.cell(x, y);
            int32_t d = int32_t(((int64_t(v) - c.sea) % (c.levelStep > 0 ? c.levelStep : kHU)) * 4 / std::max(1, c.levelStep > 0 ? c.levelStep : kHU));
            if (c.cell[size_t(c.ci(x, y))] == '~') d = 0;
            const int16_t h16 = int16_t(std::clamp(d, -4, 4));
            any = any || h16 != 0;
            a.push(Json::number(int64_t(h16)));
        }
    return any ? a : Json();
}

} // namespace rts::gen
