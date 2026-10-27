# Vendored audio libraries

| File | Version | License |
|---|---|---|
| `miniaudio.h` | 0.11.25 (github.com/mackron/miniaudio @ 9634bed) | Public Domain (Unlicense) or MIT No Attribution — `LICENSE-miniaudio.txt` |
| `stb_vorbis.c` | 1.22 (from miniaudio `extras/`) | Public Domain or MIT — `LICENSE-stb_vorbis.txt` |

Compiled once, in `engine/audio/MiniaudioImpl.c`. Do not include the implementation anywhere else.
