#include "ClientSplash.h"

#include <algorithm>
#include <cstdio>

namespace rts::client {

void ClientBoot::init(gfx::Device& dev, GameClient& client, Mode mode, std::function<void()> afterLoad) {
    dev_ = &dev;
    client_ = &client;
    mode_ = mode;
    afterLoad_ = std::move(afterLoad);
    if (mode_ == Mode::Off) {
        client.init(dev);
        if (afterLoad_) afterLoad_();
        phase_ = Phase::Done;
        return;
    }
    Json block;
    std::string text;
    try {
        if (platform::readContentFile("game.json", text)) block = Json::parse(text)["splash"];
    } catch (const std::exception&) {}   // the client reports a broken game.json in its menu
    splash_.init(dev, block, [](const std::string& p, std::string& out) { return platform::readContentFile(p, out); });
    if (!splash_.error().empty()) std::fprintf(stderr, "splash: %s\n", splash_.error().c_str());
    contentWait_ = splash_.spec().waitForContent;
    if (contentWait_) platform::requestAllContent();
}

float ClientBoot::progress() const {
    float p = 0;
    for (int i = 0; i < step_ && i < GameClient::kInitSteps; ++i) p += GameClient::initWeight(i);
    if (contentWait_) {
        platform::ContentProgress c = platform::contentProgress();
        float dl = c.bytesTotal ? float(double(c.bytesReady) / double(c.bytesTotal)) : 1.0f;
        p = p * 0.5f + dl * 0.5f;   // downloads and local loading share the bar
    }
    return p;
}

void ClientBoot::frame(gfx::Device& dev, double dt) {
    const float px = platform::dpiScale();
    if (phase_ == Phase::Done) { client_->frame(dev, dt); return; }
    if (phase_ == Phase::Fading) {
        client_->frame(dev, dt);
        const float len = splash_.spec().fadeOut;
        fade_ = len > 0 ? fade_ - float(dt) / len : 0.0f;
        splash_.update(dt, false);
        if (fade_ <= 0) { phase_ = Phase::Done; splash_.shutdown(); return; }
        splash_.draw(dev, fade_, false, px);
        return;
    }
    // Loading: the first frame only shows the splash; then one init step per frame.
    const bool stepsDone = step_ >= GameClient::kInitSteps;
    bool contentDone = true;
    if (contentWait_) {
        platform::ContentProgress c = platform::contentProgress();
        contentDone = !c.busy && c.ready + c.failed >= c.chunks;
    }
    const bool loaded = stepsDone && contentDone;
    splash_.update(dt, !loaded);
    if (step_ >= 0 && !stepsDone) client_->initStep(dev, step_);
    if (!stepsDone) ++step_;
    splash_.setProgress(progress());
    const bool leave = mode_ == Mode::LoadingOnly ? loaded && splash_.imagesLoaded() : splash_.readyToLeave(loaded);
    if (leave) {
        for (auto& w : splash_.warnings()) std::fprintf(stderr, "%s\n", w.c_str());
        if (afterLoad_) afterLoad_();
        phase_ = Phase::Fading;
        fade_ = 1;
        client_->frame(dev, dt);
        splash_.draw(dev, fade_, false, px);
        return;
    }
    splash_.draw(dev, 1, true, px);
}

void ClientBoot::event(const platform::Event& e) {
    if (phase_ != Phase::Loading) { client_->event(e); return; }
    if (e.type == platform::EventType::MouseDown || e.type == platform::EventType::TouchBegin ||
        (e.type == platform::EventType::KeyDown && !e.repeat))
        splash_.skip();
}

void ClientBoot::shutdown() {
    splash_.shutdown();
    // a window closed in the middle of loading: the half-initialised client is left to the OS
    if (phase_ != Phase::Loading || step_ >= GameClient::kInitSteps) client_->shutdown();
}

} // namespace rts::client
