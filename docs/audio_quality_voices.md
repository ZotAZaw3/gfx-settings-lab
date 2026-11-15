# Max voices (audio quality)

> Menu: **Audio → Quality → Max voices** · Setting id: `audio.quality.voices` · Owner module: `audio`
> Values: 8–256 · Presets: Low=24, Medium=48, High=96, Ultra=192

## Definition
The number of sounds the game actually mixes at once. In a busy battle there may be hundreds of
sounds "playing"; only the most important ones are heard, and the rest are tracked silently so
they can come back in sync. Low values save CPU and can make distant or quiet sounds drop out.
This is what most games call "Audio quality" or "Number of channels".

## How it works
**Real vs virtual voices.** Every playing sound is a record (up to 512). Each 256-frame block the
mixer scores every record:

```
score = volume × busVolume × distanceGain × (1 − 0.75 × occlusion) × (0.5 + priority/255)
streams +10 (never virtualized), current real voices ×1.25 (hysteresis)
```

It keeps the top `maxVoices` as **real** (`std::nth_element`, O(n)) and makes the rest **virtual**:
a virtual voice only advances its playhead (`srcPos += frames × pitch × rate ratio`), costing a
few nanoseconds instead of resampling, filtering and panning. When a virtual voice becomes loud
enough again it is promoted at the correct position, so a looping engine is still in sync.

Promotion fades in from silence over one block; demotion renders one more block fading to zero,
then frees the slot. The 25% hysteresis stops voices near the cut-off from flapping every block.

**Real-voice slots** are pre-allocated at configure time: per slot a 2-channel source window
(`256 × 8 + 32` frames, enough for 8× pitch), a 256-frame output scratch, filter state, and HRTF
delay lines when HRTF is on. That is why RAM scales with this setting and why changing it restarts
the device.

**Stealing.** If all 512 records are used, `play()` steals the record with the lowest priority,
then the quietest; it refuses if the new sound has lower priority than everything playing.

## Breakdown
1. Stage **Voice select**: score all playing records, partition, promote/demote, advance virtual voices.
2. Per real voice: Resample → Filter → Spatialize (see those docs).
3. Counters: `audio.voices_real`, `audio.voices_virtual`.

| Resource | Size | Notes |
|---|---|---|
| Voice records | 512 × (desc + runtime) ≈ 64 KB | fixed |
| Real slot | ≈ 18.7 KB each | source window 2 × 2080 floats + scratch |

## Optimization
Measured (`tools/audio_bench`, 305 playing sounds, stereo 48 kHz, cubic resampler):

| Max voices | Audio CPU (% of 1 core) | RAM (KB) | Real / virtual |
|---|---|---|---|
| 16 | 1.68 | 713 | 16 / 289 |
| 32 | 3.25 | 1009 | 32 / 273 |
| 64 | 6.29 | 1602 | 64 / 241 |
| 96 | 9.20 | 2194 | 96 / 209 |
| 128 | 12.10 | 2787 | 128 / 177 |
| 192 | 17.45 | 3973 | 192 / 113 |
| 256 | 23.22 | 5158 | 256 / 49 |

Cost is linear: ≈ 0.09% of a core and 18.5 KB per real voice on this machine. Δ GPU 0. This is the
single biggest audio CPU lever. Games typically ship 32–64 on low-end/mobile, 64–128 on PC/console;
beyond ~100 audible sounds you can't distinguish them anyway.

Tricks used: partial selection (`nth_element`) instead of a full sort; scoring once per block, not per
sample; virtual voices skip every DSP stage.

## Rebuild
1. Separate "playing" (bookkeeping) from "mixing" (expensive). Keep a playhead for every playing sound.
2. Each update, compute audibility (volume × attenuation × occlusion × priority) and mix only the
   top N; advance the rest.
3. Add hysteresis and short fades on promote/demote to avoid clicks.
4. Pre-allocate the N mixing slots; no allocation on the audio thread.
5. Verify: spawn 300 loops with max voices 16, walk around; nearby sounds must always be audible and
   never click when they swap in.

Files: `src/modules/audio/audio_mixer.cpp` (`selectVoices`, `renderChunk`), `audio_system.cpp` (`allocVoice`).
