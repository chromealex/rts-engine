// File decoding (WAV, OGG Vorbis, MP3, FLAC) through miniaudio's decoders.
#include "MiniaudioConfig.h"
#include "AudioMixer.h"

namespace rts::audio {

namespace {

bool initDecoder(const std::string& bytes, ma_format fmt, ma_uint32 channels, ma_uint32 rate, ma_decoder& dec) {
    ma_decoder_config cfg = ma_decoder_config_init(fmt, channels, rate);
    return ma_decoder_init_memory(bytes.data(), bytes.size(), &cfg, &dec) == MA_SUCCESS;
}

} // namespace

bool decodeClip(const std::string& bytes, Clip& out, std::string& error) {
    out = Clip{};
    if (bytes.empty()) { error = "empty file"; return false; }
    ma_decoder dec;
    if (!initDecoder(bytes, ma_format_s16, 0, 0, dec)) { error = "unsupported or corrupted audio (WAV, OGG, MP3, FLAC)"; return false; }
    if (dec.outputChannels > 2) { // surround sources are downmixed to stereo by the decoder
        ma_decoder_uninit(&dec);
        if (!initDecoder(bytes, ma_format_s16, 2, 0, dec)) { error = "cannot downmix to stereo"; return false; }
    }
    out.channels = int(dec.outputChannels);
    out.sampleRate = int(dec.outputSampleRate);
    ma_uint64 total = 0;
    if (ma_decoder_get_length_in_pcm_frames(&dec, &total) == MA_SUCCESS && total > 0 && total < (1ull << 31))
        out.pcm.reserve(size_t(total) * size_t(out.channels));
    int16_t chunk[4096 * 2];
    const ma_uint64 chunkFrames = sizeof(chunk) / sizeof(chunk[0]) / ma_uint64(out.channels);
    for (;;) {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&dec, chunk, chunkFrames, &got);
        out.pcm.insert(out.pcm.end(), chunk, chunk + size_t(got) * size_t(out.channels));
        if (r != MA_SUCCESS || got < chunkFrames) break;
    }
    ma_decoder_uninit(&dec);
    if (out.frames() == 0) { error = "no audio frames decoded"; return false; }
    return true;
}

struct StreamDecoder::Impl {
    ma_decoder dec{};
    bool open = false;
};

StreamDecoder::StreamDecoder() : impl_(std::make_unique<Impl>()) {}
StreamDecoder::~StreamDecoder() { close(); }

bool StreamDecoder::open(std::shared_ptr<const std::string> bytes, int sampleRate) {
    close();
    if (!bytes || bytes->empty()) return false;
    if (!initDecoder(*bytes, ma_format_f32, 2, ma_uint32(sampleRate), impl_->dec)) return false;
    bytes_ = std::move(bytes);
    impl_->open = true;
    return true;
}

void StreamDecoder::close() {
    if (impl_->open) ma_decoder_uninit(&impl_->dec);
    impl_->open = false;
    bytes_.reset();
}

bool StreamDecoder::isOpen() const { return impl_->open; }

int StreamDecoder::read(float* stereo, int frames) {
    if (!impl_->open || frames <= 0) return 0;
    ma_uint64 got = 0;
    ma_decoder_read_pcm_frames(&impl_->dec, stereo, ma_uint64(frames), &got);
    return int(got);
}

} // namespace rts::audio
