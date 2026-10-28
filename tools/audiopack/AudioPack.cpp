#include "AudioPack.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include <ogg/ogg.h>
#include <vorbis/vorbisenc.h>

#include "engine/audio/Audio.h"
#include "engine/data/Json.h"

namespace fs = std::filesystem;

namespace rts::audiopack {

namespace {

constexpr const char* kCodecNames[] = {"keep", "pcm", "adpcm", "vorbis"};

void put16(std::string& s, uint32_t v) { s += char(v & 0xFF); s += char((v >> 8) & 0xFF); }
void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s += char((v >> (8 * i)) & 0xFF); }

uint32_t fnv(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isLossy(const std::string& path, const std::string& bytes) {
    const std::string ext = lower(fs::path(path).extension().string());
    if (ext == ".ogg" || ext == ".oga" || ext == ".mp3" || ext == ".opus") return true;
    return bytes.size() >= 4 && (bytes.compare(0, 4, "OggS") == 0 || bytes.compare(0, 3, "ID3") == 0);
}

std::string formatKb(uint64_t b) {
    char buf[32];
    if (b >= 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f МБ", double(b) / (1024.0 * 1024.0));
    else std::snprintf(buf, sizeof(buf), "%.0f КБ", std::ceil(double(b) / 1024.0));
    return buf;
}

float num(const Json& j, float def) { return j.isNumber() ? float(j.fx().toDouble()) : def; }

// ---------------------------------------------------------------- IMA ADPCM
constexpr int kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
constexpr int kImaStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
    1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

struct ImaState { int pred = 0, index = 0; };

// Encodes one sample and advances the state exactly like a decoder would (no drift).
uint8_t imaEncode(ImaState& st, int sample) {
    int step = kImaStep[st.index];
    int diff = sample - st.pred;
    uint8_t nib = 0;
    if (diff < 0) { nib = 8; diff = -diff; }
    int delta = step >> 3;
    if (diff >= step) { nib |= 4; diff -= step; delta += step; }
    step >>= 1;
    if (diff >= step) { nib |= 2; diff -= step; delta += step; }
    step >>= 1;
    if (diff >= step) { nib |= 1; delta += step; }
    st.pred = std::clamp(st.pred + ((nib & 8) ? -delta : delta), -32768, 32767);
    st.index = std::clamp(st.index + kImaIndex[nib], 0, 88);
    return nib;
}

} // namespace

const char* codecName(Codec c) { return kCodecNames[int(c)]; }

bool codecFromName(const std::string& s, Codec& out) {
    for (int i = 0; i < 4; ++i)
        if (s == kCodecNames[i]) { out = Codec(i); return true; }
    return false;
}

std::string profileForTarget(const std::string& target) {
    if (target == "ios" || target == "android") return "mobile";
    if (target == "web") return "web";
    return "desktop";
}

// ---------------------------------------------------------------- encoders

std::string encodeWav(const std::vector<int16_t>& pcm, int channels, int rate) {
    std::string s;
    const uint32_t bytes = uint32_t(pcm.size() * 2);
    s += "RIFF"; put32(s, 36 + bytes); s += "WAVEfmt ";
    put32(s, 16); put16(s, 1); put16(s, uint32_t(channels)); put32(s, uint32_t(rate));
    put32(s, uint32_t(rate * channels * 2)); put16(s, uint32_t(channels * 2)); put16(s, 16);
    s += "data"; put32(s, bytes);
    for (int16_t v : pcm) put16(s, uint16_t(v));
    return s;
}

std::string encodeAdpcm(const std::vector<int16_t>& pcm, int channels, int rate, bool loop) {
    const int ch = std::clamp(channels, 1, 2);
    const uint32_t blockAlign = 512u * uint32_t(ch);
    const uint32_t spb = (blockAlign - 4u * uint32_t(ch)) * 8u / (4u * uint32_t(ch)) + 1u;  // 1017
    const size_t frames = pcm.size() / size_t(ch);
    const size_t blocks = (frames + spb - 1) / spb;
    // Decoders derive the length from the data size (miniaudio ignores "fact" for IMA), so the last
    // block is cut after the group of 8 that holds the final frame: at most 7 extra frames. They
    // are silence, or for a loop the start of the sound, so the loop stays seamless.
    auto sample = [&](size_t f, int c) -> int {
        if (f < frames) return pcm[f * size_t(ch) + size_t(c)];
        return loop && frames > 0 ? pcm[(f - frames) % frames * size_t(ch) + size_t(c)] : 0;
    };

    std::string data;
    data.reserve(blocks * blockAlign);
    ImaState st[2];
    for (size_t b = 0; b < blocks; ++b) {
        const size_t f0 = b * spb;
        uint32_t groups = (spb - 1) / 8;
        if (b + 1 == blocks) groups = uint32_t(std::min<size_t>(groups, (frames - f0 - 1 + 7) / 8));
        for (int c = 0; c < ch; ++c) { // block header: exact first sample + the best starting step index
            // The header may carry any step index: trying a few and keeping the one with the least
            // error removes the encoder's warm-up at the start of short sounds (build time only).
            double bestErr = -1;
            int bestIndex = st[c].index;
            for (int cand = -1; cand <= 88; cand += 4) {
                ImaState t{sample(f0, c), cand < 0 ? st[c].index : cand};
                double e = 0;
                for (size_t f = f0 + 1; f < f0 + 1 + size_t(groups) * 8; ++f) { const int x = sample(f, c); imaEncode(t, x); e += double(x - t.pred) * double(x - t.pred); }
                if (bestErr < 0 || e < bestErr) { bestErr = e; bestIndex = cand < 0 ? st[c].index : cand; }
            }
            st[c].index = bestIndex;
            st[c].pred = sample(f0, c);
            put16(data, uint16_t(int16_t(st[c].pred)));
            data += char(st[c].index);
            data += char(0);
        }
        // then groups of 8 samples per channel, 4 bytes each, channels interleaved per group
        for (uint32_t g = 0; g < groups; ++g) {
            for (int c = 0; c < ch; ++c) {
                for (int k = 0; k < 4; ++k) {
                    const size_t f = f0 + 1 + g * 8 + size_t(k) * 2;
                    const uint8_t lo = imaEncode(st[c], sample(f, c)), hi = imaEncode(st[c], sample(f + 1, c));
                    data += char(lo | (hi << 4));
                }
            }
        }
    }
    std::string s;
    s += "RIFF"; put32(s, uint32_t(4 + 8 + 20 + 8 + 4 + 8 + data.size())); s += "WAVE";
    s += "fmt "; put32(s, 20);
    put16(s, 0x11); put16(s, uint32_t(ch)); put32(s, uint32_t(rate));
    put32(s, uint32_t(uint64_t(rate) * blockAlign / spb)); put16(s, blockAlign); put16(s, 4);
    put16(s, 2); put16(s, spb);                          // cbSize, samples per block
    s += "fact"; put32(s, 4); put32(s, uint32_t(frames)); // true length, for decoders that read it
    s += "data"; put32(s, uint32_t(data.size()));
    s += data;
    return s;
}

bool encodeVorbis(const std::vector<int16_t>& pcm, int channels, int rate, float quality, uint32_t serial, std::string& out, std::string* error) {
    out.clear();
    const int ch = std::clamp(channels, 1, 2);
    vorbis_info vi;
    vorbis_info_init(&vi);
    if (vorbis_encode_init_vbr(&vi, ch, rate, std::clamp(quality, -0.1f, 1.0f)) != 0) {
        vorbis_info_clear(&vi);
        if (error) *error = "vorbis: unsupported format (" + std::to_string(ch) + " ch, " + std::to_string(rate) + " Hz)";
        return false;
    }
    vorbis_comment vc;
    vorbis_comment_init(&vc);
    vorbis_comment_add_tag(&vc, "ENCODER", "rts_audiopack");
    vorbis_dsp_state vd;
    vorbis_block vb;
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    ogg_stream_state os;
    ogg_stream_init(&os, int(serial & 0x7FFFFFFF));   // fixed serial: reproducible files
    ogg_page og;
    ogg_packet op, hdr, comm, code;
    auto writePage = [&] {
        out.append(reinterpret_cast<const char*>(og.header), size_t(og.header_len));
        out.append(reinterpret_cast<const char*>(og.body), size_t(og.body_len));
    };
    vorbis_analysis_headerout(&vd, &vc, &hdr, &comm, &code);
    ogg_stream_packetin(&os, &hdr);
    ogg_stream_packetin(&os, &comm);
    ogg_stream_packetin(&os, &code);
    while (ogg_stream_flush(&os, &og)) writePage();

    auto drain = [&] {
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, nullptr);
            vorbis_bitrate_addblock(&vb);
            while (vorbis_bitrate_flushpacket(&vd, &op)) {
                ogg_stream_packetin(&os, &op);
                while (ogg_stream_pageout(&os, &og)) writePage();
            }
        }
    };
    const size_t frames = pcm.size() / size_t(ch);
    for (size_t f = 0; f < frames;) {
        const int n = int(std::min<size_t>(4096, frames - f));
        float** buf = vorbis_analysis_buffer(&vd, n);
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < n; ++i) buf[c][i] = float(pcm[(f + size_t(i)) * size_t(ch) + size_t(c)]) / 32768.0f;
        vorbis_analysis_wrote(&vd, n);
        drain();
        f += size_t(n);
    }
    vorbis_analysis_wrote(&vd, 0);   // end of stream
    drain();
    while (ogg_stream_flush(&os, &og)) writePage();

    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    return true;
}

// ---------------------------------------------------------------- settings

bool settingsFor(const Json& bank, const std::string& profile, Settings& s, std::string* error) {
    s = Settings{};
    if (profile == "mobile" || profile == "web") { // phones and browsers download and store everything
        s.sounds = Codec::Adpcm;
        s.music = Codec::Vorbis;
        s.musicQuality = profile == "web" ? 0.25f : 0.3f;
        s.soundQuality = 0.4f;
        s.maxRate = 32000;
    }
    const Json& b = bank["build"];
    if (b.isNull()) return true;
    auto fail = [&](const std::string& m) { if (error) *error = "audio.json build." + profile + ": " + m; return false; };
    if (!b.isObject()) return fail("\"build\" must be an object of profiles");
    const Json& p = b[profile];
    if (p.isNull()) return true;
    if (!p.isObject()) return fail("must be an object");
    static const std::set<std::string> kKeys = {"sounds", "music", "soundQuality", "musicQuality", "maxRate", "monoSounds", "longSound", "reencodeLossy"};
    for (auto& [k, unused] : p.members())
        if (!kKeys.count(k)) return fail("unknown key '" + k + "' (sounds, music, soundQuality, musicQuality, maxRate, monoSounds, longSound, reencodeLossy)");
    for (const char* key : {"sounds", "music"}) {
        if (!p.has(key)) continue;
        Codec c;
        if (!codecFromName(p.getStr(key), c)) return fail(std::string(key) + ": unknown codec '" + p.getStr(key) + "' (keep, pcm, adpcm, vorbis)");
        (std::string(key) == "sounds" ? s.sounds : s.music) = c;
    }
    s.soundQuality = std::clamp(num(p["soundQuality"], s.soundQuality), -0.1f, 1.0f);
    s.musicQuality = std::clamp(num(p["musicQuality"], s.musicQuality), -0.1f, 1.0f);
    s.maxRate = int(p.getInt("maxRate", s.maxRate));
    if (s.maxRate != 0 && (s.maxRate < 8000 || s.maxRate > 192000)) return fail("maxRate must be 0 or 8000..192000");
    s.monoSounds = p.getBool("monoSounds", s.monoSounds);
    s.longSound = std::max(0.0f, num(p["longSound"], s.longSound));
    s.reencodeLossy = p.getBool("reencodeLossy", s.reencodeLossy);
    return true;
}

// ---------------------------------------------------------------- conversion

uint64_t Report::before() const { uint64_t n = 0; for (auto& i : items) n += i.before; return n; }
uint64_t Report::after() const { uint64_t n = 0; for (auto& i : items) n += i.after; return n; }

std::string Report::text() const {
    std::ostringstream s;
    for (auto& i : items) {
        s << "  " << i.from;
        if (i.to != i.from) s << " -> " << i.to;
        s << " [" << i.codec << "] " << formatKb(i.before) << " -> " << formatKb(i.after) << "\n";
    }
    if (!items.empty()) s << "  звук всего: " << formatKb(before()) << " -> " << formatKb(after()) << "\n";
    for (auto& w : warnings) s << "  предупреждение: " << w << "\n";
    return s.str();
}

namespace {

struct Job {
    std::string path;
    Codec codec = Codec::Keep, longCodec = Codec::Keep;  // longCodec: when a sound is longer than longSound
    float quality = 0.5f, longQuality = 0.4f;
    bool mono = false, isMusic = false, forced = false, loop = false;
};

} // namespace

bool convert(const Json& bank, const FileReader& read, const Settings& st, Output& out, std::string* error) {
    out = Output{};
    auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
    std::map<std::string, Job> jobs;  // by source path (sorted: reproducible order)
    auto addJob = [&](const std::string& path, const Job& j, const std::string& who) {
        auto [it, inserted] = jobs.emplace(path, j);
        if (!inserted && (it->second.codec != j.codec || it->second.mono != j.mono || it->second.isMusic != j.isMusic))
            out.report.warnings.push_back(who + ": '" + path + "' is shared with another entry; the first one's settings are used");
    };

    for (auto& [name, d] : bank["sounds"].members()) {
        const std::string who = "sound '" + name + "'";
        Job j;
        const std::string bus = d.getStr("bus", "sfx");
        const bool spatial = d.getBool("spatial", bus == "sfx" || bus == "ambient");
        const bool musicLike = bus == "music";
        j.codec = musicLike ? st.music : st.sounds;
        j.quality = musicLike ? st.musicQuality : st.soundQuality;
        j.longCodec = st.music;
        j.longQuality = st.musicQuality;
        if (d.has("compress")) {
            if (!codecFromName(d.getStr("compress"), j.codec)) return fail(who + ": unknown codec '" + d.getStr("compress") + "' (keep, pcm, adpcm, vorbis)");
            j.longCodec = j.codec;
            j.forced = true;
        }
        if (d.has("quality")) j.quality = j.longQuality = std::clamp(num(d["quality"], j.quality), -0.1f, 1.0f);
        j.mono = st.monoSounds && spatial;
        j.loop = d.getBool("loop", false);
        if (d["file"].isString()) addJob(d.getStr("file"), j, who);
        for (auto& f : d["files"].items()) if (f.isString()) addJob(f.str(), j, who);
    }
    for (auto& [name, m] : bank["music"].members()) {
        const std::string who = "music '" + name + "'";
        Job j;
        j.isMusic = true;
        j.codec = j.longCodec = st.music;
        j.quality = j.longQuality = st.musicQuality;
        const Json* list = &m;
        if (m.isObject()) {
            if (m.has("compress") && !codecFromName(m.getStr("compress"), j.codec)) return fail(who + ": unknown codec '" + m.getStr("compress") + "'");
            j.longCodec = j.codec;
            if (m.has("quality")) j.quality = j.longQuality = std::clamp(num(m["quality"], j.quality), -0.1f, 1.0f);
            list = &m["files"];
        }
        auto addEntry = [&](const Json& e) {
            if (e.isString()) addJob(e.str(), j, who);
            else if (e.isObject() && e["file"].isString()) addJob(e.getStr("file"), j, who);
        };
        if (list->isArray()) for (auto& e : list->items()) addEntry(e);
        else addEntry(*list);
        if (m.isObject()) for (auto& l : m["layers"].items()) addEntry(l);
    }

    std::set<std::string> taken;   // every path of the result
    for (auto& [p, unused] : jobs) taken.insert(p);
    std::map<std::string, std::string> renamed;
    for (auto& [path, j] : jobs) {
        std::string bytes;
        if (!read(path, bytes)) { out.report.warnings.push_back("file not found '" + path + "'"); continue; }
        Report::Item item{path, path, "keep", bytes.size(), bytes.size()};
        Codec codec = j.codec;
        const bool lossy = isLossy(path, bytes);
        audio::Clip clip;
        std::string why;
        if (codec != Codec::Keep && !(lossy && !st.reencodeLossy && !j.forced)) {
            if (!audio::decodeClipAs(bytes, j.mono ? 1 : 0, 0, clip, why)) {
                out.report.warnings.push_back("'" + path + "': " + why + " — kept as is");
                codec = Codec::Keep;
            } else {
                if (!j.isMusic && clip.seconds() > st.longSound) { codec = j.longCodec; j.quality = j.longQuality; }
                if (st.maxRate > 0 && clip.sampleRate > st.maxRate && !audio::decodeClipAs(bytes, j.mono ? 1 : 0, st.maxRate, clip, why))
                    return fail("'" + path + "': resampling failed: " + why);
            }
        } else {
            codec = Codec::Keep;
        }
        std::string encoded, ext;
        switch (codec) {
        case Codec::Keep: break;
        case Codec::Pcm: encoded = encodeWav(clip.pcm, clip.channels, clip.sampleRate); ext = ".wav"; break;
        case Codec::Adpcm: encoded = encodeAdpcm(clip.pcm, clip.channels, clip.sampleRate, j.loop || j.isMusic); ext = ".wav"; break;
        case Codec::Vorbis:
            if (!encodeVorbis(clip.pcm, clip.channels, clip.sampleRate, j.quality, fnv(path), encoded, &why)) return fail("'" + path + "': " + why);
            ext = ".ogg";
            break;
        }
        if (codec != Codec::Keep && encoded.size() < bytes.size()) {
            std::string to = fs::path(path).replace_extension(ext).generic_string();
            if (to != path && taken.count(to)) to = fs::path(path).replace_extension(std::string(".") + codecName(codec) + ext).generic_string();
            taken.insert(to);
            item.to = to;
            item.codec = codecName(codec);
            if (codec == Codec::Vorbis) { char q[16]; std::snprintf(q, sizeof(q), " q%.2f", double(j.quality)); item.codec += q; }
            if (clip.channels == 1 && j.mono) item.codec += ", моно";
            if (st.maxRate > 0 && clip.sampleRate == st.maxRate) item.codec += ", " + std::to_string(clip.sampleRate / 1000) + " кГц";
            item.after = encoded.size();
            out.files[to] = std::move(encoded);
            if (to != path) { renamed[path] = to; out.replaced.push_back(path); }
        } else if (codec != Codec::Keep) {
            item.codec = "keep (меньше не стало)";
        }
        out.report.items.push_back(item);
    }

    // the bank with the new names; everything else is untouched
    Json b = bank;
    auto rename = [&](const Json& v) { auto it = renamed.find(v.str()); return Json::string(it == renamed.end() ? v.str() : it->second); };
    if (Json* sounds = b.find("sounds")) {
        for (auto& [name, unused] : bank["sounds"].members()) {
            Json* d = sounds->find(name);
            if (!d || !d->isObject()) continue;
            if ((*d)["file"].isString()) d->set("file", rename((*d)["file"]));
            if ((*d)["files"].isArray()) {
                Json arr = Json::array();
                for (auto& f : (*d)["files"].items()) arr.push(f.isString() ? rename(f) : f);
                d->set("files", arr);
            }
        }
    }
    if (Json* music = b.find("music")) {
        auto entry = [&](const Json& e) -> Json {
            if (e.isString()) return rename(e);
            Json c = e;
            if (e.isObject() && e["file"].isString()) c.set("file", rename(e["file"]));
            return c;
        };
        auto list = [&](const Json& l) -> Json {
            if (!l.isArray()) return entry(l);
            Json arr = Json::array();
            for (auto& e : l.items()) arr.push(entry(e));
            return arr;
        };
        for (auto& [name, m] : bank["music"].members()) {
            if (!m.isObject()) { music->set(name, list(m)); continue; }
            Json* pm = music->find(name);
            if (m.has("files")) pm->set("files", list(m["files"]));
            if (m["layers"].isArray()) pm->set("layers", list(m["layers"]));
        }
    }
    out.audioJson = b.dump(2);
    return true;
}

namespace {
// File by file instead of fs::copy(recursive): libstdc++ on Windows (MinGW) fails with "File
// exists" when copying a directory into an existing one, which every staging does.
bool copyTree(const fs::path& from, const fs::path& to, std::error_code& ec) {
    fs::create_directories(to, ec);
    if (ec) return false;
    for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path out = to / it->path().lexically_relative(from);
        if (it->is_directory(ec)) fs::create_directories(out, ec);
        else if (it->is_regular_file(ec)) fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, ec);
        if (ec) return false;
    }
    return !ec;
}
} // namespace

bool stageGame(const std::string& projectDir, const std::string& stagingDir, const std::string& profile, Report& report, std::string* error) {
    report = Report{};
    std::error_code ec;
    const fs::path src = fs::absolute(projectDir), dst = stagingDir;
    fs::remove_all(dst, ec);
    ec.clear();
    if (!copyTree(src, dst, ec)) { if (error) *error = "audio: cannot copy " + src.string() + ": " + ec.message(); return false; }

    auto readFile = [&](const fs::path& p, std::string& out) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    };
    std::string text;
    Json manifest;
    try {
        if (readFile(src / "game.json", text)) manifest = Json::parse(text);
    } catch (const std::exception&) {} // the game's own loader reports a broken manifest
    const std::string bankName = manifest.getStr("audio", "audio.json");
    if (!readFile(src / bankName, text)) return true;   // no sound: nothing to do
    try {
        Json bank = Json::parse(text);
        Settings st;
        if (!settingsFor(bank, profile, st, error)) return false;
        if (st.sounds == Codec::Keep && st.music == Codec::Keep && st.maxRate == 0) return true;
        // content paths only: nothing outside the game folder can be pulled into a build
        const fs::path root = fs::weakly_canonical(src);
        auto read = [&](const std::string& rel, std::string& out) {
            const fs::path p = fs::weakly_canonical(root / rel);
            const auto r = std::mismatch(root.begin(), root.end(), p.begin());
            return r.first == root.end() && readFile(p, out);
        };
        Output o;
        if (!convert(bank, read, st, o, error)) return false;
        for (auto& r : o.replaced) fs::remove(dst / r, ec);
        for (auto& [path, bytes] : o.files) {
            fs::create_directories((dst / path).parent_path(), ec);
            std::ofstream f(dst / path, std::ios::binary);
            f << bytes;
            if (!f) { if (error) *error = "audio: cannot write " + (dst / path).string(); return false; }
        }
        std::ofstream f(dst / bankName, std::ios::binary);
        f << o.audioJson;
        report = std::move(o.report);
    } catch (const std::exception& e) {
        if (error) *error = "audio: " + bankName + ": " + e.what();
        return false;
    }
    return true;
}

} // namespace rts::audiopack
