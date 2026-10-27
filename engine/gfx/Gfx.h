// RHI: a thin, explicit rendering interface implemented directly on top of native APIs.
//
//   Backends:  Metal        -> macOS, iOS
//              OpenGL ES 3  -> Android, Web (WebGL2), Linux (dev/CI via EGL)
//              OpenGL 3.3   -> Windows (WGL)  [D3D11 backend planned]
//
// Conventions shared by every backend:
//   * pixel coordinates (viewport/scissor) have their origin in the TOP-LEFT corner;
//   * clip-space depth range is reported in DeviceInfo::clipDepthZeroToOne, build projection
//     matrices with it (Metal: [0,1], GL: [-1,1]);
//   * matrices are column-major, shaders multiply as  M * v;
//   * uniform data is pushed per draw with setUniforms(slot, ptr, size) (<= 4 KB, std140 layout);
//     GLSL: uniform block "U<slot>", MSL: [[buffer(1 + slot)]] in both stages;
//   * textures: GLSL sampler2D "tex<slot>" (sampler2DShadow for compare textures), MSL texture(slot) +
//     sampler(slot) (depth2d + sample_compare for compare textures);
//   * render-to-texture: when sampling a target in a shader, Metal needs v flipped (DeviceInfo::renderTargetFlipY);
//   * vertex buffer: one interleaved stream; GLSL attribute location = VertexAttr::location,
//     MSL [[attribute(location)]] via [[stage_in]] at buffer(0).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rts::gfx {

template <class Tag> struct Handle {
    uint32_t id = 0;
    bool valid() const { return id != 0; }
    bool operator==(const Handle&) const = default;
};
using BufferHandle = Handle<struct BufferTag>;
using TextureHandle = Handle<struct TextureTag>;
using ShaderHandle = Handle<struct ShaderTag>;
using PipelineHandle = Handle<struct PipelineTag>;

enum class Backend { Metal, GLES3, GL33 };
enum class BufferType { Vertex, Index };
enum class Usage { Immutable, Dynamic };          // Dynamic: updated (whole or partially) every frame
enum class PixelFormat { RGBA8, R8, Depth };   // Depth: 24/32-bit depth render target
enum class Filter { Nearest, Linear };
enum class VertexFormat { Float2, Float3, Float4, UByte4N };
enum class IndexType { None, UInt16, UInt32 };
enum class Primitive { Triangles, Lines };
enum class CullMode { None, Back, Front };
enum class BlendMode { Opaque, Alpha, Additive };

struct BufferDesc {
    BufferType type = BufferType::Vertex;
    Usage usage = Usage::Immutable;
    size_t size = 0;
    const void* data = nullptr;
    const char* label = nullptr;
};

struct TextureDesc {
    int width = 0, height = 0;
    PixelFormat format = PixelFormat::RGBA8;
    Filter filter = Filter::Linear;
    bool repeat = false;
    bool renderTarget = false;  // can be attached to a pass (color or depth)
    bool compare = false;       // depth textures: sampled with hardware depth comparison (shadow maps)
    const void* data = nullptr; // may be null (contents undefined until updated)
    const char* label = nullptr;
};

// Each backend picks the sources it understands. A shader may provide all of them.
struct ShaderDesc {
    const char* glslVertex = nullptr;   // GLSL ES 3.00 body WITHOUT #version line (added by backend)
    const char* glslFragment = nullptr;
    const char* msl = nullptr;          // one MSL source with entry points "vs_main"/"fs_main"
    int textureSlots = 0;               // number of sampled textures (tex0..)
    int uniformSlots = 1;               // number of uniform blocks (U0..)
    const char* label = nullptr;
};

struct VertexAttr {
    uint32_t location = 0;
    VertexFormat format = VertexFormat::Float3;
    uint32_t offset = 0;
};

struct PipelineDesc {
    ShaderHandle shader;
    VertexAttr attrs[8];
    int attrCount = 0;
    uint32_t stride = 0;
    Primitive primitive = Primitive::Triangles;
    IndexType indexType = IndexType::UInt16;
    CullMode cull = CullMode::Back;
    BlendMode blend = BlendMode::Opaque;
    bool depthTest = true;
    bool depthWrite = true;
    bool depthOnly = false;          // no color output (shadow maps); the pass must have no color target
    float depthBiasConstant = 0;     // polygon offset, used by shadow casters
    float depthBiasSlope = 0;
    const char* label = nullptr;
};

// Default: render to the screen. With targets: render into textures (both must match in size).
struct PassDesc {
    TextureHandle colorTarget;       // invalid = screen (or none when depthTarget is set)
    TextureHandle depthTarget;       // invalid = screen depth
    float clearColor[4] = {0.1f, 0.1f, 0.12f, 1.0f};
    bool clearColorEnabled = true;
    bool clearDepthEnabled = true;
    float clearDepth = 1.0f;
};

struct DeviceInfo {
    Backend backend = Backend::GLES3;
    bool clipDepthZeroToOne = false;
    bool renderTargetFlipY = false;  // texture v of a render target grows downward (Metal)
    std::string name;
};

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

class Device {
public:
    virtual ~Device() = default;
    virtual const DeviceInfo& info() const = 0;

    // Resources
    virtual BufferHandle createBuffer(const BufferDesc& d) = 0;
    virtual void updateBuffer(BufferHandle h, const void* data, size_t size, size_t offset = 0) = 0; // grows Dynamic buffers if needed
    virtual void destroyBuffer(BufferHandle h) = 0;
    virtual TextureHandle createTexture(const TextureDesc& d) = 0;
    virtual void updateTexture(TextureHandle h, int x, int y, int w, int h2, const void* pixels, int rowPitchBytes) = 0;
    virtual void destroyTexture(TextureHandle h) = 0;
    virtual ShaderHandle createShader(const ShaderDesc& d) = 0;   // returns invalid handle on failure (see lastError())
    virtual void destroyShader(ShaderHandle h) = 0;
    virtual PipelineHandle createPipeline(const PipelineDesc& d) = 0;
    virtual void destroyPipeline(PipelineHandle h) = 0;
    virtual const std::string& lastError() const = 0;

    // Frame (driven by the platform layer)
    virtual void beginFrame(int framebufferWidth, int framebufferHeight) = 0;
    virtual void endFrame() = 0; // presents
    int framebufferWidth() const { return fbW_; }
    int framebufferHeight() const { return fbH_; }

    // Commands (only valid between beginPass/endPass)
    virtual void beginPass(const PassDesc& d) = 0;
    virtual void endPass() = 0;
    virtual void setViewport(const Rect& r) = 0;
    virtual void setScissor(const Rect& r) = 0;   // w==0 disables
    virtual void setPipeline(PipelineHandle p) = 0;
    virtual void setVertexBuffer(BufferHandle b, size_t offset = 0) = 0;
    virtual void setIndexBuffer(BufferHandle b, size_t offset = 0) = 0;
    virtual void setTexture(int slot, TextureHandle t) = 0;
    virtual void setUniforms(int slot, const void* data, size_t size) = 0;
    virtual void draw(uint32_t vertexCount, uint32_t firstVertex = 0) = 0;
    virtual void drawIndexed(uint32_t indexCount, uint32_t firstIndex = 0, int32_t baseVertex = 0) = 0;

protected:
    int fbW_ = 0, fbH_ = 0;
};

// Backend factories. `nativeSurface` is backend specific:
//   Metal: CAMetalLayer*   GL: nullptr (context must be current on the calling thread)
std::unique_ptr<Device> createMetalDevice(void* nativeSurface);
std::unique_ptr<Device> createGLDevice(Backend glFlavor);

} // namespace rts::gfx
