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

// Distance keys shared by the "spatial" defaults and every sound.
bool parseDistance(const Json& j, DistanceDef& d, const std::string& who, std::string& error) {
    if (j.has("model")) {
        const std::string m = j.getStr("model");
        if (m == "inverse") d.model = Rolloff::Inverse;
        else if (m == "linear") d.model = Rolloff::Linear;
        else if (m == "exponential") d.model = Rolloff::Exponential;
        else { error = who + ": unknown model '" + m + "' (inverse, linear, exponential)"; return false; }
    }
    d.minDistance = num(j["minDistance"], d.minDistance);
    d.maxDistance = num(j["maxDistance"], d.maxDistance);
    d.rolloff = std::clamp(num(j["rolloff"], d.rolloff), 0.0f, 10.0f);
    d.air = std::clamp(num(j["air"], d.air), 0.0f, 4.0f);
    d.reverb = std::clamp(num(j["reverb"], d.reverb), 0.0f, 4.0f);
    if (d.minDistance <= 0 || d.maxDistance <= d.minDistance) { error = who + ": needs 0 < minDistance < maxDistance"; return false; }
    return true;
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
    float intensity = 0;
    float pendingFade = 0;

    float bankGain[size_t(Bus::Count)] = {1, 1, 1, 1, 1};
    float bankMaster = 1;
    float user[size_t(Bus::Count)] = {1, 1, 1, 1, 1};
    float userMaster = 1;
    bool muted = false, paused = false;
    ListenerPose listener;
    Environment env;
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
    void sendEnv() {
        if (!mixing()) return;
        MixCmd c;
        c.kind = MixCmd::SetEnv;
        c.env = env;
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
    m.mixer.configure(rate, cfg.maxVoices, cfg.reverb);
    m.initialized = true;
    if (m.devOk) {
        if (ma_device_start(&m.dev) == MA_SUCCESS) m.running = true;
        else m.devError = "cannot start audio device";
    }
    if (!m.devError.empty()) std::fprintf(stderr, "audio: %s, running silently\n", m.devError.c_str());
    m.sendGains();
    m.sendListener();
    m.sendEnv();
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
    m.env = Environment{};
    m.sendGains();
    m.sendEnv();
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

    // RtsAudio reads the gameplay mapping sections; anything else is most likely a typo
    static const std::set<std::string> kTopKeys = {"volume", "spatial", "sounds", "music", "events", "types", "weapons", "abilities", "build"};
    for (auto& [k, unused] : bank.members())
        if (!kTopKeys.count(k)) warnings.push_back("unknown section '" + k + "' (volume, spatial, sounds, music, events, types, weapons, abilities, build)");

    Environment env;
    if (const Json& sp = bank["spatial"]; !sp.isNull()) {
        if (!sp.isObject()) return fail("\"spatial\" must be an object");
        static const std::set<std::string> kSpatialKeys = {"model", "minDistance", "maxDistance", "rolloff", "air", "reverb",
                                                           "airAbsorption", "rear", "speedOfSound"};
        for (auto& [k, unused] : sp.members())
            if (!kSpatialKeys.count(k)) warnings.push_back("spatial: unknown key '" + k + "'");
        std::string why;
        if (!parseDistance(sp, env.defaults, "spatial", why)) return fail(why);
        env.defaults.reverb = 1;  // here "reverb" configures the reverb itself, not a send multiplier
        const Json& air = sp["airAbsorption"];
        if (air.type() == Json::Type::Bool) env.airCutoff = air.boolean() ? env.airCutoff : 0;
        else if (!air.isNull()) env.airCutoff = std::clamp(num(air, env.airCutoff), 0.0f, 20000.0f);
        env.rear = std::clamp(num(sp["rear"], env.rear), 0.0f, 1.0f);
        env.speedOfSound = std::max(0.0f, num(sp["speedOfSound"], 0));
        const Json& rv = sp["reverb"];
        if (rv.type() == Json::Type::Bool) env.reverb = rv.boolean();
        else if (rv.isObject()) {
            env.reverbSize = std::clamp(num(rv["size"], env.reverbSize), 0.0f, 1.0f);
            env.reverbDamping = std::clamp(num(rv["damping"], env.reverbDamping), 0.0f, 1.0f);
            env.reverbWet = std::clamp(num(rv["wet"], env.reverbWet), 0.0f, 2.0f);
            env.reverbNear = std::clamp(num(rv["near"], env.reverbNear), 0.0f, 1.0f);
            env.reverbFar = std::clamp(num(rv["far"], env.reverbFar), 0.0f, 1.0f);
        } else if (!rv.isNull()) return fail("spatial.reverb must be false or {\"size\", \"damping\", \"wet\", \"near\", \"far\"}");
    }
    std::vector<std::pair<SoundId, std::string>> farNames;   // resolved once every sound is known

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

    static const std::set<std::string> kSoundKeys = {"file", "files", "synth", "volume", "pitch", "bus", "maxInstances", "model",
                                                      "minDistance", "maxDistance", "rolloff", "air", "reverb", "far", "compress", "quality", "duckMusic", "sync",
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
        d.duckMusic = std::clamp(num(j["duckMusic"], d.bus == Bus::Voice ? 0.4f : 0.0f), 0.0f, 1.0f);
        if (const std::string sy = j.getStr("sync", "now"); sy == "beat") d.sync = 1;
        else if (sy == "bar") d.sync = 2;
        else if (sy != "now") return fail(who + ": unknown sync '" + sy + "' (now, beat, bar)");
        d.distance = env.defaults;
        if (std::string why; !parseDistance(j, d.distance, who, why)) return fail(why);
        if (const Json& far = j["far"]; !far.isNull()) {
            const Json& fs = far.isObject() ? far["sound"] : far;
            if (!fs.isString()) return fail(who + ": \"far\" must be a sound name or {\"sound\": name, \"from\": 0.3}");
            if (far.isObject()) d.farFrom = std::clamp(num(far["from"], d.farFrom), 0.0f, 0.95f);
            farNames.push_back({SoundId(sounds.size()), fs.str()});
        }

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

    for (auto& [id, farName] : farNames) {
        auto it = byName.find(farName);
        SoundDef& d = sounds[size_t(id)];
        if (it == byName.end()) warnings.push_back("sound '" + d.name + "': unknown far sound '" + farName + "'");
        else if (it->second == id || sounds[size_t(it->second)].far != kNoSound) warnings.push_back("sound '" + d.name + "': far sound '" + farName + "' must be a plain sound");
        else d.far = it->second;
    }

    // music: "name": file | [entries] | {"files"|"layers": [...], options}; an entry is a file name or
    // {"file", "loopStart", "loopEnd", "intensity"}
    static const std::set<std::string> kMusicKeys = {"files", "layers", "shuffle", "loop", "volume", "bpm", "beatsPerBar",
                                                     "offset", "transition", "loopStart", "loopEnd", "compress", "quality"};
    for (auto& [name, j] : bank["music"].members()) {
        const std::string who = "music '" + name + "'";
        auto pl = std::make_shared<Playlist>();
        pl->name = name;
        auto entry = [&](const Json& e, MusicTrack& t, std::string& file) -> bool {
            if (e.isString()) { file = e.str(); return true; }
            if (!e.isObject() || !e["file"].isString()) return false;
            file = e.getStr("file");
            t.loopStart = e.has("loopStart") ? std::max(0.0f, num(e["loopStart"], 0)) : -1.0f;
            t.loopEnd = std::max(0.0f, num(e["loopEnd"], 0));
            t.intensity = std::clamp(num(e["intensity"], 0), 0.0f, 1.0f);
            if (t.loopEnd > 0 && t.loopStart >= t.loopEnd) { warnings.push_back(who + ": loopStart must be before loopEnd"); t.loopStart = -1; }
            return true;
        };
        auto load = [&](const Json& list, std::vector<MusicTrack>& out) -> bool {
            std::vector<const Json*> items;
            if (list.isArray()) for (auto& e : list.items()) items.push_back(&e);
            else items.push_back(&list);
            for (auto* e : items) {
                MusicTrack t;
                std::string file, bytes;
                if (!entry(*e, t, file)) return false;
                if (file.empty() || !read || !read(file, bytes)) { warnings.push_back(who + ": file not found '" + file + "'"); continue; }
                t.bytes = std::make_shared<const std::string>(std::move(bytes));
                out.push_back(std::move(t));
            }
            return true;
        };
        const char* shape = ": needs a file, an array of files, or {\"files\": [...]} / {\"layers\": [...]}";
        if (j.isObject()) {
            for (auto& [k, unused] : j.members())
                if (!kMusicKeys.count(k)) warnings.push_back(who + ": unknown key '" + k + "'");
            pl->shuffle = j.getBool("shuffle", false);
            pl->loop = j.getBool("loop", true);
            pl->volume = std::clamp(num(j["volume"], 1), 0.0f, 4.0f);
            pl->bpm = std::clamp(num(j["bpm"], 0), 0.0f, 400.0f);
            pl->beatsPerBar = int(std::clamp<int64_t>(j.getInt("beatsPerBar", 4), 1, 16));
            pl->offset = std::max(0.0f, num(j["offset"], 0));
            const std::string tr = j.getStr("transition", pl->bpm > 0 ? "bar" : "now");
            if (tr == "now") pl->transition = Quantize::Now;
            else if (tr == "beat") pl->transition = Quantize::Beat;
            else if (tr == "bar") pl->transition = Quantize::Bar;
            else return fail(who + ": unknown transition '" + tr + "' (now, beat, bar)");
            if (j.has("layers")) {
                if (j.has("files")) return fail(who + ": either \"files\" or \"layers\"");
                if (!j["layers"].isArray() || !load(j["layers"], pl->layers)) return fail(who + shape);
                pl->layerLoopStart = std::max(0.0f, num(j["loopStart"], 0));
                pl->layerLoopEnd = std::max(0.0f, num(j["loopEnd"], 0));
            } else if (j["files"].isNull() || !load(j["files"], pl->tracks)) {
                return fail(who + shape);
            }
        } else if (j.isNull() || !load(j, pl->tracks)) {
            return fail(who + shape);
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
    m.env = env;
    m.sendGains();
    m.sendEnv();
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
    if (spatial && !d->loop && audibility(id, p.x, p.y, p.z) <= 0.001f) return kNoVoice;
    last = m.time;

    uint32_t delay = 0;   // speed of sound: far explosions arrive late
    if (spatial && m.env.speedOfSound > 0) {
        const float dx = p.x - m.listener.pos[0], dy = p.y - m.listener.pos[1], dz = p.z - m.listener.pos[2];
        const float seconds = std::min(3.0f, std::sqrt(dx * dx + dy * dy + dz * dz) / m.env.speedOfSound);
        delay = uint32_t(seconds * float(m.mixer.sampleRate()));
    }
    const VoiceId vid = m.nextId++;
    if (m.nextId == kNoVoice) m.nextId = 1;

    auto pushVoice = [&](SoundId sid, MixCmd::Role role) {
        const SoundDef& s = *sound(sid);
        if (s.clips.empty()) return;
        int variant = 0;
        if (s.clips.size() > 1) { // random, but never the same variation twice in a row
            int& prev = m.lastVariant[size_t(sid)];
            variant = int(m.frand() * float(s.clips.size())) % int(s.clips.size());
            if (variant == prev) variant = (variant + 1) % int(s.clips.size());
            prev = variant;
        }
        MixCmd c;
        c.kind = MixCmd::Play;
        c.id = vid;   // both halves of a near/far pair answer to the same voice id
        c.sound = sid;
        c.clip = s.clips[size_t(variant)];
        c.volume = s.volume * p.volume;
        c.pitch = p.pitch * (s.pitchMin + (s.pitchMax - s.pitchMin) * m.frand());
        c.bus = s.bus;
        c.priority = role == MixCmd::Far ? d->priority : s.priority;
        c.maxInstances = s.maxInstances;
        c.loop = s.loop;
        c.spatial = spatial;
        c.x = p.x; c.y = p.y; c.z = p.z;
        c.distance = s.distance;
        c.role = role;
        c.pairDistance = d->distance;
        c.pairFrom = d->farFrom;
        c.delayFrames = delay;
        c.duck = s.duckMusic;
        c.sync = Quantize(s.sync);
        m.mixer.push(std::move(c));
    };
    const bool pair = spatial && d->far != kNoSound;
    pushVoice(id, pair ? MixCmd::Near : MixCmd::Single);
    if (pair) pushVoice(d->far, MixCmd::Far);
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

void AudioEngine::setVoicePosition(VoiceId v, float x, float y, float z) {
    if (v == kNoVoice || !impl_->mixing()) return;
    MixCmd c;
    c.kind = MixCmd::SetPos;
    c.id = v;
    c.x = x; c.y = y; c.z = z;
    impl_->mixer.push(std::move(c));
}

void AudioEngine::setListener(const ListenerPose& pose) {
    ListenerPose& l = impl_->listener;
    if (std::equal(pose.pos, pose.pos + 3, l.pos) && std::equal(pose.forward, pose.forward + 3, l.forward) &&
        std::equal(pose.up, pose.up + 3, l.up) && pose.hearing == l.hearing) return;
    l = pose;
    impl_->sendListener();
}

void AudioEngine::setListener(float x, float z, float yaw, float radius) {
    ListenerPose pose;
    pose.pos[0] = x; pose.pos[1] = 0; pose.pos[2] = z;
    pose.forward[0] = -std::sin(yaw); pose.forward[1] = 0; pose.forward[2] = -std::cos(yaw);
    pose.hearing = radius / std::max(0.01f, impl_->env.defaults.minDistance);
    setListener(pose);
}

const ListenerPose& AudioEngine::listener() const { return impl_->listener; }
const Environment& AudioEngine::environment() const { return impl_->env; }

float AudioEngine::audibility(SoundId id, float x, float y, float z) const {
    const Impl& m = *impl_;
    const SoundDef* d = sound(id);
    const DistanceDef& dist = d ? d->distance : m.env.defaults;
    float g = distanceGain(dist, spatialize(m.listener, m.env, dist, x, y, z).distance, m.listener.hearing);
    if (d && d->far != kNoSound) g = std::max(g, audibility(d->far, x, y, z));
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

bool AudioEngine::musicHasLayers(std::string_view name) const {
    auto it = impl_->music.find(name);
    return it != impl_->music.end() && !it->second->layers.empty();
}

void AudioEngine::setMusicIntensity(float v) {
    Impl& m = *impl_;
    m.intensity = std::clamp(v, 0.0f, 1.0f);
    if (!m.mixing()) return;
    MixCmd c;
    c.kind = MixCmd::SetIntensity;
    c.intensity = m.intensity;
    m.mixer.push(std::move(c));
}

float AudioEngine::musicIntensity() const { return impl_->intensity; }

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
        setMusicIntensity(m.intensity);
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
