#include "ImGuiLayer.h"
#include "Shaders.h"
#include "Math3D.h"
#include "imgui.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace rts::render {

namespace {
ImGuiKey toImGuiKey(platform::Key k) {
    using platform::Key;
    if (k >= Key::A && k <= Key::Z) return ImGuiKey(ImGuiKey_A + (int(k) - int(Key::A)));
    if (k >= Key::Num0 && k <= Key::Num9) return ImGuiKey(ImGuiKey_0 + (int(k) - int(Key::Num0)));
    if (k >= Key::F1 && k <= Key::F12) return ImGuiKey(ImGuiKey_F1 + (int(k) - int(Key::F1)));
    switch (k) {
        case Key::Escape: return ImGuiKey_Escape; case Key::Enter: return ImGuiKey_Enter; case Key::Tab: return ImGuiKey_Tab;
        case Key::Backspace: return ImGuiKey_Backspace; case Key::Delete: return ImGuiKey_Delete; case Key::Insert: return ImGuiKey_Insert;
        case Key::Space: return ImGuiKey_Space; case Key::Left: return ImGuiKey_LeftArrow; case Key::Right: return ImGuiKey_RightArrow;
        case Key::Up: return ImGuiKey_UpArrow; case Key::Down: return ImGuiKey_DownArrow; case Key::Home: return ImGuiKey_Home;
        case Key::End: return ImGuiKey_End; case Key::PageUp: return ImGuiKey_PageUp; case Key::PageDown: return ImGuiKey_PageDown;
        case Key::LeftShift: return ImGuiKey_LeftShift; case Key::RightShift: return ImGuiKey_RightShift;
        case Key::LeftCtrl: return ImGuiKey_LeftCtrl; case Key::RightCtrl: return ImGuiKey_RightCtrl;
        case Key::LeftAlt: return ImGuiKey_LeftAlt; case Key::RightAlt: return ImGuiKey_RightAlt;
        case Key::LeftSuper: return ImGuiKey_LeftSuper; case Key::RightSuper: return ImGuiKey_RightSuper;
        case Key::Minus: return ImGuiKey_Minus; case Key::Equal: return ImGuiKey_Equal; case Key::Comma: return ImGuiKey_Comma;
        case Key::Period: return ImGuiKey_Period; case Key::Slash: return ImGuiKey_Slash; case Key::Semicolon: return ImGuiKey_Semicolon;
        case Key::LeftBracket: return ImGuiKey_LeftBracket; case Key::RightBracket: return ImGuiKey_RightBracket;
        case Key::Apostrophe: return ImGuiKey_Apostrophe; case Key::Backslash: return ImGuiKey_Backslash; case Key::GraveAccent: return ImGuiKey_GraveAccent;
        default: return ImGuiKey_None;
    }
}

const char* getClipboard(ImGuiContext*) {
    static std::string s;
    s = platform::clipboardText();
    return s.c_str();
}
void setClipboard(ImGuiContext*, const char* t) { platform::setClipboardText(t ? t : ""); }

constexpr uintptr_t kAlpha8Flag = 1;
} // namespace

bool ImGuiLayer::init(gfx::Device& dev, const char* fontPath, float fontSize) {
    dev_ = &dev;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "rts_rhi";
    io.BackendPlatformName = "rts_platform";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.IniFilename = nullptr; // layout is set up by the application
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_GetClipboardTextFn = getClipboard;
    pio.Platform_SetClipboardTextFn = setClipboard;

    std::string ttf;
    // game font first, then the engine's default UI font (Noto Sans, Latin + Cyrillic)
    if ((fontPath && platform::readContentFile(fontPath, ttf) && !ttf.empty()) ||
        (platform::readContentFile("engine/fonts/ui.ttf", ttf) && !ttf.empty())) {
        void* mem = IM_ALLOC(ttf.size());
        std::memcpy(mem, ttf.data(), ttf.size());
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = true;
        io.Fonts->AddFontFromMemoryTTF(mem, int(ttf.size()), fontSize, &cfg);
    } else {
        io.Fonts->AddFontDefault();
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6; st.FrameRounding = 4; st.GrabRounding = 4; st.TabRounding = 4;
    st.WindowBorderSize = 0; st.FramePadding = ImVec2(8, 5);
    if (platform::isTouchDevice()) { st.ScaleAllSizes(1.6f); st.FontScaleMain = 1.4f; st.TouchExtraPadding = ImVec2(6, 6); }

    gfx::ShaderDesc sd;
    sd.glslVertex = shaders::kImGuiVS; sd.glslFragment = shaders::kImGuiFS; sd.msl = shaders::kImGuiMSL;
    sd.textureSlots = 1; sd.label = "imgui";
    shader_ = dev.createShader(sd);
    if (!shader_.valid()) return false;
    gfx::PipelineDesc pd;
    pd.shader = shader_;
    pd.attrs[0] = {0, gfx::VertexFormat::Float2, offsetof(ImDrawVert, pos)};
    pd.attrs[1] = {1, gfx::VertexFormat::Float2, offsetof(ImDrawVert, uv)};
    pd.attrs[2] = {2, gfx::VertexFormat::UByte4N, offsetof(ImDrawVert, col)};
    pd.attrCount = 3; pd.stride = sizeof(ImDrawVert);
    pd.indexType = sizeof(ImDrawIdx) == 2 ? gfx::IndexType::UInt16 : gfx::IndexType::UInt32;
    pd.cull = gfx::CullMode::None; pd.blend = gfx::BlendMode::Alpha;
    pd.depthTest = false; pd.depthWrite = false;
    pipeline_ = dev.createPipeline(pd);
    gfx::BufferDesc vb; vb.usage = gfx::Usage::Dynamic; vb.size = 256 * 1024;
    vb_ = dev.createBuffer(vb);
    gfx::BufferDesc ib = vb; ib.type = gfx::BufferType::Index;
    ib_ = dev.createBuffer(ib);
    return pipeline_.valid();
}

void ImGuiLayer::shutdown() {
    if (!dev_) return;
    for (ImTextureData* t : ImGui::GetPlatformIO().Textures) {
        if (t->TexID != ImTextureID_Invalid) dev_->destroyTexture({uint32_t(t->TexID)});
        t->SetTexID(ImTextureID_Invalid);
        t->SetStatus(ImTextureStatus_Destroyed);
    }
    dev_->destroyBuffer(vb_); dev_->destroyBuffer(ib_);
    dev_->destroyPipeline(pipeline_); dev_->destroyShader(shader_);
    ImGui::DestroyContext();
    dev_ = nullptr;
}

bool ImGuiLayer::handleEvent(const platform::Event& e) {
    using platform::EventType;
    if (!dev_ || !ImGui::GetCurrentContext()) return false; // platforms may send focus/resize before init
    ImGuiIO& io = ImGui::GetIO();
    auto mods = [&](uint8_t m) {
        io.AddKeyEvent(ImGuiMod_Ctrl, m & platform::kModCtrl);
        io.AddKeyEvent(ImGuiMod_Shift, m & platform::kModShift);
        io.AddKeyEvent(ImGuiMod_Alt, m & platform::kModAlt);
        io.AddKeyEvent(ImGuiMod_Super, m & platform::kModSuper);
    };
    switch (e.type) {
    case EventType::MouseMove:
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        io.AddMousePosEvent(e.x, e.y); return io.WantCaptureMouse;
    case EventType::MouseDown: case EventType::MouseUp:
        mods(e.mods);
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        io.AddMousePosEvent(e.x, e.y);
        io.AddMouseButtonEvent(e.button, e.type == EventType::MouseDown);
        return io.WantCaptureMouse;
    case EventType::MouseWheel:
        io.AddMouseWheelEvent(e.wheelX, e.wheelY); return io.WantCaptureMouse;
    case EventType::TouchBegin:
        if (primaryTouch_ != -1) return io.WantCaptureMouse;
        primaryTouch_ = e.touchId;
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(e.x, e.y);
        io.AddMouseButtonEvent(0, true);
        return io.WantCaptureMouse;
    case EventType::TouchMove:
        if (e.touchId != primaryTouch_) return io.WantCaptureMouse;
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(e.x, e.y);
        return io.WantCaptureMouse;
    case EventType::TouchEnd: case EventType::TouchCancel:
        if (e.touchId != primaryTouch_) return io.WantCaptureMouse;
        primaryTouch_ = -1;
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(e.x, e.y);
        io.AddMouseButtonEvent(0, false);
        return io.WantCaptureMouse;
    case EventType::KeyDown: case EventType::KeyUp: {
        mods(e.mods);
        ImGuiKey k = toImGuiKey(e.key);
        if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == EventType::KeyDown);
        return io.WantCaptureKeyboard;
    }
    case EventType::Char: io.AddInputCharacter(e.codepoint); return io.WantCaptureKeyboard;
    case EventType::FocusGained: io.AddFocusEvent(true); return false;
    case EventType::FocusLost: io.AddFocusEvent(false); return false;
    default: return false;
    }
}

bool ImGuiLayer::wantsMouse() const { return ImGui::GetIO().WantCaptureMouse; }
bool ImGuiLayer::wantsKeyboard() const { return ImGui::GetIO().WantCaptureKeyboard; }

void ImGuiLayer::newFrame(double dt) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(platform::windowWidth(), platform::windowHeight());
    float s = platform::dpiScale();
    io.DisplayFramebufferScale = ImVec2(s, s);
    io.DeltaTime = dt > 0 ? float(dt) : 1.0f / 60.0f;
    ImGui::NewFrame();
}

void ImGuiLayer::updateTextures() {
    for (ImTextureData* t : ImGui::GetPlatformIO().Textures) {
        const bool alpha8 = t->Format == ImTextureFormat_Alpha8;
        if (t->Status == ImTextureStatus_WantCreate) {
            gfx::TextureDesc td;
            td.width = t->Width; td.height = t->Height;
            td.format = alpha8 ? gfx::PixelFormat::R8 : gfx::PixelFormat::RGBA8;
            td.data = t->GetPixels();
            td.filter = gfx::Filter::Linear;
            gfx::TextureHandle h = dev_->createTexture(td);
            t->SetTexID(ImTextureID(h.id));
            t->BackendUserData = reinterpret_cast<void*>(alpha8 ? kAlpha8Flag : 0);
            t->SetStatus(ImTextureStatus_OK);
        } else if (t->Status == ImTextureStatus_WantUpdates) {
            gfx::TextureHandle h{uint32_t(t->TexID)};
            for (const ImTextureRect& r : t->Updates)
                dev_->updateTexture(h, r.x, r.y, r.w, r.h, t->GetPixelsAt(r.x, r.y), t->GetPitch());
            t->SetStatus(ImTextureStatus_OK);
        } else if (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames > 0) {
            dev_->destroyTexture({uint32_t(t->TexID)});
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

void ImGuiLayer::render() {
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    updateTextures();
    if (!dd || dd->TotalVtxCount == 0) return;

    // Merge all lists into one vertex/index buffer upload.
    static std::vector<ImDrawVert> verts;
    static std::vector<ImDrawIdx> idx;
    verts.clear(); idx.clear();
    verts.reserve(size_t(dd->TotalVtxCount)); idx.reserve(size_t(dd->TotalIdxCount));
    for (const ImDrawList* l : dd->CmdLists) {
        verts.insert(verts.end(), l->VtxBuffer.Data, l->VtxBuffer.Data + l->VtxBuffer.Size);
        idx.insert(idx.end(), l->IdxBuffer.Data, l->IdxBuffer.Data + l->IdxBuffer.Size);
    }
    dev_->updateBuffer(vb_, verts.data(), verts.size() * sizeof(ImDrawVert));
    dev_->updateBuffer(ib_, idx.data(), idx.size() * sizeof(ImDrawIdx));

    const float fbW = dd->DisplaySize.x * dd->FramebufferScale.x, fbH = dd->DisplaySize.y * dd->FramebufferScale.y;
    if (fbW <= 0 || fbH <= 0) return;
    struct { float proj[16]; float params[4]; } u{};
    mat4 p = mat4::ortho(dd->DisplayPos.x, dd->DisplayPos.x + dd->DisplaySize.x, dd->DisplayPos.y + dd->DisplaySize.y, dd->DisplayPos.y,
                         -1, 1, dev_->info().clipDepthZeroToOne);
    std::memcpy(u.proj, p.m, sizeof u.proj);

    dev_->setViewport({0, 0, int(fbW), int(fbH)});
    dev_->setPipeline(pipeline_);
    dev_->setVertexBuffer(vb_);
    dev_->setIndexBuffer(ib_);
    uint32_t lastTex = UINT32_MAX;
    float lastAlpha = -1;
    int vtxBase = 0, idxBase = 0;
    const ImVec2 off = dd->DisplayPos, sc = dd->FramebufferScale;
    for (const ImDrawList* l : dd->CmdLists) {
        for (const ImDrawCmd& c : l->CmdBuffer) {
            if (c.UserCallback) { if (c.UserCallback != ImDrawCallback_ResetRenderState) c.UserCallback(l, &c); continue; }
            float x0 = (c.ClipRect.x - off.x) * sc.x, y0 = (c.ClipRect.y - off.y) * sc.y;
            float x1 = (c.ClipRect.z - off.x) * sc.x, y1 = (c.ClipRect.w - off.y) * sc.y;
            if (x1 <= x0 || y1 <= y0) continue;
            dev_->setScissor({int(x0), int(y0), int(x1 - x0), int(y1 - y0)});
            uint32_t tex = uint32_t(c.GetTexID());
            float alpha = 0;
            for (ImTextureData* t : ImGui::GetPlatformIO().Textures)
                if (uint32_t(t->TexID) == tex) { alpha = reinterpret_cast<uintptr_t>(t->BackendUserData) == kAlpha8Flag ? 1.f : 0.f; break; }
            if (tex != lastTex) { dev_->setTexture(0, {tex}); lastTex = tex; }
            if (alpha != lastAlpha) { u.params[0] = alpha; dev_->setUniforms(0, &u, sizeof u); lastAlpha = alpha; }
            dev_->drawIndexed(c.ElemCount, uint32_t(idxBase) + c.IdxOffset, vtxBase + int(c.VtxOffset));
        }
        vtxBase += l->VtxBuffer.Size;
        idxBase += l->IdxBuffer.Size;
    }
    dev_->setScissor({});
}

} // namespace rts::render
