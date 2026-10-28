#include "tools/branding/BrandImage.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;

namespace rts::branding {

namespace {

uint8_t to8(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

// One axis of the resampler on premultiplied float RGBA: area average when shrinking, bilinear
// when growing. `n` lines of `len` pixels, stride between pixels / lines in floats.
void resampleAxis(const std::vector<float>& src, int srcLen, std::vector<float>& dst, int dstLen, int lines,
                  int srcPixStride, int srcLineStride, int dstPixStride, int dstLineStride) {
    const double scale = double(srcLen) / double(dstLen);
    for (int i = 0; i < dstLen; ++i) {
        // weights for output pixel i
        std::vector<std::pair<int, float>> w;
        if (scale > 1.0) {
            double a = i * scale, b = (i + 1) * scale;
            for (int s = int(std::floor(a)); s < int(std::ceil(b)) && s < srcLen; ++s) {
                double cov = std::min(b, double(s + 1)) - std::max(a, double(s));
                if (cov > 0) w.push_back({s, float(cov / scale)});
            }
        } else {
            double c = (i + 0.5) * scale - 0.5;
            int s0 = int(std::floor(c));
            float f = float(c - s0);
            w.push_back({std::clamp(s0, 0, srcLen - 1), 1 - f});
            w.push_back({std::clamp(s0 + 1, 0, srcLen - 1), f});
        }
        for (int l = 0; l < lines; ++l) {
            float acc[4] = {0, 0, 0, 0};
            for (auto [s, k] : w) {
                const float* p = &src[size_t(l) * size_t(srcLineStride) + size_t(s) * size_t(srcPixStride)];
                for (int c = 0; c < 4; ++c) acc[c] += p[c] * k;
            }
            float* o = &dst[size_t(l) * size_t(dstLineStride) + size_t(i) * size_t(dstPixStride)];
            for (int c = 0; c < 4; ++c) o[c] = acc[c];
        }
    }
}

// Smooth coverage of a signed distance (negative inside), 1 px wide.
float coverage(float d) { return std::clamp(0.5f - d, 0.0f, 1.0f); }

// 5x7 glyphs, rows top to bottom, '#' = ink.
const std::map<char, const char*>& glyphs() {
    static const std::map<char, const char*> g = {
        {'0', ".###.#...##..###.#.###..##...#.###."}, {'1', "..#...##....#....#....#....#...###."},
        {'2', ".###.#...#....#...#...#...#...#####"}, {'3', "#####...#...#.....#.....##...#.###."},
        {'4', "...#...##..#.#.#..#.#####...#....#."}, {'5', "######....####.....#....##...#.###."},
        {'6', "..##..#...#....####.#...##...#.###."}, {'7', "#####....#...#...#...#....#....#..."},
        {'8', ".###.#...##...#.###.#...##...#.###."}, {'9', ".###.#...##...#.####....#...#..##.."},
        {'A', ".###.#...##...#######...##...##...#"}, {'B', "####.#...##...#####.#...##...#####."},
        {'C', ".###.#...##....#....#....#...#.###."}, {'D', "###..#..#.#...##...##...##..#.###.."},
        {'E', "######....#....####.#....#....#####"}, {'F', "######....#....####.#....#....#...."},
        {'G', ".###.#...##....#.####...##...#.####"}, {'H', "#...##...##...#######...##...##...#"},
        {'I', ".###...#....#....#....#....#...###."}, {'J', "..###...#....#....#....#.#..#..##.."},
        {'K', "#...##..#.#.#..##...#.#..#..#.#...#"}, {'L', "#....#....#....#....#....#....#####"},
        {'M', "#...###.###.#.##.#.##...##...##...#"}, {'N', "#...##...###..##.#.##..###...##...#"},
        {'O', ".###.#...##...##...##...##...#.###."}, {'P', "####.#...##...#####.#....#....#...."},
        {'Q', ".###.#...##...##...##.#.##..#..##.#"}, {'R', "####.#...##...#####.#.#..#..#.#...#"},
        {'S', ".#####....#.....###.....#....#####."}, {'T', "#####..#....#....#....#....#....#.."},
        {'U', "#...##...##...##...##...##...#.###."}, {'V', "#...##...##...##...##...#.#.#...#.."},
        {'W', "#...##...##...##.#.##.#.##.#.#.#.#."}, {'X', "#...##...#.#.#...#...#.#.#...##...#"},
        {'Y', "#...##...#.#.#...#....#....#....#.."}, {'Z', "#####....#...#...#...#...#....#####"},
        {'x', "..........#...#.#.#...#...#.#.#...#"}, {'@', ".###.#...##.###.#.##.###.#.....###."},
        {'.', ".........................##...##..."}, {'-', "...............#####..............."},
        {':', ".....##...##.........##...##......."}, {'/', "....#...#....#...#...#....#...#...."},
        {'(', "...#...#...#....#....#.....#.....#."}, {')', ".#.....#.....#....#....#...#...#..."},
        {'%', "##..###.#....#...#...#....#.###..##"}, {' ', "..................................."},
        {'=', "..........#####.....#####.........."},
    };
    return g;
}

} // namespace

bool loadImageFile(const std::string& path, Image& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    if (!atlas::decodeImageFile(ss.str(), out) || out.empty()) { if (err) *err = "cannot decode image " + path; return false; }
    return true;
}

Image makeImage(int w, int h, Color fill) {
    Image img;
    img.width = std::max(1, w);
    img.height = std::max(1, h);
    img.rgba.resize(size_t(img.width) * size_t(img.height) * 4);
    const uint8_t px[4] = {to8(fill.r), to8(fill.g), to8(fill.b), to8(fill.a)};
    for (size_t i = 0; i < img.rgba.size(); i += 4) std::memcpy(&img.rgba[i], px, 4);
    return img;
}

Image resize(const Image& src, int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (src.empty()) return makeImage(w, h);
    if (src.width == w && src.height == h) return src;
    const int sw = src.width, sh = src.height;
    std::vector<float> a(size_t(sw) * size_t(sh) * 4);
    for (size_t i = 0; i < size_t(sw) * size_t(sh); ++i) {
        float al = src.rgba[i * 4 + 3] / 255.0f;
        for (int c = 0; c < 3; ++c) a[i * 4 + size_t(c)] = src.rgba[i * 4 + size_t(c)] / 255.0f * al;
        a[i * 4 + 3] = al;
    }
    std::vector<float> b(size_t(w) * size_t(sh) * 4), c(size_t(w) * size_t(h) * 4);
    resampleAxis(a, sw, b, w, sh, 4, sw * 4, 4, w * 4);    // rows
    resampleAxis(b, sh, c, h, w, w * 4, 4, w * 4, 4);      // columns
    Image out = makeImage(w, h);
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        float al = c[i * 4 + 3];
        for (int k = 0; k < 3; ++k) out.rgba[i * 4 + size_t(k)] = to8(al > 1e-6f ? c[i * 4 + size_t(k)] / al : 0);
        out.rgba[i * 4 + 3] = to8(al);
    }
    return out;
}

Image fitContain(const Image& src, int w, int h) {
    if (src.empty()) return makeImage(w, h);
    double k = std::min(double(w) / src.width, double(h) / src.height);
    return resize(src, std::max(1, int(std::lround(src.width * k))), std::max(1, int(std::lround(src.height * k))));
}

Image fitCover(const Image& src, int w, int h) {
    if (src.empty()) return makeImage(w, h);
    double k = std::max(double(w) / src.width, double(h) / src.height);
    Image big = resize(src, std::max(w, int(std::lround(src.width * k))), std::max(h, int(std::lround(src.height * k))));
    Image out = makeImage(w, h);
    int ox = (big.width - w) / 2, oy = (big.height - h) / 2;
    for (int y = 0; y < h; ++y) std::memcpy(out.px(0, y), big.px(ox, oy + y), size_t(w) * 4);
    return out;
}

bool hasAlpha(const Image& img) {
    for (size_t i = 3; i < img.rgba.size(); i += 4) if (img.rgba[i] != 255) return true;
    return false;
}

bool cornersTransparent(const Image& img) {
    if (img.empty()) return false;
    int n = 0;
    for (auto [x, y] : {std::pair{0, 0}, {img.width - 1, 0}, {0, img.height - 1}, {img.width - 1, img.height - 1}})
        if (img.px(x, y)[3] < 128) ++n;
    return n >= 3;
}

Color edgeColor(const Image& img) {
    double acc[3] = {0, 0, 0};
    int n = 0;
    auto take = [&](int x, int y) {
        const uint8_t* p = img.px(x, y);
        if (p[3] < 250) return;
        for (int c = 0; c < 3; ++c) acc[c] += p[c];
        ++n;
    };
    for (int x = 0; x < img.width; ++x) { take(x, 0); take(x, img.height - 1); }
    for (int y = 0; y < img.height; ++y) { take(0, y); take(img.width - 1, y); }
    if (n == 0) return {1, 1, 1, 1};
    return {float(acc[0] / n / 255), float(acc[1] / n / 255), float(acc[2] / n / 255), 1};
}

void flatten(Image& img, Color bg) {
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        float a = img.rgba[i + 3] / 255.0f;
        const float b[3] = {bg.r, bg.g, bg.b};
        for (int c = 0; c < 3; ++c) img.rgba[i + size_t(c)] = to8(img.rgba[i + size_t(c)] / 255.0f * a + b[c] * (1 - a));
        img.rgba[i + 3] = 255;
    }
}

void blit(Image& dst, const Image& src, int x0, int y0, float opacity) {
    for (int y = 0; y < src.height; ++y) {
        int dy = y0 + y;
        if (dy < 0 || dy >= dst.height) continue;
        for (int x = 0; x < src.width; ++x) {
            int dx = x0 + x;
            if (dx < 0 || dx >= dst.width) continue;
            const uint8_t* s = src.px(x, y);
            uint8_t* d = dst.px(dx, dy);
            float sa = s[3] / 255.0f * opacity, da = d[3] / 255.0f;
            float oa = sa + da * (1 - sa);
            for (int c = 0; c < 3; ++c)
                d[c] = oa > 0 ? to8((s[c] / 255.0f * sa + d[c] / 255.0f * da * (1 - sa)) / oa) : 0;
            d[3] = to8(oa);
        }
    }
}

void fillRect(Image& dst, int x, int y, int w, int h, Color c) {
    Image r = makeImage(w, h, c);
    blit(dst, r, x, y);
}

void maskRoundedRect(Image& img, float radius) {
    const float W = float(img.width), H = float(img.height);
    radius = std::clamp(radius, 0.0f, std::min(W, H) * 0.5f);
    if (radius <= 0) return;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            float px = x + 0.5f, py = y + 0.5f;
            float qx = std::max(std::abs(px - W * 0.5f) - (W * 0.5f - radius), 0.0f);
            float qy = std::max(std::abs(py - H * 0.5f) - (H * 0.5f - radius), 0.0f);
            float d = std::sqrt(qx * qx + qy * qy) - radius;
            uint8_t* p = img.px(x, y);
            p[3] = uint8_t(std::lround(p[3] * coverage(d)));
        }
}

void maskCircle(Image& img) {
    const float cx = img.width * 0.5f, cy = img.height * 0.5f, r = std::min(cx, cy);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy) - r;
            uint8_t* p = img.px(x, y);
            p[3] = uint8_t(std::lround(p[3] * coverage(d)));
        }
}

void maskSquircle(Image& img) {
    const float cx = img.width * 0.5f, cy = img.height * 0.5f;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            float u = std::abs(x + 0.5f - cx) / cx, v = std::abs(y + 0.5f - cy) / cy;
            float f = std::pow(std::pow(u, 4.0f) + std::pow(v, 4.0f), 0.25f);   // 1 on the edge
            float d = (f - 1) * std::min(cx, cy);
            uint8_t* p = img.px(x, y);
            p[3] = uint8_t(std::lround(p[3] * coverage(d)));
        }
}

void drawQuad(Image& dst, const Image& src, float cx, float cy, float w, float h, float rad, float opacity, float u0, float v0, float u1, float v1) {
    if (src.empty() || w <= 0 || h <= 0 || opacity <= 0) return;
    // pre-shrink the source close to the drawn size so bilinear sampling doesn't alias
    float texW = std::abs(u1 - u0) * src.width, texH = std::abs(v1 - v0) * src.height;
    const Image* s = &src;
    Image small;
    if (texW > w * 1.5f || texH > h * 1.5f) {
        float k = std::max(w / texW, h / texH);
        small = resize(src, std::max(1, int(src.width * k)), std::max(1, int(src.height * k)));
        s = &small;
    }
    const float c = std::cos(rad), sn = std::sin(rad);
    const float ext = 0.5f * (std::abs(w * c) + std::abs(h * sn)), eyt = 0.5f * (std::abs(w * sn) + std::abs(h * c));
    int x0 = std::max(0, int(cx - ext) - 1), x1 = std::min(dst.width - 1, int(cx + ext) + 1);
    int y0 = std::max(0, int(cy - eyt) - 1), y1 = std::min(dst.height - 1, int(cy + eyt) + 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float lx = dx * c + dy * sn, ly = -dx * sn + dy * c;   // inverse rotation
            float fu = lx / w + 0.5f, fv = ly / h + 0.5f;
            if (fu < 0 || fu > 1 || fv < 0 || fv > 1) continue;
            float su = (u0 + (u1 - u0) * fu) * s->width - 0.5f, sv = (v0 + (v1 - v0) * fv) * s->height - 0.5f;
            int ix = int(std::floor(su)), iy = int(std::floor(sv));
            float fx = su - ix, fy = sv - iy;
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                int sx = std::clamp(ix + (k & 1), 0, s->width - 1), sy = std::clamp(iy + (k >> 1), 0, s->height - 1);
                float wk = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                const uint8_t* p = s->px(sx, sy);
                float a = p[3] / 255.0f;
                for (int ch = 0; ch < 3; ++ch) acc[ch] += p[ch] / 255.0f * a * wk;
                acc[3] += a * wk;
            }
            uint8_t* d = dst.px(x, y);
            float sa = acc[3] * opacity, da = d[3] / 255.0f, oa = sa + da * (1 - sa);
            for (int ch = 0; ch < 3; ++ch) {
                float sc = acc[3] > 1e-6f ? acc[ch] / acc[3] : 0;
                d[ch] = oa > 0 ? to8((sc * sa + d[ch] / 255.0f * da * (1 - sa)) / oa) : 0;
            }
            d[3] = to8(oa);
        }
}

int textWidth(const std::string& text, int scale) { return int(text.size()) * 6 * scale; }

void drawText(Image& dst, int x, int y, const std::string& text, Color c, int scale) {
    const auto& g = glyphs();
    // a lower-case 'x' between digits is the size sign ("1024x1024"); in words it is a letter
    bool upperX = false;
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == 'x' && !(i > 0 && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i - 1])) && std::isdigit(static_cast<unsigned char>(text[i + 1])))) upperX = true;
    for (char ch : text) {
        char k = ch;
        if (k >= 'a' && k <= 'z' && (k != 'x' || !upperX)) k = char(k - 32);
        auto it = g.find(k);
        if (it != g.end())
            for (int r = 0; r < 7; ++r)
                for (int col = 0; col < 5; ++col)
                    if (it->second[r * 5 + col] == '#') fillRect(dst, x + col * scale, y + r * scale, scale, scale, c);
        x += 6 * scale;
    }
}

std::string encodePng(const Image& img) {
    std::string out;
    stbi_write_png_to_func([](void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), size_t(size)); },
                           &out, img.width, img.height, 4, img.rgba.data(), img.width * 4);
    return out;
}

bool writeFile(const std::string& path, const std::string& bytes, std::string* err) {
    std::error_code ec;
    if (fs::path(path).has_parent_path()) fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    f.write(bytes.data(), std::streamsize(bytes.size()));
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    return true;
}

bool writePng(const std::string& path, const Image& img, std::string* err) { return writeFile(path, encodePng(img), err); }

namespace {
void put16(std::string& s, uint32_t v) { s += char(v & 0xff); s += char((v >> 8) & 0xff); }
void put32(std::string& s, uint32_t v) { put16(s, v & 0xffff); put16(s, v >> 16); }
void put32be(std::string& s, uint32_t v) { for (int i = 3; i >= 0; --i) s += char((v >> (8 * i)) & 0xff); }
} // namespace

std::string encodeIco(const std::vector<Image>& images) {
    std::vector<std::string> blobs;
    for (const Image& img : images) {
        if (img.width >= 256) { blobs.push_back(encodePng(img)); continue; }
        std::string b;
        const int w = img.width, h = img.height, maskRow = ((w + 31) / 32) * 4;
        put32(b, 40); put32(b, uint32_t(w)); put32(b, uint32_t(h * 2)); put16(b, 1); put16(b, 32);
        put32(b, 0); put32(b, uint32_t(w * h * 4 + maskRow * h)); put32(b, 0); put32(b, 0); put32(b, 0); put32(b, 0);
        for (int y = h - 1; y >= 0; --y)
            for (int x = 0; x < w; ++x) { const uint8_t* p = img.px(x, y); b += char(p[2]); b += char(p[1]); b += char(p[0]); b += char(p[3]); }
        for (int y = h - 1; y >= 0; --y) {
            std::string row(size_t(maskRow), '\0');
            for (int x = 0; x < w; ++x) if (img.px(x, y)[3] == 0) row[size_t(x / 8)] = char(row[size_t(x / 8)] | (0x80 >> (x % 8)));
            b += row;
        }
        blobs.push_back(b);
    }
    std::string out;
    put16(out, 0); put16(out, 1); put16(out, uint16_t(images.size()));
    uint32_t offset = 6 + 16 * uint32_t(images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        const Image& img = images[i];
        out += char(img.width >= 256 ? 0 : img.width);
        out += char(img.height >= 256 ? 0 : img.height);
        out += char(0); out += char(0);
        put16(out, 1); put16(out, 32);
        put32(out, uint32_t(blobs[i].size()));
        put32(out, offset);
        offset += uint32_t(blobs[i].size());
    }
    for (auto& b : blobs) out += b;
    return out;
}

std::string encodeIcns(const std::vector<std::pair<std::string, Image>>& entries) {
    std::string body;
    for (auto& [type, img] : entries) {
        std::string png = encodePng(img);
        body += type.substr(0, 4);
        put32be(body, uint32_t(png.size() + 8));
        body += png;
    }
    std::string out = "icns";
    put32be(out, uint32_t(body.size() + 8));
    return out + body;
}

} // namespace rts::branding
