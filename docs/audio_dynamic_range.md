# Dynamic range (night mode)

> Menu: **Audio → Dynamics → Dynamic range** · Setting id: `audio.mix.dynamic_range` · Owner module: `audio`
> Values: Wide (home theater), Medium (TV), Narrow (night mode) · Presets: none (listening environment)

## Definition
Dynamic range is the gap between the quietest and loudest sounds. **Wide** keeps whispers quiet
and explosions huge, for good speakers in a quiet room. **Narrow / night mode** squeezes them
together so you can hear dialogue without explosions waking the house. TV speakers and phones sit
in the middle. Games call this "Dynamic range", "Audio mode", "Night mode" or "Mix preset".

## How it works
A feed-forward **compressor** on the master, before master volume:

```
peak      = max |sample| over all channels (stereo-linked, so the image doesn't shift)
levelDb   = 20·log10(peak)
reduction = staticCurve(levelDb)          // dB to remove
env       = smooth(reduction, attack if rising else release)
gain      = 10^((makeup − env)/20)
```

Static curve with soft knee `k`: below `threshold − k/2` → 0; inside the knee →
`(1 − 1/ratio)·(over + k/2)²/(2k)`; above → `(1 − 1/ratio)·over`.

| Mode | Threshold | Ratio | Knee | Attack / release | Makeup |
|---|---|---|---|---|---|
| Wide | – (bypassed) | 1:1 | – | – | 0 dB |
| Medium | −20 dB | 2.5:1 | 6 dB | 10 / 200 ms | +5 dB |
| Narrow | −32 dB | 5:1 | 8 dB | 3 / 150 ms | +12 dB |

Makeup gain raises everything after compression, which is what makes quiet dialogue louder.

A **safety limiter** always runs after master volume (−1 dBFS, 20:1, 0.5 ms attack, 80 ms release),
followed by a hard clamp to ±1. It's not the user's setting; it stops the sum of many loud voices
from clipping the device.

## Breakdown
1. Stage **Dynamics**: `comp_.process(interleaved, n, channels)` (skipped when ratio ≤ 1).
2. Stage **Master**: volume → mono/balance → `limiter_.process` → clamp → meters.
3. Gain reduction is shown in Audio stats ("Compressor / limiter gain reduction").

## Optimization
Measured (`tools/audio_bench`, 69 voices plus explosions):

| Value | Audio CPU (% of 1 core) | Dynamics stage | RAM |
|---|---|---|---|
| Wide (home theater) | 6.63 | 0.001 | 0 |
| Medium (TV) | 6.59 | 0.113 | 0 |
| Narrow (night mode) | 6.74 | 0.112 | 0 |

≈ 0.11% of a core: a `log10` and a `pow` per frame. Δ RAM 0, Δ GPU 0. Cheaper: compute the detector
in the log domain once every 8–16 samples and interpolate gain; use a lookup for `dbToGain`.

Better quality: a look-ahead limiter (delay the signal 1–5 ms so the gain drops before the peak),
multiband compression (bass and voice compressed separately), and loudness-based ducking of music
under dialogue. Shipping games often combine this setting with different mix snapshots (EQ, bus
levels) per mode, e.g. Sony's and Microsoft's TV / headphones / home-theater targets.

## Rebuild
1. Measure level per sample (peak or RMS), convert to dB.
2. Compute gain reduction from threshold, ratio, knee.
3. Smooth with separate attack/release one-pole coefficients `e^(−1/(t·fs))`.
4. Apply `makeup − reduction` as linear gain to all channels.
5. Finish with a limiter and a clip.
6. Verify: play speech + explosions; in Narrow, the explosion peak in the meters should sit within
   ~10 dB of the speech, and the gain-reduction readout should jump on each blast.

Files: `src/modules/audio/audio_dsp.cpp` (`Compressor`, `configureDynamics`), `audio_mixer.cpp` (dynamics + master stages).
