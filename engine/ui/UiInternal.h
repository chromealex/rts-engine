// Private RmlUi backends of UiLayer: renderer over gfx::Device and a stb_truetype font engine.
#pragma once
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/RenderInterface.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/gfx/Gfx.h"

namespace rts::ui {

class UiRenderer final : public Rml::RenderInterface {
public:
    bool init(gfx::Device& dev);
    void shutdown();
    void beginFrame(int fbW, int fbH);
    void setDynamicTexture(const std::string& name, gfx::TextureHandle tex, int w, int h);
    using ImageSource = std::function<bool(const std::string& path, int& w, int& h, std::vector<unsigned char>& rgba)>;
    void setImageSource(ImageSource s) { imageSource_ = std::move(s); }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void SetTransform(const Rml::Matrix4f* transform) override;

private:
    struct Geometry { gfx::BufferHandle vb, ib; uint32_t count = 0; };
    struct Texture { gfx::TextureHandle tex; bool owned = false; std::string dynamicName; };
    template <class T> static uintptr_t put(std::vector<T>& v, std::vector<uintptr_t>& freeList, T item);

    gfx::Device* dev_ = nullptr;
    gfx::ShaderHandle shader_;
    gfx::PipelineHandle pipeline_;
    gfx::TextureHandle white_;
    std::vector<Geometry> geos_;
    std::vector<uintptr_t> freeGeos_;
    std::vector<Texture> texs_;
    std::vector<uintptr_t> freeTexs_;
    struct Dyn { gfx::TextureHandle tex; int w = 0, h = 0; };
    std::map<std::string, Dyn> dynamic_;
    ImageSource imageSource_;
    float proj_[16] = {};
    float transform_[16] = {};
    bool hasTransform_ = false;
    bool scissorOn_ = false;
    Rml::Rectanglei scissor_;
    int fbW_ = 0, fbH_ = 0;
};

// TrueType fonts through stb_truetype; glyphs are rasterized on demand into one atlas per face size.
// Any family that wasn't loaded falls back to the first loaded font, so documents never lose text.
class UiFontEngine final : public Rml::FontEngineInterface {
public:
    UiFontEngine();
    ~UiFontEngine() override;
    bool addFont(std::string data, const std::string& family, Rml::Style::FontStyle style, Rml::Style::FontWeight weight);

    bool LoadFontFace(const Rml::String& file_name, int face_index, bool fallback_face, Rml::Style::FontWeight weight) override;
    bool LoadFontFace(Rml::Span<const Rml::byte> data, int face_index, const Rml::String& family, Rml::Style::FontStyle style,
                      Rml::Style::FontWeight weight, bool fallback_face) override;
    Rml::FontFaceHandle GetFontFaceHandle(const Rml::String& family, Rml::Style::FontStyle style, Rml::Style::FontWeight weight, int size) override;
    Rml::FontEffectsHandle PrepareFontEffects(Rml::FontFaceHandle handle, const Rml::FontEffectList& font_effects) override;
    const Rml::FontMetrics& GetFontMetrics(Rml::FontFaceHandle handle) override;
    int GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& ctx, Rml::Character prior_character) override;
    int GenerateString(Rml::RenderManager& rm, Rml::FontFaceHandle face, Rml::FontEffectsHandle effects, Rml::StringView string,
                       Rml::Vector2f position, Rml::ColourbPremultiplied colour, float opacity, const Rml::TextShapingContext& ctx,
                       Rml::TexturedMeshList& mesh_list) override;
    int GetVersion(Rml::FontFaceHandle handle) override;
    void ReleaseFontResources() override;
    void Shutdown() override; // atlas textures must go before the render manager

    struct File;
    struct Face;
private:
    std::vector<std::unique_ptr<File>> files_;
    std::vector<std::unique_ptr<Face>> faces_;
};

} // namespace rts::ui
