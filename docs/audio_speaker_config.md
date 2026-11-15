# Speaker configuration

> Menu: **Audio → Output → Speaker configuration**, **LFE crossover** · Setting ids: `audio.output.speakers`, `audio.output.lfe_crossover` · Owner module: `audio`
> Values: Auto (device), Mono, Stereo speakers, Headphones, Quadraphonic, 5.1 surround, 7.1 surround · LFE crossover 40–200 Hz (default 80) · Presets: none

## Definition
Tells the game how many speakers you have and where they are, so a sound behind you comes out of
the rear speakers. "Headphones" is stereo but tells the game the two drivers are on your ears, which
matters for [HRTF](audio_spatial_hrtf.md).

## How it works
The mixer renders directly into the chosen channel layout. Each layout is a list of speaker
azimuths (degrees, 0 = front, +90 = right), following ITU-R BS.775:

| Layout | Channels (order) | Azimuths |
|---|---|---|
| Mono | C | 0 |
| Stereo / Headphones | FL FR | −30, +30 |
| Quad | FL FR BL BR | −45, +45, −135, +135 |
| 5.1 | FL FR C LFE SL SR | −30, +30, 0, –, −110, +110 |
| 7.1 | FL FR C LFE BL BR SL SR | −30, +30, 0, –, −150, +150, −90, +90 |

Panning (VBAP, see [3D audio](audio_spatial_hrtf.md)) uses those azimuths to pick the two speakers
around a sound's direction. The reverb return is spread to the rear speakers at −3 dB so rooms
surround you.

**LFE / bass management.** The ".1" channel is a subwoofer feed. This prototype derives it from the
mains: `LFE = LowPass4(sum of mains × 0.5)` using two cascaded Butterworth biquads (a 24 dB/oct
Linkwitz-Riley slope) at the crossover frequency. The mains stay full range ("large speakers"
mode). Film mixes and many games instead author explicit LFE content per sound (explosion sends);
the per-voice send would be one more gain in the spatializer.

**Auto** opens the device with its native channel count. If the OS reports an unusual count (3, 10…)
the mixer renders stereo and miniaudio up/down-mixes. If you pick 7.1 on a stereo device, miniaudio
down-mixes 7.1 → 2.0, so you still hear everything (rear content folded into the fronts).

## Breakdown
1. `openDevice`: `cfg.playback.channels` and an explicit `pChannelMap` that matches
   `dsp::makeLayout()` order.
2. `Mixer::configure`: allocates bus/master buffers for N channels.
3. Stage **Spatialize**: per voice, N gains (most are 0 with VBAP).
4. Stage **Reverb**: return added to FL/FR and the rears.
5. Stage **Bass management**: LFE channel overwritten with the low-passed sum.

| Resource | Size per channel | Notes |
|---|---|---|
| Bus buffers | 5 × 256 floats = 5 KB | |
| Master + interleave scratch | 2 × 256 floats = 2 KB | |
| Device buffer | period × periods × 4 B | 3.75 KB at 480 × 2 |

## Optimization
More channels mean more gains per voice and more bus memory, but VBAP keeps only two non-zero gains
per voice and the code skips zero-gain channels, so cost grows slowly.

Measured (`tools/audio_bench`, 69 voices, High preset):

| Value | Audio CPU (% of 1 core) | Bass management stage | RAM (KB) |
|---|---|---|---|
| Mono | 6.03 | 0.001 | 2184 |
| Stereo speakers | 6.59 | 0.001 | 2194 |
| Quadraphonic | 7.43 | 0.001 | 2216 |
| 5.1 surround | 7.39 | 0.033 | 2237 |
| 7.1 surround | 7.39 | 0.038 | 2259 |

Δ GPU 0. Surround costs ≈ +0.8 percentage points and +65 KB versus stereo.

## Rebuild
1. Pick a channel order and pass the matching channel mask/map to the OS (WASAPI
   `WAVEFORMATEXTENSIBLE.dwChannelMask`, e.g. `KSAUDIO_SPEAKER_7POINT1_SURROUND`); a mismatch swaps
   rear and side speakers.
2. Store speaker azimuths per layout; pan with VBAP between adjacent pairs.
3. Feed the LFE from a low-pass of the mains (or explicit sends), never full-range.
4. For headphones, keep 2 channels and enable HRTF instead of virtual-surround up-mixing.
5. Verify with a source orbiting the listener: it should move smoothly through each speaker.

Files: `src/modules/audio/audio_dsp.cpp` (`makeLayout`, `vbapGains`), `audio_mixer.cpp` (bass management), `audio_system.cpp` (`channelMap`).
