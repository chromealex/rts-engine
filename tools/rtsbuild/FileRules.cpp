#include "FileRules.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "third_party/stb/stb_image.h"
#include "third_party/stb_image_write/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace rts::build {

namespace {
bool isImage(const std::string& p) {
    std::string e = p.substr(p.find_last_of('.') == std::string::npos ? p.size() : p.find_last_of('.'));
    for (auto& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp";
}

void appendRules(const Json& list, std::vector<FileRule>& out, const std::string& where, std::string* error) {
    for (auto& r : list.items()) {
        FileRule f;
        f.match = r.getStr("match");
        if (f.match.empty()) { if (error && error->empty()) *error = where + ": each rule needs \"match\" (a glob like sprites/*.png)"; continue; }
        f.exclude = r.getBool("exclude", false);
        if (r.has("level")) f.level = std::clamp(int(r["level"].integer()), 0, 22);
        f.maxSize = int(std::max<int64_t>(0, r["maxSize"].integer(0)));
        if (r.has("scale")) f.scale = std::clamp(r["scale"].toDouble(1), 0.05, 1.0);
        out.push_back(f);
    }
}

// Box filter over the source footprint of each destination pixel; alpha-weighted colour so
// transparent edges don't turn dark.
std::vector<unsigned char> downscale(const unsigned char* src, int w, int h, int nw, int nh) {
    std::vector<unsigned char> dst(size_t(nw) * size_t(nh) * 4);
    for (int y = 0; y < nh; ++y) {
        const int y0 = y * h / nh, y1 = std::max(y0 + 1, (y + 1) * h / nh);
        for (int x = 0; x < nw; ++x) {
            const int x0 = x * w / nw, x1 = std::max(x0 + 1, (x + 1) * w / nw);
            double r = 0, g = 0, b = 0, a = 0;
            int n = 0;
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const unsigned char* p = src + (size_t(sy) * size_t(w) + size_t(sx)) * 4;
                    const double al = p[3] / 255.0;
                    r += p[0] * al; g += p[1] * al; b += p[2] * al; a += al; ++n;
                }
            unsigned char* d = &dst[(size_t(y) * size_t(nw) + size_t(x)) * 4];
            if (a > 0) { d[0] = (unsigned char)std::lround(r / a); d[1] = (unsigned char)std::lround(g / a); d[2] = (unsigned char)std::lround(b / a); }
            else d[0] = d[1] = d[2] = 0;
            d[3] = (unsigned char)std::lround(a / n * 255.0);
        }
    }
    return dst;
}

void appendBytes(void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), size_t(size)); }
} // namespace

std::vector<FileRule> readFileRules(const Json& gameJson, const std::string& target, std::string* error) {
    std::vector<FileRule> rules;
    const Json& p = gameJson["packaging"];
    appendRules(p["targets"][target]["files"], rules, "packaging.targets." + target + ".files", error);
    appendRules(p["files"], rules, "packaging.files", error);
    return rules;
}

bool applyFileRules(pack::PackWriter& w, const std::vector<FileRule>& rules, std::vector<std::pair<std::string, int>>& levelRules,
                    FileRulesReport& report, std::string* error, const std::vector<std::string>& fixedImages) {
    if (rules.empty()) return true;
    std::vector<std::string> drop;
    for (const std::string& path : w.paths()) {
        const FileRule* ex = nullptr; const FileRule* size = nullptr; const FileRule* level = nullptr;
        for (auto& r : rules) {
            if (!pack::globMatch(r.match.c_str(), path.c_str())) continue;
            if (!ex && r.exclude) ex = &r;
            if (!size && (r.maxSize > 0 || r.scale > 0)) size = &r;
            if (!level && r.level >= 0) level = &r;
        }
        if (ex) {
            std::string bytes;
            w.readInput(path, bytes);
            report.items.push_back({path, "исключён", bytes.size(), 0});
            drop.push_back(path);
            continue;
        }
        if (level) levelRules.push_back({path, level->level});
        if (!size || !isImage(path)) continue;
        if (std::find(fixedImages.begin(), fixedImages.end(), path) != fixedImages.end()) {
            report.warnings.push_back(path + ": спрайт-лист или атлас тайлов (кадры в пикселях) — размер не меняется");
            continue;
        }
        std::string bytes;
        if (!w.readInput(path, bytes)) continue;
        int iw = 0, ih = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()), int(bytes.size()), &iw, &ih, &comp, 4);
        if (!px) { report.warnings.push_back(path + ": не удалось прочитать картинку, оставлена как есть"); continue; }
        double f = size->scale > 0 ? size->scale : 1.0;
        if (size->maxSize > 0 && std::max(iw, ih) * f > size->maxSize) f = double(size->maxSize) / std::max(iw, ih);
        const int nw = std::max(1, int(std::lround(iw * f))), nh = std::max(1, int(std::lround(ih * f)));
        if (nw >= iw && nh >= ih) { stbi_image_free(px); continue; }
        std::vector<unsigned char> out = downscale(px, iw, ih, nw, nh);
        stbi_image_free(px);
        std::string png;
        stbi_write_png_compression_level = 9;
        if (!stbi_write_png_to_func(appendBytes, &png, nw, nh, 4, out.data(), nw * 4)) {
            if (error) *error = path + ": PNG encode failed";
            return false;
        }
        w.remove({path});
        if (!w.addData(path, png, error)) return false;
        report.items.push_back({path, std::to_string(iw) + "×" + std::to_string(ih) + " → " + std::to_string(nw) + "×" + std::to_string(nh), bytes.size(), png.size()});
    }
    if (!drop.empty()) w.remove(drop);
    return true;
}

} // namespace rts::build
