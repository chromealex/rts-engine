// Host-side image helpers for icons and launch screens (rtsbuild, editor, MCP, tests): decode
// (stb_image via engine/atlas), high-quality resize, flatten, rounded/circle masks, PNG / ICO / ICNS
// encoders. No GPU, no platform tools (sips/iconutil are not needed, so it works from Linux too).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "engine/atlas/AtlasFormat.h"
#include "engine/splash/SplashData.h"

namespace rts::branding {

using Image = rts::atlas::Image;
using splash::Color;

bool loadImageFile(const std::string& path, Image& out, std::string* err = nullptr);
Image makeImage(int w, int h, Color fill = {0, 0, 0, 0});

// Resample to w x h. Downscaling averages the covered area (exact box/area filter, alpha-weighted),
// upscaling is bilinear; both work on premultiplied colour so transparent edges don't darken.
Image resize(const Image& src, int w, int h);
// Scales src to fit inside (w, h) keeping the aspect ratio (contain) or to cover it (cover, cropped).
Image fitContain(const Image& src, int w, int h);
Image fitCover(const Image& src, int w, int h);

bool hasAlpha(const Image& img);                   // any pixel not fully opaque
bool cornersTransparent(const Image& img);         // a logo on transparency (not full-bleed art)
Color edgeColor(const Image& img);                 // average of the opaque border pixels (white if none)
void flatten(Image& img, Color bg);                // composite over an opaque colour, alpha = 255
void blit(Image& dst, const Image& src, int x, int y, float opacity = 1); // "over", straight alpha
void fillRect(Image& dst, int x, int y, int w, int h, Color c);
// Multiplies alpha by an anti-aliased rounded rectangle (radius in pixels) / circle covering the image.
void maskRoundedRect(Image& img, float radius);
void maskCircle(Image& img);
// Squircle-ish (superellipse n = 4) mask, as Android launchers / iOS draw it (previews only).
void maskSquircle(Image& img);
// Draws src rotated by `radians` around its centre at (cx, cy) with size (w, h) — CPU preview of a quad.
void drawQuad(Image& dst, const Image& src, float cx, float cy, float w, float h, float radians, float opacity,
              float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1);
// Tiny built-in 5x7 bitmap font (A-Z, 0-9, a few symbols; lower case drawn as upper) for labels.
void drawText(Image& dst, int x, int y, const std::string& text, Color c, int scale = 1);
int textWidth(const std::string& text, int scale = 1);

std::string encodePng(const Image& img);
bool writePng(const std::string& path, const Image& img, std::string* err = nullptr);
// Windows .ico: every image (square, <= 256) as one entry; 256 is stored as PNG, smaller ones as
// 32-bit BMP (with AND mask) so every Windows version and tool reads them.
std::string encodeIco(const std::vector<Image>& images);
// macOS .icns from (OSType, PNG image) pairs, e.g. {"ic10", 1024x1024}.
std::string encodeIcns(const std::vector<std::pair<std::string, Image>>& entries);
bool writeFile(const std::string& path, const std::string& bytes, std::string* err = nullptr);

} // namespace rts::branding
