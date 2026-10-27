#include "Synth.h"
#include <algorithm>
#include <cmath>
#include "../data/Json.h"

namespace rts::audio {

namespace {

constexpr float kTwoPi = 6.28318530718f;

float num(const Json& j, float def) { return j.isNumber() ? float(j.fx().toDouble()) : def; }

bool parseLayer(const Json& j, SynthLayer& l, std::string& error) {
    if (!j.isObject()) { error = "synth layer must be an object"; return false; }
    std::string w = j.getStr("wave", "sine");
    if (w == "sine") l.wave = Wave::Sine;
    else if (w == "square") l.wave = Wave::Square;
    else if (w == "saw") l.wave = Wave::Saw;
    else if (w == "triangle") l.wave = Wave::Triangle;
    else if (w == "noise") l.wave = Wave::Noise;
    else { error = "unknown synth wave '" + w + "' (sine, square, saw, triangle, noise)"; return false; }
    const Json& f = j["freq"];
    if (f.isArray() && f.size() >= 2) { l.freqStart = num(f[0], 440); l.freqEnd = num(f[1], l.freqStart); }
    else { l.freqStart = l.freqEnd = num(f, 440); }
    l.duration = std::clamp(num(j["duration"], 0.2f), 0.001f, 10.0f);
    l.attack = std::max(0.0f, num(j["attack"], 0.005f));
    l.release = std::max(0.0f, num(j["release"], std::min(0.1f, l.duration)));
    l.volume = std::clamp(num(j["volume"], 1.0f), 0.0f, 4.0f);
    l.duty = std::clamp(num(j["duty"], 0.5f), 0.02f, 0.98f);
    const Json& v = j["vibrato"];
    if (v.isArray() && v.size() >= 2) { l.vibratoRate = num(v[0], 0); l.vibratoDepth = num(v[1], 0); }
    l.lowpass = std::max(0.0f, num(j["lowpass"], 0));
    l.crush = int(std::clamp<int64_t>(j.getInt("crush", 0), 0, 15));
    l.delay = std::clamp(num(j["delay"], 0), 0.0f, 10.0f);
    if (l.freqStart <= 0 || l.freqEnd <= 0) { error = "synth freq must be > 0"; return false; }
    return true;
}

} // namespace

bool parseSynth(const Json& j, std::vector<SynthLayer>& out, std::string& error) {
    out.clear();
    if (j.isArray()) {
        for (auto& item : j.items()) {
            SynthLayer l;
            if (!parseLayer(item, l, error)) return false;
            out.push_back(l);
        }
    } else {
        SynthLayer l;
        if (!parseLayer(j, l, error)) return false;
        out.push_back(l);
    }
    if (out.empty()) { error = "synth has no layers"; return false; }
    return true;
}

std::vector<int16_t> renderSynth(const std::vector<SynthLayer>& layers, int sampleRate, uint32_t seed) {
    float total = 0;
    for (auto& l : layers) total = std::max(total, l.delay + l.duration);
    const size_t n = size_t(std::ceil(total * float(sampleRate)));
    std::vector<float> mix(n, 0.0f);
    const float sr = float(sampleRate);

    uint32_t rng = seed ? seed : 0x9E3779B9u;
    for (auto& l : layers) {
        const size_t start = size_t(l.delay * sr);
        const size_t len = std::min(n - std::min(n, start), size_t(l.duration * sr));
        const float ratio = l.freqEnd / l.freqStart;
        const float lp = l.lowpass > 0 ? 1.0f - std::exp(-kTwoPi * l.lowpass / sr) : 1.0f;
        float phase = 0, filt = 0, noiseHold = 0, noisePhase = 1;
        for (size_t i = 0; i < len; ++i) {
            const float t = float(i) / sr;
            const float u = len > 1 ? float(i) / float(len - 1) : 0;
            float freq = l.freqStart * std::pow(ratio, u);
            if (l.vibratoRate > 0) freq *= 1.0f + l.vibratoDepth * std::sin(kTwoPi * l.vibratoRate * t);
            phase += freq / sr;
            phase -= std::floor(phase);
            float s = 0;
            switch (l.wave) {
            case Wave::Sine: s = std::sin(kTwoPi * phase); break;
            case Wave::Square: s = phase < l.duty ? 1.0f : -1.0f; break;
            case Wave::Saw: s = 2.0f * phase - 1.0f; break;
            case Wave::Triangle: s = 4.0f * std::fabs(phase - 0.5f) - 1.0f; break;
            case Wave::Noise:
                // sample-and-hold noise: `freq` sets the grain rate, so sweeps work on noise too
                noisePhase += freq / sr;
                if (noisePhase >= 1.0f) {
                    noisePhase -= std::floor(noisePhase);
                    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                    noiseHold = float(rng >> 8) / float(1u << 23) - 1.0f;
                }
                s = noiseHold;
                break;
            }
            filt += lp * (s - filt);
            s = filt;
            float env = 1.0f;
            if (l.attack > 0 && t < l.attack) env = t / l.attack;
            const float tail = l.duration - t;
            if (l.release > 0 && tail < l.release) env *= std::pow(std::max(0.0f, tail / l.release), 1.6f);
            mix[start + i] += s * env * l.volume;
        }
    }

    std::vector<int16_t> pcm(n);
    for (size_t i = 0; i < n; ++i) {
        float v = std::clamp(mix[i], -1.0f, 1.0f);
        pcm[i] = int16_t(std::lrint(v * 32767.0f));
    }
    // "crush": N keeps N bits. Applied to the mix (the lowest request wins): for short retro
    // effects this is indistinguishable from per-layer crushing and keeps the inner loop simple.
    int bits = 16;
    for (auto& l : layers) if (l.crush > 0) bits = std::min(bits, l.crush);
    if (bits < 16) {
        const int step = 1 << (16 - bits);
        for (auto& s : pcm) s = int16_t((int(s) + 32768) / step * step - 32768);
    }
    return pcm;
}

} // namespace rts::audio
