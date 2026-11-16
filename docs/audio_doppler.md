# Doppler effect

> Menu: **Audio → 3D Audio → Doppler effect / Doppler strength** · Setting ids: `audio.3d.doppler`, `audio.3d.doppler_factor` · Owner module: `audio`
> Values: on/off · strength 0–3 (1 = physical) · Presets: none

## Definition
The pitch of a moving sound rises as it approaches and drops as it leaves: the "neeeee-oww" of a
car or jet passing by. Strength 1 is physically correct; many games use 0.3–0.7 because full
Doppler on fast vehicles makes music-like sounds (engines, sirens) sound out of tune.

## How it works
OpenAL 1.1 formula, with `SL` the vector from source to listener, `c = 343 m/s`, `DF` the strength:

```
vls = dot(SL, listenerVelocity) / |SL|      // listener speed along the line
vss = dot(SL, sourceVelocity)   / |SL|      // source speed along the line
pitch = (c − DF·vls) / (c − DF·vss)          // clamped to [0.25, 4]
```

The pitch multiplies the voice's resample step, so Doppler is "free" once you have a resampler.
It is smoothed per block (`p += (target − p)·0.3`) so velocity jitter from frame timing does not warble.

Velocities: the listener's comes from the camera (`(pos − lastPos)/dt`, ignored above 100 m/s so
teleports don't cause a sweep); sources pass theirs to `setVoicePosition`. Supersonic speeds are
clamped just below `c`.

In the demo, a car passes 2 m in front of the origin at 30 m/s every 9 s: about +9% pitch
approaching, −8% leaving.

## Breakdown
1. Stage **Spatialize** (parameter block): compute `pitch` from listener/source velocities.
2. Stage **Resample**: `step = pitch × doppler × srcRate / outRate`.

## Optimization
Measured (`tools/audio_bench`, 69 voices, car moving at 30 m/s):

| Value | Audio CPU (% of 1 core) | Spatialize stage | RAM |
|---|---|---|---|
| Off | 6.31 | 0.61 | 0 |
| On | 6.78 | 0.68 | 0 |

≈ 0.07% of a core (two dot products per voice per block). Δ GPU 0, Δ RAM 0. Doppler is effectively
free; the reason to turn it down is taste, not performance. It does force resampling of sounds that
would otherwise play at native rate.

## Rebuild
1. Track velocity for the listener and every moving source (or derive from position deltas, smoothed).
2. Compute the pitch ratio per block with the formula above; clamp and smooth it.
3. Multiply into the resampler step; make sure the resampler handles ratios up to ~2–4.
4. Expose a per-sound Doppler scale (music and UI should have 0).
5. Verify: a source passing at 30 m/s should drop about 3 semitones overall as it passes (+9.6% approaching, −8% leaving).

Files: `src/modules/audio/audio_mixer.cpp` (`renderVoice`, Doppler block), `audio_module.cpp` (listener velocity).
