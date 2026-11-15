# miniaudio 0.11.21

Single-header audio library by David Reid, https://miniaud.io, https://github.com/mackron/miniaudio
(tag 0.11.21). Dual licensed: public domain (Unlicense) or MIT No Attribution; see the end of
`miniaudio.h`.

Used by `src/modules/audio/` for device I/O (WASAPI / CoreAudio / PulseAudio / ALSA / null), file
decoding (WAV / FLAC / MP3) and WAV encoding only. Compiled once in
`src/modules/audio/miniaudio_impl.cpp` with `MA_NO_ENGINE`; all mixing and DSP is our own code.
