// Client startup with the splash / loading screen (game.json "splash", engine/splash). The first
// frame only reads game.json and draws the splash; every following frame runs one GameClient init
// step (rules, renderer, view models + textures, effects, audio, UI) while the splash animates and
// its progress bar follows the real work (plus pack chunk downloads in split builds with
// "waitForContent"). When loading is done and the minimum time has passed (or the player tapped a
// skippable splash), the menu fades in under the splash.
//
// RTS_SPLASH=off loads everything in the init callback as before; with --map (tests, CI
// screenshots) the splash only covers the loading time unless RTS_SPLASH=full.
#pragma once
#include <functional>

#include "GameClient.h"
#include "engine/splash/SplashScreen.h"

namespace rts::client {

class ClientBoot {
public:
    enum class Mode { Full, LoadingOnly, Off };
    void init(gfx::Device& dev, GameClient& client, Mode mode, std::function<void()> afterLoad);
    void frame(gfx::Device& dev, double dt);
    void event(const platform::Event& e);
    void shutdown();
    bool booting() const { return phase_ != Phase::Done; }

private:
    enum class Phase { Loading, Fading, Done };
    float progress() const;
    gfx::Device* dev_ = nullptr;
    GameClient* client_ = nullptr;
    std::function<void()> afterLoad_;
    splash::SplashScreen splash_;
    Mode mode_ = Mode::Full;
    Phase phase_ = Phase::Loading;
    int step_ = -1;              // -1: first frame (splash only)
    bool contentWait_ = false;   // waitForContent: chunk downloads are part of loading
    float fade_ = 1;
};

} // namespace rts::client
