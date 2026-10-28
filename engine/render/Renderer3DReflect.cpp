// Lighting passes (lighting agent). Planar water reflection: on the High light tier, the opaque meshes, models and
// light glows are drawn once more, mirrored in the water plane, into a half-resolution target that
// the lit shader blends over the water (LightShaders.h, waterReflect). Terrain (custom renderers)
// and effects are skipped: a riverbed mirrored above the surface would cover everything, and the
// sky colour behind the objects reads as the reflected sky anyway.
#include "Renderer3D.h"
#include <chrono>
#include <cstring>

namespace rts::render {

namespace {
struct DrawUniformsR { float model[16]; float color[4]; float params[4]; };   // = DrawUniforms (Renderer3D.cpp)
} // namespace

void Renderer3D::renderReflection() {
    if (!lights.reflectionsWanted()) return;
    const auto t0 = std::chrono::steady_clock::now();
    // the water: the flat translucent glossy mesh (the terrain draws it with specular ~0.9)
    const DrawItem* water = nullptr;
    for (const auto& it : items_)
        if (it.color.w < 0.99f && it.specular >= 0.85f && !(it.flags & kDrawUnlit)) { water = &it; break; }
    if (!water) return;
    const float planeY = water->model.transformPoint((water->mesh->boundsMin + water->mesh->boundsMax) * 0.5f).y;
    const int w = sceneW_ > 0 ? sceneW_ : dev_->framebufferWidth(), h = sceneH_ > 0 ? sceneH_ : dev_->framebufferHeight();

    // mirror across y = planeY; flipping clip-space x as well keeps the triangle winding (back-face
    // culling stays valid), the shader flips u back
    mat4 mirror;
    mirror.at(1, 1) = -1;
    mirror.at(1, 3) = 2 * planeY;
    const mat4 savedVP = viewProj_;
    const float3 savedCam = camPos_;
    viewProj_ = mat4::scale({-1, 1, 1}) * viewProj_ * mirror;
    camPos_ = {camPos_.x, 2 * planeY - camPos_.y, camPos_.z};

    const float3 sky = (lighting.fogColor + lighting.skyColor) * 0.5f;
    if (lights.beginReflection(w, h, sky)) {
        dev_->setPipeline(litPipe_);
        uploadFrameUniforms(false);
        dev_->setTexture(0, shadowMap_);
        lights.bind(2, 1, 2);   // not ready yet: binds black for the reflection slot
        for (const auto& it : items_) {
            if (it.color.w < 0.99f) continue;
            DrawUniformsR u{};
            std::memcpy(u.model, it.model.m, sizeof u.model);
            u.color[0] = it.color.x; u.color[1] = it.color.y; u.color[2] = it.color.z; u.color[3] = it.color.w;
            u.params[0] = (it.flags & kDrawUnlit) ? 1.f : 0.f;
            u.params[1] = it.specular;
            u.params[2] = (it.flags & kDrawNoReceiveShadow) ? 0.f : 1.f;
            u.params[3] = it.emissive;
            dev_->setUniforms(0, &u, sizeof u);
            dev_->setVertexBuffer(it.mesh->vb);
            dev_->setIndexBuffer(it.mesh->ib);
            dev_->drawIndexed(it.mesh->indexCount);
            ++drawCalls_;
        }
        renderModels(1);
        lights.drawHalos(viewProj_, false);   // lights glitter in the water; no ground decals
        lights.endReflection();
    }
    viewProj_ = savedVP;
    camPos_ = savedCam;
    lights.noteReflectionTime(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void Renderer3D::renderSpotShadow() {
    if (!lights.prepareSpotShadow()) return;
    // the sun shadow pipeline and casters, seen from the spot (perspective; the sun pass culling
    // decides what may cast — good enough for the area around the camera)
    const mat4 savedLVP = lightViewProj_;
    lightViewProj_ = lights.spotShadowMatrix();
    dev_->beginPass(lights.spotShadowPass());
    dev_->setViewport({0, 0, lights.spotShadowSize(), lights.spotShadowSize()});
    dev_->setPipeline(shadowPipe_);
    uploadFrameUniforms(true);
    for (const auto& it : items_) {
        if (it.flags & (kDrawNoCastShadow | kDrawUnlit) || it.color.w < 0.99f) continue;
        DrawUniformsR u{};
        std::memcpy(u.model, it.model.m, sizeof u.model);
        dev_->setUniforms(0, &u, sizeof u);
        dev_->setVertexBuffer(it.mesh->vb);
        dev_->setIndexBuffer(it.mesh->ib);
        dev_->drawIndexed(it.mesh->indexCount);
        ++drawCalls_;
    }
    renderModels(0);
    dev_->endPass();
    lightViewProj_ = savedLVP;
}

} // namespace rts::render
