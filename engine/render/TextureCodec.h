// GPU block-compressed textures (CPU side): KTX 2.0 container reader and CPU fallback decoders.
//
// parseKtx2 extracts the raw block data of every mip level so the RHI can upload it as-is when the
// GPU supports the format (ETC2/EAC on GLES3/WebGL2 mobile, BC on desktop, ASTC on Apple/Android).
// decodeBlocks expands one level to RGBA8 for GPUs that lack the format.
//
// Block layouts follow the Khronos Data Format Specification (ETC1/ETC2/EAC), the D3D/BPTC
// specifications (BC1/BC3/BC4/BC7) and the KTX 2.0 specification for the container.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rts::render {

// None = uncompressed RGBA8 (levels hold width*height*4 bytes each).
enum class BlockFormat : uint8_t { None, ETC2_RGB8, ETC2_RGBA8, EAC_R11, BC1, BC3, BC4, BC7, ASTC_4x4 };

struct CompressedImage {
    BlockFormat format = BlockFormat::None;
    int width = 0, height = 0;          // of level 0, in pixels
    bool srgb = false;                  // colour data is sRGB-encoded (use the *_SRGB GPU format)
    uint32_t vkFormat = 0;              // original VkFormat from the KTX2 header
    std::vector<std::vector<uint8_t>> levels;   // mip 0..n (0 = largest), raw blocks, rows of blocks top to bottom
};

// Bytes per 4x4 block (BC1/ETC2_RGB8/EAC_R11/BC4: 8; ETC2_RGBA8/BC3/BC7/ASTC_4x4: 16); 4 for None (per pixel).
int blockBytes(BlockFormat f);
const char* blockFormatName(BlockFormat f);
int levelWidth(const CompressedImage& img, int level);    // max(1, width >> level)
int levelHeight(const CompressedImage& img, int level);
// Expected byte size of one level: ceil(w/4) * ceil(h/4) * blockBytes, or w*h*4 for None.
size_t levelByteSize(BlockFormat f, int width, int height);

// True when bytes start with the KTX 2.0 identifier.
bool isKtx2(const std::string& bytes);

// Parses a KTX 2.0 file (supercompression none or Zstandard). Arrays and cube maps keep only the
// first layer / face; 3D textures, BasisLZ/UASTC and unsupported vkFormats fail with a readable err.
// Never reads out of bounds on malformed input.
bool parseKtx2(const std::string& bytes, CompressedImage& out, std::string& err);

// Decodes one level to tightly packed RGBA8 (levelWidth x levelHeight). Supported: None, ETC2_RGB8,
// ETC2_RGBA8, EAC_R11 (-> r,0,0,255), BC1, BC3, BC4 (-> r,0,0,255), BC7. ASTC_4x4 returns false.
// BC1 three-colour blocks decode index 3 as transparent black, or opaque black when vkFormat is
// BC1_RGB_* (131/132). BC3 colour is always four-colour (S3TC spec).
bool decodeBlocks(const CompressedImage& in, int level, std::vector<uint8_t>& rgba);

// Decodes one 4x4 block of format f (not None / ASTC) into 16 RGBA8 pixels, row-major (64 bytes).
// Returns false for formats without a CPU decoder.
bool decodeBlock(BlockFormat f, const uint8_t* block, uint8_t out[64]);

} // namespace rts::render
