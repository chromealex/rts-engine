#include "QualityGovernor.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace rts::render {

namespace {
constexpr double kWindow = 2.0;        // seconds per measurement window
constexpr int kWarmupFrames = 30;      // loading hitches and shader compiles
constexpr double kCooldown = 4.0;      // after a change, let the new tier settle
constexpr double kRecover = 30.0;      // fast this long -> give one step back
} // namespace

void QualityGovernor::reset(int shadows, int lights) {
    shadows_ = startShadows_ = shadows;
    lights_ = startLights_ = lights;
    down_ = ups_ = frames_ = windowFrames_ = windowSlow_ = slowWindows_ = 0;
    windowTime_ = sinceChange_ = fastTime_ = 0;
    bestMs_ = 1e9f;
    locked_ = false;
    const char* off = std::getenv("RTS_ADAPTIVE");
    disabled_ = off && (std::strcmp(off, "off") == 0 || std::strcmp(off, "0") == 0);
    const char* fake = std::getenv("RTS_ADAPTIVE_FAKE_MS");
    fakeMs_ = fake ? float(std::atof(fake)) : 0.0f;
}

bool QualityGovernor::stepDown() {
    // cheapest visual loss first; per-pixel lights and big shadow maps are the expensive bits
    if (lights_ == 3) lights_ = 2;
    else if (shadows_ == 3) shadows_ = 2;
    else if (shadows_ == 2) shadows_ = 1;
    else if (lights_ == 2) lights_ = 1;
    else if (shadows_ == 1) shadows_ = 0;
    else if (lights_ == 1) lights_ = 0;
    else return false;
    ++down_;
    return true;
}

bool QualityGovernor::stepUp() {
    // reverse order, never above where the match started
    if (lights_ < 1 && startLights_ >= 1) lights_ = 1;
    else if (shadows_ < 1 && startShadows_ >= 1) shadows_ = 1;
    else if (lights_ < 2 && startLights_ >= 2) lights_ = 2;
    else if (shadows_ < 2 && startShadows_ >= 2) shadows_ = 2;
    else if (shadows_ < 3 && startShadows_ >= 3) shadows_ = 3;
    else if (lights_ < 3 && startLights_ >= 3) lights_ = 3;
    else return false;
    ++ups_;
    return true;
}

bool QualityGovernor::update(double dt) {
    if (disabled_ || locked_ || dt <= 0) return false;
    const float ms = float(dt * 1000.0) + fakeMs_;
    if (++frames_ <= kWarmupFrames || ms > 250.0f) return false;   // ignore loading / pauses
    bestMs_ = std::min(bestMs_, std::max(ms, 6.0f));
    const float slowMs = std::max(bestMs_ * 1.35f, 18.0f);
    sinceChange_ += dt;
    windowTime_ += dt;
    ++windowFrames_;
    if (ms > slowMs) ++windowSlow_;
    if (windowTime_ < kWindow) return false;

    const float slowShare = float(windowSlow_) / float(std::max(windowFrames_, 1));
    windowTime_ = 0; windowFrames_ = windowSlow_ = 0;
    if (sinceChange_ < kCooldown) return false;
    slowWindows_ = slowShare > 0.5f ? slowWindows_ + 1 : 0;
    fastTime_ = slowShare < 0.03f ? fastTime_ + kWindow : 0;
    if (slowWindows_ >= 2 && stepDown()) { slowWindows_ = 0; sinceChange_ = 0; fastTime_ = 0; return true; }
    // one step back after a long smooth stretch (the heavy fight is over); only once per match,
    // otherwise a borderline device would oscillate
    if (down_ > 0 && ups_ == 0 && fastTime_ >= kRecover && stepUp()) { sinceChange_ = 0; fastTime_ = 0; return true; }
    return false;
}

} // namespace rts::render
