# libogg + libvorbis (encoder side of the build tools)

| Library | Version | License |
|---|---|---|
| libogg | github.com/xiph/ogg @ 06a5e02 | BSD-3-Clause — `LICENSE-ogg.txt` |
| libvorbis | github.com/xiph/vorbis @ 1b75110 | BSD-3-Clause — `LICENSE-vorbis.txt` |

Used only by host tools (`rts_audiopack`, `rtsbuild`) to encode OGG Vorbis at build time; games
decode with stb_vorbis (`third_party/miniaudio`). `include/ogg/config_types.h` is written by hand
(stdint types) instead of the configure-generated one; `barkmel.c`, `psytune.c`, `tone.c`
(standalone programs) are not vendored.
