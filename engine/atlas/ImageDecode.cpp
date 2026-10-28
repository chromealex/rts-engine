#include "AtlasFormat.h"

// Private copy of stb_image with internal linkage, so this library never clashes with another
// translation unit that also implements stb_image (engine/ui).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace rts::atlas {

bool decodeImageFile(const std::string& bytes, Image& out) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()), int(bytes.size()), &w, &h, &n, 4);
    if (!px) return false;
    out.width = w;
    out.height = h;
    out.rgba.assign(px, px + size_t(w) * size_t(h) * 4);
    stbi_image_free(px);
    return true;
}

} // namespace rts::atlas
