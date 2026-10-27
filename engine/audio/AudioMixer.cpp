#include "AudioMixer.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace rts::audio {

namespace {
constexpr int kBlock = 256;             // frames mixed per inner iteration
constexpr int kExtraSlots = 16;         // room for voices fading out after being stolen
constexpr float kStealFade = 0.006f;    // seconds

float softClip(float x) {
    const float a = std::fabs(x);
    if (a <= 0.8f) return x;
    const float y = 0.8f + 0.2f * std::tanh((a - 0.8f) / 0.2f);
    return x < 0 ? -y : y;
}
} // namespace

void spatialize(const Listener& l, float x, float z, float& gain, float& pan) {
    const float dx = x - l.x, dz = z - l.z;
    const float d = std::sqrt(dx * dx + dz * dz);
    const float r = std::max(1.0f, l.radius);
    if (d <= r) gain = 1.0f;
    else if (d >= 2.0f * r) gain = 0.0f;
    else { const float t = 1.0f - (d - r) / r; gain = t * t; }
    // camera right vector for the client's yaw convention: (cos yaw, 0, -sin yaw)
    const float local = dx * std::cos(l.yaw) - dz * std::sin(l.yaw);
    pan = std::clamp(local / (1.5f * r), -1.0f, 1.0f) * 0.8f;
}

void Mixer::configure(int sampleRate, int maxVoices) {
    std::lock_guard<std::mutex> lock(mutex_);
    rate_ = sampleRate > 0 ? sampleRate : 48000;
    maxVoices_ = std::max(1, maxVoices);
    voices_.assign(size_t(maxVoices_ + kExtraSlots), Voice{});
    pending_.reserve(256);
    work_.reserve(256);
    tmp_.assign(size_t(kBlock) * 2, 0.0f);
    for (auto& m : music_) m = std::make_unique<Stream>();
}

void Mixer::push(MixCmd&& c) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(std::move(c));
}

uint32_t Mixer::nextRandom() {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    return rng_;
}

Mixer::Voice* Mixer::findVoice(VoiceId id) {
    for (auto& v : voices_) if (v.active && v.id == id) return &v;
    return nullptr;
}

void Mixer::startVoice(MixCmd& c) {
    if (!c.clip || c.clip->frames() == 0) return;
    const float stealStep = -1.0f / (kStealFade * float(rate_));

    // per-sound instance limit: the oldest instance makes room
    if (c.maxInstances > 0) {
        int count = 0;
        Voice* oldest = nullptr;
        for (auto& v : voices_) {
            if (!v.active || v.stopping || v.sound != c.sound) continue;
            ++count;
            if (!oldest || v.serial < oldest->serial) oldest = &v;
        }
        if (count >= c.maxInstances && oldest) { oldest->stopping = true; oldest->fadeStep = stealStep; }
    }
    // global limit: steal the lowest priority (oldest first), or drop the new sound
    int playing = 0;
    Voice* victim = nullptr;
    for (auto& v : voices_) {
        if (!v.active || v.stopping) continue;
        ++playing;
        if (!victim || v.priority < victim->priority || (v.priority == victim->priority && v.serial < victim->serial)) victim = &v;
    }
    if (playing >= maxVoices_ && victim) {
        if (victim->priority > c.priority) return;
        victim->stopping = true;
        victim->fadeStep = stealStep;
    }
    Voice* slot = nullptr;
    for (auto& v : voices_) if (!v.active) { slot = &v; break; }
    if (!slot) { // every slot is fading out: take the quietest
        for (auto& v : voices_) if (!slot || v.fade < slot->fade) slot = &v;
    }
    Voice& v = *slot;
    v = Voice{};
    v.active = true;
    v.id = c.id;
    v.sound = c.sound;
    v.bus = c.bus;
    v.priority = c.priority;
    v.serial = ++serial_;
    v.clip = std::move(c.clip);
    v.step = double(c.pitch) * double(v.clip->sampleRate) / double(rate_);
    v.volume = c.volume;
    v.loop = c.loop;
    v.spatial = c.spatial;
    v.x = c.x; v.z = c.z;
    if (c.fade > 0) { v.fade = 0; v.fadeStep = 1.0f / (c.fade * float(rate_)); }
}

bool Mixer::openTrack(Stream& s) {
    const size_t n = s.list ? s.list->tracks.size() : 0;
    for (size_t attempt = 0; attempt < n; ++attempt) {
        if (s.index >= n) {
            if (!s.list->loop) return false;
            s.index = 0;
            if (s.list->shuffle && n > 1) { // reshuffle, avoiding an immediate repeat
                size_t last = s.order.back();
                for (size_t i = n - 1; i > 0; --i) std::swap(s.order[i], s.order[nextRandom() % (i + 1)]);
                if (s.order[0] == last) std::swap(s.order[0], s.order[n - 1]);
            }
        }
        const auto& bytes = s.list->tracks[s.order[s.index++]];
        if (bytes && s.dec.open(bytes, rate_)) return true;
    }
    return false;
}

void Mixer::apply(MixCmd& c) {
    switch (c.kind) {
    case MixCmd::Play: startVoice(c); break;
    case MixCmd::Stop:
        if (Voice* v = findVoice(c.id)) {
            v->stopping = true;
            v->fadeStep = -1.0f / (std::max(c.fade, 0.002f) * float(rate_));
        }
        break;
    case MixCmd::StopAll:
        for (auto& v : voices_) if (v.active) { v.stopping = true; v.fadeStep = -1.0f / (std::max(c.fade, 0.002f) * float(rate_)); }
        break;
    case MixCmd::SetPos:
        if (Voice* v = findVoice(c.id)) { v->x = c.x; v->z = c.z; }
        break;
    case MixCmd::SetListener: listener_ = c.listener; break;
    case MixCmd::SetGains:
        std::copy(std::begin(c.gains), std::end(c.gains), std::begin(gains_));
        master_ = c.master;
        break;
    case MixCmd::Music: {
        Stream& cur = *music_[0];
        if (cur.active && cur.list && c.playlist && cur.list->name == c.playlist->name && cur.target > 0) break;
        const float frames = std::max(1.0f, c.fade * float(rate_));
        if (cur.active) {
            std::swap(music_[0], music_[1]);   // current starts fading out
            Stream& old = *music_[1];
            old.target = 0;
            old.step = -std::max(old.gain, 0.0001f) / frames;
        }
        Stream& s = *music_[0];
        s.dec.close();
        s.list = std::move(c.playlist);
        s.active = false;
        if (!s.list || s.list->tracks.empty()) break;
        const size_t n = s.list->tracks.size();
        s.order.resize(n);
        for (size_t i = 0; i < n; ++i) s.order[i] = i;
        if (s.list->shuffle) for (size_t i = n - 1; i > 0; --i) std::swap(s.order[i], s.order[nextRandom() % (i + 1)]);
        s.index = 0;
        if (!openTrack(s)) break;
        s.active = true;
        s.gain = 0;
        s.target = s.list->volume;
        s.step = s.target / frames;
        break;
    }
    case MixCmd::StopMusic: {
        const float frames = std::max(1.0f, c.fade * float(rate_));
        for (auto& m : music_) if (m->active) { m->target = 0; m->step = -std::max(m->gain, 0.0001f) / frames; }
        break;
    }
    }
}

void Mixer::mixVoices(float* buf, int n) {
    const float inv = 1.0f / 32768.0f;
    const float master = masterCur_;
    for (auto& v : voices_) {
        if (!v.active) continue;
        float g = v.volume * gains_[size_t(v.bus)] * master, pan = 0;
        if (v.spatial) {
            float sg;
            spatialize(listener_, v.x, v.z, sg, pan);
            g *= sg;
        }
        const float tl = g * std::min(1.0f, 1.0f - pan), tr = g * std::min(1.0f, 1.0f + pan);
        if (v.fresh) { v.gl = tl; v.gr = tr; v.fresh = false; }
        const float dl = (tl - v.gl) / float(n), dr = (tr - v.gr) / float(n);
        float gl = v.gl, gr = v.gr;

        const Clip& c = *v.clip;
        const int16_t* pcm = c.pcm.data();
        const double frames = double(c.frames());
        const bool stereo = c.channels == 2;
        bool done = false;
        for (int k = 0; k < n; ++k) {
            if (v.pos >= frames) {
                if (v.loop) v.pos -= frames * std::floor(v.pos / frames);
                else { done = true; break; }
            }
            const size_t i0 = size_t(v.pos);
            size_t i1 = i0 + 1;
            if (double(i1) >= frames) i1 = v.loop ? 0 : i0;
            const float t = float(v.pos - double(i0));
            float sl, sr;
            if (stereo) {
                sl = (float(pcm[i0 * 2]) + (float(pcm[i1 * 2]) - float(pcm[i0 * 2])) * t) * inv;
                sr = (float(pcm[i0 * 2 + 1]) + (float(pcm[i1 * 2 + 1]) - float(pcm[i0 * 2 + 1])) * t) * inv;
            } else {
                sl = sr = (float(pcm[i0]) + (float(pcm[i1]) - float(pcm[i0])) * t) * inv;
            }
            gl += dl; gr += dr;
            buf[k * 2] += sl * gl * v.fade;
            buf[k * 2 + 1] += sr * gr * v.fade;
            if (v.fadeStep != 0) {
                v.fade += v.fadeStep;
                if (v.fade >= 1.0f) { v.fade = 1.0f; v.fadeStep = 0; }
                else if (v.fade <= 0.0f) { done = true; break; }
            }
            v.pos += v.step;
        }
        v.gl = tl; v.gr = tr;
        if (done) { v.active = false; v.clip.reset(); }
    }
}

void Mixer::mixStream(Stream& s, float* buf, int n) {
    if (!s.active) return;
    const float bus = gains_[size_t(Bus::Music)] * masterCur_;
    int done = 0, reopened = 0;
    while (done < n && s.active) {
        const int want = n - done;
        const int got = s.dec.isOpen() ? s.dec.read(tmp_.data(), want) : 0;
        for (int k = 0; k < got; ++k) {
            if (s.step != 0) {
                s.gain += s.step;
                if ((s.step > 0 && s.gain >= s.target) || (s.step < 0 && s.gain <= s.target)) { s.gain = s.target; s.step = 0; }
            }
            const float g = s.gain * bus;
            buf[(done + k) * 2] += tmp_[size_t(k) * 2] * g;
            buf[(done + k) * 2 + 1] += tmp_[size_t(k) * 2 + 1] * g;
        }
        done += got;
        if (s.target <= 0 && s.step == 0 && s.gain <= 0) { s.dec.close(); s.active = false; break; }
        if (got < want) { // track ended inside this block: continue with the next one
            s.dec.close();
            // a playlist of empty/broken tracks must not spin forever inside the audio callback
            if (++reopened > int(s.list->tracks.size()) || !openTrack(s)) { s.active = false; break; }
        }
    }
}

// Big battles stack dozens of sounds: a peak limiter (instant attack per block, ~0.3 s release)
// keeps the mix under the soft-clip knee instead of letting it saturate.
void Mixer::limit(float* buf, int n) {
    constexpr float kThreshold = 0.7f;
    float peak = 0;
    for (int k = 0; k < n * 2; ++k) peak = std::max(peak, std::fabs(buf[k]));
    const float release = std::exp(-float(n) / (0.3f * float(rate_)));
    limEnv_ = std::max(peak, limEnv_ * release);
    const float target = limEnv_ > kThreshold ? kThreshold / limEnv_ : 1.0f;
    // attack: never ramp up into a louder block; release: ramp smoothly across the block
    const float start = std::min(limGain_, target), step = (target - start) / float(n);
    float g = start;
    for (int k = 0; k < n; ++k) {
        g += step;
        buf[k * 2] = softClip(buf[k * 2] * g);
        buf[k * 2 + 1] = softClip(buf[k * 2 + 1] * g);
    }
    limGain_ = target;
}

void Mixer::render(float* out, int frames) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        work_.swap(pending_);
    }
    for (auto& c : work_) apply(c);
    work_.clear();

    int written = 0;
    while (written < frames) {
        const int n = std::min(kBlock, frames - written);
        float* buf = out + size_t(written) * 2;
        std::memset(buf, 0, sizeof(float) * size_t(n) * 2);
        // master follows its target over ~one block (mute/unmute without clicks)
        masterCur_ += (master_ - masterCur_) * 0.5f;
        if (std::fabs(master_ - masterCur_) < 1e-4f) masterCur_ = master_;
        mixVoices(buf, n);
        for (auto& m : music_) mixStream(*m, buf, n);
        limit(buf, n);
        written += n;
    }
    int count = 0;
    for (auto& v : voices_) count += v.active ? 1 : 0;
    active_.store(count, std::memory_order_relaxed);
}

} // namespace rts::audio
