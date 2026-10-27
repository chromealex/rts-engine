// Runtime texture lookup by logical path, transparent to atlasing.
//
//   atlas::TextureLibrary lib(dev, platform::readContentFile);
//   const atlas::TextureRef* t = lib.get("units/orc/grunt/walk_0.png");
//   if (t) {   // draw a quad of t->w x t->h source pixels at (t->offsetX, t->offsetY) relative to the
//              // original image's top-left, sample t->texture with t->uv (TL, TR, BR, BL)
//   }
//
// Whether the build atlased the file or shipped it as a plain PNG/JPG/TGA is invisible to the
// caller: an unatlased file becomes its own texture with uv = full rect, no trim, not rotated.
// Batch by `texture` (equal handles = same page = no texture switch). Pages and loose textures
// are created on first use and live until clear() / destruction.
//
// The manifest and page format are described in AtlasFormat.h and docs/game-format.md.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "engine/atlas/AtlasFormat.h"
#include "engine/gfx/Gfx.h"

namespace rts::atlas {

struct TextureRef {
    gfx::TextureHandle texture;
    float uv[8] = {0, 0, 1, 0, 1, 1, 0, 1};  // corners of the content in ORIGINAL orientation: TL, TR, BR, BL
    int w = 0, h = 0;                        // content size (trimmed) in source pixels
    int offsetX = 0, offsetY = 0;            // content position inside the original image
    int sourceW = 0, sourceH = 0;            // original image size (use it for pivots / layout)
    bool rotated = false;                    // informational: uv already accounts for it
    bool opaque = false;                     // page/texture has no transparent pixels
    int page = -1;                           // atlas page index, -1 = standalone texture
};

class TextureLibrary {
public:
    using ReadFn = std::function<bool(const std::string& path, std::string& out)>;
    TextureLibrary(gfx::Device& dev, ReadFn read, gfx::Filter filter = gfx::Filter::Linear);
    ~TextureLibrary();
    TextureLibrary(const TextureLibrary&) = delete;
    TextureLibrary& operator=(const TextureLibrary&) = delete;

    // nullptr when the file does not exist or cannot be decoded (logged once per path).
    const TextureRef* get(const std::string& path);
    bool atlased(const std::string& path) const { return images_.atlased(path); }
    // CPU pixels of the original image (atlased or not), e.g. for UI libraries.
    bool loadImage(const std::string& path, Image& out);

    const Manifest& manifest() const { return images_.manifest(); }
    size_t residentPages() const;
    void clear();   // destroys every texture this library created

private:
    gfx::TextureHandle pageTexture(int page);
    gfx::Device& dev_;
    ReadFn read_;
    gfx::Filter filter_;
    ImageSource images_;
    std::map<int, gfx::TextureHandle> pages_;
    std::map<std::string, TextureRef, std::less<>> refs_;
    std::map<std::string, bool, std::less<>> missing_;
};

} // namespace rts::atlas
