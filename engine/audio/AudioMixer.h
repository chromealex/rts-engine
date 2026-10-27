// Audio-thread side of the engine: voices, music streams, buses, spatialization.
// The main thread only pushes MixCmds; render() applies them and mixes.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "Audio.h"

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
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::shared_ptr<const std::string> bytes_;
};

struct Playlist {
    std::string name;
    std::vector<std::shared_ptr<const std::string>> tracks;
    bool shuffle = false, loop = true;
    float volume = 1.0f;
};

struct Listener { float x = 0, z = 0, yaw = 0, radius = 20; };

// Pure function shared by both threads: gain 0..1 and pan -1..1 of a point for a listener.
void spatialize(const Listener& l, float x, float z, float& gain, float& pan);

struct MixCmd {
    enum Kind : uint8_t { Play, Stop, StopAll, SetPos, SetListener, SetGains, Music, StopMusic };
    Kind kind = Play;
    VoiceId id = kNoVoice;
    SoundId sound = kNoSound;
    std::shared_ptr<const Clip> clip;
    std::shared_ptr<const Playlist> playlist;
    float volume = 1, pitch = 1, x = 0, z = 0, fade = 0;
    Bus bus = Bus::Sfx;
    int priority = 0, maxInstances = 0;
    bool loop = false, spatial = false;
    Listener listener;
    float gains[size_t(Bus::Count)] = {};
    float master = 1;
};

class Mixer {
public:
    void configure(int sampleRate, int maxVoices);
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
        float volume = 1, x = 0, z = 0;
        float gl = 0, gr = 0;          // gains applied at the end of the last block
        float fade = 1, fadeStep = 0;  // per frame
        bool fresh = true;             // first block starts at its target gains (no ramp from zero)
    };
    struct Stream {
        StreamDecoder dec;
        std::shared_ptr<const Playlist> list;
        std::vector<size_t> order;
        size_t index = 0;
        float gain = 0, target = 0, step = 0; // per frame
        bool active = false;
    };

    void apply(MixCmd& c);
    void startVoice(MixCmd& c);
    Voice* findVoice(VoiceId id);
    bool openTrack(Stream& s);
    void mixVoices(float* buf, int n);
    void mixStream(Stream& s, float* buf, int n);
    void limit(float* buf, int n);
    uint32_t nextRandom();

    std::mutex mutex_;
    std::vector<MixCmd> pending_, work_;
    int rate_ = 48000;
    std::vector<Voice> voices_;
    int maxVoices_ = 32;
    uint64_t serial_ = 0;
    Listener listener_;
    float gains_[size_t(Bus::Count)] = {1, 1, 1, 1, 1};
    float master_ = 1, masterCur_ = 1;
    float limEnv_ = 0, limGain_ = 1;   // master limiter: peak envelope and applied gain
    std::unique_ptr<Stream> music_[2];   // [0] current, [1] fading out
    std::vector<float> tmp_;
    uint32_t rng_ = 0x2545F491u;
    std::atomic<int> active_{0};
};

} // namespace rts::audio
