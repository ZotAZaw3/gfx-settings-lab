# Sample rate, bit depth and dither

> Menu: **Audio → Output → Sample rate / Bit depth / Dither** · Setting ids: `audio.output.sample_rate`, `audio.output.bit_depth`, `audio.output.dither` · Owner module: `audio`
> Values: Device default, 22.05, 44.1, 48, 96 kHz · 16-bit integer, 24-bit integer, 32-bit float · Dither on/off · Presets: none

## Definition
**Sample rate** is how many times per second the waveform is measured. It sets the highest
frequency that can exist (half the rate: 24 kHz at 48 kHz). **Bit depth** is how finely each
measurement is stored; it sets the noise floor (16-bit ≈ −96 dB, 24-bit ≈ −144 dB, float has
effectively none). **Dither** adds a tiny amount of noise before rounding to integers so very quiet
sounds fade out smoothly instead of turning into grainy distortion.

## How it works
- The whole mixer runs at the chosen rate: every resampler step, filter coefficient, reverb delay
  length and the HRTF delay is derived from it. Sounds recorded at other rates (the demo music is
  44.1 kHz) are converted by the [resampler](audio_resampling.md).
- Mixing is always 32-bit float. Only the last stage converts to the device format:
  - f32: copy.
  - s16: `round(x × 32767 + d)`, clamp. `d` is TPDF dither: the difference of two uniform randoms, a
    triangle distribution of ±1 LSB, which makes the rounding error independent of the signal.
  - s24: same at ×8 388 607, packed into 3 bytes little-endian.
- Higher rates halve the time between samples; the buffer of N frames then lasts half as long, so
  latency in ms drops while CPU per second roughly doubles.

Why 48 kHz: it's the native rate of almost every PC, console and HDMI chain. 44.1 kHz forces the OS
to resample; 96 kHz doubles the mixer work for content above human hearing (~20 kHz).

## Breakdown
1. `openDevice`: `cfg.sampleRate`, `cfg.playback.format`.
2. `Mixer::configure(sampleRate, ...)`: reverb comb lengths ×(rate/44100), filter coefficients.
3. Every stage processes `rate` frames per second.
4. Stage **Output**: `renderToFormat` converts and dithers.

## Optimization
Measured (`tools/audio_bench`, 69 voices, High preset):

| Value | Audio CPU (% of 1 core) | Output stage | RAM (KB) | Latency at 480 × 2 (ms) |
|---|---|---|---|---|
| 22.05 kHz | 4.37 | – | 2136 | 43.5 |
| 44.1 kHz | 6.05 | – | 2186 | 21.8 |
| 48 kHz | 6.41 | – | 2194 | 20.0 |
| 96 kHz | 10.16 | – | 2303 | 10.0 |
| 16-bit + dither | 6.45 | 0.045 | 2191 | 20.0 |
| 16-bit, no dither | 6.47 | 0.024 | 2191 | 20.0 |
| 24-bit + dither | 6.53 | 0.047 | 2193 | 20.0 |
| 32-bit float | 6.70 | 0.002 | 2194 | 20.0 |

Δ GPU 0. Sample rate is the second biggest CPU lever after voice count (cost ∝ rate, minus fixed
per-block overhead). Bit depth and dither cost < 0.05% of a core; the totals differ only by run noise.

What to ship: 48 kHz, float to the OS (Windows' mixer is float internally, so 16/24-bit only matter
in exclusive mode or for bit-perfect output). Always dither when going to 16-bit.

## Rebuild
1. Mix in float at one internal rate; resample sources on the way in, not the output.
2. Recompute every rate-dependent constant on rate change (filter coefficients, delay lengths,
   attack/release coefficients).
3. Convert to integers at the very end with TPDF dither: `y = round(x·scale + (r1 − r2))`.
4. Verify dither: render a −90 dBFS sine fading out to 16-bit; without dither the tail turns into
   a buzzy square-ish wave, with dither it fades into soft hiss.

Files: `src/modules/audio/audio_mixer.cpp` (`renderToFormat`), `audio_dsp.h` (`Dither`), `audio_system.cpp`.
