// Main-thread side: bank loading, cooldowns, volumes, the audio device.
#include "MiniaudioConfig.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include "../data/Json.h"
#include "AudioMixer.h"
#include "Synth.h"

namespace rts::audio {

namespace {

constexpr const char* kBusNames[] = {"sfx", "ui", "voice", "music", "ambient"};
constexpr int kSynthRate = 44100;

float num(const Json& j, float def) { return j.isNumber() ? float(j.fx().toDouble()) : def; }

uint32_t nameSeed(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

bool writeWav(const std::string& path, const std::vector<float>& stereo, int rate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto u32 = [&](uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, f); };
    auto u16 = [&](uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f); };
    const uint32_t bytes = uint32_t(stereo.size() * 2);
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(uint32_t(rate)); u32(uint32_t(rate) * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (float x : stereo) u16(uint16_t(int16_t(std::lrint(std::clamp(x, -1.0f, 1.0f) * 32767.0f))));
    return std::fclose(f) == 0;
}

} // namespace

const char* busName(Bus b) { return size_t(b) < size_t(Bus::Count) ? kBusNames[size_t(b)] : "?"; }

bool busFromName(std::string_view name, Bus& out) {
    for (size_t i = 0; i < size_t(Bus::Count); ++i)
        if (name == kBusNames[i]) { out = Bus(i); return true; }
    return false;
}

struct AudioEngine::Impl {
    AudioConfig cfg;
    Mixer mixer;
    std::vector<float> capture;   // audio thread only, written out after the device stops
    ma_context ctx{};
    ma_device dev{};
    bool ctxOk = false, devOk = false, running = false, initialized = false;
    std::string devError;

    std::vector<SoundDef> sounds;
    std::map<std::string, SoundId, std::less<>> byName;
    std::map<std::string, std::shared_ptr<const Playlist>, std::less<>> music;
    std::vector<std::string> warnings;
    std::vector<double> lastPlay;
    std::vector<int> lastVariant;
    std::string currentMusic, pendingMusic;
    float pendingFade = 0;

    float bankGain[size_t(Bus::Count)] = {1, 1, 1, 1, 1};
    float bankMaster = 1;
    float user[size_t(Bus::Count)] = {1, 1, 1, 1, 1};
    float userMaster = 1;
    bool muted = false, paused = false;
    Listener listener;
    double time = 0;
    VoiceId nextId = 1;
    uint32_t rng = 0x6C8E9CF5u;

    // Commands reach the mixer only while something renders it; otherwise they would pile up.
    bool mixing() const { return initialized && (devOk ? !paused : !cfg.openDevice); }

    float frand() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return float(rng >> 8) / float(1u << 24);
    }

    void sendGains() {
        if (!mixing()) return;
        MixCmd c;
        c.kind = MixCmd::SetGains;
        for (size_t i = 0; i < size_t(Bus::Count); ++i) c.gains[i] = bankGain[i] * user[i];
        c.master = muted ? 0.0f : bankMaster * userMaster;
        mixer.push(std::move(c));
    }
    void sendListener() {
        if (!mixing()) return;
        MixCmd c;
        c.kind = MixCmd::SetListener;
        c.listener = listener;
        mixer.push(std::move(c));
    }
    void startMusic(const std::string& name, float fade) {
        auto it = music.find(name);
        if (it == music.end() || !mixing()) return;
        MixCmd c;
        c.kind = MixCmd::Music;
        c.playlist = it->second;
        c.fade = fade;
        mixer.push(std::move(c));
    }
};

static void deviceCallback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
    auto& m = *static_cast<AudioEngine::Impl*>(dev->pUserData);
    float* buf = static_cast<float*>(out);
    m.mixer.render(buf, int(frames));
    if (!m.cfg.capturePath.empty() && m.capture.size() < size_t(m.mixer.sampleRate()) * 2 * 600) // 10 minutes max
        m.capture.insert(m.capture.end(), buf, buf + size_t(frames) * 2);
}

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() { shutdown(); }

void AudioEngine::init(const AudioConfig& cfg) {
    shutdown();
    Impl& m = *impl_;
    m.cfg = cfg;
    m.devError.clear();
    int rate = cfg.sampleRate > 0 ? cfg.sampleRate : 48000;
    if (cfg.openDevice) {
        ma_context_config cc = ma_context_config_init();
        // iOS: respect the silent switch and let the player's own music keep playing
        cc.coreaudio.sessionCategory = ma_ios_session_category_ambient;
        cc.coreaudio.sessionCategoryOptions = ma_ios_session_category_option_mix_with_others;
        const ma_backend nullBackend[] = {ma_backend_null};
        if (ma_context_init(cfg.nullBackend ? nullBackend : nullptr, cfg.nullBackend ? 1 : 0, &cc, &m.ctx) == MA_SUCCESS) {
            m.ctxOk = true;
            ma_device_config dc = ma_device_config_init(ma_device_type_playback);
            dc.playback.format = ma_format_f32;
            dc.playback.channels = 2;
            dc.sampleRate = ma_uint32(std::max(0, cfg.sampleRate));
            dc.periodSizeInMilliseconds = ma_uint32(std::max(0, cfg.periodMs));
            dc.dataCallback = deviceCallback;
            dc.pUserData = &m;
            dc.noClip = MA_TRUE;  // the mixer soft-clips
            if (ma_device_init(&m.ctx, &dc, &m.dev) == MA_SUCCESS) {
                m.devOk = true;
                rate = int(m.dev.sampleRate);
            } else {
                m.devError = "no audio output device";
            }
        } else {
            m.devError = "audio backend unavailable";
        }
    }
    m.mixer.configure(rate, cfg.maxVoices);
    m.initialized = true;
    if (m.devOk) {
        if (ma_device_start(&m.dev) == MA_SUCCESS) m.running = true;
        else m.devError = "cannot start audio device";
    }
    if (!m.devError.empty()) std::fprintf(stderr, "audio: %s, running silently\n", m.devError.c_str());
    m.sendGains();
    m.sendListener();
}

void AudioEngine::shutdown() {
    Impl& m = *impl_;
    if (m.devOk) ma_device_uninit(&m.dev);   // stops the callback before the mixer goes away
    if (m.ctxOk) ma_context_uninit(&m.ctx);
    if (!m.cfg.capturePath.empty() && !m.capture.empty()) {
        if (writeWav(m.cfg.capturePath, m.capture, m.mixer.sampleRate()))
            std::fprintf(stderr, "audio: captured %.1f s to %s\n", double(m.capture.size() / 2) / m.mixer.sampleRate(), m.cfg.capturePath.c_str());
        m.capture.clear();
    }
    m.devOk = m.ctxOk = m.running = false;
    m.initialized = false;
    m.currentMusic.clear();
}

bool AudioEngine::deviceOpen() const { return impl_->devOk; }
const std::string& AudioEngine::deviceError() const { return impl_->devError; }
int AudioEngine::sampleRate() const { return impl_->mixer.sampleRate(); }

// ------------------------------------------------------------------ bank

void AudioEngine::clearBank() {
    Impl& m = *impl_;
    m.sounds.clear();
    m.byName.clear();
    m.music.clear();
    m.warnings.clear();
    m.lastPlay.clear();
    m.lastVariant.clear();
    std::fill(std::begin(m.bankGain), std::end(m.bankGain), 1.0f);
    m.bankMaster = 1;
    m.sendGains();
}

bool AudioEngine::loadBank(const Json& bank, const FileReader& read, std::string* error) {
    auto fail = [&](const std::string& msg) { if (error) *error = "audio: " + msg; return false; };
    if (!bank.isObject()) return fail("bank must be a JSON object");

    std::vector<SoundDef> sounds;
    std::map<std::string, SoundId, std::less<>> byName;
    std::map<std::string, std::shared_ptr<const Playlist>, std::less<>> music;
    std::vector<std::string> warnings;
    float gains[size_t(Bus::Count)] = {1, 1, 1, 1, 1};
    float master = 1;

    for (auto& [key, v] : bank["volume"].members()) {
        Bus b;
        if (key == "master") master = std::clamp(num(v, 1), 0.0f, 4.0f);
        else if (busFromName(key, b)) gains[size_t(b)] = std::clamp(num(v, 1), 0.0f, 4.0f);
        else return fail("volume: unknown bus '" + key + "' (master, sfx, ui, voice, music, ambient)");
    }

    std::map<std::string, std::shared_ptr<const Clip>> cache; // a file used by several sounds decodes once
    auto loadClip = [&](const std::string& who, const std::string& path) -> std::shared_ptr<const Clip> {
        if (auto it = cache.find(path); it != cache.end()) return it->second;
        std::string bytes, why;
        std::shared_ptr<const Clip> res;
        if (!read || !read(path, bytes)) warnings.push_back(who + ": file not found '" + path + "'");
        else {
            auto clip = std::make_shared<Clip>();
            if (decodeClip(bytes, *clip, why)) res = clip;
            else warnings.push_back(who + ": cannot decode '" + path + "': " + why);
        }
        cache[path] = res;
        return res;
    };

    static const std::set<std::string> kSoundKeys = {"file", "files", "synth", "volume", "pitch", "bus", "maxInstances",
                                                      "cooldown", "priority", "loop", "spatial"};
    for (auto& [name, j] : bank["sounds"].members()) {
        const std::string who = "sound '" + name + "'";
        if (!j.isObject()) return fail(who + ": must be an object");
        for (auto& [k, unused] : j.members())
            if (!kSoundKeys.count(k)) warnings.push_back(who + ": unknown key '" + k + "'");
        SoundDef d;
        d.name = name;
        std::string b = j.getStr("bus", "sfx");
        if (!busFromName(b, d.bus)) return fail(who + ": unknown bus '" + b + "' (sfx, ui, voice, music, ambient)");
        d.volume = std::clamp(num(j["volume"], 1), 0.0f, 4.0f);
        const Json& p = j["pitch"];
        if (p.isArray() && p.size() >= 2) { d.pitchMin = num(p[0], 1); d.pitchMax = num(p[1], d.pitchMin); }
        else d.pitchMin = d.pitchMax = num(p, 1);
        if (d.pitchMin <= 0 || d.pitchMax < d.pitchMin) return fail(who + ": pitch must be > 0 and [min, max]");
        d.maxInstances = int(std::clamp<int64_t>(j.getInt("maxInstances", 4), 0, 64));
        d.cooldown = std::max(0.0f, num(j["cooldown"], 0));
        d.priority = int(std::clamp<int64_t>(j.getInt("priority", 0), -100, 100));
        d.loop = j.getBool("loop", false);
        d.spatial = j.getBool("spatial", d.bus == Bus::Sfx || d.bus == Bus::Ambient);

        int sources = (j.has("file") ? 1 : 0) + (j.has("files") ? 1 : 0) + (j.has("synth") ? 1 : 0);
        if (sources != 1) return fail(who + ": needs exactly one of \"file\", \"files\", \"synth\"");
        if (j.has("synth")) {
            std::vector<SynthLayer> layers;
            std::string why;
            if (!parseSynth(j["synth"], layers, why)) return fail(who + ": " + why);
            auto clip = std::make_shared<Clip>();
            clip->channels = 1;
            clip->sampleRate = kSynthRate;
            clip->pcm = renderSynth(layers, kSynthRate, nameSeed(name));
            d.clips.push_back(clip);
        } else if (j.has("file")) {
            if (!j["file"].isString()) return fail(who + ": \"file\" must be a string");
            if (auto c = loadClip(who, j.getStr("file"))) d.clips.push_back(c);
        } else {
            if (!j["files"].isArray() || j["files"].size() == 0) return fail(who + ": \"files\" must be a non-empty array");
            for (auto& f : j["files"].items()) {
                if (!f.isString()) return fail(who + ": \"files\" must contain strings");
                if (auto c = loadClip(who, f.str())) d.clips.push_back(c);
            }
        }
        byName[name] = SoundId(sounds.size());
        sounds.push_back(std::move(d));
    }

    for (auto& [name, j] : bank["music"].members()) {
        const std::string who = "music '" + name + "'";
        auto pl = std::make_shared<Playlist>();
        pl->name = name;
        std::vector<std::string> files;
        const Json* list = &j;
        if (j.isObject()) {
            list = &j["files"];
            pl->shuffle = j.getBool("shuffle", false);
            pl->loop = j.getBool("loop", true);
            pl->volume = std::clamp(num(j["volume"], 1), 0.0f, 4.0f);
        }
        if (list->isString()) files.push_back(list->str());
        else if (list->isArray()) for (auto& f : list->items()) files.push_back(f.str());
        if (files.empty()) return fail(who + ": needs a file name, an array of files or {\"files\": [...]}");
        for (auto& f : files) {
            std::string bytes;
            if (f.empty() || !read || !read(f, bytes)) { warnings.push_back(who + ": file not found '" + f + "'"); continue; }
            pl->tracks.push_back(std::make_shared<const std::string>(std::move(bytes)));
        }
        music[name] = pl;
    }

    Impl& m = *impl_;
    m.sounds = std::move(sounds);
    m.byName = std::move(byName);
    m.music = std::move(music);
    m.warnings = std::move(warnings);
    m.lastPlay.assign(m.sounds.size(), -1e9);
    m.lastVariant.assign(m.sounds.size(), -1);
    std::copy(std::begin(gains), std::end(gains), std::begin(m.bankGain));
    m.bankMaster = master;
    m.sendGains();
    for (auto& w : m.warnings) std::fprintf(stderr, "audio: %s\n", w.c_str());
    return true;
}

SoundId AudioEngine::find(std::string_view name) const {
    auto it = impl_->byName.find(name);
    return it == impl_->byName.end() ? kNoSound : it->second;
}

const SoundDef* AudioEngine::sound(SoundId id) const {
    return id >= 0 && size_t(id) < impl_->sounds.size() ? &impl_->sounds[size_t(id)] : nullptr;
}

size_t AudioEngine::soundCount() const { return impl_->sounds.size(); }
const std::vector<std::string>& AudioEngine::warnings() const { return impl_->warnings; }
bool AudioEngine::hasMusic(std::string_view name) const { return impl_->music.count(name) > 0; }

// ------------------------------------------------------------------ playback

VoiceId AudioEngine::play(SoundId id, const PlayParams& p) {
    Impl& m = *impl_;
    const SoundDef* d = sound(id);
    if (!d || d->clips.empty() || !m.mixing()) return kNoVoice;
    double& last = m.lastPlay[size_t(id)];
    if (m.time - last < double(d->cooldown)) return kNoVoice;
    const bool spatial = d->spatial && p.positional;
    if (spatial && !d->loop && audibility(p.x, p.z) <= 0.001f) return kNoVoice;
    last = m.time;

    int variant = 0;
    if (d->clips.size() > 1) { // random, but never the same variation twice in a row
        int& prev = m.lastVariant[size_t(id)];
        variant = int(m.frand() * float(d->clips.size())) % int(d->clips.size());
        if (variant == prev) variant = (variant + 1) % int(d->clips.size());
        prev = variant;
    }
    MixCmd c;
    c.kind = MixCmd::Play;
    c.id = m.nextId++;
    if (m.nextId == kNoVoice) m.nextId = 1;
    c.sound = id;
    c.clip = d->clips[size_t(variant)];
    c.volume = d->volume * p.volume;
    c.pitch = p.pitch * (d->pitchMin + (d->pitchMax - d->pitchMin) * m.frand());
    c.bus = d->bus;
    c.priority = d->priority;
    c.maxInstances = d->maxInstances;
    c.loop = d->loop;
    c.spatial = spatial;
    c.x = p.x; c.z = p.z;
    const VoiceId vid = c.id;
    m.mixer.push(std::move(c));
    return vid;
}

void AudioEngine::stop(VoiceId v, float fade) {
    if (v == kNoVoice || !impl_->mixing()) return;
    MixCmd c;
    c.kind = MixCmd::Stop;
    c.id = v;
    c.fade = fade;
    impl_->mixer.push(std::move(c));
}

void AudioEngine::stopAll(float fade) {
    if (!impl_->mixing()) return;
    MixCmd c;
    c.kind = MixCmd::StopAll;
    c.fade = fade;
    impl_->mixer.push(std::move(c));
}

void AudioEngine::setVoicePosition(VoiceId v, float x, float z) {
    if (v == kNoVoice || !impl_->mixing()) return;
    MixCmd c;
    c.kind = MixCmd::SetPos;
    c.id = v;
    c.x = x; c.z = z;
    impl_->mixer.push(std::move(c));
}

void AudioEngine::setListener(float x, float z, float yaw, float radius) {
    Listener& l = impl_->listener;
    if (l.x == x && l.z == z && l.yaw == yaw && l.radius == radius) return;
    l = {x, z, yaw, radius};
    impl_->sendListener();
}

float AudioEngine::audibility(float x, float z) const {
    float g, pan;
    spatialize(impl_->listener, x, z, g, pan);
    return g;
}

// ------------------------------------------------------------------ music

bool AudioEngine::playMusic(std::string_view name, float fade) {
    Impl& m = *impl_;
    auto it = m.music.find(name);
    if (it == m.music.end()) return false;
    if (m.currentMusic == name) return true;
    m.currentMusic = std::string(name);
    if (m.mixing()) m.startMusic(m.currentMusic, fade);
    else { m.pendingMusic = m.currentMusic; m.pendingFade = fade; }
    return true;
}

void AudioEngine::stopMusic(float fade) {
    Impl& m = *impl_;
    m.currentMusic.clear();
    m.pendingMusic.clear();
    if (!m.mixing()) return;
    MixCmd c;
    c.kind = MixCmd::StopMusic;
    c.fade = fade;
    m.mixer.push(std::move(c));
}

const std::string& AudioEngine::currentMusic() const { return impl_->currentMusic; }

// ------------------------------------------------------------------ volumes, pause

void AudioEngine::setMasterVolume(float v) { impl_->userMaster = std::clamp(v, 0.0f, 1.0f); impl_->sendGains(); }
float AudioEngine::masterVolume() const { return impl_->userMaster; }
void AudioEngine::setVolume(Bus b, float v) {
    if (size_t(b) >= size_t(Bus::Count)) return;
    impl_->user[size_t(b)] = std::clamp(v, 0.0f, 1.0f);
    impl_->sendGains();
}
float AudioEngine::volume(Bus b) const { return size_t(b) < size_t(Bus::Count) ? impl_->user[size_t(b)] : 0.0f; }
void AudioEngine::setMuted(bool on) { impl_->muted = on; impl_->sendGains(); }
bool AudioEngine::muted() const { return impl_->muted; }
bool AudioEngine::paused() const { return impl_->paused; }

void AudioEngine::setPaused(bool p) {
    Impl& m = *impl_;
    if (m.paused == p) return;
    m.paused = p;
    if (!m.devOk) return;
    if (p) {
        if (m.running) ma_device_stop(&m.dev);
        m.running = false;
    } else {
        m.running = ma_device_start(&m.dev) == MA_SUCCESS;
        // state changed while nothing was mixing: resend it
        m.sendGains();
        m.sendListener();
        if (!m.pendingMusic.empty()) { m.startMusic(m.pendingMusic, m.pendingFade); m.pendingMusic.clear(); }
    }
}

std::string AudioEngine::saveSettings() const {
    const Impl& m = *impl_;
    std::ostringstream s;
    s << "master " << m.userMaster << "\n";
    for (size_t i = 0; i < size_t(Bus::Count); ++i) s << kBusNames[i] << " " << m.user[i] << "\n";
    s << "muted " << (m.muted ? 1 : 0) << "\n";
    return s.str();
}

void AudioEngine::loadSettings(const std::string& text) {
    Impl& m = *impl_;
    std::istringstream s(text);
    std::string key;
    float v;
    while (s >> key >> v) {
        if (!std::isfinite(v)) continue;
        Bus b;
        if (key == "master") m.userMaster = std::clamp(v, 0.0f, 1.0f);
        else if (key == "muted") m.muted = v != 0;
        else if (busFromName(key, b)) m.user[size_t(b)] = std::clamp(v, 0.0f, 1.0f);
    }
    m.sendGains();
}

void AudioEngine::update(double dt) {
    Impl& m = *impl_;
    m.time += std::max(0.0, dt);
    if (!m.pendingMusic.empty() && m.mixing()) { m.startMusic(m.pendingMusic, m.pendingFade); m.pendingMusic.clear(); }
}

void AudioEngine::render(float* out, int frames) {
    if (impl_->devOk || frames <= 0) return;
    impl_->mixer.render(out, frames);
}

int AudioEngine::activeVoices() const { return impl_->mixer.activeVoices(); }

} // namespace rts::audio
