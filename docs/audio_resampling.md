# Resampling quality

> Menu: **Audio → Quality → Resampling quality** · Setting id: `audio.quality.resampler` · Owner module: `audio`
> Values: Nearest, Linear, Cubic Hermite, Windowed sinc (8 tap) · Presets: Low=Linear, Medium=Cubic, High=Cubic, Ultra=Sinc

## Definition
Sounds are rarely played at exactly their recorded speed: they're recorded at 44.1 kHz but the
device runs at 48 kHz, they're pitched up and down for variety, and Doppler bends them in real time.
The resampler reads "in between" the recorded samples. Better interpolation keeps high frequencies
clean; worse interpolation adds a metallic, aliased fizz, most audible on pitched-up sounds.

## How it works
Each real voice keeps a fractional playhead `pos` and a step
`step = pitch × doppler × sourceRate / outputRate`. For every output sample it reads the source
around `pos`, then `pos += step`.

| Mode | Formula | Taps | Character |
|---|---|---|---|
| Nearest | `x[round(pos)]` | 1 | Harsh aliasing and zipper, retro |
| Linear | `x0 + (x1 − x0)·t` | 2 | Dull highs + some aliasing |
| Cubic Hermite (Catmull-Rom) | `((c3·t + c2)·t + c1)·t + x0` | 4 | Good, the usual game default |
| Windowed sinc | `Σ x[n−3+k] · h[phase][k]` | 8 | Closest to ideal band-limited |

The sinc table is built once: 513 fractional phases × 8 taps, a sinc with cutoff at 0.92 × Nyquist,
Blackman-windowed and normalized per phase so DC gain is exactly 1. Lookup picks the nearest phase,
so there is no per-sample `sin()`.

To feed the kernels, each voice keeps a contiguous window of source frames with 4 frames of history
before `floor(pos)` and 5 after the last read (`kResampleTapsBefore/After`). Each block fetches what
it needs (with loop wrap for looping sounds, or from the stream ring buffer) and shifts out consumed
frames.

## Breakdown
1. Stage **Resample**, per real voice per block: fetch → `dsp::resample(quality, ...)` per source
   channel → shift the window.
2. Output: 256-frame mono or stereo scratch, consumed by the filter and spatializer.

## Optimization
Measured (`tools/audio_bench`, 69 real voices, 48 kHz):

| Value | Audio CPU (% of 1 core) | Resample stage | RAM (KB) |
|---|---|---|---|
| Nearest | 5.50 | 2.80 | 2194 |
| Linear | 5.86 | 3.11 | 2194 |
| Cubic Hermite | 6.28 | 3.62 | 2194 |
| Windowed sinc, 8 tap | 6.57 | 3.92 | 2194 |

Δ RAM 0 (the 4 KB sinc table is shared), Δ GPU 0. Most of the Resample stage is the fetch and window
management that every mode pays; the kernel itself adds ≈ 0.3% (linear) to 1.1% (sinc) of a core.

Notes:
- The sinc filter is fixed at 0.92 × output Nyquist. When `step > 1` (pitching up) the cutoff should
  scale down by `1/step` to stay alias-free; production resamplers (libsamplerate, r8brain, Wwise)
  pick a filter per ratio. That would be the next upgrade here.
- Cheap wins: resample at load time when a sound's rate doesn't match and its pitch never changes;
  SIMD the 8-tap dot product (4–8× faster).

## Rebuild
1. Keep a double-precision playhead per voice (float runs out of precision after a few minutes).
2. Implement linear first; then cubic Hermite (4 taps).
3. For sinc: precompute a polyphase table; normalize each phase's taps to sum to 1.
4. Keep enough history/look-ahead frames around block boundaries, or you'll hear clicks every block.
5. Verify: play a 15 kHz sine pitched ×1.3 and look at a spectrum; nearest/linear show aliases
   folding down, sinc shows (almost) none.

Files: `src/modules/audio/audio_dsp.cpp` (`resample`, `buildSinc`), `audio_mixer.cpp` (`fetch`, `renderVoice`).
