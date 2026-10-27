# zstd 1.5.6 (vendored)

Source: https://github.com/facebook/zstd, tag v1.5.6. License: BSD (see LICENSE).

Generated with `build/single_file_libs/combine.py` (unmodified upstream script):

- `zstd.c`        full library (compression + decompression) — host tools only (assetpipe, rtsbuild, tests)
- `zstddeclib.c`  decoder only — linked into the game runtime to keep binaries small
- `zstd.h`        public header (`lib/zstd.h`)

Do not edit these files; regenerate from upstream instead.
