// The one translation unit that compiles miniaudio (single-header library,
// public domain / MIT-0, https://miniaud.io). Only the device, decoder and
// encoder layers are used; all mixing/DSP is our own code in this folder.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENGINE       // we use our own mixer instead of ma_engine
#define MA_NO_GENERATION
#include "../../../third_party/miniaudio/miniaudio.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
