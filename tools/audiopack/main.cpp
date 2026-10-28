// rts_audiopack — build-time audio compression outside rtsbuild (preview, CI, single files).
//
//   rts_audiopack --project templates/skirmish --profile mobile [--out dir]
//       converts the game's audio for a profile; without --out only prints what it would do
//   rts_audiopack --encode in.wav out.ogg --codec vorbis [--quality 0.4] [--mono] [--rate 32000]
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "engine/audio/Audio.h"
#include "tools/audiopack/AudioPack.h"

namespace fs = std::filesystem;
using namespace rts;

static int usage() {
    std::puts("usage: rts_audiopack --project <game dir> --profile <desktop|mobile|web> [--out <dir>]\n"
              "       rts_audiopack --encode <in> <out> --codec <pcm|adpcm|vorbis> [--quality q] [--mono] [--rate hz]");
    return 2;
}

int main(int argc, char** argv) {
    std::string project, profile = "desktop", outDir, in, outFile, codec = "vorbis";
    float quality = 0.4f;
    int rate = 0;
    bool mono = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--project") project = next();
        else if (a == "--profile") profile = next();
        else if (a == "--out") outDir = next();
        else if (a == "--encode") { in = next(); outFile = next(); }
        else if (a == "--codec") codec = next();
        else if (a == "--quality") quality = std::strtof(next().c_str(), nullptr);
        else if (a == "--rate") rate = std::atoi(next().c_str());
        else if (a == "--mono") mono = true;
        else return usage();
    }
    std::string err;
    if (!in.empty()) {
        std::ifstream f(in, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        audio::Clip clip;
        if (!f || !audio::decodeClipAs(ss.str(), mono ? 1 : 0, rate, clip, err)) { std::fprintf(stderr, "%s: %s\n", in.c_str(), err.c_str()); return 1; }
        std::string bytes;
        if (codec == "pcm") bytes = audiopack::encodeWav(clip.pcm, clip.channels, clip.sampleRate);
        else if (codec == "adpcm") bytes = audiopack::encodeAdpcm(clip.pcm, clip.channels, clip.sampleRate);
        else if (codec == "vorbis") { if (!audiopack::encodeVorbis(clip.pcm, clip.channels, clip.sampleRate, quality, 1, bytes, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; } }
        else return usage();
        std::ofstream o(outFile, std::ios::binary);
        o << bytes;
        std::printf("%s -> %s: %zu -> %zu bytes\n", in.c_str(), outFile.c_str(), size_t(ss.str().size()), bytes.size());
        return o ? 0 : 1;
    }
    if (project.empty()) return usage();
    const fs::path staging = outDir.empty() ? fs::temp_directory_path() / "rts_audiopack_preview" : fs::path(outDir);
    audiopack::Report report;
    if (!audiopack::stageGame(project, staging.string(), profile, report, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::printf("audio, profile %s:\n%s", profile.c_str(), report.items.empty() ? "  nothing to convert\n" : report.text().c_str());
    if (outDir.empty()) fs::remove_all(staging);
    else std::printf("staged into %s\n", staging.string().c_str());
    return 0;
}
