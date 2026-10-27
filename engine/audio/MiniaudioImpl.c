// The one translation unit with the miniaudio + stb_vorbis implementations.
// On Apple platforms it is compiled through MiniaudioImpl.m as Objective-C (AVAudioSession code).
#define MINIAUDIO_IMPLEMENTATION
#include "MiniaudioConfig.h"
#undef STB_VORBIS_HEADER_ONLY
#include "../../third_party/miniaudio/stb_vorbis.c"
