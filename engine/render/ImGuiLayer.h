// Dear ImGui integration: platform events -> ImGuiIO, draw data -> RHI. Same code on every backend.
#pragma once
#include "../gfx/Gfx.h"
#include "../platform/Platform.h"

namespace rts::render {

class ImGuiLayer {
public:
    // fontPath: TTF from content (needs Cyrillic for Russian UI); falls back to ImGui's built-in font.
    bool init(gfx::Device& dev, const char* fontPath = "fonts/ui.ttf", float fontSize = 16.0f);
    void shutdown();
    // Returns true if ImGui wants this event (the game should ignore it).
    bool handleEvent(const platform::Event& e);
    void newFrame(double dt);
    void render(); // inside an active render pass
    bool wantsMouse() const;
    bool wantsKeyboard() const;

private:
    void updateTextures();
    gfx::Device* dev_ = nullptr;
    gfx::ShaderHandle shader_;
    gfx::PipelineHandle pipeline_;
    gfx::BufferHandle vb_, ib_;
    int64_t primaryTouch_ = -1;
};

} // namespace rts::render
