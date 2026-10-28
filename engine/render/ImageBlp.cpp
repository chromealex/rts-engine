// Blizzard BLP textures (Warcraft III, World of Warcraft), read from the user's own game copy.
//
//   BLP1 (Warcraft III classic): content 0 = JPEG (a JPEG header shared by all mips + per-mip
//        scan data; 4 components stored as B, G, R, A without any colour transform, which
//        stb_image cannot decode, hence the small baseline decoder below), content 1 = paletted
//        (256 BGRA colours, one index byte per pixel, then 0 / 1 / 4 / 8 alpha bits per pixel).
//   BLP2 (WoW, Reforged legacy): encoding 1 = paletted (as BLP1), 2 = DXT1 / DXT3 / DXT5,
//        3 = uncompressed BGRA.
// Only mip 0 is decoded (the engine builds its own mips). Every read is bounds-checked: broken
// files return false.
#include "Model.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rts::render {

namespace {

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// ---------------------------------------------------------------- baseline JPEG

const uint8_t kZigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
                             28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// Baseline (SOF0 / SOF1) huffman JPEG with any number of components (1..4) and sampling factors
// 1..4. Outputs the raw component planes (no colour conversion), upsampled to full size.
class JpegDecoder {
public:
    int width = 0, height = 0, comps = 0;
    bool adobe = false;                  // APP14 marker present (standard JPEGs)
    std::vector<std::vector<uint8_t>> planes;   // per component, width * height

    bool decode(const uint8_t* data, size_t size) {
        d_ = data; n_ = size; p_ = 0;
        if (n_ < 4 || d_[0] != 0xFF || d_[1] != 0xD8) return false;
        p_ = 2;
        bool frame = false;
        while (p_ + 4 <= n_) {
            if (d_[p_] != 0xFF) { ++p_; continue; }
            const uint8_t marker = d_[p_ + 1];
            p_ += 2;
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0xFF) { if (marker == 0xFF) --p_; continue; }
            if (marker == 0xD9) break;
            if (p_ + 2 > n_) return false;
            const size_t len = size_t(d_[p_]) << 8 | d_[p_ + 1];
            if (len < 2 || p_ + len > n_) return false;
            const uint8_t* s = d_ + p_ + 2;
            const size_t sl = len - 2;
            switch (marker) {
                case 0xDB: if (!readDqt(s, sl)) return false; break;
                case 0xC4: if (!readDht(s, sl)) return false; break;
                case 0xC0: case 0xC1: if (!readSof(s, sl)) return false; frame = true; break;
                case 0xC2: case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
                    return false;   // progressive / lossless / arithmetic: not used by BLP
                case 0xDD: if (sl < 2) return false; restart_ = int(s[0]) << 8 | s[1]; break;
                case 0xEE: adobe = true; break;
                case 0xDA: {
                    if (!frame) return false;
                    p_ += len;
                    if (!readScan(s, sl)) return false;
                    continue;
                }
                default: break;   // APPn, COM
            }
            p_ += len;
        }
        if (!frame || !scanned_) return false;
        output();
        return true;
    }

private:
    struct Huff { uint8_t vals[256] = {}; int maxcode[18] = {}; int valptr[17] = {}, mincode[17] = {}; bool set = false; };
    struct Comp { int id = 0, h = 1, v = 1, tq = 0, td = 0, ta = 0, pred = 0, bw = 0, bh = 0; std::vector<uint8_t> px; };

    bool readDqt(const uint8_t* s, size_t n) {
        size_t i = 0;
        while (i < n) {
            const int pq = s[i] >> 4, tq = s[i] & 15;
            ++i;
            if (tq > 3 || i + (pq ? 128u : 64u) > n) return false;
            for (int k = 0; k < 64; ++k) { qt_[tq][k] = pq ? uint16_t(s[i] << 8 | s[i + 1]) : s[i]; i += pq ? 2 : 1; }
        }
        return true;
    }
    bool readDht(const uint8_t* s, size_t n) {
        size_t i = 0;
        while (i + 17 <= n) {
            const int tc = s[i] >> 4, th = s[i] & 15;
            if (tc > 1 || th > 3) return false;
            Huff& h = tc ? ac_[th] : dc_[th];
            int total = 0;
            for (int l = 1; l <= 16; ++l) total += s[i + size_t(l)];
            if (total > 256 || i + 17 + size_t(total) > n) return false;
            std::memcpy(h.vals, s + i + 17, size_t(total));
            int code = 0, k = 0;
            for (int l = 1; l <= 16; ++l) {
                const int cnt = s[i + size_t(l)];
                h.valptr[l] = k; h.mincode[l] = code;
                code += cnt; k += cnt;
                h.maxcode[l] = cnt ? code - 1 : -1;
                code <<= 1;
            }
            h.maxcode[17] = 0x7fffffff;
            h.set = true;
            i += 17 + size_t(total);
        }
        return true;
    }
    bool readSof(const uint8_t* s, size_t n) {
        if (n < 6 || s[0] != 8) return false;
        height = s[1] << 8 | s[2]; width = s[3] << 8 | s[4]; comps = s[5];
        if (width <= 0 || height <= 0 || comps < 1 || comps > 4 || n < 6 + size_t(comps) * 3) return false;
        if (size_t(width) * size_t(height) > (64u << 20)) return false;
        hmax_ = vmax_ = 1;
        for (int c = 0; c < comps; ++c) {
            Comp& k = comp_[c];
            k.id = s[6 + c * 3]; k.h = s[7 + c * 3] >> 4; k.v = s[7 + c * 3] & 15; k.tq = s[8 + c * 3];
            if (k.h < 1 || k.h > 4 || k.v < 1 || k.v > 4 || k.tq > 3) return false;
            hmax_ = std::max(hmax_, k.h); vmax_ = std::max(vmax_, k.v);
        }
        mcux_ = (width + 8 * hmax_ - 1) / (8 * hmax_);
        mcuy_ = (height + 8 * vmax_ - 1) / (8 * vmax_);
        for (int c = 0; c < comps; ++c) {
            Comp& k = comp_[c];
            k.bw = mcux_ * k.h; k.bh = mcuy_ * k.v;
            k.px.assign(size_t(k.bw) * 8 * size_t(k.bh) * 8, 0);
        }
        return true;
    }

    // ---- entropy decoding
    uint32_t bits_ = 0;
    int nbits_ = 0;
    bool marker_ = false;
    int fill() {   // next byte of the scan, 0 after a marker or the end
        if (marker_ || p_ >= n_) { marker_ = true; ++overrun_; return 0; }
        const uint8_t b = d_[p_];
        if (b == 0xFF) {
            const uint8_t nx = p_ + 1 < n_ ? d_[p_ + 1] : 0;
            if (nx == 0) { p_ += 2; return 0xFF; }
            marker_ = true; ++overrun_;
            return 0;
        }
        ++p_;
        return b;
    }
    int bit() {
        if (nbits_ == 0) { bits_ = uint32_t(fill()); nbits_ = 8; }
        --nbits_;
        return int(bits_ >> nbits_) & 1;
    }
    int receive(int s) { int v = 0; for (int i = 0; i < s; ++i) v = v << 1 | bit(); return v; }
    static int extend(int v, int s) { return s == 0 ? 0 : v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }
    int huff(const Huff& h) {
        int code = 0;
        for (int l = 1; l <= 16; ++l) {
            code = code << 1 | bit();
            if (code <= h.maxcode[l]) {
                const int idx = h.valptr[l] + code - h.mincode[l];
                return idx >= 0 && idx < 256 ? h.vals[idx] : 0;
            }
        }
        bad_ = true;
        return 0;
    }
    void resetBits() { bits_ = 0; nbits_ = 0; }

    bool decodeBlock(Comp& c, int bx, int by) {
        const Huff& dc = dc_[c.td];
        const Huff& ac = ac_[c.ta];
        float coef[64] = {};
        const int t = huff(dc);
        if (t > 16) return false;
        c.pred += extend(receive(t), t);
        const uint16_t* q = qt_[c.tq];
        coef[0] = float(c.pred * q[0]);
        for (int k = 1; k < 64;) {
            const int rs = huff(ac), r = rs >> 4, s = rs & 15;
            if (s == 0) {
                if (r != 15) break;
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) return false;
            coef[kZigzag[k]] = float(extend(receive(s), s) * q[k]);
            ++k;
        }
        idct(coef, c.px.data() + size_t(by) * 8 * size_t(c.bw) * 8 + size_t(bx) * 8, size_t(c.bw) * 8);
        return !bad_;
    }

    static void idct(const float* in, uint8_t* out, size_t stride) {
        struct Tab { float c[8][8]; };
        static const Tab tab = [] {
            Tab t{};
            for (int x = 0; x < 8; ++x)
                for (int u = 0; u < 8; ++u)
                    t.c[x][u] = (u == 0 ? 0.70710678f : 1.0f) * std::cos(float(2 * x + 1) * float(u) * 3.14159265f / 16.0f);
            return t;
        }();
        const auto& cosTab = tab.c;
        float tmp[64];
        for (int y = 0; y < 8; ++y)      // rows: tmp[y][x] = sum_u in[y][u] cos(x,u)
            for (int x = 0; x < 8; ++x) {
                float s = 0;
                for (int u = 0; u < 8; ++u) s += in[y * 8 + u] * cosTab[x][u];
                tmp[y * 8 + x] = s;
            }
        for (int x = 0; x < 8; ++x)
            for (int y = 0; y < 8; ++y) {
                float s = 0;
                for (int v = 0; v < 8; ++v) s += tmp[v * 8 + x] * cosTab[y][v];
                const int px = int(std::lround(s * 0.25f + 128.0f));
                out[size_t(y) * stride + size_t(x)] = uint8_t(px < 0 ? 0 : px > 255 ? 255 : px);
            }
    }

    bool readScan(const uint8_t* s, size_t n) {
        if (n < 1) return false;
        const int ns = s[0];
        if (ns < 1 || ns > comps || n < 4 + size_t(ns) * 2) return false;
        Comp* sc[4] = {};
        for (int i = 0; i < ns; ++i) {
            const int id = s[1 + i * 2], tables = s[2 + i * 2];
            for (int c = 0; c < comps; ++c) if (comp_[c].id == id) sc[i] = &comp_[c];
            if (!sc[i]) return false;
            sc[i]->td = tables >> 4; sc[i]->ta = tables & 15;
            if (sc[i]->td > 3 || sc[i]->ta > 3 || !dc_[sc[i]->td].set || !ac_[sc[i]->ta].set) return false;
            sc[i]->pred = 0;
        }
        resetBits();
        marker_ = false;
        overrun_ = 0;
        int sinceRestart = 0;
        auto restart = [&]() {
            if (restart_ <= 0 || sinceRestart < restart_) return;
            sinceRestart = 0;
            resetBits();
            // skip to the RSTn marker
            while (p_ + 1 < n_ && !(d_[p_] == 0xFF && d_[p_ + 1] >= 0xD0 && d_[p_ + 1] <= 0xD7)) ++p_;
            if (p_ + 1 < n_) p_ += 2;
            marker_ = false;
            for (int i = 0; i < ns; ++i) sc[i]->pred = 0;
        };
        if (ns == 1) {   // non-interleaved: the component's own block grid
            Comp& c = *sc[0];
            const int bw = (width * c.h / hmax_ + 7) / 8, bh = (height * c.v / vmax_ + 7) / 8;
            for (int by = 0; by < bh; ++by)
                for (int bx = 0; bx < bw; ++bx) {
                    restart();
                    if (!decodeBlock(c, bx, by) || overrun_ > 64) return false;
                    ++sinceRestart;
                }
        } else {
            for (int my = 0; my < mcuy_; ++my)
                for (int mx = 0; mx < mcux_; ++mx) {
                    restart();
                    for (int i = 0; i < ns; ++i)
                        for (int v = 0; v < sc[i]->v; ++v)
                            for (int h = 0; h < sc[i]->h; ++h)
                                if (!decodeBlock(*sc[i], mx * sc[i]->h + h, my * sc[i]->v + v) || overrun_ > 64) return false;
                    ++sinceRestart;
                }
        }
        // continue after the scan data (next marker)
        while (p_ + 1 < n_ && !(d_[p_] == 0xFF && d_[p_ + 1] != 0 && !(d_[p_ + 1] >= 0xD0 && d_[p_ + 1] <= 0xD7))) ++p_;
        scanned_ = true;
        return true;
    }

    void output() {
        planes.assign(size_t(comps), std::vector<uint8_t>(size_t(width) * size_t(height)));
        for (int c = 0; c < comps; ++c) {
            const Comp& k = comp_[c];
            const int sx = hmax_ / k.h, sy = vmax_ / k.v;   // replicate subsampled components
            const size_t stride = size_t(k.bw) * 8;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    planes[size_t(c)][size_t(y) * size_t(width) + size_t(x)] = k.px[size_t(y / std::max(sy, 1)) * stride + size_t(x / std::max(sx, 1))];
        }
    }

    const uint8_t* d_ = nullptr;
    size_t n_ = 0, p_ = 0;
    uint16_t qt_[4][64] = {};
    Huff dc_[4], ac_[4];
    Comp comp_[4];
    int hmax_ = 1, vmax_ = 1, mcux_ = 0, mcuy_ = 0, restart_ = 0, overrun_ = 0;
    bool bad_ = false, scanned_ = false;
};

// ---------------------------------------------------------------- BLP

bool validSize(uint32_t w, uint32_t h) { return w > 0 && h > 0 && w <= 65536 && h <= 65536 && uint64_t(w) * h <= (64u << 20); }

bool decodePaletted(const uint8_t* d, size_t n, size_t paletteAt, size_t off, size_t size, uint32_t w, uint32_t h, uint32_t alphaBits, Image& out) {
    const size_t px = size_t(w) * h;
    const size_t alphaBytes = alphaBits == 8 ? px : alphaBits == 4 ? (px + 1) / 2 : alphaBits == 1 ? (px + 7) / 8 : 0;
    if (alphaBits != 0 && alphaBits != 1 && alphaBits != 4 && alphaBits != 8) return false;
    if (paletteAt + 1024 > n || off > n || size > n - off || px + alphaBytes > size) return false;
    const uint8_t* pal = d + paletteAt;
    const uint8_t* idx = d + off;
    const uint8_t* al = idx + px;
    out.rgba.resize(px * 4);
    for (size_t i = 0; i < px; ++i) {
        const uint8_t* c = pal + size_t(idx[i]) * 4;   // B, G, R, (unused)
        uint8_t a = 255;
        if (alphaBits == 8) a = al[i];
        else if (alphaBits == 4) a = uint8_t(((al[i / 2] >> ((i & 1) * 4)) & 15) * 17);
        else if (alphaBits == 1) a = (al[i / 8] >> (i & 7)) & 1 ? 255 : 0;
        out.rgba[i * 4 + 0] = c[2]; out.rgba[i * 4 + 1] = c[1]; out.rgba[i * 4 + 2] = c[0]; out.rgba[i * 4 + 3] = a;
    }
    out.width = int(w); out.height = int(h);
    return true;
}

bool decodeBlp1Jpeg(const uint8_t* d, size_t n, size_t off, size_t size, uint32_t w, uint32_t h, uint32_t alphaBits, Image& out) {
    if (n < 160) return false;
    const size_t hdr = rd32(d + 156);
    if (hdr > n - 160 || off > n || size > n - off) return false;
    std::vector<uint8_t> jpg(hdr + size);
    std::memcpy(jpg.data(), d + 160, hdr);
    if (size) std::memcpy(jpg.data() + hdr, d + off, size);
    JpegDecoder j;
    if (!j.decode(jpg.data(), jpg.size())) return false;
    // the JPEG may be larger than the declared size (padded); crop to the BLP size
    const uint32_t ow = std::min<uint32_t>(w, uint32_t(j.width)), oh = std::min<uint32_t>(h, uint32_t(j.height));
    out.width = int(ow); out.height = int(oh);
    out.rgba.resize(size_t(ow) * oh * 4);
    for (uint32_t y = 0; y < oh; ++y)
        for (uint32_t x = 0; x < ow; ++x) {
            const size_t s = size_t(y) * size_t(j.width) + x, o = (size_t(y) * ow + x) * 4;
            uint8_t c[4] = {0, 0, 0, 255};
            for (int k = 0; k < j.comps; ++k) c[k] = j.planes[size_t(k)][s];
            if (j.comps >= 3) { out.rgba[o] = c[2]; out.rgba[o + 1] = c[1]; out.rgba[o + 2] = c[0]; }   // stored as B, G, R
            else out.rgba[o] = out.rgba[o + 1] = out.rgba[o + 2] = c[0];
            out.rgba[o + 3] = j.comps == 4 && alphaBits > 0 ? c[3] : 255;
        }
    return true;
}

// DXT1 / DXT3 / DXT5 through the shared block decoders; DXT3 (explicit 4-bit alpha) is decoded
// as BC3 colour (always four-colour, like DXT3) plus its own alpha.
bool decodeDxt(const uint8_t* d, size_t n, size_t off, size_t size, uint32_t w, uint32_t h, int kind, bool alpha, Image& out) {
    const size_t bw = (w + 3) / 4, bh = (h + 3) / 4, bb = kind == 1 ? 8 : 16;
    if (off > n || size > n - off || bw * bh * bb > size) return false;
    out.width = int(w); out.height = int(h);
    out.rgba.assign(size_t(w) * h * 4, 255);
    uint8_t px[64];
    for (size_t by = 0; by < bh; ++by)
        for (size_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = d + off + (by * bw + bx) * bb;
            if (kind == 1) decodeBlock(BlockFormat::BC1, b, px);
            else if (kind == 5) decodeBlock(BlockFormat::BC3, b, px);
            else {
                uint8_t fake[16] = {255, 255, 0, 0, 0, 0, 0, 0};
                std::memcpy(fake + 8, b + 8, 8);
                decodeBlock(BlockFormat::BC3, fake, px);
                for (int i = 0; i < 16; ++i) px[i * 4 + 3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
            }
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    const size_t X = bx * 4 + size_t(x), Y = by * 4 + size_t(y);
                    if (X >= w || Y >= h) continue;
                    std::memcpy(&out.rgba[(Y * w + X) * 4], px + (y * 4 + x) * 4, 4);
                    if (!alpha) out.rgba[(Y * w + X) * 4 + 3] = 255;
                }
        }
    return true;
}

} // namespace

bool decodeBlp(const std::string& bytes, Image& out) {
    const auto* d = reinterpret_cast<const uint8_t*>(bytes.data());
    const size_t n = bytes.size();
    if (n < 4) return false;
    Image img;
    img.name = out.name;
    bool ok = false;
    if (std::memcmp(d, "BLP1", 4) == 0) {
        if (n < 156) return false;
        const uint32_t content = rd32(d + 4), alphaBits = rd32(d + 8), w = rd32(d + 12), h = rd32(d + 16);
        const uint32_t off = rd32(d + 28), size = rd32(d + 92);
        if (!validSize(w, h)) return false;
        if (content == 0) ok = decodeBlp1Jpeg(d, n, off, size, w, h, alphaBits, img);
        else if (content == 1) ok = decodePaletted(d, n, 156, off, size, w, h, alphaBits, img);
    } else if (std::memcmp(d, "BLP2", 4) == 0) {
        if (n < 148) return false;
        const uint32_t type = rd32(d + 4);
        const uint8_t enc = d[8], alphaDepth = d[9], alphaType = d[10];
        const uint32_t w = rd32(d + 12), h = rd32(d + 16), off = rd32(d + 20), size = rd32(d + 84);
        if (!validSize(w, h) || type != 1) return false;   // type 0 (JPEG) BLP2 does not occur in practice
        if (enc == 1) ok = decodePaletted(d, n, 148, off, size, w, h, alphaDepth, img);
        else if (enc == 2) ok = decodeDxt(d, n, off, size, w, h, alphaType == 7 ? 5 : alphaType == 1 ? 3 : 1, alphaDepth > 0, img);
        else if (enc == 3) {
            const size_t px = size_t(w) * h;
            if (off <= n && size <= n - off && px * 4 <= size) {
                img.width = int(w); img.height = int(h);
                img.rgba.resize(px * 4);
                for (size_t i = 0; i < px; ++i) {
                    const uint8_t* c = d + off + i * 4;
                    img.rgba[i * 4] = c[2]; img.rgba[i * 4 + 1] = c[1]; img.rgba[i * 4 + 2] = c[0]; img.rgba[i * 4 + 3] = alphaDepth ? c[3] : 255;
                }
                ok = true;
            }
        }
    }
    if (!ok) return false;
    out.width = img.width; out.height = img.height;
    out.rgba = std::move(img.rgba);
    return true;
}

} // namespace rts::render
