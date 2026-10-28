#include "AudioReverb.h"
#include <algorithm>

namespace rts::audio {

namespace {
// Freeverb tunings at 44.1 kHz; the right channel is offset to decorrelate it.
constexpr int kCombLen[] = {1116, 1188, 1277, 1356};
constexpr int kAllpassLen[] = {556, 441};
constexpr int kStereoSpread = 23;
// calibrated so that wet = 1 with a full send gives a tail about as loud (in energy) as the dry sound
constexpr float kInputGain = 0.1f;
} // namespace

void Reverb::configure(int sampleRate, float size, float damping) {
    const double scale = double(sampleRate) / 44100.0;
    for (int ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < kCombs; ++i) comb_[ch][i] = Comb{std::vector<float>(size_t((kCombLen[i] + ch * kStereoSpread) * scale) + 1, 0.0f)};
        for (int i = 0; i < kAllpasses; ++i) allpass_[ch][i] = Allpass{std::vector<float>(size_t((kAllpassLen[i] + ch * kStereoSpread) * scale) + 1, 0.0f)};
    }
    feedback_ = 0.7f + 0.28f * std::clamp(size, 0.0f, 1.0f);
    damp_ = 0.4f * std::clamp(damping, 0.0f, 1.0f);
}

void Reverb::clear() {
    for (auto& side : comb_) for (auto& c : side) { std::fill(c.buf.begin(), c.buf.end(), 0.0f); c.store = 0; }
    for (auto& side : allpass_) for (auto& a : side) std::fill(a.buf.begin(), a.buf.end(), 0.0f);
}

void Reverb::process(const float* in, float* out, int n, float wet) {
    if (comb_[0][0].buf.empty()) return;
    for (int k = 0; k < n; ++k) {
        const float x = in[k] * kInputGain;
        for (int ch = 0; ch < 2; ++ch) {
            float acc = 0;
            for (auto& c : comb_[ch]) {
                const float y = c.buf[c.pos];
                c.store = y * (1.0f - damp_) + c.store * damp_;
                c.buf[c.pos] = x + c.store * feedback_;
                if (++c.pos == c.buf.size()) c.pos = 0;
                acc += y;
            }
            for (auto& a : allpass_[ch]) {
                const float b = a.buf[a.pos];
                a.buf[a.pos] = acc + b * 0.5f;
                acc = b - acc;
                if (++a.pos == a.buf.size()) a.pos = 0;
            }
            out[k * 2 + ch] += acc * wet;
        }
    }
}

} // namespace rts::audio
