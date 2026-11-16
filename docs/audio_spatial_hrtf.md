# 3D audio: stereo pan, speaker panning (VBAP) and HRTF

> Menu: **Audio → 3D Audio → 3D audio** · Setting id: `audio.3d.spatial` · Owner module: `audio`
> Values: Stereo pan, Speaker panning (VBAP), HRTF (binaural headphones) · Presets: none (depends on your hardware)

## Definition
How the game turns "this sound is over there" into what reaches your ears.
**Stereo pan** only knows left/right. **Speaker panning** places sounds between your actual
speakers (front, side, rear). **HRTF** (head-related transfer function) imitates how your head and
ears filter sound, so headphones can give a sense of front/back and direction. It's the same family as
Windows Sonic, Dolby Atmos for Headphones, DTS Headphone:X, Steam Audio, Tempest 3D.

## How it works
For every 3D voice, once per block, the mixer converts the source position into listener space
(`right`, `forward` from the camera) and computes azimuth `az = atan2(x, z)`.

**Stereo pan** — `pan = sin(az)`, constant-power law `L = cos((pan+1)·π/4)`, `R = sin(...)`.
A sound directly behind is centered, same as directly ahead.

**Speaker panning (2D VBAP)** — sort speakers by azimuth, find the adjacent pair `(a, b)` that
brackets `az`, `t = (az − a)/(b − a)`, gains `cos(t·π/2)` and `sin(t·π/2)`. Exactly 2 speakers
play each sound (constant power). Near the listener (`d < refDistance`) a spread term blends toward
all speakers so sources don't snap between speakers as they pass through your head.

**HRTF (spherical-head model, Brown & Duda 1998)** — rendered to 2 channels only (Stereo/Headphones):
1. **ITD** (interaural time difference): Woodworth formula
   `itd = a/c · (θ + sin θ)` with head radius `a = 8.75 cm`, `c = 343 m/s`, `θ` = lateral angle.
   Up to 0.66 ms. The far ear reads a fractional-delay line (128 samples, linear interpolation),
   ramped across the block so moving sources don't click.
2. **ILD / head shadow**: a 1-pole/1-zero filter per ear,
   `H(s) = (1 + α·s/2ω0) / (1 + s/2ω0)`, `ω0 = c/a`, `α(θ) = 1.05 + 0.95·cos(θ/150°·π)` — boosts treble on
   the near ear (α≈2), cuts it on the far ear (α≈0.1). Bilinear-transformed to a digital IIR.
3. **Front/back cue**: a low-pass that closes from 18 kHz (front) to 5 kHz (directly behind),
   standing in for pinna shadowing.

This model gets left/right and front/back right and costs little. Commercial HRTF uses measured
impulse responses (HRIRs, e.g. from SOFA files, 128–512 taps per ear) convolved per voice, often
with partitioned FFT convolution and interpolation between measured directions; it adds elevation
and much stronger externalization, at 5–20× the cost. Windows Sonic / Atmos do this in the OS
when the game outputs to the spatial-audio API (ISpatialAudioClient) instead of a channel bed.

## Breakdown
1. Stage **Spatialize** per real 3D voice: listener-space direction → gains (pan/VBAP) or HRTF
   params → mix into the voice's bus.
2. HRTF path per sample: write delay line → 2 fractional taps → head-shadow IIR → rear LP → L/R.

| Resource | Size | Notes |
|---|---|---|
| HRTF delay line | 128 floats per real voice | allocated only when HRTF is on |
| Filter state | 6 floats per voice | |

## Optimization
Measured (`tools/audio_bench`, 69 real voices, stereo 48 kHz):

| Value | Audio CPU (% of 1 core) | Spatialize stage | RAM (KB) |
|---|---|---|---|
| Stereo pan | 6.20 | 0.55 | 2194 |
| Speaker panning (VBAP) | 6.36 | 0.61 | 2194 |
| HRTF | 10.01 | 4.33 | 2242 (+48 KB) |

Δ GPU 0. HRTF is ~7× the spatialize cost because it is per-sample work per voice (delay + 3 filters)
instead of a per-block gain. Production tricks: HRTF only the nearest/most important 16–32 voices and
pan the rest; or mix all voices into an ambisonic bed and run a single binaural decode (cost
independent of voice count); SIMD across voices.

Pick HRTF only with Headphones/Stereo; on speakers, crosstalk between speakers destroys the cues
(the mixer silently uses VBAP when HRTF is selected with more than 2 channels).

## Rebuild
1. Build a listener basis from the camera (forward, up, right = forward × up); project `source − listener`.
2. Panning: implement constant-power pan; then VBAP with your speaker azimuth table.
3. HRTF: start with ITD (fractional delay) + head-shadow filter; then upgrade to HRIR convolution:
   load a SOFA set, pick/interpolate the nearest 3 measured directions, convolve with overlap-add,
   crossfade when the direction changes.
4. Smooth every parameter across the block.
5. Verify with eyes closed: a source orbiting at ear height should go round, not bounce left/right.

Files: `src/modules/audio/audio_dsp.cpp` (`vbapGains`, `stereoPanGains`, `HeadShadow`), `audio_mixer.cpp` (`renderVoice`, HRTF block).
