// Shared miniaudio configuration: every file that includes miniaudio.h goes through this header so
// declarations and the single implementation (MiniaudioImpl.c) agree.
#pragma once
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_GENERATION
#define MA_NO_ENCODING
#if defined(__APPLE__)
// link CoreAudio/AudioToolbox normally instead of dlopen: runtime linking can fail notarization
#define MA_NO_RUNTIME_LINKING
#endif
#define STB_VORBIS_HEADER_ONLY
#include "../../third_party/miniaudio/stb_vorbis.c"
#include "../../third_party/miniaudio/miniaudio.h"
