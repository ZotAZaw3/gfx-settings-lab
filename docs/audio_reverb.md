# Reverb

> Menu: **Audio → Environment → Reverb quality / Reverb level / Room size / Damping** · Setting ids: `audio.env.reverb`, `audio.env.reverb_level`, `audio.env.room_size`, `audio.env.damping` · Owner module: `audio`
> Values: Off, Low (mono, 4 comb), High (stereo, 8 comb) · level/room/damping 0–1 · Presets: Low=Low, Medium=Low, High=High, Ultra=High

## Definition
Reverb is the sound of the room: thousands of reflections off walls that blur into a tail after
every sound. It tells you whether you are in a cathedral, a corridor or outdoors. Higher quality
gives a smoother, wider, less "metallic" tail.

## How it works
One shared reverb unit on a send bus (not one per voice). Every voice adds
`mono × send` into a send buffer; the reverb processes that once per block and its output is added
back to the master.

The algorithm is **Freeverb** (Jezar at Dreampoint, public domain), a Schroeder/Moorer design:

```
in ×0.015 ─┬─ comb 1116 ─┐
           ├─ comb 1188 ─┤
           ├─  ...       ├─ Σ ─ allpass 556 ─ allpass 441 ─ allpass 341 ─ allpass 225 ─ out
           └─ comb 1617 ─┘
comb:    y = buf[i];  lp = y·(1−damp) + lp·damp;  buf[i] = x + lp·feedback
allpass: b = buf[i];  y = −x + b;  buf[i] = x + 0.5·b
feedback = 0.7 + 0.28·roomSize     damp = 0.4·damping
```

Lengths are in samples at 44.1 kHz and scaled by `rate/44100`; mutually prime-ish lengths keep
echoes from lining up. The low-pass inside each comb makes highs die faster than lows, like real
absorptive surfaces.

- **Low**: 4 combs + 2 allpasses, one channel copied to L and R.
- **High**: 8 combs + 4 allpasses per side; right side lengths +23 samples for stereo decorrelation.
- The return is added to FL/FR and at −3 dB to rear speakers on surround layouts.

Reverb level is the wet gain. Per-voice send scales with `√distanceGain` and rises with occlusion,
so far and hidden sources sound more "roomy" (see [distance](audio_distance_attenuation.md)).

## Breakdown
1. Stage **Spatialize**: each voice adds to `sendBuf_` (mono, 256 frames).
2. Stage **Reverb**: `reverb_.process(send, wetL, wetR)` then `master += wet × reverbLevel`.

| Resource | Size (48 kHz) | Notes |
|---|---|---|
| Comb delay lines | Σ lengths ≈ 11 000 floats per side | |
| Allpass delay lines | ≈ 1 700 floats per side | |
| High total | ≈ 108 KB | Low ≈ 25 KB |

## Optimization
Measured (`tools/audio_bench`, 69 voices):

| Value | Audio CPU (% of 1 core) | Reverb stage | RAM (KB) |
|---|---|---|---|
| Off | 6.09 | 0.001 | 2086 |
| Low (mono, 4 comb) | 6.34 | 0.044 | 2112 (+26) |
| High (stereo, 8 comb) | 6.44 | 0.153 | 2194 (+108) |

Δ GPU 0. Because it's one shared send effect, reverb cost doesn't grow with voice count; even High
is ≈ 0.15% of a core. That's why games can afford several reverb buses (one per room/zone) and
crossfade between them.

Higher-end options: feedback delay networks (FDN, 8–16 lines with a Hadamard mixing matrix; smoother
than Freeverb), convolution with a measured impulse response (partitioned FFT; 2–5× the cost, exact
rooms), and geometric/ray-traced acoustics (Steam Audio, Project Acoustics) that bake early
reflections per location.

## Rebuild
1. Create a send bus; voices add `signal × send` to it.
2. Implement comb (with damping) and allpass delay lines; 8 combs in parallel → 4 allpasses in series.
3. Scale delay lengths with sample rate; offset the right channel's lengths for width.
4. Map UI to feedback (decay) and damping; expose wet level.
5. Add pre-delay (time before the tail) and early reflections for a more realistic room.
6. Verify: a short click should produce a smooth tail without flutter or ringing at one pitch.

Files: `src/modules/audio/audio_dsp.cpp` (`Reverb`), `audio_mixer.cpp` (reverb stage, sends).
