// DirectDraw Surface textures (.dds): the texture container of C&C Generals and most D3D-era games.
//
// Reads mip 0 of a 2D texture (cube maps / arrays: the first face) into RGBA8:
//   FourCC DXT1/BC1, DXT2/DXT3/BC2, DXT4/DXT5/BC3, ATI1/BC4U, ATI2/BC5U, DX10 (BC1/2/3/4/5/7, RGBA8,
//   BGRA8, BGRX8), D3DFMT codes in the FourCC field (A8R8G8B8, R5G6B5, L8, ...), and uncompressed
//   pixel formats described by bit masks (8/16/24/32 bit RGB, luminance, alpha).
// BC1 / BC3 / BC7 also keep the original blocks of every mip level in Image::compressed so they stay
// compressed in VRAM where the GPU samples them (same path as KTX2). BC5 (two-channel normal maps)
// gets its blue channel reconstructed (z = sqrt(1 - x^2 - y^2)). Signed formats and volume textures
// are rejected. Every read is bounds-checked: broken files return false.
#include "Model.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rts::render {

namespace {

uint32_t rd32(const std::string& b, size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}
constexpr uint32_t fourcc(const char* s) { return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24; }

enum class Kind { Unknown, BC1, BC2, BC3, BC4, BC5, BC7, Masks };

struct Masks { uint32_t bits = 0, r = 0, g = 0, b = 0, a = 0; bool lum = false; };

// Expands one masked channel to 8 bits.
uint8_t channel(uint32_t px, uint32_t mask) {
    if (!mask) return 0;
    int shift = 0;
    while (!((mask >> shift) & 1u)) ++shift;
    const uint32_t m = mask >> shift;
    const uint32_t v = (px >> shift) & m;
    return uint8_t((uint64_t(v) * 255u + m / 2) / m);
}

bool decodeMasks(const uint8_t* src, size_t avail, int w, int h, const Masks& f, std::vector<uint8_t>& rgba) {
    const size_t bpp = f.bits / 8;
    if (bpp < 1 || bpp > 4 || f.bits % 8) return false;
    const size_t need = size_t(w) * size_t(h) * bpp;
    if (avail < need) return false;
    rgba.resize(size_t(w) * size_t(h) * 4);
    for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i) {
        uint32_t px = 0;
        for (size_t k = 0; k < bpp; ++k) px |= uint32_t(src[i * bpp + k]) << (8 * k);
        uint8_t* o = &rgba[i * 4];
        if (f.lum) o[0] = o[1] = o[2] = channel(px, f.r);
        else { o[0] = channel(px, f.r); o[1] = channel(px, f.g); o[2] = channel(px, f.b); }
        if (!f.r && !f.g && !f.b && !f.lum) o[0] = o[1] = o[2] = 255;   // alpha-only texture
        o[3] = f.a ? channel(px, f.a) : 255;
    }
    return true;
}

// BC2 colour is always four-colour like BC3, so it goes through the BC3 decoder with an opaque alpha block.
void decodeBc2(const uint8_t* blk, uint8_t out[64]) {
    uint8_t b3[16] = {255, 255, 0, 0, 0, 0, 0, 0};
    std::memcpy(b3 + 8, blk + 8, 8);
    decodeBlock(BlockFormat::BC3, b3, out);
    for (int i = 0; i < 16; ++i) {
        const uint8_t a = uint8_t((blk[i / 2] >> ((i & 1) * 4)) & 15);
        out[i * 4 + 3] = uint8_t(a * 17);
    }
}

void decodeBc5(const uint8_t* blk, uint8_t out[64]) {
    uint8_t r[64], g[64];
    decodeBlock(BlockFormat::BC4, blk, r);
    decodeBlock(BlockFormat::BC4, blk + 8, g);
    for (int i = 0; i < 16; ++i) {
        const float x = r[i * 4] / 127.5f - 1, y = g[i * 4] / 127.5f - 1;
        const float z = std::sqrt(std::fmax(0.0f, 1 - x * x - y * y));
        out[i * 4 + 0] = r[i * 4];
        out[i * 4 + 1] = g[i * 4];
        out[i * 4 + 2] = uint8_t(std::lround((z + 1) * 127.5f));
        out[i * 4 + 3] = 255;
    }
}

// True when a BC1 block uses the three-colour mode and its index 3 (transparent black).
bool bc1UsesTransparent(const uint8_t* blk) {
    const uint16_t c0 = uint16_t(blk[0] | blk[1] << 8), c1 = uint16_t(blk[2] | blk[3] << 8);
    if (c0 > c1) return false;
    for (int i = 0; i < 16; ++i)
        if (((blk[4 + i / 4] >> ((i % 4) * 2)) & 3) == 3) return true;
    return false;
}

} // namespace

bool decodeDds(const std::string& bytes, Image& out) {
    if (bytes.size() < 128 || bytes.compare(0, 4, "DDS ") != 0 || rd32(bytes, 4) != 124) return false;
    const uint32_t flags = rd32(bytes, 8);
    const int h = int(rd32(bytes, 12)), w = int(rd32(bytes, 16));
    const uint32_t mipCount = (flags & 0x20000u) ? rd32(bytes, 28) : 1;
    const uint32_t pfFlags = rd32(bytes, 80), cc = rd32(bytes, 84);
    const uint32_t caps2 = rd32(bytes, 112);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    if ((caps2 & 0x200000u) || ((flags & 0x800000u) && rd32(bytes, 24) > 1)) return false;   // volume texture
    size_t dataAt = 128;
    Kind kind = Kind::Unknown;
    Masks masks;
    bool bc1Alpha = (pfFlags & 0x1u) != 0;
    auto setMasks = [&](uint32_t bits, uint32_t r, uint32_t g, uint32_t b, uint32_t a, bool lum = false) {
        kind = Kind::Masks; masks = {bits, r, g, b, a, lum};
    };
    if (pfFlags & 0x4u) {
        switch (cc) {
            case fourcc("DXT1"): kind = Kind::BC1; break;
            case fourcc("DXT2"): case fourcc("DXT3"): kind = Kind::BC2; break;
            case fourcc("DXT4"): case fourcc("DXT5"): kind = Kind::BC3; break;
            case fourcc("ATI1"): case fourcc("BC4U"): kind = Kind::BC4; break;
            case fourcc("ATI2"): case fourcc("BC5U"): kind = Kind::BC5; break;
            // D3DFMT codes stored as FourCC by some exporters
            case 20: setMasks(24, 0xff0000, 0xff00, 0xff, 0); break;                     // R8G8B8
            case 21: setMasks(32, 0xff0000, 0xff00, 0xff, 0xff000000u); break;           // A8R8G8B8
            case 22: setMasks(32, 0xff0000, 0xff00, 0xff, 0); break;                     // X8R8G8B8
            case 23: setMasks(16, 0xf800, 0x7e0, 0x1f, 0); break;                        // R5G6B5
            case 24: setMasks(16, 0x7c00, 0x3e0, 0x1f, 0); break;                        // X1R5G5B5
            case 25: setMasks(16, 0x7c00, 0x3e0, 0x1f, 0x8000); break;                   // A1R5G5B5
            case 26: setMasks(16, 0xf00, 0xf0, 0xf, 0xf000); break;                      // A4R4G4B4
            case 28: setMasks(8, 0, 0, 0, 0xff); break;                                  // A8
            case 32: setMasks(32, 0xff, 0xff00, 0xff0000, 0xff000000u); break;           // A8B8G8R8
            case 50: setMasks(8, 0xff, 0, 0, 0, true); break;                            // L8
            case 51: setMasks(16, 0xff, 0, 0, 0xff00, true); break;                      // A8L8
            case fourcc("DX10"): {
                if (bytes.size() < 148) return false;
                const uint32_t dxgi = rd32(bytes, 128), dim = rd32(bytes, 132);
                if (dim == 4) return false;   // TEXTURE3D
                dataAt = 148;
                switch (dxgi) {
                    case 70: case 71: case 72: kind = Kind::BC1; bc1Alpha = true; break;
                    case 73: case 74: case 75: kind = Kind::BC2; break;
                    case 76: case 77: case 78: kind = Kind::BC3; break;
                    case 79: case 80: kind = Kind::BC4; break;
                    case 82: case 83: kind = Kind::BC5; break;
                    case 97: case 98: case 99: kind = Kind::BC7; break;
                    case 27: case 28: case 29: setMasks(32, 0xff, 0xff00, 0xff0000, 0xff000000u); break;
                    case 87: case 90: case 91: setMasks(32, 0xff0000, 0xff00, 0xff, 0xff000000u); break;
                    case 88: case 92: case 93: setMasks(32, 0xff0000, 0xff00, 0xff, 0); break;
                    case 85: setMasks(16, 0xf800, 0x7e0, 0x1f, 0); break;
                    case 86: setMasks(16, 0x7c00, 0x3e0, 0x1f, 0x8000); break;
                    case 115: setMasks(16, 0xf00, 0xf0, 0xf, 0xf000); break;
                    case 61: case 62: setMasks(8, 0xff, 0, 0, 0, true); break;
                    case 65: setMasks(8, 0, 0, 0, 0xff); break;
                    default: return false;   // float, signed, packed or exotic formats
                }
                break;
            }
            default: return false;
        }
    } else if (pfFlags & (0x40u | 0x20000u | 0x2u)) {   // RGB, luminance, alpha-only
        const bool alpha = (pfFlags & 0x1u) || (pfFlags & 0x2u);
        setMasks(rd32(bytes, 88), rd32(bytes, 92), rd32(bytes, 96), rd32(bytes, 100), alpha ? rd32(bytes, 104) : 0, (pfFlags & 0x20000u) != 0);
    } else return false;

    const uint8_t* data = reinterpret_cast<const uint8_t*>(bytes.data()) + dataAt;
    const size_t avail = bytes.size() - dataAt;
    std::vector<uint8_t> rgba;
    if (kind == Kind::Masks) {
        if (!decodeMasks(data, avail, w, h, masks, rgba)) return false;
        out.width = w; out.height = h;
        out.rgba = std::move(rgba);
        out.compressed.reset();
        return true;
    }

    const size_t bb = kind == Kind::BC1 || kind == Kind::BC4 ? 8 : 16;
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    const size_t level0 = size_t(bw) * size_t(bh) * bb;
    if (avail < level0) return false;
    uint8_t px[64];
    rgba.assign(size_t(w) * size_t(h) * 4, 0);
    bool transparentIdx = false;
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = data + (size_t(by) * size_t(bw) + size_t(bx)) * bb;
            switch (kind) {
                case Kind::BC1:
                    decodeBlock(BlockFormat::BC1, blk, px);
                    if (bc1UsesTransparent(blk)) {
                        transparentIdx = true;
                        if (!bc1Alpha) for (int i = 0; i < 16; ++i) px[i * 4 + 3] = 255;   // DXT1 without alpha: opaque black
                    }
                    break;
                case Kind::BC2: decodeBc2(blk, px); break;
                case Kind::BC3: decodeBlock(BlockFormat::BC3, blk, px); break;
                case Kind::BC4: decodeBlock(BlockFormat::BC4, blk, px); break;
                case Kind::BC5: decodeBc5(blk, px); break;
                case Kind::BC7: decodeBlock(BlockFormat::BC7, blk, px); break;
                default: return false;
            }
            for (int y = 0; y < 4 && by * 4 + y < h; ++y) {
                const int cols = std::min(4, w - bx * 4);
                std::memcpy(rgba.data() + (size_t(by * 4 + y) * size_t(w) + size_t(bx * 4)) * 4, px + y * 16, size_t(cols) * 4);
            }
        }
    out.width = w; out.height = h;
    out.rgba = std::move(rgba);
    out.compressed.reset();
    if (kind == Kind::BC1 || kind == Kind::BC3 || kind == Kind::BC7) {
        // keep the GPU blocks of the mip chain (stops at the first truncated level)
        auto ci = std::make_shared<CompressedImage>();
        ci->format = kind == Kind::BC1 ? BlockFormat::BC1 : kind == Kind::BC3 ? BlockFormat::BC3 : BlockFormat::BC7;
        ci->width = w; ci->height = h;
        ci->srgb = true;
        // BC1: RGBA (133) unless the file says "no alpha" and a block relies on index 3 being black,
        // which only the CPU copy reproduces (131 = decoded fallback in uploadModel)
        ci->vkFormat = kind == Kind::BC1 ? (!bc1Alpha && transparentIdx ? 131u : 133u) : kind == Kind::BC3 ? 138u : 146u;
        size_t at = 0;
        for (uint32_t l = 0; l < std::max<uint32_t>(1, std::min<uint32_t>(mipCount, 16)); ++l) {
            const int lw = std::max(1, w >> l), lh = std::max(1, h >> l);
            const size_t sz = levelByteSize(ci->format, lw, lh);
            if (at + sz > avail) break;
            ci->levels.emplace_back(data + at, data + at + sz);
            at += sz;
            if (lw == 1 && lh == 1) break;
        }
        out.compressed = std::move(ci);
    }
    return true;
}

} // namespace rts::render
