# Distance attenuation, rolloff and air absorption

> Menu: **Audio → 3D Audio → Distance attenuation / Rolloff factor / Air absorption** · Setting ids: `audio.3d.attenuation`, `audio.3d.rolloff`, `audio.3d.air_absorption` · Owner module: `audio`
> Values: None, Linear, Inverse distance, Exponential · Rolloff 0.1–4 · Air absorption on/off · Presets: air absorption Low=off, others on

## Definition
Sounds get quieter as they get farther away, and distant sounds also lose their treble because
air absorbs high frequencies. These options choose the curve, how steep it is, and whether the
treble loss is simulated. Steeper curves make the world feel bigger and quieter; flatter curves let
you hear far-away action.

## How it works
Each sound has a reference distance `ref` (full volume inside it) and a max distance `max`.
With `d` clamped to `[ref, max]` and rolloff `r`:

| Model | Gain | Notes |
|---|---|---|
| None | 1 | 2D-style, used for debugging |
| Linear | `1 − r·(d − ref)/(max − ref)` | reaches exactly 0 at `max`; easy to tune, sounds unnatural |
| Inverse distance | `ref / (ref + r·(d − ref))` | physical: −6 dB per doubling of distance (r = 1); default |
| Exponential | `(d / ref)^(−r)` | same shape as inverse, steeper with r > 1 |

These are the OpenAL / FMOD / Wwise standard curves; designers often hand-draw curves per sound instead.

**Air absorption** adds a per-voice one-pole low-pass with cutoff `20 kHz · e^(−d/100 m)`: about 11 kHz
at 60 m, 2.7 kHz at 200 m. Real absorption depends on humidity and frequency (ISO 9613-1), but the
exponential cutoff is a cheap, convincing stand-in.

The reverb send falls off more slowly (`√gain`), so far sources become proportionally more reverberant:
the direct-to-reverb ratio is one of the strongest distance cues our ears use.

## Breakdown
1. Stage **Spatialize**: `gain = volume × attenuation(d)`, reverb send `∝ √attenuation`.
2. Stage **Filter**: if `cutoff < 19 kHz`, one-pole LP per channel (shared with [occlusion](audio_occlusion.md)).
3. Stage **Voice select** also uses attenuation to rank voices (far sounds go virtual first).

## Optimization
Measured (`tools/audio_bench`, 69 voices, most 6–56 m away):

| Value | Audio CPU (% of 1 core) | Own stage | RAM |
|---|---|---|---|
| None | 6.31 | Spatialize 0.60 | 0 |
| Linear | 6.49 | Spatialize 0.59 | 0 |
| Inverse distance | 6.58 | Spatialize 0.63 | 0 |
| Exponential | 6.33 | Spatialize 0.61 | 0 |
| Air absorption off | 4.99 | Filter 0.18 | 0 |
| Air absorption on | 6.45 | Filter 1.57 | 0 |

The curve is computed once per voice per block, so all models cost the same (the differences are run
noise). Air absorption costs ≈ 1.4% of a core because it is per-sample work on every distant voice.
Δ GPU 0, Δ RAM 0. Optimisation: skip the filter when the cutoff is above ~19 kHz (done here) and
update coefficients per block, not per sample (done here).

## Rebuild
1. Give every 3D sound `minDistance`/`maxDistance`; compute `d` per block.
2. Apply the chosen curve; clamp at `max` (or stop the voice when inaudible).
3. Add a distance low-pass; reuse the same filter for occlusion so you only pay once.
4. Scale reverb send by `√gain` or a separate curve for distance perception.
5. Verify: walk away from a source; volume should halve (−6 dB) each time distance doubles with Inverse.

Files: `src/modules/audio/audio_mixer.cpp` (`attenuation`, `renderVoice`).
