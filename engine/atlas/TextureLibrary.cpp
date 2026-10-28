#include "TextureLibrary.h"

#include <cstdio>

namespace rts::atlas {

namespace {
bool allOpaque(const Image& img) {
    for (size_t i = 3; i < img.rgba.size(); i += 4) if (img.rgba[i] != 255) return false;
    return true;
}
} // namespace

TextureLibrary::TextureLibrary(gfx::Device& dev, ReadFn read, gfx::Filter filter)
    : dev_(dev), read_(read), filter_(filter), images_(read, decodeImageFile) {}

TextureLibrary::~TextureLibrary() { clear(); }

void TextureLibrary::clear() {
    for (auto& [i, t] : pages_) if (t.valid()) dev_.destroyTexture(t);
    for (auto& [p, r] : refs_) if (r.page < 0 && r.texture.valid()) dev_.destroyTexture(r.texture);
    pages_.clear();
    refs_.clear();
    missing_.clear();
    images_.dropCache();
}

size_t TextureLibrary::residentPages() const { return pages_.size(); }

gfx::TextureHandle TextureLibrary::pageTexture(int page) {
    auto it = pages_.find(page);
    if (it != pages_.end()) return it->second;
    const Page& pg = manifest().pages()[size_t(page)];
    std::string bytes;
    Image img;
    gfx::TextureHandle h;
    if (read_(pg.file, bytes) && loadPageImage(bytes, decodeImageFile, img)) {
        gfx::TextureDesc td;
        td.width = img.width;
        td.height = img.height;
        td.filter = filter_;
        td.data = img.rgba.data();
        td.label = "atlas-page";
        h = dev_.createTexture(td);
    } else {
        std::fprintf(stderr, "atlas: cannot load page '%s'\n", pg.file.c_str());
    }
    pages_[page] = h; // also caches failures: no retry every frame
    return h;
}

const TextureRef* TextureLibrary::get(const std::string& path) {
    if (auto it = refs_.find(path); it != refs_.end()) return &it->second;
    if (missing_.count(path)) return nullptr;
    TextureRef ref;
    if (const Region* r = manifest().find(path)) {
        const Page& pg = manifest().pages()[size_t(r->page)];
        ref.texture = pageTexture(r->page);
        regionUVs(*r, pg.width, pg.height, ref.uv);
        ref.w = r->w; ref.h = r->h;
        ref.offsetX = r->offsetX; ref.offsetY = r->offsetY;
        ref.sourceW = r->sourceW; ref.sourceH = r->sourceH;
        ref.rotated = r->rotated;
        ref.opaque = !pg.alpha;
        ref.page = r->page;
    } else {
        std::string bytes;
        Image img;
        if (read_(path, bytes) && (isRtex(bytes) ? decodeRtex(bytes, img) : decodeImageFile(bytes, img))) {
            gfx::TextureDesc td;
            td.width = img.width;
            td.height = img.height;
            td.filter = filter_;
            td.data = img.rgba.data();
            td.label = "texture";
            ref.texture = dev_.createTexture(td);
            ref.w = ref.sourceW = img.width;
            ref.h = ref.sourceH = img.height;
            ref.opaque = allOpaque(img);
        }
    }
    if (!ref.texture.valid()) {
        std::fprintf(stderr, "texture: cannot load '%s'\n", path.c_str());
        missing_[path] = true;
        return nullptr;
    }
    return &refs_.emplace(path, ref).first->second;
}

bool TextureLibrary::loadImage(const std::string& path, Image& out) {
    if (images_.load(path, out)) return true;
    std::string bytes;
    return read_(path, bytes) && decodeImageFile(bytes, out);
}

} // namespace rts::atlas
