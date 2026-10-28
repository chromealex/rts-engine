// OpenGL ES 3.0 / WebGL2 / OpenGL 3.3 core backend.
#include "../Gfx.h"
#include "GLApi.h"

#include <cstring>
#include <string>
#include <vector>

#if defined(RTS_GL_LOADER)
#define RTS_GL_DEFINE(ret, name, args) PFN_##name name = nullptr;
RTS_GL_FUNCS(RTS_GL_DEFINE)
#undef RTS_GL_DEFINE
namespace rts::gfx {
bool loadGLFunctions() {
    bool ok = true;
#define RTS_GL_LOAD(ret, name, args) name = (PFN_##name)wglGetProcAddress(#name); if (!name) ok = false;
    RTS_GL_FUNCS(RTS_GL_LOAD)
#undef RTS_GL_LOAD
    return ok;
}
} // namespace rts::gfx
#endif

namespace rts::gfx {
namespace {

template <class T> class Pool {
public:
    uint32_t add(T v) {
        if (!free_.empty()) { uint32_t i = free_.back(); free_.pop_back(); items_[i] = std::move(v); alive_[i] = 1; return i + 1; }
        items_.push_back(std::move(v)); alive_.push_back(1); return uint32_t(items_.size());
    }
    T* get(uint32_t id) { return id && id <= items_.size() && alive_[id - 1] ? &items_[id - 1] : nullptr; }
    void remove(uint32_t id) { if (get(id)) { alive_[id - 1] = 0; free_.push_back(id - 1); } }
private:
    std::vector<T> items_; std::vector<uint8_t> alive_; std::vector<uint32_t> free_;
};

struct GLBuffer { GLuint id = 0; GLenum target = 0; size_t size = 0; Usage usage = Usage::Immutable; };
struct GLTexture { GLuint id = 0; PixelFormat fmt = PixelFormat::RGBA8; int w = 0, h = 0; bool mips = false; };
struct GLFramebuffer { uint32_t color = 0, depth = 0; GLuint fbo = 0; };
struct GLShader { GLuint program = 0; };
struct GLPipeline { PipelineDesc desc; GLuint program = 0; };

constexpr size_t kUniformRingSize = 1 << 20;
constexpr int kMaxUniformSlots = 8;

class GLDevice final : public Device {
public:
    explicit GLDevice(Backend flavor) {
        info_.backend = flavor;
        info_.clipDepthZeroToOne = false;
        const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* v = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        info_.name = std::string(flavor == Backend::GL33 ? "GL33 " : "GLES3 ") + (r ? r : "?") + " / " + (v ? v : "?");
        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);
        GLint align = 256;
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
        uboAlign_ = align > 0 ? size_t(align) : 256;
        glGenBuffers(1, &ubo_);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
        glBufferData(GL_UNIFORM_BUFFER, kUniformRingSize, nullptr, GL_STREAM_DRAW);
    }
    ~GLDevice() override {
        glDeleteBuffers(1, &ubo_);
        glDeleteVertexArrays(1, &vao_);
    }

    const DeviceInfo& info() const override { return info_; }
    const std::string& lastError() const override { return error_; }

    // ---------- buffers ----------
    BufferHandle createBuffer(const BufferDesc& d) override {
        GLBuffer b;
        b.target = d.type == BufferType::Index ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
        b.size = d.size; b.usage = d.usage;
        glGenBuffers(1, &b.id);
        glBindBuffer(b.target, b.id);
        glBufferData(b.target, GLsizeiptr(d.size), d.data, d.usage == Usage::Dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        attribsDirty_ = true;
        return {buffers_.add(b)};
    }
    void updateBuffer(BufferHandle h, const void* data, size_t size, size_t offset) override {
        auto* b = buffers_.get(h.id);
        if (!b) return;
        glBindBuffer(b->target, b->id);
        if (offset + size > b->size) {
            b->size = (offset + size) * 3 / 2;
            glBufferData(b->target, GLsizeiptr(b->size), nullptr, GL_DYNAMIC_DRAW);
        } else if (offset == 0 && b->usage == Usage::Dynamic) {
            glBufferData(b->target, GLsizeiptr(b->size), nullptr, GL_DYNAMIC_DRAW); // orphan
        }
        glBufferSubData(b->target, GLintptr(offset), GLsizeiptr(size), data);
        attribsDirty_ = true;
    }
    void destroyBuffer(BufferHandle h) override {
        if (auto* b = buffers_.get(h.id)) { glDeleteBuffers(1, &b->id); buffers_.remove(h.id); }
    }

    // ---------- textures ----------
    TextureHandle createTexture(const TextureDesc& d) override {
        GLTexture t; t.fmt = d.format; t.w = d.width; t.h = d.height;
        t.mips = d.mipmaps && d.format != PixelFormat::Depth && !d.renderTarget;
        glGenTextures(1, &t.id);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (d.format == PixelFormat::Depth) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, d.width, d.height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
            if (d.compare) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
            }
        } else if (d.format == PixelFormat::R8) glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, d.width, d.height, 0, GL_RED, GL_UNSIGNED_BYTE, d.data);
        else if (d.format == PixelFormat::RGBA32F) { t.mips = false; glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, d.width, d.height, 0, GL_RGBA, GL_FLOAT, d.data); }
        else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, d.width, d.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, d.data);
        GLint f = d.filter == Filter::Linear && d.format != PixelFormat::RGBA32F ? GL_LINEAR : GL_NEAREST; // float textures are not filterable on ES3
        if (t.mips) {
            if (d.data) glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, d.filter == Filter::Linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
        } else glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        GLint wrap = d.repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        return {textures_.add(t)};
    }
    void updateTexture(TextureHandle h, int x, int y, int w, int hh, const void* pixels, int rowPitch) override {
        auto* t = textures_.get(h.id);
        if (!t) return;
        glBindTexture(GL_TEXTURE_2D, t->id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const int bpp = t->fmt == PixelFormat::R8 ? 1 : t->fmt == PixelFormat::RGBA32F ? 16 : 4;
        const GLenum fmt = t->fmt == PixelFormat::R8 ? GL_RED : GL_RGBA;
        const GLenum type = t->fmt == PixelFormat::RGBA32F ? GL_FLOAT : GL_UNSIGNED_BYTE;
        if (rowPitch == w * bpp) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, hh, fmt, type, pixels);
        } else { // GL_UNPACK_ROW_LENGTH exists in ES3 but upload row by row for maximum portability
            for (int r = 0; r < hh; ++r)
                glTexSubImage2D(GL_TEXTURE_2D, 0, x, y + r, w, 1, fmt, type, static_cast<const uint8_t*>(pixels) + size_t(r) * rowPitch);
        }
        if (t->mips) glGenerateMipmap(GL_TEXTURE_2D);
    }
    void destroyTexture(TextureHandle h) override {
        if (auto* t = textures_.get(h.id)) { glDeleteTextures(1, &t->id); textures_.remove(h.id); }
        for (size_t i = 0; i < fbos_.size();) {
            if (fbos_[i].color == h.id || fbos_[i].depth == h.id) { glDeleteFramebuffers(1, &fbos_[i].fbo); fbos_.erase(fbos_.begin() + long(i)); }
            else ++i;
        }
    }
    GLuint framebufferFor(uint32_t color, uint32_t depth) {
        for (auto& f : fbos_) if (f.color == color && f.depth == depth) return f.fbo;
        GLFramebuffer f; f.color = color; f.depth = depth;
        glGenFramebuffers(1, &f.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, f.fbo);
        if (auto* c = textures_.get(color)) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->id, 0);
        if (auto* d = textures_.get(depth)) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, d->id, 0);
        GLenum none = GL_NONE, c0 = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, textures_.get(color) ? &c0 : &none);
        glReadBuffer(textures_.get(color) ? GL_COLOR_ATTACHMENT0 : GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) error_ = "framebuffer incomplete";
        fbos_.push_back(f);
        return f.fbo;
    }

    // ---------- shaders ----------
    GLuint compile(GLenum type, const char* body) {
        std::string src = info_.backend == Backend::GL33 ? "#version 330 core\n" : "#version 300 es\nprecision highp float;\nprecision highp int;\n";
        src += body;
        GLuint s = glCreateShader(type);
        const char* p = src.c_str();
        glShaderSource(s, 1, &p, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048]; GLsizei n = 0;
            glGetShaderInfoLog(s, sizeof log, &n, log);
            error_ = std::string(type == GL_VERTEX_SHADER ? "vertex" : "fragment") + " shader: " + std::string(log, size_t(n));
            glDeleteShader(s);
            return 0;
        }
        return s;
    }
    ShaderHandle createShader(const ShaderDesc& d) override {
        if (!d.glslVertex || !d.glslFragment) { error_ = "shader has no GLSL source"; return {}; }
        GLuint vs = compile(GL_VERTEX_SHADER, d.glslVertex);
        if (!vs) return {};
        GLuint fs = compile(GL_FRAGMENT_SHADER, d.glslFragment);
        if (!fs) { glDeleteShader(vs); return {}; }
        GLuint p = glCreateProgram();
        glAttachShader(p, vs); glAttachShader(p, fs);
        glLinkProgram(p);
        glDeleteShader(vs); glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048]; GLsizei n = 0;
            glGetProgramInfoLog(p, sizeof log, &n, log);
            error_ = "link: " + std::string(log, size_t(n));
            glDeleteProgram(p);
            return {};
        }
        glUseProgram(p);
        for (int i = 0; i < d.uniformSlots; ++i) {
            std::string n = "U" + std::to_string(i);
            GLuint bi = glGetUniformBlockIndex(p, n.c_str());
            if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p, bi, GLuint(i));
        }
        for (int i = 0; i < d.textureSlots; ++i) {
            std::string n = "tex" + std::to_string(i);
            GLint loc = glGetUniformLocation(p, n.c_str());
            if (loc >= 0) glUniform1i(loc, i);
        }
        currentProgram_ = p;
        return {shaders_.add(GLShader{p})};
    }
    void destroyShader(ShaderHandle h) override {
        if (auto* s = shaders_.get(h.id)) { glDeleteProgram(s->program); shaders_.remove(h.id); }
    }

    PipelineHandle createPipeline(const PipelineDesc& d) override {
        auto* s = shaders_.get(d.shader.id);
        if (!s) { error_ = "pipeline: invalid shader"; return {}; }
        return {pipelines_.add(GLPipeline{d, s->program})};
    }
    void destroyPipeline(PipelineHandle h) override { pipelines_.remove(h.id); }

    // ---------- frame ----------
    CaptureFn capture_;
    void beginFrame(int w, int h) override {
        fbW_ = w; fbH_ = h;
        uboOffset_ = 0;
        for (auto& d : slotData_) d.clear();
        glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
        glBufferData(GL_UNIFORM_BUFFER, kUniformRingSize, nullptr, GL_STREAM_DRAW); // orphan the ring
        glBindVertexArray(vao_);
    }
    void endFrame() override { // swap is done by the platform layer (EGL/WGL/browser)
        if (!capture_) return;
        std::vector<uint8_t> px(size_t(fbW_) * size_t(fbH_) * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, fbW_, fbH_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        const size_t row = size_t(fbW_) * 4;
        std::vector<uint8_t> tmp(row);
        for (int y = 0; y < fbH_ / 2; ++y) { // GL is bottom-up
            uint8_t* a = px.data() + size_t(y) * row;
            uint8_t* b = px.data() + size_t(fbH_ - 1 - y) * row;
            std::copy(a, a + row, tmp.data()); std::copy(b, b + row, a); std::copy(tmp.data(), tmp.data() + row, b);
        }
        auto fn = std::move(capture_);
        capture_ = nullptr;
        fn(fbW_, fbH_, std::move(px));
    }
    void captureNextFrame(CaptureFn fn) override { capture_ = std::move(fn); }

    void beginPass(const PassDesc& d) override {
        glBindVertexArray(vao_);
        glDisable(GL_SCISSOR_TEST);
        passW_ = fbW_; passH_ = fbH_;
        if (d.colorTarget.valid() || d.depthTarget.valid()) {
            glBindFramebuffer(GL_FRAMEBUFFER, framebufferFor(d.colorTarget.id, d.depthTarget.id));
            auto* t = textures_.get(d.colorTarget.valid() ? d.colorTarget.id : d.depthTarget.id);
            if (t) { passW_ = t->w; passH_ = t->h; }
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        glViewport(0, 0, passW_, passH_);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        GLbitfield mask = 0;
        if (d.clearColorEnabled) { glClearColor(d.clearColor[0], d.clearColor[1], d.clearColor[2], d.clearColor[3]); mask |= GL_COLOR_BUFFER_BIT; }
        if (d.clearDepthEnabled) {
            glDepthMask(GL_TRUE);
#if defined(RTS_GL_LOADER)
            glClearDepth(d.clearDepth);
#else
            glClearDepthf(d.clearDepth);
#endif
            mask |= GL_DEPTH_BUFFER_BIT;
        }
        if (mask) glClear(mask);
        pipelineId_ = 0;
    }
    void endPass() override {
        if (passW_ != fbW_ || passH_ != fbH_ || true) glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_POLYGON_OFFSET_FILL);
        passW_ = fbW_; passH_ = fbH_;
    }

    void setViewport(const Rect& r) override { glViewport(r.x, passH_ - (r.y + r.h), r.w, r.h); }
    void setScissor(const Rect& r) override {
        if (r.w <= 0 || r.h <= 0) { glDisable(GL_SCISSOR_TEST); return; }
        glEnable(GL_SCISSOR_TEST);
        glScissor(r.x, passH_ - (r.y + r.h), r.w, r.h);
    }

    void setPipeline(PipelineHandle h) override {
        auto* p = pipelines_.get(h.id);
        pipelineId_ = p ? h.id : 0;
        if (!p) return;
        const auto& d = p->desc;
        glUseProgram(p->program);
        if (d.cull == CullMode::None) glDisable(GL_CULL_FACE);
        else { glEnable(GL_CULL_FACE); glCullFace(d.cull == CullMode::Back ? GL_BACK : GL_FRONT); glFrontFace(GL_CCW); }
        if (d.depthTest) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); } else glDisable(GL_DEPTH_TEST);
        glDepthMask(d.depthWrite ? GL_TRUE : GL_FALSE);
        if (d.depthBiasConstant != 0 || d.depthBiasSlope != 0) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(d.depthBiasSlope, d.depthBiasConstant);
        } else glDisable(GL_POLYGON_OFFSET_FILL);
        switch (d.blend) {
            case BlendMode::Opaque: glDisable(GL_BLEND); break;
            case BlendMode::Alpha:
                glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD);
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
            case BlendMode::Additive:
                glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD);
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE); break;
        }
        attribsDirty_ = true;
    }
    void setVertexBuffer(BufferHandle b, size_t offset) override { vbId_ = b.id; vbOffset_ = offset; attribsDirty_ = true; }
    void setIndexBuffer(BufferHandle b, size_t offset) override {
        ibId_ = b.id; ibOffset_ = offset;
        if (auto* ib = buffers_.get(b.id)) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->id);
    }
    void setTexture(int slot, TextureHandle h) override {
        auto* t = textures_.get(h.id);
        glActiveTexture(GL_TEXTURE0 + GLenum(slot));
        glBindTexture(GL_TEXTURE_2D, t ? t->id : 0);
    }
    void setUniforms(int slot, const void* data, size_t size) override {
        if (slot < 0 || slot >= kMaxUniformSlots) return;
        size_t off = (uboOffset_ + uboAlign_ - 1) / uboAlign_ * uboAlign_;
        if (off + size > kUniformRingSize) { // ring exhausted: orphan and restart
            glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
            glBufferData(GL_UNIFORM_BUFFER, kUniformRingSize, nullptr, GL_STREAM_DRAW);
            uboOffset_ = 0;
            // the other slots still point into the orphaned storage: upload their last data again
            for (int s = 0; s < kMaxUniformSlots; ++s)
                if (s != slot && !slotData_[s].empty()) writeUniforms(s, slotData_[s].data(), slotData_[s].size());
            off = (uboOffset_ + uboAlign_ - 1) / uboAlign_ * uboAlign_;
        }
        slotData_[slot].assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        uboOffset_ = off;
        writeUniforms(slot, data, size);
    }
    void writeUniforms(int slot, const void* data, size_t size) {
        size_t off = (uboOffset_ + uboAlign_ - 1) / uboAlign_ * uboAlign_;
        glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
        glBufferSubData(GL_UNIFORM_BUFFER, GLintptr(off), GLsizeiptr(size), data);
        glBindBufferRange(GL_UNIFORM_BUFFER, GLuint(slot), ubo_, GLintptr(off), GLsizeiptr(size));
        uboOffset_ = off + size;
    }

    void applyAttribs(int32_t baseVertex) {
        auto* pl = pipelines_.get(pipelineId_);
        auto* vb = buffers_.get(vbId_);
        if (!pl || !vb) return;
        if (!attribsDirty_ && baseVertex == lastBaseVertex_) return;
        const auto& d = pl->desc;
        glBindBuffer(GL_ARRAY_BUFFER, vb->id);
        for (GLuint i = 0; i < 8; ++i) glDisableVertexAttribArray(i);
        const size_t base = vbOffset_ + size_t(baseVertex) * d.stride;
        for (int i = 0; i < d.attrCount; ++i) {
            const auto& a = d.attrs[i];
            GLint n = 3; GLenum type = GL_FLOAT; GLboolean norm = GL_FALSE;
            switch (a.format) {
                case VertexFormat::Float2: n = 2; break;
                case VertexFormat::Float3: n = 3; break;
                case VertexFormat::Float4: n = 4; break;
                case VertexFormat::UByte4N: n = 4; type = GL_UNSIGNED_BYTE; norm = GL_TRUE; break;
                case VertexFormat::UByte4: n = 4; type = GL_UNSIGNED_BYTE; break;
            }
            glEnableVertexAttribArray(a.location);
            glVertexAttribPointer(a.location, n, type, norm, GLsizei(d.stride), reinterpret_cast<const void*>(base + a.offset));
        }
        attribsDirty_ = false;
        lastBaseVertex_ = baseVertex;
    }
    GLenum prim() { auto* p = pipelines_.get(pipelineId_); return p && p->desc.primitive == Primitive::Lines ? GL_LINES : GL_TRIANGLES; }

    void draw(uint32_t count, uint32_t first) override {
        if (!pipelines_.get(pipelineId_)) return;
        applyAttribs(0);
        glDrawArrays(prim(), GLint(first), GLsizei(count));
    }
    void drawIndexed(uint32_t count, uint32_t firstIndex, int32_t baseVertex, uint32_t instances) override {
        auto* pl = pipelines_.get(pipelineId_);
        if (!pl || !buffers_.get(ibId_)) return;
        // ES 3.0 / WebGL2 lack glDrawElementsBaseVertex: emulate by offsetting attribute pointers.
        applyAttribs(baseVertex);
        const bool u32 = pl->desc.indexType == IndexType::UInt32;
        const size_t off = ibOffset_ + size_t(firstIndex) * (u32 ? 4 : 2);
        if (instances > 1) glDrawElementsInstanced(prim(), GLsizei(count), u32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, reinterpret_cast<const void*>(off), GLsizei(instances));
        else glDrawElements(prim(), GLsizei(count), u32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, reinterpret_cast<const void*>(off));
    }

private:
    DeviceInfo info_;
    std::string error_;
    GLuint vao_ = 0, ubo_ = 0, currentProgram_ = 0;
    size_t uboAlign_ = 256, uboOffset_ = 0;
    std::vector<uint8_t> slotData_[kMaxUniformSlots]; // last data per slot, re-uploaded when the ring is orphaned
    int passW_ = 0, passH_ = 0;
    std::vector<GLFramebuffer> fbos_;
    Pool<GLBuffer> buffers_;
    Pool<GLTexture> textures_;
    Pool<GLShader> shaders_;
    Pool<GLPipeline> pipelines_;
    uint32_t pipelineId_ = 0, vbId_ = 0, ibId_ = 0; // ids, not pointers: pools may reallocate
    size_t vbOffset_ = 0, ibOffset_ = 0;
    bool attribsDirty_ = true;
    int32_t lastBaseVertex_ = 0;
};

} // namespace

std::unique_ptr<Device> createGLDevice(Backend flavor) { return std::make_unique<GLDevice>(flavor); }

} // namespace rts::gfx
