// Adaptive graphics quality: steps shadow / light tiers down when frames are consistently too slow,
// so one GPU list can't strand a weak phone at 15 fps. Pure logic (no GPU): the client feeds frame
// times and applies the tiers it returns.
//
// Slow = a frame longer than 1.35x the best frame seen (the display's refresh, e.g. 16.7 ms) and
// over 18 ms. A device that is capped at 30 fps from the start keeps its tiers (it is not slowing
// down). Steps, cheapest loss first: lights High->Medium, shadows High->Medium, shadows ->Low,
// lights ->Low, shadows ->Off, lights ->Off. After a long fast period one step is given back.
// A manual choice by the player (lock()) stops it. RTS_ADAPTIVE=off disables it,
// RTS_ADAPTIVE_FAKE_MS=<ms> adds load to every frame (testing).
#pragma once
#include <cstdint>

namespace rts::render {

class QualityGovernor {
public:
    // tiers as ints: ShadowQuality / LightQuality (0 off .. 3 high)
    void reset(int shadows, int lights);
    // Feed the frame time (seconds); true when the tiers changed (read shadows()/lights()).
    bool update(double dt);
    void lock() { locked_ = true; }
    bool locked() const { return locked_; }
    int shadows() const { return shadows_; }
    int lights() const { return lights_; }
    int stepsDown() const { return down_; }
    float bestFrameMs() const { return bestMs_; }

private:
    bool stepDown();
    bool stepUp();

    int shadows_ = 2, lights_ = 2, startShadows_ = 2, startLights_ = 2;
    int down_ = 0, ups_ = 0;
    int frames_ = 0, windowFrames_ = 0, windowSlow_ = 0, slowWindows_ = 0;
    double windowTime_ = 0, sinceChange_ = 0, fastTime_ = 0;
    float bestMs_ = 1e9f, fakeMs_ = 0;
    bool locked_ = false, disabled_ = false;
};

} // namespace rts::render
