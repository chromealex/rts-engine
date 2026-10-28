#include "TextureCodec.h"

#include <algorithm>
#include <cstring>

#include "../../third_party/zstd/zstd.h"

namespace rts::render {

namespace {

inline uint8_t clamp255(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }

// ---------------------------------------------------------------------------------------------
// ETC1 / ETC2 / EAC (Khronos Data Format Specification, sections "ETC1" and "ETC2").
// Blocks are big-endian 64-bit words; pixel indices are stored column-major (i = x * 4 + y).
// ---------------------------------------------------------------------------------------------

const int kEtcModifiers[8][4] = {
    {2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
    {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183},
};
const int kEtcDistances[8] = {3, 6, 11, 16, 23, 32, 41, 64};
const int kEacModifiers[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10}, {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9}, {-2, -4, -8, -10, 1, 3, 7, 9}, {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9}, {-1, -2, -3, -10, 0, 1, 2, 9}, {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8},
};

inline int ext4(uint32_t v) { return int(v << 4 | v); }
inline int ext5(uint32_t v) { return int(v << 3 | v >> 2); }
inline int ext6(uint32_t v) { return int(v << 2 | v >> 4); }
inline int ext7(uint32_t v) { return int(v << 1 | v >> 6); }
inline int sext3(uint32_t v) { return v & 4 ? int(v) - 8 : int(v); }

struct Rgb { int r, g, b; };
inline Rgb add(Rgb c, int d) { return {clamp255(c.r + d), clamp255(c.g + d), clamp255(c.b + d)}; }

void decodeEtc2Rgb(const uint8_t* b, uint8_t out[64]) {
    const uint32_t hi = be32(b), lo = be32(b + 4);
    auto index = [&](int x, int y) { const int i = x * 4 + y; return int((lo >> (16 + i) & 1) << 1 | (lo >> i & 1)); };
    auto put = [&](int x, int y, Rgb c) {
        uint8_t* p = out + (y * 4 + x) * 4;
        p[0] = uint8_t(c.r); p[1] = uint8_t(c.g); p[2] = uint8_t(c.b); p[3] = 255;
    };
    const bool diff = hi >> 1 & 1, flip = hi & 1;
    Rgb c1{}, c2{};
    if (!diff) {
        c1 = {ext4(hi >> 28 & 15), ext4(hi >> 20 & 15), ext4(hi >> 12 & 15)};
        c2 = {ext4(hi >> 24 & 15), ext4(hi >> 16 & 15), ext4(hi >> 8 & 15)};
    } else {
        const int r = int(hi >> 27 & 31), g = int(hi >> 19 & 31), bl = int(hi >> 11 & 31);
        const int r2 = r + sext3(hi >> 24 & 7), g2 = g + sext3(hi >> 16 & 7), b2 = bl + sext3(hi >> 8 & 7);
        if (r2 < 0 || r2 > 31) {   // T mode
            const Rgb t1{ext4((hi >> 27 & 3) << 2 | (hi >> 24 & 3)), ext4(hi >> 20 & 15), ext4(hi >> 16 & 15)};
            const Rgb t2{ext4(hi >> 12 & 15), ext4(hi >> 8 & 15), ext4(hi >> 4 & 15)};
            const int d = kEtcDistances[(hi >> 2 & 3) << 1 | (hi & 1)];
            const Rgb paint[4] = {t1, add(t2, d), t2, add(t2, -d)};
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) put(x, y, paint[index(x, y)]);
            return;
        }
        if (g2 < 0 || g2 > 31) {   // H mode
            const uint32_t r1 = hi >> 27 & 15, g1 = (hi >> 24 & 7) << 1 | (hi >> 20 & 1), b1 = (hi >> 19 & 1) << 3 | (hi >> 15 & 7);
            const uint32_t rr2 = hi >> 11 & 15, gg2 = hi >> 7 & 15, bb2 = hi >> 3 & 15;
            const uint32_t v1 = r1 << 8 | g1 << 4 | b1, v2 = rr2 << 8 | gg2 << 4 | bb2;
            const int d = kEtcDistances[(hi >> 2 & 1) << 2 | (hi & 1) << 1 | (v1 >= v2 ? 1 : 0)];
            const Rgb h1{ext4(r1), ext4(g1), ext4(b1)}, h2{ext4(rr2), ext4(gg2), ext4(bb2)};
            const Rgb paint[4] = {add(h1, d), add(h1, -d), add(h2, d), add(h2, -d)};
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) put(x, y, paint[index(x, y)]);
            return;
        }
        if (b2 < 0 || b2 > 31) {   // planar mode
            const int ro = ext6(hi >> 25 & 63), go = ext7((hi >> 24 & 1) << 6 | (hi >> 17 & 63));
            const int bo = ext6((hi >> 16 & 1) << 5 | (hi >> 11 & 3) << 3 | (hi >> 7 & 7));
            const int rh = ext6((hi >> 2 & 31) << 1 | (hi & 1)), gh = ext7(lo >> 25 & 127), bh = ext6(lo >> 19 & 63);
            const int rv = ext6(lo >> 13 & 63), gv = ext7(lo >> 6 & 127), bv = ext6(lo & 63);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    put(x, y, {clamp255((x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2),
                               clamp255((x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2),
                               clamp255((x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2)});
            return;
        }
        c1 = {ext5(uint32_t(r)), ext5(uint32_t(g)), ext5(uint32_t(bl))};
        c2 = {ext5(uint32_t(r2)), ext5(uint32_t(g2)), ext5(uint32_t(b2))};
    }
    const int table1 = int(hi >> 5 & 7), table2 = int(hi >> 2 & 7);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const bool second = flip ? y >= 2 : x >= 2;
            put(x, y, add(second ? c2 : c1, kEtcModifiers[second ? table2 : table1][index(x, y)]));
        }
}

// EAC: 8-bit base, 4-bit multiplier, 4-bit table, 16 x 3-bit indices (column-major, MSB first).
uint64_t eacIndices(const uint8_t* b) {
    uint64_t bits = 0;
    for (int i = 2; i < 8; ++i) bits = bits << 8 | b[i];
    return bits;
}

void decodeEacAlpha(const uint8_t* b, uint8_t out[64], int channel) {
    const int base = b[0], mul = b[1] >> 4;
    const int* mods = kEacModifiers[b[1] & 15];
    const uint64_t bits = eacIndices(b);
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y) {
            const int idx = int(bits >> (45 - 3 * (x * 4 + y)) & 7);
            out[(y * 4 + x) * 4 + channel] = clamp255(base + mods[idx] * mul);
        }
}

void decodeEacR11(const uint8_t* b, uint8_t out[64]) {
    const int base = b[0] * 8 + 4, mul = b[1] >> 4;
    const int* mods = kEacModifiers[b[1] & 15];
    const uint64_t bits = eacIndices(b);
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y) {
            const int idx = int(bits >> (45 - 3 * (x * 4 + y)) & 7);
            int v = base + (mul ? mods[idx] * mul * 8 : mods[idx]);
            v = std::clamp(v, 0, 2047);
            uint8_t* p = out + (y * 4 + x) * 4;
            p[0] = uint8_t((v * 255 + 1023) / 2047); p[1] = 0; p[2] = 0; p[3] = 255;
        }
}

// ---------------------------------------------------------------------------------------------
// BC1 / BC3 / BC4 (D3D block compression). Little-endian, indices row-major (i = y * 4 + x).
// ---------------------------------------------------------------------------------------------

void decodeBc1(const uint8_t* b, uint8_t out[64], bool fourColor) {
    const uint32_t c0 = uint32_t(b[0]) | uint32_t(b[1]) << 8, c1 = uint32_t(b[2]) | uint32_t(b[3]) << 8;
    auto expand = [](uint32_t c, uint8_t* p) {
        p[0] = uint8_t(ext5(c >> 11 & 31)); p[1] = uint8_t(ext6(c >> 5 & 63)); p[2] = uint8_t(ext5(c & 31)); p[3] = 255;
    };
    uint8_t pal[4][4];
    expand(c0, pal[0]); expand(c1, pal[1]);
    for (int k = 0; k < 3; ++k) {
        if (c0 > c1 || fourColor) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    pal[2][3] = 255;
    pal[3][3] = (c0 > c1 || fourColor) ? 255 : 0;
    const uint32_t idx = le32(b + 4);
    for (int i = 0; i < 16; ++i) std::memcpy(out + i * 4, pal[idx >> (2 * i) & 3], 4);
}

void decodeBc4(const uint8_t* b, uint8_t out[64], int channel) {
    const int a0 = b[0], a1 = b[1];
    int pal[8] = {a0, a1};
    if (a0 > a1) {
        for (int i = 2; i < 8; ++i) pal[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
    } else {
        for (int i = 2; i < 6; ++i) pal[i] = ((6 - i) * a0 + (i - 1) * a1) / 5;
        pal[6] = 0; pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 7; i >= 2; --i) bits = bits << 8 | b[i];
    for (int i = 0; i < 16; ++i) out[i * 4 + channel] = uint8_t(pal[bits >> (3 * i) & 7]);
}

// ---------------------------------------------------------------------------------------------
// BC7 (BPTC UNORM, ARB_texture_compression_bptc / D3D11). Tables are from the specification:
// partitions packed as 2 bits per pixel (pixel 0 in the low bits), anchors of subsets 1 and 2.
// ---------------------------------------------------------------------------------------------

const uint32_t kBc7P2[64] = {
    0x50505050, 0x40404040, 0x54545454, 0x54505040, 0x50404000, 0x55545450, 0x55545040, 0x54504000,
    0x50400000, 0x55555450, 0x55544000, 0x54400000, 0x55555440, 0x55550000, 0x55555500, 0x55000000,
    0x55150100, 0x00004054, 0x15010000, 0x00405054, 0x00004050, 0x15050100, 0x05010000, 0x40505054,
    0x00404050, 0x05010100, 0x14141414, 0x05141450, 0x01155440, 0x00555500, 0x15014054, 0x05414150,
    0x44444444, 0x55005500, 0x11441144, 0x05055050, 0x05500550, 0x11114444, 0x41144114, 0x44111144,
    0x15055054, 0x01055040, 0x05041050, 0x05455150, 0x14414114, 0x50050550, 0x41411414, 0x00141400,
    0x00041504, 0x00105410, 0x10541000, 0x04150400, 0x50410514, 0x41051450, 0x05415014, 0x14054150,
    0x41050514, 0x41505014, 0x40011554, 0x54150140, 0x50505500, 0x00555050, 0x15151010, 0x54540404,
};
const uint32_t kBc7P3[64] = {
    0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050, 0x5555a0a0, 0x5a5a5050,
    0xaa550000, 0xaa555500, 0xaaaa5500, 0x90909090, 0x94949494, 0xa4a4a4a4, 0xa9a59450, 0x2a0a4250,
    0xa5945040, 0x0a425054, 0xa5a5a500, 0x55a0a0a0, 0xa8a85454, 0x6a6a4040, 0xa4a45000, 0x1a1a0500,
    0x0050a4a4, 0xaaa59090, 0x14696914, 0x69691400, 0xa08585a0, 0xaa821414, 0x50a4a450, 0x6a5a0200,
    0xa9a58000, 0x5090a0a8, 0xa8a09050, 0x24242424, 0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50,
    0x500aa550, 0xaaaa4444, 0x66660000, 0xa5a0a5a0, 0x50a050a0, 0x69286928, 0x44aaaa44, 0x66666600,
    0xaa444444, 0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580, 0xaa141414, 0x96960000,
    0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000, 0x40804080, 0xa9a8a9a8, 0xaaaaaa44, 0x2a4a5254,
};
const uint8_t kBc7A2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
};
const uint8_t kBc7A3a[64] = {
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3,
    3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15,
    3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
};
const uint8_t kBc7A3b[64] = {
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,
    15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8,
    15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
};
const uint8_t kBc7Weights2[4] = {0, 21, 43, 64};
const uint8_t kBc7Weights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
const uint8_t kBc7Weights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Bc7Mode { int ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2; };
const Bc7Mode kBc7Modes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

struct BitReader {
    const uint8_t* p;
    int pos = 0;
    uint32_t read(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i, ++pos) v |= uint32_t(p[pos >> 3] >> (pos & 7) & 1) << i;
        return v;
    }
};

inline const uint8_t* bc7Weights(int bits) { return bits == 2 ? kBc7Weights2 : bits == 3 ? kBc7Weights3 : kBc7Weights4; }

void decodeBc7(const uint8_t* b, uint8_t out[64]) {
    int mode = 0;
    while (mode < 8 && !(b[0] >> mode & 1)) ++mode;
    if (mode == 8) { std::memset(out, 0, 64); return; }   // reserved encoding
    const Bc7Mode& m = kBc7Modes[mode];
    BitReader br{b, mode + 1};
    const int partition = int(br.read(m.pb)), rotation = int(br.read(m.rb)), indexSel = int(br.read(m.isb));
    int ep[3][2][4] = {};   // [subset][endpoint][rgba]
    for (int c = 0; c < 3; ++c)
        for (int s = 0; s < m.ns; ++s)
            for (int e = 0; e < 2; ++e) ep[s][e][c] = int(br.read(m.cb));
    for (int s = 0; s < m.ns; ++s)
        for (int e = 0; e < 2; ++e) ep[s][e][3] = m.ab ? int(br.read(m.ab)) : 255;
    int pbits[3][2] = {};
    if (m.epb) for (int s = 0; s < m.ns; ++s) for (int e = 0; e < 2; ++e) pbits[s][e] = int(br.read(1));
    if (m.spb) for (int s = 0; s < m.ns; ++s) pbits[s][0] = pbits[s][1] = int(br.read(1));
    const bool hasP = m.epb || m.spb;
    for (int s = 0; s < m.ns; ++s)
        for (int e = 0; e < 2; ++e)
            for (int c = 0; c < 4; ++c) {
                const int bits = c < 3 ? m.cb : m.ab;
                if (bits == 0) continue;   // alpha absent: stays 255
                int v = ep[s][e][c], n = bits;
                if (hasP) { v = v << 1 | pbits[s][e]; ++n; }
                v <<= 8 - n;
                ep[s][e][c] = v | v >> n;
            }
    const uint32_t part = m.ns == 2 ? kBc7P2[partition] : m.ns == 3 ? kBc7P3[partition] : 0;
    auto subsetOf = [&](int i) { return int(part >> (2 * i) & 3); };
    auto isAnchor = [&](int i) {
        if (i == 0) return true;
        if (m.ns == 2) return i == kBc7A2[partition];
        if (m.ns == 3) return i == kBc7A3a[partition] || i == kBc7A3b[partition];
        return false;
    };
    int idx1[16], idx2[16] = {};
    for (int i = 0; i < 16; ++i) idx1[i] = int(br.read(isAnchor(i) ? m.ib - 1 : m.ib));
    if (m.ib2) for (int i = 0; i < 16; ++i) idx2[i] = int(br.read(i == 0 ? m.ib2 - 1 : m.ib2));
    // Index selection: color from the secondary index when the selection bit is set (mode 4);
    // alpha from the secondary index when present and not swapped.
    const int colorBits = m.ib2 && indexSel ? m.ib2 : m.ib, alphaBits = m.ib2 && !indexSel ? m.ib2 : m.ib;
    const uint8_t* wc = bc7Weights(colorBits);
    const uint8_t* wa = bc7Weights(alphaBits);
    for (int i = 0; i < 16; ++i) {
        const int s = subsetOf(i);
        const int ci = m.ib2 && indexSel ? idx2[i] : idx1[i], ai = m.ib2 && !indexSel ? idx2[i] : idx1[i];
        int px[4];
        for (int c = 0; c < 4; ++c) {
            const int w = c < 3 ? wc[ci] : wa[ai];
            px[c] = ((64 - w) * ep[s][0][c] + w * ep[s][1][c] + 32) >> 6;
        }
        if (rotation) std::swap(px[3], px[rotation - 1]);
        for (int c = 0; c < 4; ++c) out[i * 4 + c] = uint8_t(px[c]);
    }
}

// ---------------------------------------------------------------------------------------------
// KTX 2.0
// ---------------------------------------------------------------------------------------------

const uint8_t kKtx2Id[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};

bool mapVkFormat(uint32_t vk, BlockFormat& f, bool& srgb) {
    srgb = false;
    switch (vk) {
    case 37: f = BlockFormat::None; return true;                       // R8G8B8A8_UNORM
    case 43: f = BlockFormat::None; srgb = true; return true;          // R8G8B8A8_SRGB
    case 131: case 133: f = BlockFormat::BC1; return true;             // BC1_RGB(A)_UNORM
    case 132: case 134: f = BlockFormat::BC1; srgb = true; return true;
    case 137: f = BlockFormat::BC3; return true;
    case 138: f = BlockFormat::BC3; srgb = true; return true;
    case 139: f = BlockFormat::BC4; return true;
    case 145: f = BlockFormat::BC7; return true;
    case 146: f = BlockFormat::BC7; srgb = true; return true;
    case 147: f = BlockFormat::ETC2_RGB8; return true;
    case 148: f = BlockFormat::ETC2_RGB8; srgb = true; return true;
    case 151: f = BlockFormat::ETC2_RGBA8; return true;
    case 152: f = BlockFormat::ETC2_RGBA8; srgb = true; return true;
    case 153: f = BlockFormat::EAC_R11; return true;
    case 157: f = BlockFormat::ASTC_4x4; return true;
    case 158: f = BlockFormat::ASTC_4x4; srgb = true; return true;
    default: return false;
    }
}

} // namespace

int blockBytes(BlockFormat f) {
    switch (f) {
    case BlockFormat::ETC2_RGB8: case BlockFormat::EAC_R11: case BlockFormat::BC1: case BlockFormat::BC4: return 8;
    case BlockFormat::ETC2_RGBA8: case BlockFormat::BC3: case BlockFormat::BC7: case BlockFormat::ASTC_4x4: return 16;
    case BlockFormat::None: break;
    }
    return 4;
}

const char* blockFormatName(BlockFormat f) {
    switch (f) {
    case BlockFormat::None: return "RGBA8";
    case BlockFormat::ETC2_RGB8: return "ETC2_RGB8";
    case BlockFormat::ETC2_RGBA8: return "ETC2_RGBA8";
    case BlockFormat::EAC_R11: return "EAC_R11";
    case BlockFormat::BC1: return "BC1";
    case BlockFormat::BC3: return "BC3";
    case BlockFormat::BC4: return "BC4";
    case BlockFormat::BC7: return "BC7";
    case BlockFormat::ASTC_4x4: return "ASTC_4x4";
    }
    return "?";
}

int levelWidth(const CompressedImage& img, int level) { return std::max(1, img.width >> level); }
int levelHeight(const CompressedImage& img, int level) { return std::max(1, img.height >> level); }

size_t levelByteSize(BlockFormat f, int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    if (f == BlockFormat::None) return size_t(width) * size_t(height) * 4;
    return size_t((width + 3) / 4) * size_t((height + 3) / 4) * size_t(blockBytes(f));
}

bool isKtx2(const std::string& bytes) {
    return bytes.size() >= sizeof(kKtx2Id) && std::memcmp(bytes.data(), kKtx2Id, sizeof(kKtx2Id)) == 0;
}

bool parseKtx2(const std::string& bytes, CompressedImage& out, std::string& err) {
    out = CompressedImage{};
    const auto* d = reinterpret_cast<const uint8_t*>(bytes.data());
    const uint64_t size = bytes.size();
    if (!isKtx2(bytes)) { err = "not a KTX 2.0 file"; return false; }
    if (size < 80) { err = "KTX2: truncated header"; return false; }
    const uint32_t vkFormat = le32(d + 12), width = le32(d + 20), height = le32(d + 24), depth = le32(d + 28);
    const uint32_t layers = le32(d + 32), faces = le32(d + 36), levelCount = le32(d + 40), scheme = le32(d + 44);
    if (scheme == 1 || vkFormat == 0) { err = "KTX2 BasisLZ/UASTC needs a transcoder; encode as ETC2/BC7/ASTC"; return false; }
    if (scheme != 0 && scheme != 2) { err = "KTX2: unsupported supercompression scheme " + std::to_string(scheme) + " (use none or zstd)"; return false; }
    BlockFormat fmt;
    bool srgb;
    if (!mapVkFormat(vkFormat, fmt, srgb)) { err = "KTX2: unsupported vkFormat " + std::to_string(vkFormat); return false; }
    if (width == 0 || height == 0 || width > 16384 || height > 16384) { err = "KTX2: bad size " + std::to_string(width) + "x" + std::to_string(height); return false; }
    if (depth > 1) { err = "KTX2: 3D textures are not supported"; return false; }
    if (faces != 1 && faces != 6) { err = "KTX2: bad faceCount"; return false; }
    if (layers > 2048) { err = "KTX2: bad layerCount"; return false; }
    const uint32_t levels = std::max(1u, levelCount);
    uint32_t maxLevels = 1;
    while ((std::max(width, height) >> maxLevels) > 0) ++maxLevels;
    if (levels > maxLevels) { err = "KTX2: levelCount " + std::to_string(levelCount) + " exceeds the mip chain"; return false; }
    if (80 + uint64_t(levels) * 24 > size) { err = "KTX2: truncated level index"; return false; }
    const uint64_t images = uint64_t(std::max(1u, layers)) * faces;
    out.format = fmt; out.width = int(width); out.height = int(height); out.srgb = srgb; out.vkFormat = vkFormat;
    out.levels.resize(levels);
    for (uint32_t l = 0; l < levels; ++l) {
        const uint8_t* e = d + 80 + l * 24;
        const uint64_t off = le64(e), len = le64(e + 8), raw = le64(e + 16);
        const size_t imageBytes = levelByteSize(fmt, levelWidth(out, int(l)), levelHeight(out, int(l)));
        const uint64_t expected = imageBytes * images;
        const std::string where = "KTX2 level " + std::to_string(l) + ": ";
        if (off > size || len > size - off) { err = where + "data out of file bounds"; out = {}; return false; }
        std::vector<uint8_t>& dst = out.levels[l];
        if (scheme == 0) {
            if (len < expected) { err = where + "data too short"; out = {}; return false; }
            dst.assign(d + off, d + off + imageBytes);
        } else {
            if (raw != expected) { err = where + "unexpected uncompressed size"; out = {}; return false; }
            std::vector<uint8_t> tmp(static_cast<size_t>(raw));
            const size_t r = ZSTD_decompress(tmp.data(), tmp.size(), d + off, size_t(len));
            if (ZSTD_isError(r) || r != raw) { err = where + "zstd: " + (ZSTD_isError(r) ? ZSTD_getErrorName(r) : "size mismatch"); out = {}; return false; }
            tmp.resize(imageBytes);   // keep the first layer / face only
            dst = std::move(tmp);
        }
    }
    return true;
}

bool decodeBlock(BlockFormat f, const uint8_t* block, uint8_t out[64]) {
    switch (f) {
    case BlockFormat::ETC2_RGB8: decodeEtc2Rgb(block, out); return true;
    case BlockFormat::ETC2_RGBA8: decodeEtc2Rgb(block + 8, out); decodeEacAlpha(block, out, 3); return true;
    case BlockFormat::EAC_R11: decodeEacR11(block, out); return true;
    case BlockFormat::BC1: decodeBc1(block, out, false); return true;
    case BlockFormat::BC3: decodeBc1(block + 8, out, true); decodeBc4(block, out, 3); return true;
    case BlockFormat::BC4:
        decodeBc4(block, out, 0);
        for (int i = 0; i < 16; ++i) { out[i * 4 + 1] = 0; out[i * 4 + 2] = 0; out[i * 4 + 3] = 255; }
        return true;
    case BlockFormat::BC7: decodeBc7(block, out); return true;
    case BlockFormat::ASTC_4x4: case BlockFormat::None: break;
    }
    return false;
}

bool decodeBlocks(const CompressedImage& in, int level, std::vector<uint8_t>& rgba) {
    if (level < 0 || size_t(level) >= in.levels.size() || in.width <= 0 || in.height <= 0) return false;
    const int w = levelWidth(in, level), h = levelHeight(in, level);
    const std::vector<uint8_t>& src = in.levels[size_t(level)];
    if (src.size() < levelByteSize(in.format, w, h)) return false;
    if (in.format == BlockFormat::None) {
        rgba.assign(src.begin(), src.begin() + ptrdiff_t(size_t(w) * size_t(h) * 4));
        return true;
    }
    if (in.format == BlockFormat::ASTC_4x4) return false;
    rgba.assign(size_t(w) * size_t(h) * 4, 0);
    const int bw = (w + 3) / 4, bh = (h + 3) / 4, bb = blockBytes(in.format);
    // BC1_RGB_* (vkFormat 131/132) has no alpha: the GPU samples index-3 black as opaque.
    const bool bc1Opaque = in.format == BlockFormat::BC1 && (in.vkFormat == 131 || in.vkFormat == 132);
    uint8_t px[64];
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            if (!decodeBlock(in.format, src.data() + size_t(by * bw + bx) * size_t(bb), px)) return false;
            if (bc1Opaque) for (int i = 0; i < 16; ++i) px[i * 4 + 3] = 255;
            for (int y = 0; y < 4 && by * 4 + y < h; ++y) {
                const int cols = std::min(4, w - bx * 4);
                std::memcpy(rgba.data() + (size_t(by * 4 + y) * size_t(w) + size_t(bx * 4)) * 4, px + y * 16, size_t(cols) * 4);
            }
        }
    return true;
}

} // namespace rts::render
