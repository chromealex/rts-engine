// Procedural sound effects: a game can describe sounds as parameters instead of shipping files
// (prototypes, playables, tiny web builds). Generated once at bank load into an ordinary Clip.
//
//   "synth": { "wave": "square", "freq": [880, 220], "duration": 0.18, "attack": 0.005,
//              "release": 0.12, "volume": 0.6, "duty": 0.3, "vibrato": [12, 0.03],
//              "lowpass": 3000, "crush": 0, "delay": 0 }
//   "synth": [ {layer}, {layer}, ... ]   // layers are mixed; "delay" offsets a layer in seconds
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rts {
class Json;
}

namespace rts::audio {

enum class Wave : uint8_t { Sine, Square, Saw, Triangle, Noise };

struct SynthLayer {
    Wave wave = Wave::Sine;
    float freqStart = 440, freqEnd = 440;   // Hz, exponential sweep
    float duration = 0.2f;                  // seconds (without delay)
    float attack = 0.005f, release = 0.1f;  // linear attack, exponential-ish release at the end
    float volume = 1.0f;
    float duty = 0.5f;                      // square pulse width
    float vibratoRate = 0, vibratoDepth = 0; // Hz, fraction of frequency
    float lowpass = 0;                      // one-pole cutoff in Hz, 0 = off
    int crush = 0;                          // keep only this many bits (1..15), 0 = off
    float delay = 0;                        // seconds before the layer starts
};

// Parses one layer object or an array of layers. Unknown wave names produce a readable error.
bool parseSynth(const Json& j, std::vector<SynthLayer>& out, std::string& error);

// Renders the layers into mono 16-bit PCM. `seed` makes noise reproducible per sound name.
std::vector<int16_t> renderSynth(const std::vector<SynthLayer>& layers, int sampleRate, uint32_t seed);

} // namespace rts::audio
