// Small stereo reverb (Freeverb topology: parallel damped combs + series allpasses) for the
// "distance" send: the farther a sound, the more of it goes here, so far sounds get roomy and
// lose their attack. Mono in, stereo out; buffers sized once in configure() (no allocations in
// the audio callback).
#pragma once
#include <vector>

namespace rts::audio {

class Reverb {
public:
    void configure(int sampleRate, float size, float damping);
    // Adds `wet` x reverb of `in` (mono, n frames) into `outStereo` (interleaved).
    void process(const float* in, float* outStereo, int n, float wet);
    void clear();

private:
    struct Comb {
        std::vector<float> buf;
        size_t pos = 0;
        float store = 0;
    };
    struct Allpass {
        std::vector<float> buf;
        size_t pos = 0;
    };
    static constexpr int kCombs = 4, kAllpasses = 2;
    Comb comb_[2][kCombs];
    Allpass allpass_[2][kAllpasses];
    float feedback_ = 0.84f, damp_ = 0.2f;
};

} // namespace rts::audio
