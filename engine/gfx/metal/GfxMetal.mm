// Metal backend (macOS, iOS). Compiled as Objective-C++ with ARC (-fobjc-arc).
#include "../Gfx.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <cstring>
#include <string>
#include <vector>

namespace rts::gfx {
namespace {

constexpr int kFramesInFlight = 3;

template <class T> class Pool {
public:
    uint32_t add(T v) {
        if (!free_.empty()) { uint32_t i = free_.back(); free_.pop_back(); items_[i] = std::move(v); alive_[i] = 1; return i + 1; }
        items_.push_back(std::move(v)); alive_.push_back(1); return uint32_t(items_.size());
    }
    T* get(uint32_t id) { return id && id <= items_.size() && alive_[id - 1] ? &items_[id - 1] : nullptr; }
    void remove(uint32_t id) { if (get(id)) { items_[id - 1] = T{}; alive_[id - 1] = 0; free_.push_back(id - 1); } }
private:
    std::vector<T> items_; std::vector<uint8_t> alive_; std::vector<uint32_t> free_;
};

struct MTBuffer {
    id<MTLBuffer> bufs[kFramesInFlight];
    int count = 1;          // 1 for immutable, kFramesInFlight for dynamic (one copy per frame in flight)
    size_t size = 0;
    uint64_t updatedFrame = UINT64_MAX;
};
struct MTTexture { id<MTLTexture> tex; id<MTLSamplerState> sampler; PixelFormat fmt = PixelFormat::RGBA8; bool mips = false; };
struct MTShader { id<MTLFunction> vs; id<MTLFunction> fs; };
struct MTPipeline { id<MTLRenderPipelineState> state; id<MTLDepthStencilState> depth; PipelineDesc desc; };

MTLVertexFormat toMTL(VertexFormat f) {
    switch (f) {
        case VertexFormat::Float2: return MTLVertexFormatFloat2;
        case VertexFormat::Float3: return MTLVertexFormatFloat3;
        case VertexFormat::Float4: return MTLVertexFormatFloat4;
        case VertexFormat::UByte4N: return MTLVertexFormatUChar4Normalized;
        case VertexFormat::UByte4: return MTLVertexFormatUChar4;
    }
    return MTLVertexFormatFloat3;
}

class MetalDevice final : public Device {
public:
    explicit MetalDevice(CAMetalLayer* layer) : layer_(layer) {
        device_ = layer.device ? layer.device : MTLCreateSystemDefaultDevice();
        layer_.device = device_;
        layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer_.framebufferOnly = YES;
        queue_ = [device_ newCommandQueue];
        frameSemaphore_ = dispatch_semaphore_create(kFramesInFlight);
        info_.backend = Backend::Metal;
        info_.clipDepthZeroToOne = true;
        info_.renderTargetFlipY = true;
        info_.name = std::string("Metal ") + device_.name.UTF8String;
    }

    const DeviceInfo& info() const override { return info_; }
    const std::string& lastError() const override { return error_; }

    // ---------- buffers ----------
    BufferHandle createBuffer(const BufferDesc& d) override {
        MTBuffer b;
        b.size = d.size ? d.size : 16;
        b.count = d.usage == Usage::Dynamic ? kFramesInFlight : 1;
        for (int i = 0; i < b.count; ++i) {
            b.bufs[i] = d.data ? [device_ newBufferWithBytes:d.data length:b.size options:MTLResourceStorageModeShared]
                               : [device_ newBufferWithLength:b.size options:MTLResourceStorageModeShared];
        }
        return {buffers_.add(b)};
    }
    id<MTLBuffer> current(MTBuffer& b) { return b.bufs[b.count == 1 ? 0 : int(frame_ % kFramesInFlight)]; }
    void updateBuffer(BufferHandle h, const void* data, size_t size, size_t offset) override {
        auto* b = buffers_.get(h.id);
        if (!b) return;
        if (offset + size > b->size) {
            b->size = (offset + size) * 3 / 2;
            for (int i = 0; i < b->count; ++i) b->bufs[i] = [device_ newBufferWithLength:b->size options:MTLResourceStorageModeShared];
        }
        if (b->count == 1 && frame_ > 0) {
            // Immutable buffer updated after creation: safest is a fresh allocation (GPU may still read the old one).
            id<MTLBuffer> nb = [device_ newBufferWithLength:b->size options:MTLResourceStorageModeShared];
            memcpy(nb.contents, b->bufs[0].contents, b->size);
            b->bufs[0] = nb;
        }
        memcpy(static_cast<uint8_t*>(current(*b).contents) + offset, data, size);
    }
    void destroyBuffer(BufferHandle h) override { buffers_.remove(h.id); }

    // ---------- textures ----------
    TextureHandle createTexture(const TextureDesc& d) override {
        MTLPixelFormat pf = d.format == PixelFormat::R8 ? MTLPixelFormatR8Unorm
                          : d.format == PixelFormat::RGBA32F ? MTLPixelFormatRGBA32Float
                          : d.format == PixelFormat::Depth ? MTLPixelFormatDepth32Float
                          : d.renderTarget ? MTLPixelFormatBGRA8Unorm   // same format as the screen: pipelines are shared
                          : MTLPixelFormatRGBA8Unorm;
        const bool mips = d.mipmaps && d.format != PixelFormat::Depth && d.format != PixelFormat::RGBA32F && !d.renderTarget;
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
            width:NSUInteger(d.width) height:NSUInteger(d.height) mipmapped:mips ? YES : NO];
        td.usage = MTLTextureUsageShaderRead | (d.renderTarget || d.format == PixelFormat::Depth ? MTLTextureUsageRenderTarget : 0);
        if (d.format == PixelFormat::Depth) td.storageMode = MTLStorageModePrivate;
        MTTexture t;
        t.fmt = d.format;
        t.tex = [device_ newTextureWithDescriptor:td];
        if (d.data && d.format != PixelFormat::Depth) {
            int bpp = d.format == PixelFormat::R8 ? 1 : d.format == PixelFormat::RGBA32F ? 16 : 4;
            [t.tex replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(d.width), NSUInteger(d.height)) mipmapLevel:0
                       withBytes:d.data bytesPerRow:NSUInteger(d.width * bpp)];
            if (mips) generateMips(t.tex);
        }
        t.mips = mips;
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = d.filter == Filter::Linear && d.format != PixelFormat::RGBA32F ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        if (mips) sd.mipFilter = d.filter == Filter::Linear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        sd.sAddressMode = sd.tAddressMode = d.repeat ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
        if (d.compare) sd.compareFunction = MTLCompareFunctionLessEqual;
        t.sampler = [device_ newSamplerStateWithDescriptor:sd];
        return {textures_.add(t)};
    }
    void updateTexture(TextureHandle h, int x, int y, int w, int hh, const void* pixels, int rowPitch) override {
        auto* t = textures_.get(h.id);
        if (!t) return;
        [t->tex replaceRegion:MTLRegionMake2D(NSUInteger(x), NSUInteger(y), NSUInteger(w), NSUInteger(hh)) mipmapLevel:0
                    withBytes:pixels bytesPerRow:NSUInteger(rowPitch)];
        if (t->mips) generateMips(t->tex);
    }
    // Own command buffer: textures are usually created while loading, outside of a frame.
    void generateMips(id<MTLTexture> tex) {
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit generateMipmapsForTexture:tex];
        [blit endEncoding];
        [cb commit];
    }
    void destroyTexture(TextureHandle h) override { textures_.remove(h.id); }

    // ---------- shaders / pipelines ----------
    ShaderHandle createShader(const ShaderDesc& d) override {
        if (!d.msl) { error_ = "shader has no MSL source"; return {}; }
        NSError* err = nil;
        id<MTLLibrary> lib = [device_ newLibraryWithSource:[NSString stringWithUTF8String:d.msl] options:nil error:&err];
        if (!lib) { error_ = std::string("MSL compile: ") + (err ? err.localizedDescription.UTF8String : "?"); return {}; }
        MTShader s;
        s.vs = [lib newFunctionWithName:@"vs_main"];
        s.fs = [lib newFunctionWithName:@"fs_main"]; // optional for depth-only shaders
        if (!s.vs) { error_ = "MSL: vs_main not found"; return {}; }
        return {shaders_.add(s)};
    }
    void destroyShader(ShaderHandle h) override { shaders_.remove(h.id); }

    PipelineHandle createPipeline(const PipelineDesc& d) override {
        auto* s = shaders_.get(d.shader.id);
        if (!s) { error_ = "pipeline: invalid shader"; return {}; }
        MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
        for (int i = 0; i < d.attrCount; ++i) {
            vd.attributes[d.attrs[i].location].format = toMTL(d.attrs[i].format);
            vd.attributes[d.attrs[i].location].offset = d.attrs[i].offset;
            vd.attributes[d.attrs[i].location].bufferIndex = 0;
        }
        vd.layouts[0].stride = d.stride;
        vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = s->vs;
        pd.fragmentFunction = s->fs; // depth-only shaders without fs_main get nil (no fragment stage)
        pd.vertexDescriptor = vd;
        pd.colorAttachments[0].pixelFormat = d.depthOnly ? MTLPixelFormatInvalid : layer_.pixelFormat;
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        auto* ca = pd.colorAttachments[0];
        if (d.blend != BlendMode::Opaque) {
            ca.blendingEnabled = YES;
            ca.rgbBlendOperation = ca.alphaBlendOperation = MTLBlendOperationAdd;
            ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            ca.destinationRGBBlendFactor = d.blend == BlendMode::Alpha ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOne;
            ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
            ca.destinationAlphaBlendFactor = d.blend == BlendMode::Alpha ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOne;
        }
        NSError* err = nil;
        MTPipeline p;
        p.desc = d;
        p.state = [device_ newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!p.state) { error_ = std::string("pipeline: ") + (err ? err.localizedDescription.UTF8String : "?"); return {}; }
        MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = d.depthTest ? MTLCompareFunctionLessEqual : MTLCompareFunctionAlways;
        dd.depthWriteEnabled = d.depthWrite ? YES : NO;
        p.depth = [device_ newDepthStencilStateWithDescriptor:dd];
        return {pipelines_.add(p)};
    }
    void destroyPipeline(PipelineHandle h) override { pipelines_.remove(h.id); }

    // ---------- frame ----------
    // ---------- capture (tools) ----------
    CaptureFn capture_;              // requested, waiting for a readable drawable
    int captureDelay_ = 0;
    struct Captured { int w = 0, h = 0; std::vector<uint8_t> px; std::atomic<bool> ready{false}; CaptureFn fn; };
    std::shared_ptr<Captured> captured_;
    void captureNextFrame(CaptureFn fn) override {
        capture_ = std::move(fn);
        layer_.framebufferOnly = NO; // drawables acquired from now on can be blitted
        captureDelay_ = 1;
    }
    void deliverCapture() {
        if (captured_ && captured_->ready) {
            auto c = captured_;
            captured_.reset();
            layer_.framebufferOnly = YES;
            if (c->fn) c->fn(c->w, c->h, std::move(c->px));
        }
    }

    void beginFrame(int w, int h) override {
        fbW_ = w; fbH_ = h;
        dispatch_semaphore_wait(frameSemaphore_, DISPATCH_TIME_FOREVER);
        deliverCapture(); // the previous frame's GPU work is done here
        ++frame_;
        layer_.drawableSize = CGSizeMake(w, h);
        drawable_ = [layer_ nextDrawable];
        if (!depth_ || int(depth_.width) != w || int(depth_.height) != h) {
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                width:NSUInteger(w) height:NSUInteger(h) mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget;
            td.storageMode = MTLStorageModePrivate;
            depth_ = [device_ newTextureWithDescriptor:td];
        }
        cmd_ = [queue_ commandBuffer];
        __block dispatch_semaphore_t sem = frameSemaphore_;
        [cmd_ addCompletedHandler:^(id<MTLCommandBuffer>) { dispatch_semaphore_signal(sem); }];
        firstPass_ = true;
    }
    void endFrame() override {
        if (enc_) { [enc_ endEncoding]; enc_ = nil; }
        if (capture_ && drawable_ && captureDelay_-- <= 0 && !drawable_.texture.framebufferOnly) {
            // copy the finished frame into a shared buffer before presenting it
            const NSUInteger w = drawable_.texture.width, h = drawable_.texture.height, row = w * 4;
            id<MTLBuffer> buf = [device_ newBufferWithLength:row * h options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> blit = [cmd_ blitCommandEncoder];
            [blit copyFromTexture:drawable_.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
            [blit endEncoding];
            auto c = std::make_shared<Captured>();
            c->w = int(w); c->h = int(h); c->fn = std::move(capture_);
            capture_ = nullptr;
            captured_ = c;
            [cmd_ addCompletedHandler:^(id<MTLCommandBuffer>) {
                const uint8_t* src = static_cast<const uint8_t*>(buf.contents);
                c->px.resize(size_t(row * h));
                for (size_t i = 0; i < size_t(w * h); ++i) { // BGRA -> RGBA
                    c->px[i * 4 + 0] = src[i * 4 + 2]; c->px[i * 4 + 1] = src[i * 4 + 1];
                    c->px[i * 4 + 2] = src[i * 4 + 0]; c->px[i * 4 + 3] = 255;
                }
                c->ready = true; // read on the main thread in the next beginFrame (after the semaphore)
            }];
        }
        if (drawable_) [cmd_ presentDrawable:drawable_];
        [cmd_ commit];
        cmd_ = nil;
        drawable_ = nil;
    }

    void beginPass(const PassDesc& d) override {
        if (!cmd_) return;
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        const bool offscreen = d.colorTarget.valid() || d.depthTarget.valid();
        passW_ = fbW_; passH_ = fbH_;
        if (offscreen) {
            auto* ct = textures_.get(d.colorTarget.id);
            auto* dt = textures_.get(d.depthTarget.id);
            id<MTLTexture> any = ct ? ct->tex : dt ? dt->tex : nil;
            if (any) { passW_ = int(any.width); passH_ = int(any.height); }
            if (ct) {
                rp.colorAttachments[0].texture = ct->tex;
                rp.colorAttachments[0].loadAction = d.clearColorEnabled ? MTLLoadActionClear : MTLLoadActionLoad;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(d.clearColor[0], d.clearColor[1], d.clearColor[2], d.clearColor[3]);
            }
            rp.depthAttachment.texture = dt ? dt->tex : nil;
            rp.depthAttachment.loadAction = d.clearDepthEnabled ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.depthAttachment.clearDepth = d.clearDepth;
            enc_ = [cmd_ renderCommandEncoderWithDescriptor:rp];
            [enc_ setFrontFacingWinding:MTLWindingCounterClockwise];
            pipelineId_ = 0;
            return;
        }
        if (!drawable_) return;
        rp.colorAttachments[0].texture = drawable_.texture;
        rp.colorAttachments[0].loadAction = d.clearColorEnabled ? MTLLoadActionClear : (firstPass_ ? MTLLoadActionDontCare : MTLLoadActionLoad);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(d.clearColor[0], d.clearColor[1], d.clearColor[2], d.clearColor[3]);
        rp.depthAttachment.texture = depth_;
        rp.depthAttachment.loadAction = d.clearDepthEnabled ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.depthAttachment.clearDepth = d.clearDepth;
        enc_ = [cmd_ renderCommandEncoderWithDescriptor:rp];
        [enc_ setFrontFacingWinding:MTLWindingCounterClockwise];
        firstPass_ = false;
        pipelineId_ = 0;
    }
    void endPass() override {
        if (enc_) { [enc_ endEncoding]; enc_ = nil; }
    }

    void setViewport(const Rect& r) override {
        if (enc_) [enc_ setViewport:MTLViewport{double(r.x), double(r.y), double(r.w), double(r.h), 0.0, 1.0}];
    }
    void setScissor(const Rect& r) override {
        if (!enc_) return;
        if (r.w <= 0 || r.h <= 0) { [enc_ setScissorRect:MTLScissorRect{0, 0, NSUInteger(passW_), NSUInteger(passH_)}]; return; }
        int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
        int x1 = std::min(passW_, r.x + r.w), y1 = std::min(passH_, r.y + r.h);
        if (x1 <= x0 || y1 <= y0) { x0 = y0 = 0; x1 = y1 = 1; } // Metal forbids empty rects; 1px is invisible enough
        [enc_ setScissorRect:MTLScissorRect{NSUInteger(x0), NSUInteger(y0), NSUInteger(x1 - x0), NSUInteger(y1 - y0)}];
    }
    void setPipeline(PipelineHandle h) override {
        auto* p = pipelines_.get(h.id);
        if (!enc_ || !p) return;
        pipelineId_ = h.id;
        [enc_ setRenderPipelineState:p->state];
        [enc_ setDepthStencilState:p->depth];
        [enc_ setCullMode:p->desc.cull == CullMode::None ? MTLCullModeNone : p->desc.cull == CullMode::Back ? MTLCullModeBack : MTLCullModeFront];
        [enc_ setDepthBias:p->desc.depthBiasConstant slopeScale:p->desc.depthBiasSlope clamp:0.0f];
    }
    void setVertexBuffer(BufferHandle h, size_t offset) override {
        auto* b = buffers_.get(h.id);
        if (enc_ && b) [enc_ setVertexBuffer:current(*b) offset:offset atIndex:0];
    }
    void setIndexBuffer(BufferHandle h, size_t offset) override { ibId_ = h.id; ibOffset_ = offset; }
    void setTexture(int slot, TextureHandle h) override {
        auto* t = textures_.get(h.id);
        if (!enc_ || !t) return;
        [enc_ setFragmentTexture:t->tex atIndex:NSUInteger(slot)];
        [enc_ setFragmentSamplerState:t->sampler atIndex:NSUInteger(slot)];
        [enc_ setVertexTexture:t->tex atIndex:NSUInteger(slot)];   // bone / morph data textures are read in the vertex stage
        [enc_ setVertexSamplerState:t->sampler atIndex:NSUInteger(slot)];
    }
    void setUniforms(int slot, const void* data, size_t size) override {
        if (!enc_) return;
        [enc_ setVertexBytes:data length:size atIndex:NSUInteger(1 + slot)];
        [enc_ setFragmentBytes:data length:size atIndex:NSUInteger(1 + slot)];
    }
    MTLPrimitiveType prim() {
        auto* p = pipelines_.get(pipelineId_);
        return p && p->desc.primitive == Primitive::Lines ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle;
    }
    void draw(uint32_t count, uint32_t first) override {
        if (!enc_ || !pipelines_.get(pipelineId_)) return;
        [enc_ drawPrimitives:prim() vertexStart:first vertexCount:count];
    }
    void drawIndexed(uint32_t count, uint32_t firstIndex, int32_t baseVertex, uint32_t instances) override {
        auto* p = pipelines_.get(pipelineId_);
        auto* ib = buffers_.get(ibId_);
        if (!enc_ || !p || !ib) return;
        const bool u32 = p->desc.indexType == IndexType::UInt32;
        [enc_ drawIndexedPrimitives:prim() indexCount:count indexType:u32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                        indexBuffer:current(*ib) indexBufferOffset:ibOffset_ + size_t(firstIndex) * (u32 ? 4 : 2)
                      instanceCount:std::max<uint32_t>(instances, 1) baseVertex:baseVertex baseInstance:0];
    }

private:
    CAMetalLayer* layer_;
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    id<MTLCommandBuffer> cmd_;
    id<MTLRenderCommandEncoder> enc_;
    id<CAMetalDrawable> drawable_;
    id<MTLTexture> depth_;
    dispatch_semaphore_t frameSemaphore_;
    uint64_t frame_ = 0;
    bool firstPass_ = true;
    DeviceInfo info_;
    std::string error_;
    Pool<MTBuffer> buffers_;
    Pool<MTTexture> textures_;
    Pool<MTShader> shaders_;
    Pool<MTPipeline> pipelines_;
    uint32_t pipelineId_ = 0, ibId_ = 0;
    size_t ibOffset_ = 0;
    int passW_ = 0, passH_ = 0;
};

} // namespace

std::unique_ptr<Device> createMetalDevice(void* nativeSurface) {
    return std::make_unique<MetalDevice>((__bridge CAMetalLayer*)nativeSurface);
}

} // namespace rts::gfx
