// Build-time audio compression (host tools only: rtsbuild, rts_audiopack, tests).
//
// The game's audio.json chooses codecs per build profile ("build" section); rtsbuild stages a copy
// of the game with the referenced sound and music files re-encoded and audio.json pointing at
// the new files, then packs that copy. Games never link this: they only decode.
//
//   "build": {
//     "desktop": { "sounds": "keep", "music": "keep" },
//     "mobile":  { "sounds": "adpcm", "music": "vorbis", "musicQuality": 0.3, "maxRate": 32000 },
//     "web":     { "sounds": "adpcm", "music": "vorbis", "musicQuality": 0.25, "maxRate": 32000 }
//   }
//
// Codecs: keep (file as is), pcm (16-bit WAV), adpcm (IMA ADPCM WAV, 4:1, nearly free to decode:
// short effects), vorbis (OGG, quality -0.1..1: music, long sounds). A sound can override with
// "compress" / "quality". Files that are already lossy (OGG, MP3) are kept unless the profile
// sets "reencodeLossy": true — transcoding lossy to lossy only loses quality.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rts {
class Json;
}

namespace rts::audiopack {

enum class Codec { Keep, Pcm, Adpcm, Vorbis };
const char* codecName(Codec c);
bool codecFromName(const std::string& s, Codec& out);

struct Settings {
    Codec sounds = Codec::Keep, music = Codec::Keep;
    float soundQuality = 0.5f, musicQuality = 0.4f;  // vorbis quality
    int maxRate = 0;               // resample above this rate (0 = never)
    bool monoSounds = true;        // positional sounds are panned by the mixer: their stereo is wasted
    float longSound = 8.0f;        // seconds: longer sounds use the music codec (ambient loops, stingers)
    bool reencodeLossy = false;
};

// Built-in defaults per profile, then the bank's "build" -> "<profile>" overrides.
// Profiles: desktop, mobile, web (any other name starts from desktop).
bool settingsFor(const Json& bank, const std::string& profile, Settings& out, std::string* error);
std::string profileForTarget(const std::string& target);   // ios/android -> mobile, web -> web, else desktop

struct Report {
    struct Item { std::string from, to, codec; uint64_t before = 0, after = 0; };
    std::vector<Item> items;
    std::vector<std::string> warnings;
    uint64_t before() const;
    uint64_t after() const;
    std::string text() const;      // human-readable table (Russian, like rtsbuild's other output)
};

using FileReader = std::function<bool(const std::string& path, std::string& out)>;

struct Output {
    std::map<std::string, std::string> files;   // new path -> contents
    std::vector<std::string> replaced;           // original paths no longer referenced
    std::string audioJson;                       // rewritten bank (comments are not kept)
    Report report;
};

// Re-encodes every file the bank references. `seed` keeps OGG streams reproducible.
bool convert(const Json& bank, const FileReader& read, const Settings& s, Output& out, std::string* error);

// Copies `projectDir` to `stagingDir` with the audio converted for `profile`. A game without
// audio.json, or a profile that keeps everything, is copied unchanged.
bool stageGame(const std::string& projectDir, const std::string& stagingDir, const std::string& profile, Report& report, std::string* error);

// ---- encoders (interleaved 16-bit PCM in)
std::string encodeWav(const std::vector<int16_t>& pcm, int channels, int rate);
std::string encodeAdpcm(const std::vector<int16_t>& pcm, int channels, int rate, bool loop = false);
bool encodeVorbis(const std::vector<int16_t>& pcm, int channels, int rate, float quality, uint32_t serial, std::string& out, std::string* error);

} // namespace rts::audiopack
