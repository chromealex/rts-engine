// Audio-thread side of the engine: voices, music streams, buses, 3D spatialization, distance
// effects (air absorption, reverb send, near/far crossfade). The main thread only pushes MixCmds;
// render() applies them and mixes.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "Audio.h"
#include "AudioReverb.h"

namespace rts::audio {

// Streaming decoder for music (implemented on top of miniaudio in AudioDecode.cpp).
class StreamDecoder {
public:
    StreamDecoder();
    ~StreamDecoder();
    StreamDecoder(const StreamDecoder&) = delete;
    StreamDecoder& operator=(const StreamDecoder&) = delete;
    // Decodes to interleaved stereo float at `sampleRate`. `bytes` must outlive the decoder.
    bool open(std::shared_ptr<const std::string> bytes, int sampleRate);
    void close();
    bool isOpen() const;
    int read(float* stereo, int frames);   // returns frames produced; < frames at end of track
    bool seek(int64_t frame);              // in output frames
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::shared_ptr<const std::string> bytes_;
};

struct MusicTrack {
    std::shared_ptr<const std::string> bytes;
    float loopStart = -1;   // seconds; >= 0: after the end (or loopEnd) play on from here, forever
    float loopEnd = 0;      // seconds; 0 = end of file
    float intensity = 0;    // layers: fades in as the music intensity passes this value
};

enum class Quantize : uint8_t { Now, Beat, Bar };

struct Playlist {
    std::string name;
    std::vector<MusicTrack> tracks;   // played in order (or shuffled)
    std::vector<MusicTrack> layers;   // adaptive: synchronized stems mixed by intensity (instead of tracks)
    float layerLoopStart = 0, layerLoopEnd = 0;  // the layer set loops between these (seconds)
    bool shuffle = false, loop = true;
    float volume = 1.0f;
    float bpm = 0;                    // > 0: beat grid for transitions and synced stingers
    int beatsPerBar = 4;
    float offset = 0;                 // seconds before the first beat
    Quantize transition = Quantize::Now;  // switching away from this playlist waits for the next beat / bar
};

// Result of placing a source relative to the listener. Pure function shared by both threads.
struct Spatial {
    float gain = 1;      // distance (and rear) attenuation, 0..1
    float pan = 0;       // -1 left .. 1 right
    float t = 0;         // 0 at min distance .. 1 at max distance
    float cutoff = 0;    // low-pass Hz (air absorption, rear); 0 = open
    float send = 0;      // reverb send 0..1
    float distance = 0;  // world units
};
Spatial spatialize(const ListenerPose& l, const Environment& env, const DistanceDef& d, float x, float y, float z);
float distanceGain(const DistanceDef& d, float distance, float hearing);
float farWeight(float t, float from);   // 0 = all near variant, 1 = all far variant

struct MixCmd {
    enum Kind : uint8_t { Play, Stop, StopAll, SetPos, SetListener, SetEnv, SetGains, Music, StopMusic, SetIntensity, SetGroups };
    enum Role : uint8_t { Single, Near, Far };   // near/far: the two halves of a "far" pair
    Kind kind = Play;
    VoiceId id = kNoVoice;
    SoundId sound = kNoSound;
    std::shared_ptr<const Clip> clip;
    std::shared_ptr<const Playlist> playlist;
    float volume = 1, pitch = 1, x = 0, y = 0, z = 0, fade = 0;
    Bus bus = Bus::Sfx;
    int priority = 0, maxInstances = 0;
    Steal steal = Steal::Farthest;
    int8_t groups[kMaxSoundGroups] = {-1, -1, -1, -1};
    std::shared_ptr<const std::vector<SoundGroupDef>> groupDefs;   // SetGroups
    bool loop = false, spatial = false;
    DistanceDef distance;
    Role role = Single;
    DistanceDef pairDistance;   // distances of the near sound: they drive the crossfade of both
    float pairFrom = 0.3f;
    uint32_t delayFrames = 0;   // speed of sound
    float duck = 0;             // music is lowered by this fraction while the voice plays
    Quantize sync = Quantize::Now;  // stingers: start on the music's next beat / bar
    float intensity = 0;
    ListenerPose listener;
    Environment env;
    float gains[size_t(Bus::Count)] = {};
    float master = 1;
};

class Mixer {
public:
    void configure(int sampleRate, int maxVoices, bool reverb);
    void push(MixCmd&& c);                    // main thread
    void render(float* out, int frames);      // audio thread
    int activeVoices() const { return active_.load(std::memory_order_relaxed); }
    int sampleRate() const { return rate_; }

private:
    struct Voice {
        bool active = false, stopping = false, loop = false, spatial = false;
        VoiceId id = kNoVoice;
        SoundId sound = kNoSound;
        Bus bus = Bus::Sfx;
        int priority = 0;
        uint64_t serial = 0;
        std::shared_ptr<const Clip> clip;
        double pos = 0, step = 1;
        float volume = 1, x = 0, y = 0, z = 0;
        DistanceDef distance, pairDistance;
        MixCmd::Role role = MixCmd::Single;
        float pairFrom = 0.3f;
        uint32_t delay = 0;            // frames of silence before the sound starts
        int8_t groups[kMaxSoundGroups] = {-1, -1, -1, -1};
        float duck = 0;
        float gl = 0, gr = 0;          // gains applied at the end of the last block
        float lp = 1, send = 0;        // low-pass coefficient and reverb send at the end of the last block
        float f1l = 0, f2l = 0, f1r = 0, f2r = 0;  // two-pole low-pass state per channel
        float fade = 1, fadeStep = 0;  // per frame
        bool fresh = true;             // first block starts at its targets (no ramp from zero)
    };
    struct Layer {
        StreamDecoder dec;
        float threshold = 0, gain = 1;
    };
    struct Stream {
        std::shared_ptr<const Playlist> list;
        std::vector<size_t> order;
        size_t index = 0;
        std::vector<std::unique_ptr<Layer>> layers;   // the playing track: one layer, or the playlist's stems
        int64_t pos = 0;                  // frames since the track started (loops jump back)
        int64_t loopStart = -1, loopEnd = 0;
        float gain = 0, target = 0, step = 0; // per frame
        bool active = false;
    };

    void apply(MixCmd& c);
    void startVoice(MixCmd& c);
    // Makes room among the playing voices that `match` (a limit of `max`), or returns false: the
    // new sound must be dropped. Counts logical sounds: the far half of a near/far pair is ignored
    // and stopped together with its near half.
    template <class Match> bool makeRoom(const MixCmd& c, int max, Steal steal, Match match);
    float distanceTo(float x, float y, float z) const;
    void stopVoices(VoiceId id, float fadeStep);
    bool openTrack(Stream& s);
    bool openLayers(Stream& s, const std::vector<MusicTrack>& defs, float loopStart, float loopEnd);
    bool loopBack(Stream& s);
    int readTrack(Stream& s, float* out, int n);
    void startMusic(MixCmd& c);
    int64_t framesToNext(const Stream& s, Quantize q) const;
    void mixVoices(float* buf, float* send, int n);
    void mixStream(Stream& s, float* buf, int n);
    void limit(float* buf, int n);
    float lowpassCoef(float cutoff) const;
    uint32_t nextRandom();

    std::mutex mutex_;
    std::vector<MixCmd> pending_, work_;
    int rate_ = 48000;
    std::vector<Voice> voices_;
    int maxVoices_ = 32;
    uint64_t serial_ = 0;
    ListenerPose listener_;
    Environment env_;
    bool reverbAllowed_ = true;
    Reverb reverb_;
    float gains_[size_t(Bus::Count)] = {1, 1, 1, 1, 1};
    float master_ = 1, masterCur_ = 1;
    float limEnv_ = 0, limGain_ = 1;   // master limiter: peak envelope and applied gain
    std::unique_ptr<Stream> music_[2];   // [0] current, [1] fading out
    std::vector<float> tmp_, send_, track_;
    std::unique_ptr<MixCmd> pendingMusic_;   // a playlist switch waiting for the beat / bar
    int64_t pendingDelay_ = 0;
    float intensity_ = 0, intensityTarget_ = 0;
    float duck_ = 0;                          // current music ducking (0 = none)
    uint32_t rng_ = 0x2545F491u;
    std::atomic<int> active_{0};
    std::shared_ptr<const std::vector<SoundGroupDef>> groupDefs_;
};

} // namespace rts::audio
