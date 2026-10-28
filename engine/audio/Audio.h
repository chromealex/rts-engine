// Audio: sound effects, music and voice for every platform (CoreAudio on macOS/iOS, AAudio /
// OpenSL ES on Android, Web Audio, WASAPI, ALSA/PulseAudio — through miniaudio's device layer).
//
// Presentation only: audio never touches the simulation and may use float and its own RNG.
// Everything a game sounds like comes from data (`audio.json`, see docs in README):
//
//   {
//     "volume": { "music": 0.6, "sfx": 1.0, "ui": 0.8, "voice": 1.0, "ambient": 0.7 },
//     "sounds": {
//       "sword":  { "files": ["sounds/sword1.ogg", "sounds/sword2.ogg"], "volume": 0.8,
//                   "pitch": [0.92, 1.08], "maxInstances": 3, "cooldown": 0.06 },
//       "alarm":  { "file": "voice/under_attack.ogg", "bus": "voice", "spatial": false, "priority": 10 },
//       "blip":   { "synth": { "wave": "square", "freq": [880, 440], "duration": 0.08 }, "bus": "ui" }
//     },
//     "music": { "menu": "music/menu.ogg", "match": { "files": ["music/a.ogg", "music/b.ogg"], "shuffle": true } }
//   }
//
// Files: WAV, OGG Vorbis, MP3, FLAC. Read through a caller-provided reader (the client passes
// platform::readContentFile, so Android assets / web preload / packs work unchanged).
//
// Threading: all public methods are called from the main thread. Mixing runs on the device's audio
// thread (or the browser's audio callback); the two sides talk through a command queue.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rts {
class Json;
}

namespace rts::audio {

enum class Bus : uint8_t { Sfx, Ui, Voice, Music, Ambient, Count };
const char* busName(Bus b);
bool busFromName(std::string_view name, Bus& out);

using SoundId = int;
constexpr SoundId kNoSound = -1;
using VoiceId = uint32_t;          // 0 = none
constexpr VoiceId kNoVoice = 0;

// Decoded sample data, 16-bit interleaved, at its own rate (the mixer resamples).
struct Clip {
    std::vector<int16_t> pcm;
    int channels = 1;
    int sampleRate = 44100;
    size_t frames() const { return channels > 0 ? pcm.size() / size_t(channels) : 0; }
    float seconds() const { return sampleRate > 0 ? float(frames()) / float(sampleRate) : 0.0f; }
};

// How a positional sound fades with distance (world units, e.g. map cells).
enum class Rolloff : uint8_t { Inverse, Linear, Exponential };

struct DistanceDef {
    Rolloff model = Rolloff::Inverse;
    float minDistance = 12;   // full volume closer than this
    float maxDistance = 60;   // silent beyond this (the last 20 % of the range fades to zero)
    float rolloff = 1;        // steepness of the curve
    float air = 1;            // multiplier of the environment's air absorption (0 = keeps its highs)
    float reverb = 1;         // multiplier of the distance-dependent reverb send (0 = dry)
};

// "Distant sounds": the environment of a bank (its "spatial" section).
struct Environment {
    DistanceDef defaults;     // for sounds that don't override
    float airCutoff = 2500;   // Hz of the low-pass at max distance; 0 = no air absorption
    float rear = 0;           // 0..1: sources behind the listener get duller and a bit quieter
    float speedOfSound = 0;   // world units per second; > 0 delays distant sounds; 0 = off
    bool reverb = true;
    float reverbSize = 0.5f, reverbDamping = 0.5f;  // 0..1
    float reverbWet = 0.35f;                         // output level of the reverb
    float reverbNear = 0.05f, reverbFar = 0.6f;      // send at min / max distance
};

struct SoundDef {
    std::string name;
    std::vector<std::shared_ptr<const Clip>> clips;  // variations, one picked per play
    float volume = 1.0f;
    float pitchMin = 1.0f, pitchMax = 1.0f;
    Bus bus = Bus::Sfx;
    int maxInstances = 4;   // more plays steal the oldest instance of this sound
    float cooldown = 0.0f;  // seconds between two starts (stops 40 archers from firing 40 sounds)
    int priority = 0;       // when all voices are busy, lower priority is stolen first
    bool loop = false;
    bool spatial = true;    // attenuated and panned around the listener when played at a position
    DistanceDef distance;   // from the bank's "spatial" defaults plus the sound's own keys
    SoundId far = kNoSound; // distant variant crossfaded in towards max distance ("far")
    float farFrom = 0.3f;   // fraction of the distance range where the crossfade starts
};

struct MusicDef {
    std::string name;
    std::vector<std::string> files;
    bool shuffle = false;
    bool loop = true;       // loop the playlist
    float volume = 1.0f;
};

struct PlayParams {
    float volume = 1.0f;
    float pitch = 1.0f;     // multiplied with the sound's random pitch
    bool positional = false;
    float x = 0, y = 0, z = 0;  // world position (y = height)
};

// The ear: position and orientation in the world. `hearing` scales every sound's distances (an
// RTS raises it when the camera zooms out, so the whole visible battlefield stays audible).
struct ListenerPose {
    float pos[3] = {0, 0, 0};
    float forward[3] = {0, 0, -1};
    float up[3] = {0, 1, 0};
    float hearing = 1;
};

struct AudioConfig {
    bool openDevice = true;  // false: silent engine driven by render() (tests, headless)
    int sampleRate = 0;      // 0 = device native (usually 48000)
    int maxVoices = 32;
    int periodMs = 0;        // 0 = backend default (~10 ms; larger on web)
    bool reverb = true;      // distance reverb (switch off on the weakest devices)
    bool nullBackend = false; // real audio thread without hardware (CI): miniaudio's null device
    std::string capturePath;  // debug: record everything the device plays into this WAV on shutdown
};

// Reads a content file (relative path) into `out`.
using FileReader = std::function<bool(const std::string& path, std::string& out)>;

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Never fails hard: without an audio device the engine runs silently.
    void init(const AudioConfig& cfg = {});
    void shutdown();
    bool deviceOpen() const;
    const std::string& deviceError() const;
    int sampleRate() const;

    // ---- data
    // Loads `audio.json` (replacing the previous bank). Returns false with a readable message on
    // malformed data; missing or undecodable files are only warnings (that sound stays silent).
    bool loadBank(const Json& bank, const FileReader& read, std::string* error = nullptr);
    void clearBank();
    SoundId find(std::string_view name) const;
    const SoundDef* sound(SoundId id) const;
    size_t soundCount() const;
    const std::vector<std::string>& warnings() const;
    bool hasMusic(std::string_view name) const;

    // ---- playback
    VoiceId play(SoundId id, const PlayParams& p = {});
    VoiceId play(std::string_view name, const PlayParams& p = {}) { return play(find(name), p); }
    void stop(VoiceId v, float fadeSeconds = 0.05f);
    void stopAll(float fadeSeconds = 0.1f);
    void setVoicePosition(VoiceId v, float x, float z) { setVoicePosition(v, x, 0, z); }
    void setVoicePosition(VoiceId v, float x, float y, float z);

    // Listener for positional sounds (3D).
    void setListener(const ListenerPose& pose);
    // Shortcut for a top-down view: an ear on the ground at (x, z) turned by `yaw`; `radius` is
    // where sounds with the default distances start to fade (hearing = radius / minDistance).
    void setListener(float x, float z, float yaw, float radius);
    const ListenerPose& listener() const;
    const Environment& environment() const;
    // 0..1: how loud a sound would be at that position (`id`: with its own distances, including
    // its far variant). Used to skip inaudible sounds.
    float audibility(float x, float z) const { return audibility(kNoSound, x, 0, z); }
    float audibility(SoundId id, float x, float y, float z) const;

    // ---- music (named playlists from the bank's "music" section, crossfaded)
    bool playMusic(std::string_view name, float fadeSeconds = 1.5f);
    void stopMusic(float fadeSeconds = 1.5f);
    const std::string& currentMusic() const;

    // ---- volumes: user settings on top of the bank's mix (0..1)
    void setMasterVolume(float v);
    float masterVolume() const;
    void setVolume(Bus b, float v);
    float volume(Bus b) const;
    void setMuted(bool m);
    bool muted() const;
    // Background / focus loss: mobile apps must go quiet (and release the device on iOS/Android).
    void setPaused(bool p);
    bool paused() const;
    std::string saveSettings() const;            // small text blob, store in userDataDir
    void loadSettings(const std::string& text);

    // Once per frame on the main thread (time for cooldowns, settings, finished music).
    void update(double dt);

    // ---- offline / tests: mix `frames` of interleaved stereo float. Only valid without a device.
    void render(float* out, int frames);
    int activeVoices() const;   // snapshot from the mixer (approximate while a device runs)

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

// Decodes WAV/OGG/MP3/FLAC bytes into a Clip. Returns false with a reason.
bool decodeClip(const std::string& bytes, Clip& out, std::string& error);

} // namespace rts::audio
