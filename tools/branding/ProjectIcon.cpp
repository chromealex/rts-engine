#include "ProjectIcon.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

#include "tools/branding/Icons.h"

namespace fs = std::filesystem;

namespace rts::branding {

namespace {

// coverage of the page silhouette at (u, v): rounded rectangle minus the folded corner
bool inPage(float u, float v, float x0, float y0, float x1, float y1, float r, float fold) {
    if (u < x0 || u > x1 || v < y0 || v > y1) return false;
    const float cx = std::clamp(u, x0 + r, x1 - r), cy = std::clamp(v, y0 + r, y1 - r);
    if ((u - cx) * (u - cx) + (v - cy) * (v - cy) > r * r) return false;
    const float fx = u - (x1 - fold), fy = v - y0;   // corner square, origin at its top-left
    return !(fx > 0 && fy < fold && fx > fy);
}

void put(Image& img, int x, int y, Color c) {
    uint8_t* p = img.px(x, y);
    p[0] = uint8_t(std::lround(std::clamp(c.r, 0.f, 1.f) * 255));
    p[1] = uint8_t(std::lround(std::clamp(c.g, 0.f, 1.f) * 255));
    p[2] = uint8_t(std::lround(std::clamp(c.b, 0.f, 1.f) * 255));
    p[3] = uint8_t(std::lround(std::clamp(c.a, 0.f, 1.f) * 255));
}

constexpr Color kPaper{0.955f, 0.963f, 0.975f, 1}, kEdge{0.62f, 0.66f, 0.72f, 1}, kFold{0.80f, 0.83f, 0.88f, 1},
                kBand{0.20f, 0.43f, 0.85f, 1}, kWhite{1, 1, 1, 1};

} // namespace

Image editorAppIcon(int size) {
    IconLook look;
    look.margin = 0.08f;          // macOS-like tile with a little air around it
    look.cornerRadius = 0.225f;
    return renderIcon(defaultIcon(std::max(64, size)), size, look);
}

Image projectDocumentIcon(int size) {
    size = std::max(16, size);
    const int H = size <= 256 ? size * 4 : size * 2;   // drawn large, area-averaged down: anti-aliased edges
    Image img = makeImage(H, H);
    const float x0 = 0.16f, y0 = 0.05f, x1 = 0.84f, y1 = 0.95f, r = 0.045f, fold = 0.22f, e = 0.014f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < H; ++x) {
            const float u = (x + 0.5f) / H, v = (y + 0.5f) / H;
            if (!inPage(u, v, x0 - e, y0 - e, x1 + e, y1 + e, r + e, fold + e * 0.6f)) continue;
            Color c = kEdge;
            if (inPage(u, v, x0, y0, x1, y1, r, fold)) {
                c = kPaper;
                const float fx = u - (x1 - fold), fy = v - y0;
                if (fx > 0 && fy < fold && fx <= fy) c = (fy - fx) < e ? kEdge : kFold;   // the folded flap
                if (v > 0.73f && v < 0.87f) c = kBand;
            }
            put(img, x, y, c);
        }
    // the engine icon on the page
    const int emblem = int(H * 0.46f);
    IconLook look;
    look.cornerRadius = 0.2f;
    blit(img, renderIcon(defaultIcon(std::max(256, emblem)), emblem, look), (H - emblem) / 2, int(H * 0.20f));
    // "RTS" on the band (unreadable below 32 px: the band alone says enough there)
    if (size >= 32) {
        const int scale = std::max(1, int(H * 0.10f / 7));
        const int tw = textWidth("RTS", scale);
        drawText(img, (H - tw) / 2, int(H * 0.80f) - 7 * scale / 2, "RTS", kWhite, scale);
    }
    return resize(img, size, size);
}

std::vector<std::string> writeEditorIcons(const std::string& dir, std::string* err) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto icns = [](Image (*draw)(int)) {
        std::vector<std::pair<std::string, Image>> e;
        for (auto [type, px] : std::initializer_list<std::pair<const char*, int>>{
                 {"icp4", 16}, {"icp5", 32}, {"ic11", 32}, {"ic12", 64}, {"ic07", 128},
                 {"ic13", 256}, {"ic08", 256}, {"ic14", 512}, {"ic09", 512}, {"ic10", 1024}})
            e.push_back({type, draw(px)});
        return encodeIcns(e);
    };
    auto ico = [](Image (*draw)(int)) {
        std::vector<Image> imgs;
        for (int px : {16, 24, 32, 48, 64, 128, 256}) imgs.push_back(draw(px));
        return encodeIco(imgs);
    };
    const fs::path d(dir);
    auto file = [&](const char* name, const std::string& bytes) {
        const std::string p = (d / name).string();
        if (!writeFile(p, bytes, err)) return false;
        out.push_back(p);
        return true;
    };
    if (!file("rtsproj.png", encodePng(projectDocumentIcon(256))) || !file("rtsproj.ico", ico(projectDocumentIcon)) ||
        !file("rtsproj.icns", icns(projectDocumentIcon)) || !file("editor.png", encodePng(editorAppIcon(256))) ||
        !file("editor.ico", ico(editorAppIcon)) || !file("AppIcon.icns", icns(editorAppIcon)))
        return {};
    return out;
}

} // namespace rts::branding
