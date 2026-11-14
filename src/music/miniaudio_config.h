// The miniaudio switches, included before miniaudio.h everywhere (the implementation file and
// every user). Only the device (WASAPI shared mode and the null backend for tests), the WAV, FLAC
// and MP3 decoders, the ring buffer and the data converter are compiled in. The high-level engine
// and the resource manager are not used. stb_vorbis is never included before miniaudio, so its
// own Vorbis backend stays out; OGG goes through stb_vorbis's pull API directly.
#pragma once

#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#define MA_ENABLE_NULL
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
